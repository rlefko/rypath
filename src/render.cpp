// SPDX-License-Identifier: GPL-3.0-only

#include "render.hpp"
#include "triangle_attributes.hpp"

#include "cie_xyz_data.hpp"
#include "rgb_spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace rypath {
namespace {

constexpr double wavelength_min = 360;
constexpr double wavelength_max = 830;
constexpr double wavelength_span = wavelength_max - wavelength_min;

constexpr double integrate_y() {
    const auto &table = detail::cie_xyz;
    double integral = (table.front()[1] + table.back()[1]) / 2;
    for (std::size_t i = 1; i + 1 < table.size(); ++i) {
        integral += table[i][1];
    }
    return integral;
}

bool finite(GeometryPoint point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

using Vector = std::array<double, 3>;

Vector difference(const Vector &a, const Vector &b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vector apply_transform(const GeometryTransform &transform, const Vector &p, bool point) {
    Vector result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto *row = transform.values.data() + i * 4;
        const double bound = std::abs(row[0] * p[0]) + std::abs(row[1] * p[1]) +
                             std::abs(row[2] * p[2]) + (point ? std::abs(row[3]) : 0);
        // Metal dot products may round intermediate terms before cancellation.
        // Require a representable absolute sum regardless of contraction order.
        result[i] = bound <= std::numeric_limits<float>::max()
                        ? row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + (point ? row[3] : 0)
                        : std::numeric_limits<double>::infinity();
    }
    return result;
}

bool binary32_range(const Vector &v) {
    return std::ranges::all_of(v, [](double component) {
        return std::abs(component) <= std::numeric_limits<float>::max();
    });
}

Vector binary32_round(Vector v) {
    for (auto &component : v) {
        component = static_cast<float>(component);
    }
    return v;
}

Vector rounded_cross(const Vector &a, const Vector &b) {
    Vector result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto j = (i + 1) % result.size();
        const auto k = (i + 2) % result.size();
        const double forward = a[j] * b[k];
        const double reverse = a[k] * b[j];
        constexpr double largest = std::numeric_limits<float>::max();
        if (std::abs(forward) > largest || std::abs(reverse) > largest) {
            return {std::numeric_limits<double>::infinity(), 0, 0};
        }
        result[i] = double{static_cast<float>(forward)} - static_cast<float>(reverse);
    }
    return result;
}

double squared_length(const Vector &v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; }

Vector to_vector(GeometryPoint point) { return {point.x, point.y, point.z}; }

bool valid_projection(RenderModel model, double exposure) {
    return (model == RenderModel::demo && exposure == 1) ||
           (model == RenderModel::pbrt && std::isfinite(exposure) && exposure >= 0);
}

bool valid_raster_camera(const RasterCamera &camera) {
    const auto &[origin, corner, dx, dy] = camera;
    if (!finite(origin) || !finite(corner) || !finite(dx) || !finite(dy)) {
        return false;
    }
    const double nx = double{dx.y} * dy.z - double{dx.z} * dy.y;
    const double ny = double{dx.z} * dy.x - double{dx.x} * dy.z;
    const double nz = double{dx.x} * dy.y - double{dx.y} * dy.x;
    // A nonzero scalar triple product keeps the raster's ray plane away
    // from the zero vector and requires two independent raster directions.
    return nx * corner.x + ny * corner.y + nz * corner.z != 0;
}

bool valid_pixel_filter(const PixelFilter &filter) {
    switch (filter.kind) {
    case PixelFilterKind::box:
        return filter.x_radius == 0.5f && filter.y_radius == 0.5f;
    case PixelFilterKind::triangle:
        return std::isnormal(filter.x_radius) && filter.x_radius > 0 &&
               std::isnormal(filter.y_radius) && filter.y_radius > 0;
    }
    return false;
}

constexpr double float_max = std::numeric_limits<float>::max();
constexpr double float_min = std::numeric_limits<float>::min();
// Metal allows round-toward-zero for float operations. Use a full relative epsilon.
constexpr double float_roundoff = std::numeric_limits<float>::epsilon();
constexpr double double_slack = 8. * std::numeric_limits<double>::epsilon();

// Triangle rays evaluate ((float(pixel) + 0.5f) + offset) / float(extent).
// Enclose both outer pixels using the full closed tent support and allow for
// the Metal precise operations, including possible subnormal flushing.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<std::array<double, 2>> triangle_raster_interval(std::uint32_t extent, float radius) {
    const double offset_bound = radius * (1. + 4. * float_roundoff) + float_min;
    const double last_center = static_cast<float>(extent - 1) + 0.5f;
    if (last_center + offset_bound > float_max) {
        return std::nullopt;
    }
    const double divisor = static_cast<float>(extent);
    const auto edge = [&](double numerator, double outward) {
        const double value = numerator / divisor;
        const double error = 8. * float_roundoff * std::abs(value) + 2. * float_min;
        return value + outward * error;
    };
    return std::array{edge(0.5 - offset_bound, -1.), edge(last_center + offset_bound, 1.)};
}

double endpoint_bound(const std::array<double, 2> &interval, double slope, double intercept) {
    return std::max(std::abs(std::fma(interval[0], slope, intercept)),
                    std::abs(std::fma(interval[1], slope, intercept)));
}

double dot(const Vector &a, const Vector &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

double dot_error(const Vector &a, const Vector &b) {
    return double_slack * (std::abs(a[0] * b[0]) + std::abs(a[1] * b[1]) + std::abs(a[2] * b[2]));
}

double subnormal_magnitude(double value) {
    return std::abs(value) < float_min ? std::abs(value) : 0.;
}

bool triangle_ray_range(const RasterCamera &camera, const std::array<double, 2> &x,
                        const std::array<double, 2> &y) {
    const auto corner = to_vector(camera.corner);
    const auto dx = to_vector(camera.dx);
    const auto dy = to_vector(camera.dy);
    const double x_abs = std::max(std::abs(x[0]), std::abs(x[1]));
    const double y_abs = std::max(std::abs(y[0]), std::abs(y[1]));
    Vector direction_error{};
    for (std::size_t axis = 0; axis < corner.size(); ++axis) {
        // Match d = fma(y, dy, fma(x, dx, corner)). Bound the actual affine
        // extrema, and carry the first FMA's rounding into the second.
        const double inner = endpoint_bound(x, dx[axis], corner[axis]);
        const double inner_error =
            float_roundoff * inner + float_min + subnormal_magnitude(corner[axis]) +
            x_abs * subnormal_magnitude(dx[axis]) +
            double_slack * (std::abs(corner[axis]) + x_abs * std::abs(dx[axis]));
        if (inner + inner_error > float_max) {
            return false;
        }
        double outer = 0;
        for (const double column : x) {
            outer = std::max(outer,
                             endpoint_bound(y, dy[axis], std::fma(column, dx[axis], corner[axis])));
        }
        const double outer_error = inner_error + y_abs * subnormal_magnitude(dy[axis]) +
                                   float_roundoff * (outer + inner_error) + float_min +
                                   double_slack * (inner + y_abs * std::abs(dy[axis]));
        if (outer + outer_error > float_max) {
            return false;
        }
        direction_error[axis] = outer_error;
    }

    // The computed cross is just a candidate separating vector. Bound its
    // residual dots with dx and dy instead of assuming exact orthogonality.
    const Vector normal = cross(dx, dy);
    const Vector abs_normal{std::abs(normal[0]), std::abs(normal[1]), std::abs(normal[2])};
    const double separation = std::abs(dot(normal, corner)) - dot_error(normal, corner);
    const double uncertainty = x_abs * (std::abs(dot(normal, dx)) + dot_error(normal, dx)) +
                               y_abs * (std::abs(dot(normal, dy)) + dot_error(normal, dy)) +
                               dot(abs_normal, direction_error);
    // If every direction component were subnormal, their dot with the
    // separating vector could not exceed this limit.
    const double minimum_scale = (abs_normal[0] + abs_normal[1] + abs_normal[2]) * float_min;
    return separation > (uncertainty + minimum_scale) * (1. + double_slack);
}

bool valid_sampled_material(const SpectralMaterial &material) {
    float previous = -std::numeric_limits<float>::infinity();
    for (const auto [wavelength, value] : material.knots) {
        if (!(wavelength > previous && std::isfinite(wavelength) && value >= 0 && value <= 1)) {
            return false;
        }
        previous = wavelength;
    }
    return material.knots.size() >= 2;
}

bool valid_rgb_coefficients(const std::array<float, 3> &coefficients) {
    if (!std::ranges::all_of(coefficients, [](float c) { return std::isfinite(c); })) {
        return false;
    }
    const double a = coefficients[0];
    const double b = coefficients[1];
    const double c = coefficients[2];
    constexpr double largest = std::numeric_limits<float>::max();
    const double inner_max = std::max(std::abs(std::fma(a, wavelength_min, b)),
                                      std::abs(std::fma(a, wavelength_max, b)));
    if (inner_max > largest) {
        return false;
    }
    const auto polynomial = [&](double wavelength) {
        return std::fma(std::fma(a, wavelength, b), wavelength, c);
    };
    double peak =
        std::max(std::abs(polynomial(wavelength_min)), std::abs(polynomial(wavelength_max)));
    if (a != 0) {
        const double stationary = -b / (2 * a);
        if (stationary > wavelength_min && stationary < wavelength_max) {
            peak = std::max(peak, std::abs(polynomial(stationary)));
        }
    }
    // Allow for binary32 rounding in the inner FMA before multiplication by
    // the wavelength in the outer FMA.
    const double rounding = std::numeric_limits<float>::epsilon() * inner_max * wavelength_max;
    return peak + rounding <= largest;
}

std::string validate_pbrt_material(const SpectralMaterial &material) {
    constexpr std::array<float, 3> zero_coefficients{};
    switch (material.kind) {
    case SpectralMaterialKind::sampled:
        if (material.constant != 0 || material.coefficients != zero_coefficients) {
            return "PBRT sampled reflectance contains a constant or RGB payload";
        }
        if (!valid_sampled_material(material)) {
            return "PBRT reflectance requires at least two finite, strictly increasing "
                   "wavelengths and values within [0, 1]";
        }
        return {};
    case SpectralMaterialKind::constant:
        if (!material.knots.empty() || material.coefficients != zero_coefficients ||
            !(material.constant >= 0 && material.constant <= 1)) {
            return "PBRT constant reflectance requires a single finite value within [0, 1]";
        }
        return {};
    case SpectralMaterialKind::rgb_sigmoid:
        if (!material.knots.empty() || material.constant != 0 ||
            !valid_rgb_coefficients(material.coefficients)) {
            return "PBRT RGB reflectance requires three finite, evaluable coefficients";
        }
        return {};
    }
    return "Invalid PBRT spectral material kind";
}

bool valid_film_size(std::uint32_t width, std::uint32_t height) {
    const auto pixels = std::uint64_t{width} * height;
    return pixels > 0 && pixels <= std::numeric_limits<std::uint32_t>::max() &&
           pixels <= std::vector<FilmPixel>{}.max_size();
}

GeometryMesh quad(std::array<GeometryPoint, 4> corners) {
    return {{corners.begin(), corners.end()}, {{0, 1, 2}, {0, 2, 3}}};
}

struct DemoBox {
    GeometryPoint offset;
    GeometryPoint scale;
    float angle;
};

GeometryTransform box_transform(const DemoBox &box) {
    const auto [x, y, z] = box.offset;
    const auto [width, height, depth] = box.scale;
    const float c = std::cos(box.angle);
    const float s = std::sin(box.angle);
    return {{c * width, 0, s * depth, x, 0, height, 0, y, -s * width, 0, c * depth, z}};
}

void accumulate(double value, double &sum, double &correction) {
    const double updated = sum + value;
    // Neumaier's correction also handles a newly added term larger than the sum.
    correction +=
        std::abs(sum) >= std::abs(value) ? (sum - updated) + value : (value - updated) + sum;
    sum = updated;
}

} // namespace

std::string validate_render_settings(const RenderSettings &settings) {
    if (!valid_film_size(settings.width, settings.height)) {
        return "Render dimensions must be positive and fit the film and 32-bit pixel indices";
    }
    if (settings.samples == 0 || settings.batch_size == 0) {
        return "Render sample count and batch size must be positive";
    }
    const auto pixels = std::uint64_t{settings.width} * settings.height;
    if (settings.samples > std::numeric_limits<std::uint64_t>::max() / pixels) {
        return "Total render sample count exceeds 64-bit capacity";
    }
    return validate_sampler_range(settings.sampler, settings.seed, settings.width, settings.height,
                                  0, settings.samples);
}

std::string validate_raster_support(const RasterCamera &camera, const PixelFilter &filter,
                                    std::uint32_t width, std::uint32_t height) {
    if (!valid_pixel_filter(filter)) {
        return "Invalid pixel filter or radius";
    }
    if (filter.kind == PixelFilterKind::box) {
        return {};
    }
    if (!valid_raster_camera(camera) || width == 0 || height == 0) {
        return "Triangle filter requires finite nondegenerate raster coefficients and positive "
               "dimensions";
    }
    const auto x = triangle_raster_interval(width, filter.x_radius);
    const auto y = triangle_raster_interval(height, filter.y_radius);
    if (!x || !y || !triangle_ray_range(camera, *x, *y)) {
        return "Triangle filter support exceeds the safe binary32 raster direction range";
    }
    return {};
}

std::optional<GeometryPoint> area_world_vertex(GeometryPoint position,
                                               const GeometryTransform &transform) {
    const auto point = apply_transform(transform, to_vector(position), true);
    if (!binary32_range(point)) {
        return std::nullopt;
    }
    return GeometryPoint{static_cast<float>(point[0]), static_cast<float>(point[1]),
                         static_cast<float>(point[2])};
}

std::string validate_area_geometry(const GeometryMesh &mesh, const GeometryTransform &transform) {
    constexpr double normal_min = std::numeric_limits<float>::min();
    constexpr double finite_max = std::numeric_limits<float>::max();
    const bool transformed = transform.values != GeometryTransform{}.values;
    if (transformed) {
        for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
            if (!area_world_vertex(mesh.positions[i], transform)) {
                return "Area light vertex " + std::to_string(i) +
                       " is outside the binary32 world-space rendering range";
            }
        }
    }
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        const auto [a, b, c] = mesh.triangles[i];
        if (std::max({a, b, c}) >= mesh.positions.size()) {
            return "Area light triangle " + std::to_string(i) + " has an out-of-range vertex index";
        }
        const auto p0 = to_vector(mesh.positions[a]);
        const auto p1 = to_vector(mesh.positions[b]);
        const auto p2 = to_vector(mesh.positions[c]);
        const auto e1 = difference(p1, p0);
        const auto e2 = difference(p2, p0);
        const auto w1 = apply_transform(transform, e1, false);
        const auto w2 = apply_transform(transform, e2, false);
        const double squared = squared_length(cross(w1, w2));
        if (!(squared >= normal_min && squared <= finite_max) || !binary32_range(e1) ||
            !binary32_range(e2) || !binary32_range(difference(e2, e1)) || !binary32_range(w1) ||
            !binary32_range(w2)) {
            return "Area light triangle " + std::to_string(i) +
                   " has world area or edges outside the binary32 rendering range";
        }
        // Metal builds emissive geometry from world-space binary32 vertices.
        // Validate the edges and area the shader reconstructs from those values.
        const auto q0 = area_world_vertex(mesh.positions[a], transform);
        const auto q1 = area_world_vertex(mesh.positions[b], transform);
        const auto q2 = area_world_vertex(mesh.positions[c], transform);
        if (!q0 || !q1 || !q2) {
            return "Area light triangle " + std::to_string(i) +
                   " has world vertices outside the binary32 rendering range";
        }
        const auto baked_e1 = difference(to_vector(*q1), to_vector(*q0));
        const auto baked_e2 = difference(to_vector(*q2), to_vector(*q0));
        if (!binary32_range(baked_e1) || !binary32_range(baked_e2)) {
            return "Area light triangle " + std::to_string(i) +
                   " has baked world edges outside the binary32 rendering range";
        }
        const auto rounded_e1 = binary32_round(baked_e1);
        const auto rounded_e2 = binary32_round(baked_e2);
        const double rounded_squared = squared_length(rounded_cross(rounded_e1, rounded_e2));
        if (!binary32_range(difference(rounded_e2, rounded_e1)) ||
            !(rounded_squared >= normal_min && rounded_squared <= finite_max)) {
            return "Area light triangle " + std::to_string(i) +
                   " has rounded world area or edges outside the binary32 rendering range";
        }
    }
    return {};
}

std::string validate_render_scene(const RenderScene &scene) {
    if (!valid_pixel_filter(scene.filter)) {
        return "Invalid pixel filter or radius";
    }
    if (scene.model == RenderModel::demo && scene.filter.kind != PixelFilterKind::box) {
        return "Triangle filtering requires the PBRT render model";
    }
    if (auto error = validate_geometry(scene.geometry); !error.empty()) {
        return error;
    }
    if (scene.instance_materials.size() != scene.geometry.instances.size()) {
        return "Each render instance requires a material index";
    }
    for (const auto material : scene.instance_materials) {
        if (material >= scene.materials.size()) {
            return "Render instance material index is out of range";
        }
    }
    if (!scene.instance_reverse_orientation.empty() &&
        (scene.instance_reverse_orientation.size() != scene.geometry.instances.size() ||
         !std::ranges::all_of(scene.instance_reverse_orientation,
                              [](std::uint32_t flag) { return flag <= 1; }))) {
        return "Render orientation flags must supply one Boolean per instance";
    }
    if (!valid_projection(scene.model, scene.exposure)) {
        return "Invalid render model or exposure";
    }
    if (!scene.instance_emissions.empty() &&
        scene.instance_emissions.size() != scene.geometry.instances.size()) {
        return "Render emission bindings must supply one index per instance";
    }
    if (scene.model == RenderModel::demo && !scene.area_emissions.empty()) {
        return "Area emission requires the PBRT render model";
    }
    for (const auto &emission : scene.area_emissions) {
        if (!std::ranges::all_of(emission.radiance,
                                 [](float value) { return std::isfinite(value) && value >= 0; })) {
            return "PBRT area radiance must be finite and nonnegative";
        }
    }
    std::uint64_t emitter_count = 0;
    for (std::size_t i = 0; i < scene.instance_emissions.size(); ++i) {
        const auto emission = scene.instance_emissions[i];
        if (emission == no_area_emission) {
            continue;
        }
        if (emission >= scene.area_emissions.size()) {
            return "Render instance emission index is out of range";
        }
        const auto &instance = scene.geometry.instances[i];
        const auto &mesh = scene.geometry.meshes[instance.mesh_index];
        emitter_count += mesh.triangles.size();
        // Keep bit_ceil(emitter_count) representable as a 32-bit slot count.
        if (emitter_count > (std::uint64_t{1} << 31)) {
            return "Render area emitter count exceeds selection index capacity";
        }
        if (auto error = validate_area_geometry(mesh, instance.transform); !error.empty()) {
            return "Instance " + std::to_string(i) + ": " + error;
        }
    }
    std::vector<GeometryPoint> normals;
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        const auto &instance = scene.geometry.instances[i];
        const auto &mesh = scene.geometry.meshes[instance.mesh_index];
        if (mesh.normals.empty()) {
            continue;
        }
        const bool reverse = !scene.instance_reverse_orientation.empty() &&
                             scene.instance_reverse_orientation[i] != 0;
        if (auto error = prepare_shading_normals(mesh, instance.transform, reverse, normals);
            !error.empty()) {
            return "Instance " + std::to_string(i) + ": " + error;
        }
        if (!scene.instance_emissions.empty() && scene.instance_emissions[i] != no_area_emission) {
            const auto &emission = scene.area_emissions[scene.instance_emissions[i]];
            if (auto error = validate_normal_emission(mesh, instance.transform, reverse,
                                                      emission.two_sided, normals);
                !error.empty()) {
                return "Instance " + std::to_string(i) + ": " + error;
            }
        }
    }
    if (scene.model == RenderModel::pbrt) {
        if (!scene.raster_camera || !valid_raster_camera(*scene.raster_camera)) {
            return "PBRT camera requires finite, nondegenerate raster ray coefficients";
        }
        if (scene.max_depth > render_emergency_limit) {
            return "PBRT max depth exceeds the renderer limit";
        }
        if (!std::ranges::all_of(scene.environment,
                                 [](float value) { return std::isfinite(value) && value >= 0; })) {
            return "PBRT environment radiance must be finite and nonnegative";
        }
        for (const auto &material : scene.materials) {
            if (auto error = validate_pbrt_material(material); !error.empty()) {
                return error;
            }
        }
        return {};
    }
    for (const auto &material : scene.materials) {
        if (material.kind != SpectralMaterialKind::sampled || material.constant != 0 ||
            material.coefficients != std::array<float, 3>{}) {
            return "Demo spectral material requires a sampled reflectance";
        }
        if (!std::ranges::all_of(material.reflectance,
                                 [](float value) { return value >= 0 && value <= 0.75f; })) {
            return "Spectral reflectance must be finite and within [0, 0.75]";
        }
    }
    const auto &[origin, target, up, fov] = scene.camera;
    if (!finite(origin) || !finite(target) || !finite(up) || !(fov > 0 && fov < 180)) {
        return "Camera needs finite vectors and a vertical field of view within (0, 180)";
    }
    const double x = static_cast<double>(target.x) - origin.x;
    const double y = static_cast<double>(target.y) - origin.y;
    const double z = static_cast<double>(target.z) - origin.z;
    if (y * up.z == z * up.y && z * up.x == x * up.z && x * up.y == y * up.x) {
        return "Camera viewing and up directions must be nonzero and nonparallel";
    }
    return {};
}

RenderScene make_demo_scene() {
    RenderScene scene;
    scene.camera = {{4.1f, 3.0f, 6.2f}, {0, 1, -0.45f}, {0, 1, 0}, 42};
    scene.geometry.meshes = {
        quad({{{-3, 0, -3}, {-3, 0, 3}, {3, 0, 3}, {3, 0, -3}}}),
        quad({{{-3, 0, -3}, {3, 0, -3}, {3, 3.2f, -3}, {-3, 3.2f, -3}}}),
        quad({{{-3, 0, 3}, {-3, 0, -3}, {-3, 3.2f, -3}, {-3, 3.2f, 3}}}),
        {{{-0.5f, 0, -0.5f},
          {0.5f, 0, -0.5f},
          {0.5f, 0, 0.5f},
          {-0.5f, 0, 0.5f},
          {-0.5f, 1, -0.5f},
          {0.5f, 1, -0.5f},
          {0.5f, 1, 0.5f},
          {-0.5f, 1, 0.5f}},
         {{0, 1, 2},
          {0, 2, 3},
          {4, 6, 5},
          {4, 7, 6},
          {0, 4, 5},
          {0, 5, 1},
          {1, 5, 6},
          {1, 6, 2},
          {2, 6, 7},
          {2, 7, 3},
          {3, 7, 4},
          {3, 4, 0}}},
    };
    scene.geometry.instances = {
        {0, {}},
        {1, {}},
        {2, {}},
        {3, box_transform({{-1.25f, -0.05f, -0.6f}, {1.25f, 2.25f, 1.25f}, 0.18f})},
        {3, box_transform({{0.25f, -0.05f, 0.8f}, {1.45f, 1.08f, 1.3f}, -0.24f})},
        {3, box_transform({{1.4f, -0.05f, -1.5f}, {1.07f, 1.55f, 1.06f}, 0.44f})},
    };
    scene.materials = {
        {{0.54f, 0.54f, 0.55f, 0.58f, 0.61f, 0.63f, 0.65f, 0.65f}},
        {{0.69f, 0.68f, 0.67f, 0.65f, 0.64f, 0.62f, 0.61f, 0.61f}},
        {{0.34f, 0.34f, 0.36f, 0.43f, 0.55f, 0.61f, 0.63f, 0.63f}},
        {{0.10f, 0.10f, 0.16f, 0.52f, 0.40f, 0.09f, 0.06f, 0.06f}},
        {{0.07f, 0.08f, 0.09f, 0.13f, 0.39f, 0.73f, 0.72f, 0.72f}},
        {{0.32f, 0.47f, 0.72f, 0.23f, 0.11f, 0.08f, 0.09f, 0.09f}},
    };
    scene.instance_materials = {0, 1, 2, 3, 4, 5};
    return scene;
}

Film make_film(std::uint32_t width, std::uint32_t height, RenderModel model, double exposure) {
    if (!valid_film_size(width, height) || !valid_projection(model, exposure)) {
        return {};
    }
    return {width, height, std::vector<FilmPixel>(static_cast<std::size_t>(width) * height), model,
            exposure};
}

std::array<double, 3> film_mean(const FilmPixel &pixel) {
    std::array<double, 3> result{};
    if (pixel.count > 0) {
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = (pixel.sum[i] + pixel.correction[i]) / static_cast<double>(pixel.count);
        }
    }
    return result;
}

double cie_y_integral() {
    static constexpr double integral = integrate_y();
    return integral;
}

std::array<double, 3> wavelength_xyz(double wavelength) {
    if (!(wavelength >= wavelength_min && wavelength <= wavelength_max)) {
        return {};
    }
    const double offset = wavelength - wavelength_min;
    const auto lower = static_cast<std::size_t>(offset);
    const auto upper = std::min(lower + 1, detail::cie_xyz.size() - 1);
    std::array<double, 3> xyz{};
    for (std::size_t i = 0; i < xyz.size(); ++i) {
        xyz[i] = std::lerp(detail::cie_xyz[lower][i], detail::cie_xyz[upper][i],
                           offset - static_cast<double>(lower)) *
                 (wavelength_span / cie_y_integral());
    }
    return xyz;
}

std::array<double, 3> pbrt_cie_xyz(double wavelength) {
    if (!(wavelength >= wavelength_min && wavelength <= wavelength_max)) {
        return {};
    }
    return detail::cie_xyz[static_cast<std::size_t>(std::round(wavelength) - wavelength_min)];
}

double pbrt_y_inner_product(const std::array<float, 471> &spectrum) {
    static_assert(detail::cie_xyz.size() ==
                  std::tuple_size_v<std::remove_cvref_t<decltype(spectrum)>>);
    double integral = 0;
    for (std::size_t i = 0; i < spectrum.size(); ++i) {
        integral += spectrum[i] * detail::cie_xyz[i][1];
    }
    return integral;
}

double spectral_reflectance(const SpectralMaterial &material, double wavelength) {
    switch (material.kind) {
    case SpectralMaterialKind::constant:
    case SpectralMaterialKind::rgb_sigmoid:
        if (!(wavelength >= wavelength_min && wavelength <= wavelength_max)) {
            return 0;
        }
        return evaluate_srgb({.is_constant = material.kind == SpectralMaterialKind::constant,
                              .constant = material.constant,
                              .coefficients = material.coefficients},
                             static_cast<float>(wavelength));
    case SpectralMaterialKind::sampled:
        break;
    default:
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (!material.knots.empty()) {
        if (!(wavelength >= material.knots.front().wavelength &&
              wavelength <= material.knots.back().wavelength)) {
            return 0;
        }
        const auto next =
            std::ranges::lower_bound(material.knots, wavelength, {}, &SpectralKnot::wavelength);
        if (next == material.knots.begin()) {
            return next->value;
        }
        const auto &previous = *(next - 1);
        const double fraction =
            (wavelength - previous.wavelength) / (double{next->wavelength} - previous.wavelength);
        return std::lerp(double{previous.value}, double{next->value}, fraction);
    }
    if (!(wavelength >= spectral_knots.front() && wavelength <= spectral_knots.back())) {
        return 0;
    }
    const auto next =
        std::lower_bound(spectral_knots.begin() + 1, spectral_knots.end(), wavelength);
    const auto upper = static_cast<std::size_t>(next - spectral_knots.begin());
    const auto lower = upper - 1;
    const double fraction =
        (wavelength - spectral_knots[lower]) / (spectral_knots[upper] - spectral_knots[lower]);
    return std::lerp(static_cast<double>(material.reflectance[lower]),
                     static_cast<double>(material.reflectance[upper]), fraction);
}

// Parameter order follows the sampler key and counter layout.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::array<std::uint32_t, 4> sample_words(std::uint64_t seed, std::uint32_t pixel,
                                          std::uint64_t sample, std::uint32_t block) {
    // Philox4x32-10, from Random123 by D. E. Shaw Research.
    // See third_party/random123 for the source and license.
    std::array<std::uint32_t, 4> counter{pixel, static_cast<std::uint32_t>(sample),
                                         static_cast<std::uint32_t>(sample >> 32), block};
    auto key0 = static_cast<std::uint32_t>(seed);
    auto key1 = static_cast<std::uint32_t>(seed >> 32);
    for (int round = 0; round < 10; ++round) {
        const auto product0 = std::uint64_t{0xd2511f53} * counter[0];
        const auto product1 = std::uint64_t{0xcd9e8d57} * counter[2];
        counter = {static_cast<std::uint32_t>(product1 >> 32) ^ counter[1] ^ key0,
                   static_cast<std::uint32_t>(product1),
                   static_cast<std::uint32_t>(product0 >> 32) ^ counter[3] ^ key1,
                   static_cast<std::uint32_t>(product0)};
        key0 += 0x9e3779b9;
        key1 += 0xbb67ae85;
    }
    return counter;
}

float sample_uniform(std::uint32_t word) { return static_cast<float>(word >> 8) * 0x1p-24f; }

// Use the same unit variate, radius order as the shader and PBRT SampleTent.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
float triangle_filter_offset(float u, float radius) {
    const float s = 2.f * u - 1.f;
    return radius * (s / (1.f + std::sqrt(1.f - std::abs(s))));
}

std::array<double, 3> linear_rgb(const std::array<double, 3> &xyz) {
    const auto [x, y, z] = xyz;
    return {
        3.2404542 * x - 1.5371385 * y - 0.4985314 * z,
        -0.9692660 * x + 1.8760108 * y + 0.0415560 * z,
        0.0556434 * x - 0.2040259 * y + 1.0572252 * z,
    };
}

std::array<double, 3> linear_rgb(const Film &film, const FilmPixel &pixel) {
    const auto xyz = film_mean(pixel);
    if (film.model == RenderModel::demo) {
        return linear_rgb(xyz);
    }
    const auto [x, y, z] = xyz;
    // Derived from the pinned PBRT v4 stdillum-D65 spectrum, CIE observer,
    // and sRGB primaries. The matrix maps unit relative white luminance to 1.
    return {
        film.exposure * (3.240444291419 * x - 1.537133830545 * y - 0.498529890988 * z),
        film.exposure * (-0.969266752560 * x + 1.876012242980 * y + 0.041556048487 * z),
        film.exposure * (0.055643519672 * x - 0.204026238798 * y + 1.057226873771 * z),
    };
}

namespace {

std::array<std::uint8_t, 3> encode_srgb(const std::array<double, 3> &linear) {
    std::array<std::uint8_t, 3> rgb{};
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        const double channel = std::clamp(linear[i], 0.0, 1.0);
        const double encoded =
            channel <= 0.0031308 ? 12.92 * channel : 1.055 * std::pow(channel, 1 / 2.4) - 0.055;
        if (std::isfinite(encoded)) {
            rgb[i] = static_cast<std::uint8_t>(std::round(255 * encoded));
        }
    }
    return rgb;
}

} // namespace

std::array<std::uint8_t, 3> display_rgb(const std::array<double, 3> &xyz) {
    return encode_srgb(linear_rgb(xyz));
}

std::array<std::uint8_t, 3> display_rgb(const Film &film, const FilmPixel &pixel) {
    return encode_srgb(linear_rgb(film, pixel));
}

FilmBatch::FilmBatch(const Film &film, std::uint64_t sample_count) {
    if (!valid_film_size(film.width, film.height) ||
        film.pixels.size() != static_cast<std::size_t>(film.width) * film.height) {
        error_ = "Invalid film dimensions or storage";
        return;
    }
    if (!valid_projection(film.model, film.exposure)) {
        error_ = "Invalid film model or exposure";
        return;
    }
    begin_ = film.pixels.front().count;
    if (sample_count == 0 || sample_count > std::numeric_limits<std::uint64_t>::max() - begin_) {
        error_ = "Film batch count must be positive and fit the sample indices";
        return;
    }
    if (!std::ranges::all_of(film.pixels, [this](const FilmPixel &pixel) {
            return pixel.count == begin_ &&
                   std::ranges::all_of(pixel.sum,
                                       [](double value) { return std::isfinite(value); }) &&
                   std::ranges::all_of(pixel.correction,
                                       [](double value) { return std::isfinite(value); });
        })) {
        error_ = "Film has inconsistent sample counts or nonfinite accumulation";
        return;
    }
    end_ = begin_ + sample_count;
    staged_ = film;
}

std::string FilmBatch::add(std::span<const RenderSample> samples) {
    if (!error_.empty()) {
        return error_;
    }
    for (const auto &sample : samples) {
        if (sample.pixel >= staged_.pixels.size() ||
            !(sample.wavelength >= wavelength_min && sample.wavelength <= wavelength_max) ||
            !std::isfinite(sample.radiance) || sample.radiance < 0) {
            error_ = "Film batch contains an invalid pixel, wavelength, or radiance";
            return error_;
        }
        auto &pixel = staged_.pixels[sample.pixel];
        if (sample.sample != pixel.count || sample.sample >= end_) {
            error_ = "Film batch sample index is duplicated, missing, or outside the batch";
            return error_;
        }
        const bool pbrt = staged_.model == RenderModel::pbrt;
        const auto xyz = pbrt ? pbrt_cie_xyz(sample.wavelength) : wavelength_xyz(sample.wavelength);
        const double weighted_radiance = pbrt ? wavelength_span * sample.radiance : sample.radiance;
        for (std::size_t i = 0; i < xyz.size(); ++i) {
            accumulate(xyz[i] * weighted_radiance, pixel.sum[i], pixel.correction[i]);
            if (!std::isfinite(pixel.sum[i] + pixel.correction[i])) {
                error_ = "Film batch accumulation overflow";
                return error_;
            }
        }
        ++pixel.count;
    }
    return {};
}

std::string FilmBatch::commit(Film &film) {
    if (!error_.empty()) {
        return error_;
    }
    if (!std::ranges::all_of(staged_.pixels,
                             [this](const FilmPixel &pixel) { return pixel.count == end_; })) {
        error_ = "Film batch is incomplete";
        return error_;
    }
    if (film.width != staged_.width || film.height != staged_.height ||
        film.model != staged_.model || film.exposure != staged_.exposure ||
        film.pixels.size() != staged_.pixels.size() ||
        !std::ranges::all_of(film.pixels,
                             [this](const FilmPixel &pixel) { return pixel.count == begin_; })) {
        error_ = "Film changed before the batch could commit";
        return error_;
    }
    static_assert(std::is_nothrow_move_assignable_v<Film>);
    error_ = "Film batch has already committed";
    film = std::move(staged_);
    return {};
}

} // namespace rypath
