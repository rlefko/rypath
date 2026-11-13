// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"
#include "triangle_attributes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace rypath;

constexpr double pi = std::numbers::pi;
constexpr float rho = 0.625f;
constexpr float footprint = 0.0625f;
constexpr float saddle_twist = 3;
constexpr std::uint32_t paths = 32749;
constexpr std::array<std::uint64_t, 2> seeds{0x9e3779b97f4a7c15ULL, 0x243f6a8885a308d3ULL};

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}

GeometryMesh patch(std::array<GeometryPoint, 4> corners) {
    GeometryMesh mesh;
    mesh.positions.assign(corners.begin(), corners.end());
    mesh.patches.push_back({0, 1, 2, 3});
    return mesh;
}

GeometryMesh plane(float z) { return patch({{{-1, -1, z}, {1, -1, z}, {-1, 1, z}, {1, 1, z}}}); }

void add_mesh(RenderScene &scene, GeometryMesh mesh, std::uint32_t material,
              std::uint32_t emission = no_area_emission) {
    const auto index = static_cast<std::uint32_t>(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({.mesh_index = index});
    scene.instance_materials.push_back(material);
    scene.instance_emissions.push_back(emission);
}

RenderScene scene_base() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.max_depth = 1;
    scene.materials = {{.kind = SpectralMaterialKind::constant},
                       {.kind = SpectralMaterialKind::constant, .constant = rho}};
    scene.area_emissions.resize(1);
    scene.area_emissions.front().radiance.fill(1);
    return scene;
}

RenderScene directional_scene(bool smooth_receiver) {
    auto scene = scene_base();
    scene.raster_camera = RasterCamera{{-footprint, footprint, -2},
                                       {-footprint, -footprint, 2},
                                       {2 * footprint, 0, 0},
                                       {0, 2 * footprint, 0}};
    auto receiver = plane(0);
    if (smooth_receiver) {
        receiver.normals = {{-1, 1, -1}, {0, -1, -1}, {-2, -1, -2}, {7, 2, -2}};
        receiver.uvs = {{{9, -3}}, {{1, 8}}, {{-2, -4}}, {{0, 0}}};
    } else {
        receiver.patches.clear();
        receiver.triangles = {{0, 1, 2}, {1, 3, 2}};
    }
    add_mesh(scene, std::move(receiver), 1);

    // P(u,v)=(1/8+3u/4, -5/8+5v/4, -1+15uv/16). The receiver
    // sees this entire curved patch and the facing triangle on its left.
    auto emitter = patch({{{0.125f, -0.625f, -1},
                           {0.875f, -0.625f, -1},
                           {0.125f, 0.625f, -1},
                           {0.875f, 0.625f, -0.0625f}}});
    emitter.positions.insert(emitter.positions.end(), {{-0.75f, 0, -0.75f},
                                                       {-0.25f, 0, -0.75f},
                                                       {-0.25f, 0.75f, -0.75f},
                                                       {-1, -1, 1},
                                                       {1, -1, 1},
                                                       {-1, 1, 1}});
    emitter.triangles = {{4, 5, 6}, {7, 8, 9}};
    add_mesh(scene, std::move(emitter), 0, 0);
    return scene;
}

RenderScene saddle_scene() {
    auto scene = scene_base();
    scene.raster_camera = RasterCamera{{0.5f, 0.5f, 0.5f},
                                       {-footprint, -footprint, -0.5f},
                                       {2 * footprint, 0, 0},
                                       {0, 2 * footprint, 0}};
    constexpr float h = saddle_twist / 4;
    add_mesh(scene, patch({{{0, 0, h}, {1, 0, -h}, {0, 1, -h}, {1, 1, h}}}), 1, 0);
    return scene;
}

struct Node {
    double x;
    double weight;
};

// Gauss-Legendre nodes on [0,1]. Recompute each order so the convergence
// check changes the full raster and emitter rules independently of sampling.
std::vector<Node> gauss_rule(int order) {
    std::vector<Node> rule;
    for (int k = 0; k < order; ++k) {
        double root = std::cos(pi * (k + 0.75) / (order + 0.5));
        for (int iteration = 0; iteration < 8; ++iteration) {
            double p = 1, previous = 0;
            for (int j = 1; j <= order; ++j) {
                const double value = ((2 * j - 1) * root * p - (j - 1) * previous) / j;
                previous = p;
                p = value;
            }
            const double slope = order * (previous - root * p) / (1 - root * root);
            const double step = p / slope;
            if (std::abs(step) < 1e-15) {
                rule.push_back({(root + 1) / 2, 1 / ((1 - root * root) * slope * slope)});
                break;
            }
            root -= step;
        }
    }
    require(rule.size() == static_cast<std::size_t>(order), "Gauss rule failed to converge");
    return rule;
}

struct Moments {
    double mean = 0;
    double second = 0;
    double bsdf_mean = 0;
};

struct AtReceiver {
    double physical_mean = 0;
    double light_mean = 0;
    double bsdf_mean = 0;
    double raw_second = 0;

    // q is the emitter proposal's probability density in this chart;
    // f is the cosine-sampling hit density pulled back from solid angle.
    // On visible support the Lambertian physical integrand is rho*f.
    // Given a camera ray the two draws are independent, including null slots.
    void integrate(double q, double f, double weight) {
        const double sum_squares = q * q + f * f;
        const double light_value = f * q / sum_squares;
        const double bsdf_value = f * f / sum_squares;
        physical_mean += weight * f;
        light_mean += weight * q * light_value;
        bsdf_mean += weight * f * bsdf_value;
        raw_second += weight * (q * light_value * light_value + f * bsdf_value * bsdf_value);
    }

    void accumulate(Moments &total, double weight) const {
        total.mean += weight * rho * physical_mean;
        total.bsdf_mean += weight * rho * bsdf_mean;
        total.second += weight * rho * rho * (raw_second + 2 * light_mean * bsdf_mean);
    }
};

AtReceiver directional_receiver(std::array<double, 2> point, bool smooth,
                                const std::vector<Node> &rule) {
    const auto [x, y] = point;
    double nx = 0, ny = 0, nz = -1;
    if (smooth) {
        // Invert the planar square analytically. Expand the input normal
        // field in monomials to preserve the unequal corner lengths.
        const double u = (x + 1) / 2;
        const double v = (y + 1) / 2;
        nx = -1 + u - v + 8 * u * v;
        ny = 1 - 2 * u - 2 * v + 5 * u * v;
        nz = -1 - v;
    }
    const double normalizer = pi * std::hypot(nx, ny, nz);
    const auto hit_density = [&](double dx, double dy, double dz, double projected_area) {
        const double r2 = dx * dx + dy * dy + dz * dz;
        return std::max(0.0, nx * dx + ny * dy + nz * dz) * projected_area / (normalizer * r2 * r2);
    };
    AtReceiver result;
    for (const auto [u, wu] : rule) {
        for (const auto [v, wv] : rule) {
            // Compute projected area directly for z=-1+(15/16)uv,
            // using dOmega = (-R dot (P_u cross P_v)) / |R|^3 du dv.
            constexpr double width = 0.75, height = 1.25, twist = 0.9375;
            const double dx = 0.125 + width * u - x;
            const double dy = -0.625 + height * v - y;
            const double dz = -1 + twist * u * v;
            const double projected =
                height * twist * v * dx + width * twist * u * dy - width * height * dz;
            // A positive projected area throughout the square means every
            // target is the only root seen from this receiver. A second
            // isolated root of the bilinear graph has the opposite sign.
            require(projected > 0, "Directional patch fixture contains a hidden root");
            result.integrate(0.25, hit_density(dx, dy, dz, projected), wu * wv);

            // A horizontal triangle chord at x=-3/4+u/2 spans y=[0,3u/4].
            // Its normalized area measure is 2u du dv. A third primitive
            // behind the receiver and the fourth (null) slot are both black.
            const double tri_x = -0.75 + 0.5 * u - x;
            const double tri_y = 0.75 * u * v - y;
            const double projected_triangle = 0.75 * 0.5 * 0.75 * u;
            result.integrate(0.5 * u, hit_density(tri_x, tri_y, -0.75, projected_triangle),
                             wu * wv);
        }
    }
    return result;
}

AtReceiver saddle_receiver(double dx, double dy, const std::vector<Node> &rule) {
    // The finite camera footprint solves (1-t)/2 = twist*dx*dy*t^2.
    // Only this root is in the patch on every camera ray in the fixture.
    const double t = 2 / (1 + std::sqrt(1 + 8 * saddle_twist * dx * dy));
    const double a = 0.5 + dx * t, b = 0.5 + dy * t;
    const double normalizer =
        pi * std::hypot(saddle_twist * (a - 0.5), saddle_twist * (b - 0.5), 1.0);
    AtReceiver result;
    // For a chord from (a,b) to (a+u,b+v), both oriented projected
    // cosines have numerator twist*u*v. The chord meets the surface
    // only at its endpoints. The two same-sign quadrants are visible;
    // integrate each with two Duffy triangles anchored at the receiver.
    for (const int sign : {-1, 1}) {
        const double width = sign < 0 ? -a : 1 - a;
        const double height = sign < 0 ? -b : 1 - b;
        for (const auto [r, wr] : rule) {
            for (const auto [s, ws] : rule) {
                for (const bool transpose : {false, true}) {
                    const double u = width * (transpose ? s : 1);
                    const double v = height * (transpose ? 1 : s);
                    // Cancel the common radial factor analytically before
                    // evaluating the projected solid-angle kernel.
                    const double z = saddle_twist * ((a - 0.5) * v + (b - 0.5) * u + r * u * v);
                    const double r2 = u * u + v * v + z * z;
                    const double projected = saddle_twist * u * v;
                    const double density = projected * projected / (normalizer * r2 * r2);
                    result.integrate(1, density, wr * ws * width * height * r);
                }
            }
        }
    }
    return result;
}

enum class Fixture { directional, smooth, saddle };

Moments quadrature(Fixture fixture, int raster_order, int emitter_order) {
    const auto raster = gauss_rule(raster_order);
    const auto emitter = gauss_rule(emitter_order);
    Moments result;
    for (const auto [i, wi] : raster) {
        for (const auto [j, wj] : raster) {
            const double x = footprint * (2 * i - 1);
            const double y = footprint * (2 * j - 1);
            const auto receiver = fixture == Fixture::saddle
                                      ? saddle_receiver(x, y, emitter)
                                      : directional_receiver({x - footprint, y + footprint},
                                                             fixture == Fixture::smooth, emitter);
            receiver.accumulate(result, wi * wj);
        }
    }
    return result;
}

Moments reference(Fixture fixture) {
    const auto coarse = quadrature(fixture, 6, 24);
    const auto fine = quadrature(fixture, 10, 48);
    require(std::abs(fine.mean - coarse.mean) < 2e-7 &&
                std::abs(fine.second - coarse.second) < 2e-7 &&
                std::abs(fine.bsdf_mean - coarse.bsdf_mean) < 2e-7 &&
                fine.second > fine.mean * fine.mean && fine.bsdf_mean > 1e-5,
            "Reference quadrature did not converge or has negligible BSDF-hit signal");
    return fine;
}

Moments furnace_reference() {
    // On a plane every direction in the shading hemisphere escapes, even
    // when it leaves through the back of the geometric plane. Integrate
    // over c = cos(theta): the sphere proposal has density 1/2 there and
    // the Lambertian proposal has density 2c.
    AtReceiver receiver;
    for (const auto [c, weight] : gauss_rule(32)) {
        receiver.integrate(0.5, 2 * c, weight);
    }
    Moments result;
    receiver.accumulate(result, 1);
    require(std::abs(result.mean - rho) < 1e-14 && result.second > result.mean * result.mean,
            "Furnace quadrature disagrees with the exact Lambertian response");
    return result;
}

void validation_contract() {
    auto mesh = patch({{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 1}}});
    require(validate_patch_geometry(mesh, {}).empty(), "Valid patch derivatives were rejected");
    for (const float scale : {0x1p-40f, 0x1p40f}) {
        const GeometryTransform transform{{scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0}};
        require(validate_patch_geometry(mesh, transform).find("world derivatives") !=
                    std::string::npos,
                "Out-of-range squared world Jacobian was accepted");
    }

    const auto cancelled = patch({{{0, 0, 0}, {1, 1, 1}, {1, 0, 1}, {2, 1, 2}}});
    const GeometryTransform matrix{{0x1p24f, 1, -0x1p24f, 0, 0x1p24f, 0, -0x1p24f, 0, 1, 0, 0, 0}};
    require(validate_patch_geometry(cancelled, matrix).find("tangent arithmetic") !=
                std::string::npos,
            "Float tangent cancellation must be rejected before GPU execution");
    auto offset_matrix = matrix;
    for (const std::size_t i : {0U, 2U, 4U, 6U}) {
        offset_matrix.values[i] /= 0x1p14f;
    }
    require(validate_patch_geometry(cancelled, offset_matrix).find("ray offsets") !=
                std::string::npos,
            "Excessive inverse-traversal displacement must be rejected");
    require(validate_patch_geometry(plane(0x1p20f), {}).find("world-coordinate precision") !=
                std::string::npos,
            "A large normal translation must respect the offset precision domain");
    auto tangential = plane(0);
    for (auto &p : tangential.positions) {
        p.x += 0x1p20f;
        p.y += 0x1p20f;
    }
    require(validate_patch_geometry(tangential, {}).find("endpoint connections") !=
                std::string::npos,
            "Tangential coordinate precision must preserve distinct endpoint connections");
    GeometryTransform recenter;
    recenter.values[11] = -1024;
    require(validate_patch_geometry(plane(1024), recenter).find("ray offsets") != std::string::npos,
            "Inverse translation roundoff must be bounded even with small world coordinates");
    require(validate_patch_geometry(plane(1024), {}).empty(),
            "A bounded translation should retain adequate offset precision");

    for (const float sign : {-1.0f, 1.0f}) {
        mesh.normals.assign(4, {0, 0, sign});
        require(validate_normal_emission(mesh, {}, false, false, mesh.normals).empty(),
                "A strict sign for the whole square should certify either orientation");
    }
    for (const float z : {0.125f, 0.375f}) {
        // The signed projection is z-u(1-u), positive at all four corners.
        // For z=1/8 it reverses inside. For z=3/8 it stays positive, but the
        // middle Bernstein coefficient z-1/2 is negative in both cases.
        mesh.normals = {{0, 1, z}, {0, 0, z}, {0, 1, z}, {0, 0, z}};
        require(validate_normal_emission(mesh, {}, false, false, mesh.normals)
                        .find("uncertified shading-normal orientation") != std::string::npos,
                "Corner-only checks incorrectly certified one-sided patch normals");
        require(validate_normal_emission(mesh, {}, false, true, mesh.normals).empty(),
                "Two-sided emitter needlessly required a certified orientation");
    }
    mesh.normals = {{}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    require(!validate_normal_emission(mesh, {}, false, false, mesh.normals).empty(),
            "A zero corner coefficient was certified");
    mesh.patches.front()[3] = 4;
    require(validate_patch_geometry(mesh, {}).find("index is out of range") != std::string::npos,
            "Out-of-range patch index reached derivative reconstruction");
}

std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto result = build_metal_renderer(scene);
    require(result.status == GeometryStatus::success && result.scene, result.diagnostic);
    return std::move(result.scene);
}

RenderRequest request(std::uint32_t count, std::uint64_t seed = seeds.front()) {
    return {.width = 1, .height = 1, .seed = seed, .pixel_count = 1, .sample_count = count};
}

std::vector<RenderSample> sample(MetalRenderScene &renderer, const RenderRequest &query) {
    auto result = renderer.sample(query);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == query.sample_count && result.guard_count > 0,
            "Expected a complete guarded patch dispatch");
    for (std::size_t i = 0; i < result.samples.size(); ++i) {
        require(result.samples[i].sample == query.sample_begin + i && result.samples[i].pixel == 0,
                "Patch dispatch changed absolute sample indices");
    }
    return std::move(result.samples);
}

void check_mean(std::string_view label, MetalRenderScene &renderer, Moments expected,
                double primary = 0) {
    for (const auto seed : seeds) {
        const auto samples = sample(renderer, request(paths, seed));
        double observed = 0;
        for (const auto &s : samples) {
            require(s.scatterings == 1 && s.radiance >= primary,
                    "Patch reference paths must scatter once and preserve primary emission");
            observed += s.radiance - primary;
        }
        observed /= paths;
        // Each fixed seed has a six-standard-error gate, with a fixed
        // additive bound for float arithmetic and displaced shadow endpoints.
        const double se = std::sqrt((expected.second - expected.mean * expected.mean) / paths);
        const double limit = 6 * se + 3e-5;
        std::cout << std::setprecision(10) << label << " seed=" << seed
                  << " expected=" << expected.mean << " observed=" << observed << " se=" << se
                  << " limit=" << limit << " bsdf_mean=" << expected.bsdf_mean << '\n';
        require(std::abs(observed - expected.mean) <= limit, "Patch transport mean mismatch");
    }
}

void blocker() {
    auto scene = directional_scene(false);
    if (!scene.raster_camera) {
        throw std::runtime_error("Directional fixture requires a raster camera");
    }
    scene.raster_camera->origin.z = -0.03125f;
    scene.raster_camera->corner.z = 0.03125f;
    // The pinhole lies between the receiver and this opaque patch. Every
    // light connection or BSDF ray toward the visible emitters crosses it.
    add_mesh(scene, plane(-0.046875f), 0);
    const auto samples = sample(*build(scene), request(2051));
    require(std::ranges::all_of(
                samples, [](const auto &s) { return s.radiance == 0 && s.scatterings == 1; }),
            "Opaque patch leaked an area-light connection");
}

void furnace() {
    auto scene = scene_base();
    scene.environment.fill(1);
    auto receiver = plane(0);
    receiver.normals.assign(4, {3, 0, -1});
    add_mesh(scene, std::move(receiver), 1);
    const auto expected = furnace_reference();
    for (const float side : {-1.0f, 1.0f}) {
        scene.raster_camera = RasterCamera{{0, 0, 2 * side},
                                           {-footprint, -footprint, -2 * side},
                                           {2 * footprint, 0, 0},
                                           {0, 2 * footprint, 0}};
        check_mean(side < 0 ? "Patch furnace front" : "Patch furnace back", *build(scene),
                   expected);
    }
}

void affine_furnace() {
    const auto expected = furnace_reference();
    for (const float coefficient : {1.0f, 16.0f}) {
        auto scene = scene_base();
        scene.environment.fill(1);
        add_mesh(scene, patch({{{0, 0, 0}, {1, 1, 1}, {1, 0, 1}, {2, 1, 2}}}), 1);
        scene.geometry.instances.front().transform = {
            {coefficient, 1, -coefficient, 0, coefficient, 0, -coefficient, 0, 1, 0, 0, 0}};
        scene.raster_camera = RasterCamera{{0.5f, 16, 1},
                                           {-footprint, -16, -footprint},
                                           {2 * footprint, 0, 0},
                                           {0, 0, 2 * footprint}};
        check_mean("Affine patch furnace", *build(scene), expected);
    }
}

void recentered_furnace() {
    auto scene = scene_base();
    scene.environment.fill(1);
    add_mesh(scene, plane(16), 1);
    scene.geometry.instances.front().transform.values[11] = -16;
    scene.raster_camera = RasterCamera{
        {0, 0, 2}, {-footprint, -footprint, -2}, {2 * footprint, 0, 0}, {0, 2 * footprint, 0}};
    check_mean("Recentered patch furnace", *build(scene), furnace_reference());
}

void coincident_patch_sample() {
    auto scene = scene_base();
    auto mesh = plane(0);
    for (auto &p : mesh.positions) {
        p.x += 8192;
        p.y += 8192;
    }
    add_mesh(scene, std::move(mesh), 1, 0);
    scene.area_emissions.front().two_sided = true;
    scene.raster_camera = RasterCamera{{8192, 8192, 0.5f},
                                       {-footprint, -footprint, -1},
                                       {2 * footprint, 0, 0},
                                       {0, 2 * footprint, 0}};
    auto query = request(1, 263);
    query.sample_begin = 49180;
    const auto result = sample(*build(scene), query);
    require(result.front().radiance == 1 && result.front().scatterings == 1,
            "A coincident same-patch connection must be empty without dropping the continuation");
}

void translated_lighting() {
    double baseline = 0;
    for (const float translation : {0.0f, 1024.0f}) {
        auto scene = scene_base();
        scene.materials[1].constant = 1;
        scene.area_emissions.front().two_sided = true;
        add_mesh(scene, plane(translation), 1);
        add_mesh(scene, plane(translation + 1), 0, 0);
        scene.raster_camera = RasterCamera{{0, 0, translation + 0.5f},
                                           {-footprint, -footprint, -1},
                                           {2 * footprint, 0, 0},
                                           {0, 2 * footprint, 0}};
        double mean = 0;
        for (const auto &value : sample(*build(scene), request(4096, 2))) {
            mean += value.radiance / 4096;
        }
        if (translation == 0) {
            baseline = mean;
            require(baseline > 0.4, "Translated lighting fixture must carry a strong signal");
        } else {
            // Identical variates; an accepted coordinate offset is below
            // 1/1024 of tangent altitude. Allow 1% aggregate geometric drift,
            // including changed grazing visibility, without a statistical retry.
            require(std::abs(mean - baseline) < 0.01 * baseline,
                    "Translation changed the patch lighting beyond the precision budget");
        }
    }
}

void sidedness() {
    auto scene = saddle_scene();
    scene.max_depth = 0;
    scene.environment.fill(3);
    for (const bool mirror : {false, true}) {
        auto &instance = scene.geometry.instances.front();
        instance.transform.values[0] = mirror ? -1 : 1;
        instance.transform.values[3] = mirror ? 1 : 0;
        for (const bool reverse : {false, true}) {
            scene.instance_reverse_orientation = {mirror != reverse ? 1U : 0U};
            for (const bool two_sided : {false, true}) {
                scene.area_emissions.front().two_sided = two_sided;
                const float expected = reverse && !two_sided ? 0 : 1;
                const auto samples = sample(*build(scene), request(37));
                require(std::ranges::all_of(samples,
                                            [&](const auto &s) {
                                                return s.scatterings == 0 && s.radiance == expected;
                                            }),
                        "Bilinear primary emission changed under orientation or sidedness");
            }
        }
    }
}

void batching(MetalRenderScene &renderer) {
    constexpr std::uint32_t count = 67;
    auto full_query = request(count);
    full_query.threadgroup_width = 32;
    const auto full = sample(renderer, full_query);
    require(std::ranges::any_of(full, [](const auto &s) { return s.radiance > 0; }),
            "Patch batching fixture had no light signal");
    for (std::uint32_t begin = 0; begin < count; begin += 7) {
        auto part_query = request(std::min(7U, count - begin));
        part_query.threadgroup_width = 64;
        part_query.sample_begin = begin;
        const auto part = sample(renderer, part_query);
        for (std::size_t i = 0; i < part.size(); ++i) {
            const auto &a = full[begin + i];
            const auto &b = part[i];
            require(a.random_words == b.random_words && a.wavelength == b.wavelength &&
                        a.radiance == b.radiance && a.scatterings == b.scatterings,
                    "Patch sample changed with batching or threadgroup width");
        }
    }
}

} // namespace

int main(int argc, char **argv) try {
    require(argc >= 2, "Expected --reference or --metal");
    const std::string_view mode{argv[1]};
    if (mode == "--reference") {
        require(argc == 2, "Unexpected reference test arguments");
        validation_contract();
        furnace_reference();
        for (const Fixture fixture : {Fixture::directional, Fixture::smooth, Fixture::saddle}) {
            const auto moments = reference(fixture);
            std::cout << std::setprecision(10) << "Patch reference mean=" << moments.mean
                      << " second=" << moments.second << " bsdf_mean=" << moments.bsdf_mean << '\n';
        }
        return 0;
    }
    require(mode == "--metal", "Unexpected patch renderer test mode");
    bool allow_no_device = false, allow_no_ray_tracing = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (option == "--allow-no-device" && !allow_no_device) {
            allow_no_device = true;
        } else if (option == "--allow-no-ray-tracing" && !allow_no_ray_tracing) {
            allow_no_ray_tracing = true;
        } else {
            throw std::runtime_error("Unexpected patch renderer Metal test option");
        }
    }
    auto native = build_metal_renderer(directional_scene(false));
    if ((native.status == GeometryStatus::no_device && allow_no_device) ||
        (native.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
        require(!native.scene && !native.diagnostic.empty(), "Malformed Metal availability result");
        std::cout << "SKIP Metal patch rendering. " << native.diagnostic << '\n';
        return 77;
    }
    require(native.status == GeometryStatus::success && native.scene, native.diagnostic);
    std::cout << "Bilinear patch rendering on " << native.device.name << '\n';
    check_mean("Curved emitter", *native.scene, reference(Fixture::directional));
    const auto smooth = build(directional_scene(true));
    check_mean("Unequal patch normals", *smooth, reference(Fixture::smooth));
    check_mean("Self-lighting saddle", *build(saddle_scene()), reference(Fixture::saddle), 1);
    blocker();
    translated_lighting();
    affine_furnace();
    recentered_furnace();
    coincident_patch_sample();
    furnace();
    sidedness();
    batching(*smooth);
    return 0;
} catch (const std::exception &error) {
    std::cerr << "Bilinear patch rendering test failed. " << error.what() << '\n';
    return 1;
}
