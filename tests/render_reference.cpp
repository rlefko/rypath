// SPDX-License-Identifier: GPL-3.0-only

#include "render_reference.hpp"

#include "cie_xyz_data.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <stdexcept>

namespace rypath::test {
namespace {

using XYZ = std::array<double, 3>;

double reference_y_normalizer() {
    double integral = 0;
    for (std::size_t row = 1; row < detail::cie_xyz.size(); ++row) {
        integral += (detail::cie_xyz[row - 1][1] + detail::cie_xyz[row][1]) * 0.5;
    }
    return integral;
}

XYZ reference_sensor(std::size_t row, double fraction, double scale) {
    XYZ xyz{};
    for (std::size_t c = 0; c < xyz.size(); ++c) {
        xyz[c] = scale * ((1 - fraction) * detail::cie_xyz[row][c] +
                          fraction * detail::cie_xyz[row + 1][c]);
    }
    return xyz;
}

double reference_reflectance(const SpectralMaterial &material, double nm) {
    for (std::size_t k = 1; k < spectral_knots.size(); ++k) {
        if (nm <= spectral_knots[k]) {
            const double left = spectral_knots[k - 1];
            const double right = spectral_knots[k];
            return ((right - nm) * material.reflectance[k - 1] +
                    (nm - left) * material.reflectance[k]) /
                   (right - left);
        }
    }
    throw std::runtime_error("Reference wavelength is out of range");
}

double floor_directional_second_moment() {
    // Let c = cos(theta) above the plane. NEE samples c with density 1/2,
    // plus a zero contribution below the plane. BSDF samples c with density
    // 2c. The two directions are independent at a fixed wavelength.
    constexpr int intervals = 65536;
    double nee_mean = 0;
    double bsdf_mean = 0;
    double square_mean = 0;
    for (int i = 0; i < intervals; ++i) {
        const double c = (i + 0.5) / intervals;
        const double ratio = 4 * c;
        const double nee = ratio / (1 + ratio * ratio);
        const double bsdf = ratio * ratio / (1 + ratio * ratio);
        nee_mean += 0.5 * nee / intervals;
        bsdf_mean += 2 * c * bsdf / intervals;
        square_mean += (0.5 * nee * nee + 2 * c * bsdf * bsdf) / intervals;
    }
    if (std::abs(nee_mean + bsdf_mean - 1) > 1e-9) {
        throw std::runtime_error("Floor MIS quadrature violated the Lambertian identity");
    }
    return square_mean + 2 * nee_mean * bsdf_mean;
}

struct Vec {
    double x = 0;
    double y = 0;
    double z = 0;
};

Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, double b) { return {a.x * b, a.y * b, a.z * b}; }
double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec unit(Vec v) { return v * (1 / std::sqrt(dot(v, v))); }
Vec from_point(GeometryPoint p) { return {p.x, p.y, p.z}; }
Vec transform_point(GeometryPoint p, const GeometryTransform &transform) {
    const auto &m = transform.values;
    return {dot({m[0], m[1], m[2]}, from_point(p)) + m[3],
            dot({m[4], m[5], m[6]}, from_point(p)) + m[7],
            dot({m[8], m[9], m[10]}, from_point(p)) + m[11]};
}

struct Projected {
    double u;
    double v;
};
Projected project(Vec p, int axis) {
    if (axis == 0) {
        return {p.y, p.z};
    }
    if (axis == 1) {
        return {p.z, p.x};
    }
    return {p.x, p.y};
}
double edge(Projected a, Projected b, Projected c) {
    return (b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u);
}

struct Triangle {
    Vec anchor;
    Vec normal;
    int axis;
    std::array<Projected, 3> corners;
    std::uint32_t material;
};

std::vector<Triangle> triangles(const RenderScene &scene) {
    std::vector<Triangle> result;
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        const auto &instance = scene.geometry.instances[i];
        const auto &mesh = scene.geometry.meshes[instance.mesh_index];
        for (const auto &[a, b, c] : mesh.triangles) {
            const Vec p0 = transform_point(mesh.positions[a], instance.transform);
            const Vec p1 = transform_point(mesh.positions[b], instance.transform);
            const Vec p2 = transform_point(mesh.positions[c], instance.transform);
            const Vec n = unit(cross(p1 - p0, p2 - p0));
            const std::array magnitudes{std::abs(n.x), std::abs(n.y), std::abs(n.z)};
            const auto axis =
                static_cast<int>(std::ranges::max_element(magnitudes) - magnitudes.begin());
            result.push_back({p0,
                              n,
                              axis,
                              {project(p0, axis), project(p1, axis), project(p2, axis)},
                              scene.instance_materials[i]});
        }
    }
    return result;
}

struct Hit {
    const Triangle *triangle = nullptr;
    double distance = std::numeric_limits<double>::infinity();
};

Hit intersect(const std::vector<Triangle> &mesh, Vec origin, Vec direction) {
    Hit hit;
    for (const auto &t : mesh) {
        const double slope = dot(t.normal, direction);
        if (slope == 0) {
            continue;
        }
        const double distance = dot(t.normal, t.anchor - origin) / slope;
        if (!(distance > 0 && distance < hit.distance)) {
            continue;
        }
        const auto p = project(origin + direction * distance, t.axis);
        const double a = edge(t.corners[0], t.corners[1], p);
        const double b = edge(t.corners[1], t.corners[2], p);
        const double c = edge(t.corners[2], t.corners[0], p);
        if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0)) {
            hit = {&t, distance};
        }
    }
    return hit;
}

double uniform(std::mt19937_64 &rng) { return static_cast<double>(rng() >> 11) * 0x1p-53; }

Vec sample_cosine(Vec n, std::mt19937_64 &rng) {
    const Vec tangent = unit(cross(n, std::abs(n.z) < 0.9 ? Vec{0, 0, 1} : Vec{0, 1, 0}));
    const Vec bitangent = cross(n, tangent);
    const double azimuth = 2 * std::numbers::pi * uniform(rng);
    const double cosine = std::sqrt(uniform(rng));
    const double sine = std::sqrt(1 - cosine * cosine);
    return tangent * (sine * std::cos(azimuth)) + bitangent * (sine * std::sin(azimuth)) +
           n * cosine;
}

double trace(const RenderScene &scene, const std::vector<Triangle> &mesh, Vec origin, Vec direction,
             double nm, std::mt19937_64 &rng, std::uint32_t &max_scatterings) {
    double throughput = 1;
    // BSDF-only independent estimator. Environment is the sole emitter.
    for (std::uint32_t scattered = 0; scattered < 1024; ++scattered) {
        const auto hit = intersect(mesh, origin, direction);
        if (hit.triangle == nullptr) {
            return throughput;
        }
        max_scatterings = std::max(max_scatterings, scattered + 1);
        Vec n = hit.triangle->normal;
        if (dot(n, direction) > 0) {
            n = n * -1;
        }
        throughput *= reference_reflectance(scene.materials[hit.triangle->material], nm);
        if (scattered >= 2) {
            if (uniform(rng) >= 0.75) {
                return 0;
            }
            throughput /= 0.75;
        }
        // Reference geometry is well-conditioned and uses a double-precision
        // offset over four orders of magnitude smaller than scene features.
        origin = origin + direction * hit.distance + n * 1e-10;
        direction = sample_cosine(n, rng);
    }
    throw std::runtime_error("Reference exceeded emergency scattering limit");
}

} // namespace

AnalyticMoments analytic_xyz(const SpectralMaterial *floor) {
    AnalyticMoments moments;
    // Three-point Gauss-Legendre integrates the product of two linear spectra
    // and its square exactly inside each 1 nm interval.
    const double node = std::sqrt(3.0 / 5.0) / 2;
    const std::array offsets{0.5 - node, 0.5, 0.5 + node};
    constexpr std::array weights{5.0 / 18.0, 4.0 / 9.0, 5.0 / 18.0};
    const double sensor_scale = 470 / reference_y_normalizer();
    const double angular_factor = floor != nullptr ? floor_directional_second_moment() : 1;
    for (std::size_t row = 0; row + 1 < detail::cie_xyz.size(); ++row) {
        for (std::size_t k = 0; k < offsets.size(); ++k) {
            const double nm = 360 + static_cast<double>(row) + offsets[k];
            const double rho = floor != nullptr ? reference_reflectance(*floor, nm) : 1;
            const XYZ sensor = reference_sensor(row, offsets[k], sensor_scale);
            for (std::size_t c = 0; c < sensor.size(); ++c) {
                const double signal = rho * sensor[c];
                moments.mean[c] += weights[k] * signal / 470;
                moments.variance[c] += weights[k] * signal * signal * angular_factor / 470;
            }
        }
    }
    for (std::size_t c = 0; c < moments.mean.size(); ++c) {
        moments.variance[c] -= moments.mean[c] * moments.mean[c];
    }
    return moments;
}

ReferenceImage reference_image(const RenderScene &scene, const RenderSettings &settings) {
    const auto &[width, height, samples, batch_size, seed] = settings;
    const auto mesh = triangles(scene);
    const auto &camera = scene.camera;
    const Vec origin = from_point(camera.origin);
    const Vec forward = unit(from_point(camera.target) - origin);
    const Vec right = unit(cross(forward, from_point(camera.up)));
    const Vec up = cross(right, forward);
    const double half_height = std::tan(camera.vertical_fov_degrees * std::numbers::pi / 360);
    const double half_width = half_height * width / height;
    const double sensor_scale = 470 / reference_y_normalizer();
    std::mt19937_64 camera_rng(seed);
    std::mt19937_64 path_rng(seed ^ 0xd287392a194d6fe9ULL);
    ReferenceImage image;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint64_t s = 0; s < samples; ++s) {
                const double u = (x + uniform(camera_rng)) / width;
                const double v = (y + uniform(camera_rng)) / height;
                const Vec direction = unit(forward + right * ((2 * u - 1) * half_width) +
                                           up * ((1 - 2 * v) * half_height));
                const double offset = 470 * uniform(camera_rng);
                const auto row =
                    std::min(static_cast<std::size_t>(offset), detail::cie_xyz.size() - 2);
                const double radiance = trace(scene, mesh, origin, direction, 360 + offset,
                                              path_rng, image.max_scatterings);
                const XYZ sensor =
                    reference_sensor(row, offset - static_cast<double>(row), sensor_scale);
                for (std::size_t c = 0; c < sensor.size(); ++c) {
                    image.mean[c] += sensor[c] * radiance;
                }
            }
        }
    }
    for (auto &channel : image.mean) {
        channel /= static_cast<double>(width) * height * static_cast<double>(samples);
    }
    return image;
}

} // namespace rypath::test
