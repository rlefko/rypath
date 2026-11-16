// SPDX-License-Identifier: GPL-3.0-only

#include "render.hpp"
#include "bilinear.hpp"
#include "bsdf_table.hpp"
#include "triangle_attributes.hpp"

#include "cie_xyz_data.hpp"
#include "rgb_spectrum.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <type_traits>
#include <utility>

namespace rypath {
namespace {

constexpr double wavelength_min = 360;
constexpr double wavelength_max = 830;
constexpr double wavelength_span = wavelength_max - wavelength_min;
// Pinned PBRT v4 Planck and Wien constants in nm K.
constexpr double planck_exponent_nm_kelvin = 6.62606957e-34 * 299792458. / 1.3806488e-23 * 1e9;
constexpr double wien_nm_kelvin = 2.8977721e6;
constexpr double wien_exponent = planck_exponent_nm_kelvin / wien_nm_kelvin;

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

std::string validate_pbrt_material(const SpectralMaterial &material);

std::string validate_surface_material(const SpectralMaterial &m) {
    const auto &a = m.surface;
    if (m.scattering == ScatteringKind::measured || m.scattering == ScatteringKind::fourier) {
        if (!m.table ||
            m.table->kind != (m.scattering == ScatteringKind::measured ? BsdfTableKind::measured
                                                                       : BsdfTableKind::fourier))
            return "Tabulated material requires a checked table of the matching format";
        return {};
    }
    if (m.scattering == ScatteringKind::mix) {
        if (!std::isfinite(a.mix_amount) || m.bump_root != UINT32_MAX || m.normal_map != UINT32_MAX)
            return "Mix material requires a finite amount and child-owned surface detail";
        return {};
    }
    if (m.scattering == ScatteringKind::thin_dielectric) {
        auto thin = m;
        thin.scattering = ScatteringKind::dielectric;
        if (thin.u_roughness != 0 || thin.v_roughness != 0 || thin.u_roughness_root != UINT32_MAX ||
            thin.v_roughness_root != UINT32_MAX)
            return "Thin dielectric has no roughness parameters";
        return validate_dielectric_material(thin);
    }
    const bool layered = m.scattering == ScatteringKind::coated_diffuse ||
                         m.scattering == ScatteringKind::coated_conductor;
    if (layered) {
        if (!std::isfinite(a.thickness) || !std::isfinite(a.albedo) || !std::isfinite(a.g) ||
            a.max_depth > 1024 || a.samples == 0 || a.samples > 64)
            return "Invalid or excessive layered material parameters";
        SpectralMaterial interface;
        interface.scattering = ScatteringKind::dielectric;
        interface.eta = m.scattering == ScatteringKind::coated_diffuse ? m.eta : a.interface_eta;
        interface.surface.eta_root =
            m.scattering == ScatteringKind::coated_diffuse ? a.eta_root : a.interface_eta_root;
        interface.u_roughness = m.scattering == ScatteringKind::coated_diffuse
                                    ? m.u_roughness
                                    : a.interface_u_roughness;
        interface.v_roughness = m.scattering == ScatteringKind::coated_diffuse
                                    ? m.v_roughness
                                    : a.interface_v_roughness;
        if (auto error = validate_dielectric_material(interface); !error.empty())
            return error;
        if (m.scattering == ScatteringKind::coated_conductor) {
            auto substrate = m;
            substrate.scattering = ScatteringKind::conductor;
            return validate_conductor_material(substrate);
        }
    } else if (m.scattering == ScatteringKind::diffuse_transmission) {
        if (!std::isfinite(a.transmittance) || !std::isfinite(a.transmission_scale))
            return "Diffuse transmission requires finite reflectance, transmittance and scale";
    } else {
        return "Unknown surface scattering kind";
    }
    auto base = m;
    base.scattering = ScatteringKind::diffuse;
    base.eta = {};
    base.k = {};
    base.u_roughness = base.v_roughness = 0;
    base.u_roughness_root = base.v_roughness_root = UINT32_MAX;
    return validate_pbrt_material(base);
}

std::string validate_pbrt_material(const SpectralMaterial &material) {
    if (material.scattering == ScatteringKind::gltf) {
        const auto &g = material.gltf;
        const auto unit = [](float x) { return std::isfinite(x) && x >= 0 && x <= 1; };
        if (!std::ranges::all_of(g.base_color, unit) || !unit(g.metallic) || !unit(g.roughness) ||
            !std::isfinite(g.normal_scale) || !std::isfinite(g.alpha_cutoff) ||
            g.alpha_cutoff < 0 || g.alpha_mode > 2 || !std::ranges::all_of(g.emissive, unit))
            return "Invalid glTF metallic-roughness parameters";
        if (material.bump_root != UINT32_MAX || material.normal_map != UINT32_MAX ||
            material.texture_root != UINT32_MAX || material.table || !material.eta.knots.empty() ||
            !material.k.knots.empty())
            return "glTF material contains a foreign surface payload";
        return {};
    }

    if (material.scattering == ScatteringKind::null_interface) {
        if (material.bump_root != UINT32_MAX || material.normal_map != UINT32_MAX ||
            material.texture_root != UINT32_MAX || material.table)
            return "Null boundaries cannot carry surface detail or scattering tables";
        return {};
    }
    if (material.scattering == ScatteringKind::conductor)
        return validate_conductor_material(material);
    if (material.scattering == ScatteringKind::dielectric)
        return validate_dielectric_material(material);
    if (material.scattering >= ScatteringKind::thin_dielectric)
        return validate_surface_material(material);
    if (material.scattering != ScatteringKind::diffuse || !material.eta.knots.empty() ||
        !material.k.knots.empty() || material.u_roughness != 0 || material.v_roughness != 0 ||
        material.u_roughness_root != UINT32_MAX || material.v_roughness_root != UINT32_MAX)
        return "Invalid diffuse scattering payload";
    constexpr std::array<float, 3> zero_coefficients{};
    if (material.kind != SpectralMaterialKind::image && material.image_texture != 0) {
        return "Constant spectral material contains an image binding";
    }
    if (material.kind != SpectralMaterialKind::blackbody && material.temperature_kelvin != 0) {
        return "Non-blackbody material contains a temperature payload";
    }
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
    case SpectralMaterialKind::texture_graph:
    case SpectralMaterialKind::image:
        if (!material.knots.empty() || material.constant != 0 ||
            material.coefficients != zero_coefficients) {
            return "Image reflectance contains a constant spectral payload";
        }
        return {};
    case SpectralMaterialKind::blackbody:
        if (!(std::isfinite(material.temperature_kelvin) && material.temperature_kelvin > 0)) {
            return "PBRT blackbody reflectance requires a positive finite temperature";
        }
        if (material.reflectance != std::array<float, 8>{} || !material.knots.empty() ||
            material.constant != 0 || material.coefficients != zero_coefficients) {
            return "PBRT blackbody reflectance contains another spectral payload";
        }
        return {};
    }
    return "Invalid PBRT spectral material kind";
}

std::string validate_image_textures(const RenderScene &scene) {
    namespace graph = texture_graph_wire;
    if (const auto error = validate_texture_graph(scene.texture_graph, scene.image_textures.size());
        !error.empty()) {
        return error;
    }
    const auto root_valid = [&](std::uint32_t root, std::uint32_t type) {
        return root < scene.texture_graph.nodes.size() &&
               scene.texture_graph.nodes[root].type == type;
    };
    const auto base_valid = [&](std::uint32_t index) {
        return index < scene.image_textures.size() && scene.image_textures[index].image &&
               scene.image_textures[index].image->native_base &&
               !scene.image_textures[index].image->levels.empty() &&
               scene.image_textures[index].image->source_channels == 3;
    };
    for (const auto &material : scene.materials) {
        if (material.scattering == ScatteringKind::gltf) {
            const auto &g = material.gltf;
            for (const auto index : {g.base_color_texture, g.metallic_roughness_texture,
                                     g.normal_texture, g.emissive_texture}) {
                if (index != UINT32_MAX && (index >= scene.image_textures.size() ||
                                            !scene.image_textures[index].gltf_sampler ||
                                            !scene.image_textures[index].image ||
                                            !scene.image_textures[index].image->gltf))
                    return "glTF material requires a glTF image sampler";
            }
        }
        for (const auto root :
             {material.u_roughness_root, material.v_roughness_root,
              material.surface.interface_u_roughness_root,
              material.surface.interface_v_roughness_root, material.surface.thickness_root,
              material.surface.g_root, material.surface.transmission_scale_root,
              material.surface.mix_amount_root}) {
            if (root != UINT32_MAX && !root_valid(root, graph::scalar))
                return "Conductor roughness requires a scalar texture graph";
        }
        for (const auto root :
             {material.surface.eta_root, material.surface.k_root,
              material.surface.interface_eta_root, material.surface.conductor_reflectance_root,
              material.surface.albedo_root, material.surface.transmittance_root}) {
            if (root != UINT32_MAX && !root_valid(root, graph::spectrum))
                return "Surface parameter requires a valid spectrum texture graph";
        }
        if (material.kind == SpectralMaterialKind::image &&
            material.image_texture >= scene.image_textures.size()) {
            return "Image material references an invalid texture";
        }
        if (material.kind == SpectralMaterialKind::texture_graph &&
            !root_valid(material.texture_root, graph::spectrum)) {
            return "Material references an invalid spectrum texture graph";
        }
        if (material.kind != SpectralMaterialKind::texture_graph &&
            material.texture_root != UINT32_MAX) {
            return "Non-graph material contains a texture graph binding";
        }
        if (material.bump_root != UINT32_MAX && !root_valid(material.bump_root, graph::scalar)) {
            return "Bump mapping requires a scalar texture graph";
        }
        if (material.normal_map != UINT32_MAX && !base_valid(material.normal_map)) {
            return "Normal mapping requires a native-resolution image";
        }
    }
    const auto count = scene.geometry.instances.size();
    if ((!scene.instance_alpha_roots.empty() && scene.instance_alpha_roots.size() != count) ||
        (!scene.instance_invisible_emitters.empty() &&
         scene.instance_invisible_emitters.size() != count) ||
        (!scene.instance_surface_transforms.empty() &&
         scene.instance_surface_transforms.size() != count)) {
        return "Texture consumer bindings require one entry per source instance";
    }
    std::set<const ImagePyramid *> certified_images;
    const auto certified_mask = [&](std::uint32_t root) {
        const auto &node = scene.texture_graph.nodes[root];
        if (node.kind == graph::image && node.mapping.kind == graph::uv &&
            node.payload[0] < scene.image_textures.size()) {
            const auto &texture = scene.image_textures[node.payload[0]];
            if (texture.filter != ImageFilter::point || texture.scale != 1 || !texture.image)
                return false;
            if (certified_images.contains(texture.image.get()))
                return true;
            if (!certify_binary_emitter_mask(scene.texture_graph, root, scene.image_textures))
                return false;
            certified_images.insert(texture.image.get());
            return true;
        }
        return certify_binary_emitter_mask(scene.texture_graph, root, scene.image_textures);
    };
    for (std::size_t i = 0; i < count; ++i) {
        const auto root =
            scene.instance_alpha_roots.empty() ? UINT32_MAX : scene.instance_alpha_roots[i];
        const bool emissive =
            !scene.instance_emissions.empty() && scene.instance_emissions[i] != no_area_emission;
        const auto invisible =
            scene.instance_invisible_emitters.empty() ? 0 : scene.instance_invisible_emitters[i];
        if (root != UINT32_MAX && !root_valid(root, graph::scalar)) {
            return "Alpha requires a scalar texture graph";
        }
        if (emissive && root != UINT32_MAX && !certified_mask(root)) {
            return "Emitting alpha requires a certified binary constant or point texture";
        }
        const bool zero = root != UINT32_MAX &&
                          scene.texture_graph.nodes[root].kind == graph::constant_node &&
                          scene.texture_graph.nodes[root].values[0] == 0;
        if (invisible > 1 || bool(invisible) != (emissive && zero)) {
            return "Invisible emitter binding must match a zero constant alpha";
        }
        const auto &material = scene.materials[scene.instance_materials[i]];
        if (((material.scattering >= ScatteringKind::dielectric &&
              material.scattering != ScatteringKind::null_interface) ||
             material.bump_root != UINT32_MAX || material.normal_map != UINT32_MAX) &&
            scene.instance_surface_transforms.empty()) {
            return "Dielectric or mapped surfaces require definition and placement transforms";
        }
    }
    for (const auto &emission : scene.area_emissions) {
        if (emission.gltf_material != UINT32_MAX &&
            (emission.gltf_material >= scene.materials.size() ||
             scene.materials[emission.gltf_material].scattering != ScatteringKind::gltf))
            return "glTF area emission requires a glTF material";
        if (emission.image_texture != UINT32_MAX &&
            (!base_valid(emission.image_texture) || !std::isfinite(emission.image_scale) ||
             emission.image_scale < 0)) {
            return "Area image emission requires a native image and finite nonnegative scale";
        }
    }
    if (scene.environment_image != UINT32_MAX) {
        if (!base_valid(scene.environment_image) || !std::isfinite(scene.environment_image_scale) ||
            scene.environment_image_scale < 0) {
            return "Environment image requires a native image and finite nonnegative scale";
        }
        const auto &level = scene.image_textures[scene.environment_image].image->levels.front();
        if (level.width != level.height || level.width == 0 ||
            level.width > ImageLimits{}.max_dimension) {
            return "Environment image must use a bounded square equal-area map";
        }
        const auto &m = scene.environment_texture_from_world.values;
        if (!std::ranges::all_of(m, [](float v) { return std::isfinite(v); }))
            return "Environment image transform must be finite";
        constexpr double tolerance = 64 * std::numeric_limits<float>::epsilon();
        for (unsigned row = 0; row < 3; ++row) {
            for (unsigned column = 0; column < 3; ++column) {
                double product = 0;
                for (unsigned k = 0; k < 3; ++k)
                    product += double(m[4 * row + k]) * m[4 * column + k];
                if (std::abs(product - double(row == column)) > tolerance)
                    return "Environment image transform must be orthonormal";
            }
        }
    }
    for (const auto &surface : scene.instance_surface_transforms) {
        for (const auto *m : {&surface.shape, &surface.placement}) {
            if (!std::ranges::all_of(*m, [](double v) { return std::isfinite(v); }) ||
                (*m)[12] != 0 || (*m)[13] != 0 || (*m)[14] != 0 || (*m)[15] != 1)
                return "Surface detail transform must be finite and affine";
        }
    }
    const bool microfacets = std::ranges::any_of(scene.materials, [](const auto &m) {
        return m.scattering != ScatteringKind::diffuse &&
               m.scattering != ScatteringKind::null_interface;
    });
    if (scene.image_textures.empty() && scene.texture_graph.nodes.empty() && !microfacets) {
        return scene.instance_texture_frames.empty() ? "" : "Texture frames require image textures";
    }
    if (scene.model != RenderModel::pbrt ||
        scene.instance_texture_frames.size() != scene.geometry.instances.size()) {
        return "Image textures require PBRT and one frame table per source instance";
    }
    const auto finite_scalar = [](float v) { return std::isfinite(v); };
    if (!std::ranges::all_of(scene.texture_camera.world_from_camera, finite_scalar) ||
        !std::ranges::all_of(scene.texture_camera.camera_from_world, finite_scalar)) {
        return "Image texture camera matrices must be finite";
    }
    const auto &forward = scene.texture_camera.world_from_camera;
    const auto &inverse = scene.texture_camera.camera_from_world;
    constexpr double inverse_tolerance = 32 * std::numeric_limits<float>::epsilon();
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            double ab = 0, ba = 0;
            for (std::size_t k = 0; k < 3; ++k) {
                ab += static_cast<double>(forward[3 * row + k]) * inverse[3 * k + column];
                ba += static_cast<double>(inverse[3 * row + k]) * forward[3 * k + column];
            }
            const double expected = row == column ? 1 : 0;
            if (std::abs(ab - expected) > inverse_tolerance ||
                std::abs(ba - expected) > inverse_tolerance) {
                return "Image texture camera matrices must be an inverse pair";
            }
        }
    }
    std::set<const ImagePyramid *> checked;
    std::size_t remaining = ImageLimits{}.retained_bytes;
    if (scene.environment_image != UINT32_MAX) {
        const auto width = std::min<std::size_t>(
            render_wire::environment_grid_limit,
            scene.image_textures[scene.environment_image].image->levels.front().width);
        const auto bytes = (width + 1) * (width + 1) * sizeof(float);
        if (bytes > remaining)
            return "Environment sampling table exceeds the image memory budget";
        remaining -= bytes;
    }
    for (const auto &texture : scene.image_textures) {
        if (texture.filter != ImageFilter::point && texture.filter != ImageFilter::bilinear &&
            texture.filter != ImageFilter::trilinear && texture.filter != ImageFilter::ewa) {
            return "Invalid image texture filter";
        }
        if (texture.filter == ImageFilter::ewa &&
            !(texture.max_anisotropy >= 1 && texture.max_anisotropy <= 16)) {
            return "Image texture EWA maxanisotropy must be in [1, 16]";
        }
        const std::array mapping{texture.uscale, texture.vscale, texture.udelta, texture.vdelta,
                                 texture.scale};
        if (!texture.image || !std::ranges::all_of(mapping, finite_scalar)) {
            return "Image texture requires an owned pyramid and finite mapping";
        }
        const auto &image = *texture.image;
        if (bool(texture.gltf_sampler) != image.gltf)
            return "glTF image/sampler policy mismatch";
        if (texture.gltf_sampler) {
            const auto &s = *texture.gltf_sampler;
            const auto wrap = [](auto v) { return v == 10497 || v == 33071 || v == 33648; };
            if (!wrap(s.wrap_s) || !wrap(s.wrap_t) ||
                (s.mag_filter != 9728 && s.mag_filter != 9729) ||
                (s.min_filter != 9728 && s.min_filter != 9729 &&
                 (s.min_filter < 9984 || s.min_filter > 9987)) ||
                image.storage != ImageStorage::linear_float || image.source_channels != 4)
                return "Invalid glTF sampler or image storage";
        }
        if (!checked.insert(&image).second) {
            continue;
        }
        if (image.source_channels != 1 && image.source_channels != 3 &&
            image.source_channels != 4) {
            return "Invalid image channel layout";
        }
        if (image.custom_transfer) {
            if (remaining < sizeof(*image.custom_transfer) ||
                !std::ranges::all_of(*image.custom_transfer, [](float v) {
                    return std::isfinite(v) && v >= 0 && v <= 1;
                })) {
                return "Invalid or excessive image transfer table";
            }
            remaining -= sizeof(*image.custom_transfer);
        } else if (image.encoding == ImageEncoding::gamma &&
                   image.storage == ImageStorage::encoded8) {
            return "Gamma byte image requires an owned transfer table";
        }
        if ((image.encoding != ImageEncoding::srgb && image.encoding != ImageEncoding::linear &&
             image.encoding != ImageEncoding::gamma) ||
            (image.storage != ImageStorage::encoded8 &&
             image.storage != ImageStorage::linear_half &&
             image.storage != ImageStorage::linear_float) ||
            (image.wrap != ImageWrap::repeat && image.wrap != ImageWrap::clamp &&
             image.wrap != ImageWrap::black) ||
            image.levels.empty() || image.levels.size() > 13) {
            return "Invalid image texture encoding, storage, wrap or mip count";
        }
        if (image.texels.size() > remaining / sizeof(std::uint32_t)) {
            return "Image texture retained memory limit exceeded";
        }
        remaining -= image.texels.size() * sizeof(std::uint32_t);
        if (image.levels.size() > remaining / sizeof(ImageMipLevel)) {
            return "Image texture retained memory limit exceeded";
        }
        remaining -= image.levels.size() * sizeof(ImageMipLevel);
        std::size_t offset = 0;
        const std::size_t words_per_texel = image_storage_words(image.storage);
        auto width = image.levels.front().width;
        auto height = image.levels.front().height;
        if (width == 0 || height == 0 ||
            (!image.native_base && !image.gltf &&
             (!std::has_single_bit(width) || !std::has_single_bit(height))) ||
            width > ImageLimits{}.max_dimension || height > ImageLimits{}.max_dimension) {
            return "Image mip dimensions must be bounded positive powers of two";
        }
        for (const auto &level : image.levels) {
            const auto words = std::size_t{width} * height * words_per_texel;
            if (level.width != width || level.height != height || level.offset != offset ||
                words > image.texels.size() - offset) {
                return "Invalid image mip layout";
            }
            offset += words;
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        if (offset != image.texels.size() ||
            (image.native_base
                 ? image.levels.size() != 1
                 : (image.levels.back().width != 1 || image.levels.back().height != 1 ||
                    image.levels.size() !=
                        static_cast<std::size_t>(std::bit_width(
                            std::max(image.levels.front().width, image.levels.front().height)))))) {
            return "Incomplete image mip chain";
        }
        if (image.storage == ImageStorage::linear_half) {
            const auto valid_half = [](std::uint32_t bits) { return (bits & 0x7fffu) < 0x7c00u; };
            for (std::size_t i = 0; i < image.texels.size(); i += 2) {
                const auto rg = image.texels[i], b = image.texels[i + 1];
                if (!valid_half(rg & 0xffffu) || !valid_half(rg >> 16u) ||
                    !valid_half(b & 0xffffu) || !valid_half(b >> 16u) ||
                    (image.source_channels != 4u && (b >> 16u) != 0)) {
                    return "Invalid linear-half image texel payload";
                }
            }
        } else if (image.storage == ImageStorage::linear_float) {
            if ((!image.gltf && image.encoding != ImageEncoding::linear) || image.custom_transfer) {
                return "Floating-point images require linear encoding";
            }
            for (std::size_t i = 0; i < image.texels.size(); ++i) {
                if (!std::isfinite(std::bit_cast<float>(image.texels[i])) ||
                    (image.source_channels != 4u && i % 4 == 3 && image.texels[i] != 0)) {
                    return "Invalid linear-float image texel payload";
                }
            }
        }
    }
    std::set<const ImagePyramid *> checked_emission;
    const auto check_emission = [&](std::uint32_t index) {
        const auto &image = *scene.image_textures[index].image;
        return checked_emission.insert(&image).second ? validate_image_light(image) : std::string{};
    };
    for (const auto &emission : scene.area_emissions) {
        if (emission.image_texture != UINT32_MAX) {
            if (auto error = check_emission(emission.image_texture); !error.empty())
                return error;
        }
    }
    if (scene.environment_image != UINT32_MAX) {
        if (auto error = check_emission(scene.environment_image); !error.empty())
            return error;
    }
    std::size_t remaining_frames = render_patch_corner_limit;
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        const auto &frames = scene.instance_texture_frames[i];
        const auto &mesh = scene.geometry.meshes[scene.geometry.instances[i].mesh_index];
        const bool textured = instance_uses_texture_context(scene, i);
        if (frames.size() != (textured ? mesh.triangles.size() : 0) ||
            frames.size() > remaining_frames) {
            return "Invalid or excessive texture triangle frame table";
        }
        remaining_frames -= frames.size();
        for (const auto &frame : frames) {
            const std::array values{frame.dpdu.x, frame.dpdu.y, frame.dpdu.z,
                                    frame.dpdv.x, frame.dpdv.y, frame.dpdv.z};
            if (!std::ranges::all_of(values, finite_scalar)) {
                return "Texture triangle frame must be finite";
            }
        }
    }
    return {};
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

double optical_spectrum(const OpticalSpectrum &spectrum, double wavelength) {
    if (spectrum.knots.empty())
        return 0;
    if (spectrum.extend_endpoints) {
        if (wavelength < spectrum.knots.front().wavelength)
            return spectrum.knots.front().value;
        if (wavelength > spectrum.knots.back().wavelength)
            return spectrum.knots.back().value;
    }
    return sampled_spectrum(spectrum.knots, wavelength);
}

std::string validate_conductor_material(const SpectralMaterial &material) {
    if (material.scattering != ScatteringKind::conductor)
        return "Expected conductor scattering";
    // Check the complete piecewise-linear domain, including file support jumps.
    std::vector<double> boundaries{360, 830};
    const bool dynamic_eta = material.surface.eta_root != UINT32_MAX;
    const bool dynamic_k = material.surface.k_root != UINT32_MAX;
    const bool shortcut = material.surface.conductor_reflectance_root != UINT32_MAX;
    if (shortcut &&
        (dynamic_eta || dynamic_k || !material.eta.knots.empty() || !material.k.knots.empty()))
        return "Conductor reflectance cannot be combined with eta or k";
    for (const auto *curve : {&material.eta, &material.k}) {
        if (shortcut || (curve == &material.eta ? dynamic_eta : dynamic_k))
            continue;
        if (curve->knots.size() < 2)
            return "Optical spectra require at least two knots";
        float previous = -std::numeric_limits<float>::infinity();
        for (const auto [wavelength, value] : curve->knots) {
            if (!std::isfinite(wavelength) || wavelength <= previous || !std::isfinite(value) ||
                value < 0)
                return "Optical spectra require increasing finite wavelengths and finite "
                       "nonnegative values";
            previous = wavelength;
            if (wavelength >= 360 && wavelength <= 830)
                boundaries.push_back(wavelength);
        }
    }
    std::ranges::sort(boundaries);
    const auto zero_pair = [&](double wavelength) {
        return optical_spectrum(material.eta, wavelength) == 0 &&
               optical_spectrum(material.k, wavelength) == 0;
    };
    for (std::size_t i = 0; !shortcut && !dynamic_eta && !dynamic_k && i < boundaries.size(); ++i) {
        if (zero_pair(boundaries[i]) ||
            (i && zero_pair(std::midpoint(boundaries[i - 1], boundaries[i]))))
            return "Conductor eta and k cannot both be zero in the transport wavelength range";
    }
    if (!std::isfinite(material.u_roughness) || material.u_roughness < 0 ||
        !std::isfinite(material.v_roughness) || material.v_roughness < 0)
        return "Conductor roughness must be finite and nonnegative";
    if (!material.knots.empty() || material.reflectance != std::array<float, 8>{} ||
        material.constant != 0 || material.coefficients != std::array<float, 3>{} ||
        material.temperature_kelvin != 0 || material.image_texture != 0 ||
        material.texture_root != UINT32_MAX ||
        (material.kind != SpectralMaterialKind::sampled &&
         material.kind != SpectralMaterialKind::constant))
        return "Conductor contains a diffuse reflectance payload";
    return {};
}

std::string validate_dielectric_material(const SpectralMaterial &material) {
    if (material.scattering != ScatteringKind::dielectric)
        return "Expected dielectric scattering";
    if (material.surface.eta_root == UINT32_MAX && material.eta.knots.size() < 2)
        return "Dielectric eta requires at least two knots";
    float previous = -std::numeric_limits<float>::infinity();
    for (const auto [wavelength, value] : material.eta.knots) {
        if (!std::isfinite(wavelength) || wavelength <= previous || !std::isfinite(value) ||
            value < 0)
            return "Dielectric eta requires increasing finite wavelengths and finite "
                   "nonnegative values";
        previous = wavelength;
    }
    if (!std::isfinite(material.u_roughness) || material.u_roughness < 0 ||
        !std::isfinite(material.v_roughness) || material.v_roughness < 0)
        return "Dielectric roughness must be finite and nonnegative";
    if (!material.k.knots.empty() || material.k.extend_endpoints || !material.knots.empty() ||
        material.reflectance != std::array<float, 8>{} || material.constant != 0 ||
        material.coefficients != std::array<float, 3>{} || material.temperature_kelvin != 0 ||
        material.image_texture != 0 || material.texture_root != UINT32_MAX ||
        (material.kind != SpectralMaterialKind::sampled &&
         material.kind != SpectralMaterialKind::constant))
        return "Dielectric contains an unrelated scattering payload";
    return {};
}

bool scene_has_dielectrics(const RenderScene &scene) {
    return std::ranges::any_of(scene.materials, [](const auto &material) {
        return material.scattering == ScatteringKind::dielectric ||
               material.scattering == ScatteringKind::gltf;
    });
}

std::string validate_image_light(const ImagePyramid &image) {
    if (!image.native_base || image.levels.size() != 1 || image.source_channels != 3)
        return "Image lights require a native RGB image";
    const auto &level = image.levels.front();
    if (level.width == 0 || level.height == 0 || level.width > ImageLimits{}.max_dimension ||
        level.height > ImageLimits{}.max_dimension)
        return "Image light dimensions exceed the supported range";
    for (std::uint32_t y = 0; y < level.height; ++y) {
        for (std::uint32_t x = 0; x < level.width; ++x) {
            const auto rgb = image_base_texel_linear_rgb(image, x, y);
            if (!rgb || !std::ranges::all_of(
                            *rgb, [](float value) { return std::isfinite(value) && value >= 0; }))
                return "Image lights require finite nonnegative RGB samples";
            // RGBIlluminant first forms twice the largest component in binary32.
            if (*std::ranges::max_element(*rgb) > std::numeric_limits<float>::max() / 2)
                return "Image light RGBIlluminant reconstruction overflows binary32";
        }
    }
    return {};
}

std::string validate_projection_image(const ImagePyramid &image) {
    if (image.gltf || image.wrap != ImageWrap::clamp)
        return "Projection lights require a native RGB image with clamp boundaries";
    if (const auto error = validate_image_light(image); !error.empty())
        return error;
    const auto &level = image.levels.front();
    for (std::uint32_t y = 0; y < level.height; ++y)
        for (std::uint32_t x = 0; x < level.width; ++x) {
            const auto rgb = image_base_texel_linear_rgb(image, x, y);
            if (!rgb || !std::ranges::all_of(*rgb, [](float value) {
                    return std::fpclassify(value) != FP_SUBNORMAL;
                }))
                return "Projection RGB samples must remain representable on the GPU";
        }
    return {};
}

std::string validate_goniometric_image(const ImagePyramid &image) {
    if (!image.native_base || image.levels.size() != 1 || image.source_channels != 1 ||
        image.wrap != ImageWrap::clamp || image.gltf)
        return "Goniometric lights require a native scalar image with clamp boundaries";
    const auto &level = image.levels.front();
    if (level.width == 0 || level.width != level.height ||
        level.width > ImageLimits{}.max_dimension)
        return "Goniometric image must be square and within the dimension limit";
    for (std::uint32_t y = 0; y < level.height; ++y) {
        for (std::uint32_t x = 0; x < level.width; ++x) {
            const auto rgb = image_base_texel_linear_rgb(image, x, y);
            if (!rgb || !std::isfinite((*rgb)[0]) || (*rgb)[0] < 0 || (*rgb)[0] != (*rgb)[1] ||
                (*rgb)[0] != (*rgb)[2])
                return "Goniometric image requires finite nonnegative scalar samples";
            if (std::fpclassify((*rgb)[0]) == FP_SUBNORMAL)
                return "Goniometric scalar samples must remain representable on the GPU";
        }
    }
    return {};
}

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

// Enclose the complete raster/filter and aperture support before device execution.
// These conservative bounds include binary32 rounding and Metal subnormal flushing.
std::string validate_lens_support(const RasterCamera &camera, const PerspectiveLens &lens,
                                  const PixelFilter &filter, std::uint32_t width,
                                  std::uint32_t height) {
    if (!std::isnormal(lens.radius) || lens.radius <= 0 || !std::isnormal(lens.focal_distance) ||
        lens.focal_distance <= 0) {
        return "Perspective lens radius and focal distance must be positive normal binary32 values";
    }
    const double ratio = double{lens.radius} / lens.focal_distance;
    if (ratio < float_min || ratio > float_max) {
        return "Perspective lens radius/focal distance ratio is not normal binary32";
    }
    if (!valid_raster_camera(camera) || !valid_pixel_filter(filter) || width == 0 || height == 0 ||
        !finite(lens.camera_x) || !finite(lens.camera_y)) {
        return "Perspective lens requires finite raster coefficients, axes and positive dimensions";
    }
    const auto u = to_vector(lens.camera_x);
    const auto v = to_vector(lens.camera_y);
    const auto corner = to_vector(camera.corner);
    const auto dx = to_vector(camera.dx);
    const auto dy = to_vector(camera.dy);
    constexpr double tolerance = 128 * float_roundoff;
    if (std::abs(dot(u, u) - 1) > tolerance || std::abs(dot(v, v) - 1) > tolerance ||
        std::abs(dot(u, v)) > tolerance ||
        squared_length(cross(u, dx)) > tolerance * tolerance * squared_length(dx) ||
        squared_length(cross(v, dy)) > tolerance * tolerance * squared_length(dy)) {
        return "Perspective lens axes must be rigid and aligned with the raster basis";
    }
    const auto x = triangle_raster_interval(width, filter.x_radius);
    const auto y = triangle_raster_interval(height, filter.y_radius);
    if (!x || !y || !triangle_ray_range(camera, *x, *y)) {
        return "Perspective lens filter support exceeds the safe binary32 raster direction range";
    }
    const double x_abs = std::max(std::abs((*x)[0]), std::abs((*x)[1]));
    const double y_abs = std::max(std::abs((*y)[0]), std::abs((*y)[1]));
    const auto origin = to_vector(camera.origin);
    Vector direction_error{}, aperture_bound{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double offset = lens.radius * (std::abs(u[axis]) + std::abs(v[axis]));
        const double ideal_error = 32 * float_roundoff * offset + 8 * float_min;
        const double origin_bound = std::abs(origin[axis]) + offset + ideal_error;
        if (origin_bound * (1 + float_roundoff) + float_min > float_max) {
            return "Perspective lens aperture origin overflows binary32";
        }
        // The shader subtracts the camera center from the rounded ray origin.
        // Bound addition and subtraction rounding, including permitted RTZ.
        const double origin_error = 2 * float_roundoff * origin_bound + float_min;
        const double offset_error = ideal_error + origin_error +
                                    2 * float_roundoff * (offset + ideal_error + origin_error) +
                                    float_min;
        const double raster_bound =
            std::abs(corner[axis]) + x_abs * std::abs(dx[axis]) + y_abs * std::abs(dy[axis]);
        aperture_bound[axis] = offset / lens.focal_distance;
        direction_error[axis] = 32 * float_roundoff * raster_bound + 8 * float_min +
                                offset_error / lens.focal_distance +
                                8 * float_roundoff * aperture_bound[axis];
        double direction_bound = 0;
        for (const double column : *x) {
            direction_bound =
                std::max(direction_bound,
                         endpoint_bound(*y, dy[axis], std::fma(column, dx[axis], corner[axis])));
        }
        if (offset + offset_error > float_max ||
            direction_bound + aperture_bound[axis] + direction_error[axis] > float_max) {
            return "Perspective lens aperture or direction overflows binary32";
        }
    }
    // Each independent aperture axis must move the binary32 world origin.
    // Otherwise a nominally positive aperture silently becomes a pinhole/line.
    std::array<Vector, 2> aperture_diameters{};
    std::size_t diameter_index = 0;
    for (const auto &basis : {u, v}) {
        bool moves = false;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double displacement = lens.radius * basis[axis];
            aperture_diameters[diameter_index][axis] =
                double{static_cast<float>(origin[axis] + displacement)} -
                static_cast<float>(origin[axis] - displacement);
            moves = moves || static_cast<float>(origin[axis] + displacement) != origin[axis] ||
                    static_cast<float>(origin[axis] - displacement) != origin[axis];
        }
        if (!moves) {
            return "Perspective lens aperture collapses at the binary32 camera origin";
        }
        ++diameter_index;
    }
    if (squared_length(cross(aperture_diameters[0], aperture_diameters[1])) == 0) {
        return "Perspective lens aperture axes collapse at the binary32 camera origin";
    }
    const auto normal = cross(dx, dy);
    const Vector abs_normal{std::abs(normal[0]), std::abs(normal[1]), std::abs(normal[2])};
    const double separation = std::abs(dot(normal, corner)) - dot_error(normal, corner);
    const double aperture_projection = ratio * (std::abs(dot(normal, u)) + dot_error(normal, u) +
                                                std::abs(dot(normal, v)) + dot_error(normal, v));
    const double uncertainty = x_abs * (std::abs(dot(normal, dx)) + dot_error(normal, dx)) +
                               y_abs * (std::abs(dot(normal, dy)) + dot_error(normal, dy)) +
                               aperture_projection + dot(abs_normal, direction_error);
    if (!(separation > (uncertainty + (abs_normal[0] + abs_normal[1] + abs_normal[2]) * float_min) *
                           (1 + double_slack))) {
        return "Perspective lens direction can collapse in binary32 over the aperture support";
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

std::string validate_patch_geometry(const GeometryMesh &mesh, const GeometryTransform &transform) {
    for (std::size_t i = 0; i < mesh.patches.size(); ++i) {
        const auto &patch = mesh.patches[i];
        std::array<GeometryPoint, 4> world{};
        for (std::size_t j = 0; j < 4; ++j) {
            if (patch[j] >= mesh.positions.size()) {
                return "Bilinear patch index is out of range";
            }
            const auto p = area_world_vertex(mesh.positions[patch[j]], transform);
            if (!p) {
                return "Bilinear patch has an unrepresentable world vertex";
            }
            world[j] = *p;
        }
        const auto bounds = bilinear_jacobian_bounds(world);
        if (!bounds ||
            (*bounds)[0] * (*bounds)[0] < 4 * double{std::numeric_limits<float>::min()} ||
            (*bounds)[1] * (*bounds)[1] > double{std::numeric_limits<float>::max()} / 4) {
            return "Bilinear patch " + std::to_string(i) +
                   " has singular or unrepresentable world derivatives";
        }
        // The shader interpolates local edges before applying the matrix;
        // rounded world corners alone do not certify that arithmetic. Enclose
        // each tangent relative to the corresponding world-corner tangent.
        std::array<Vector, 2> extent{}, error{};
        constexpr double eps = std::numeric_limits<float>::epsilon();
        constexpr double floor = 128 * double{std::numeric_limits<float>::min()};
        const std::array edges{std::array{0U, 1U}, std::array{2U, 3U}, std::array{0U, 2U},
                               std::array{1U, 3U}};
        for (std::size_t k = 0; k < edges.size(); ++k) {
            const auto edge = edges[k];
            const auto d = difference(to_vector(mesh.positions[patch[edge[1]]]),
                                      to_vector(mesh.positions[patch[edge[0]]]));
            const auto w = apply_transform(transform, d, false);
            const auto baked = difference(to_vector(world[edge[1]]), to_vector(world[edge[0]]));
            if (!binary32_range(d) || !binary32_range(w)) {
                return "Bilinear patch tangent exceeds the binary32 rendering range";
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                double products = 0, matrix_sum = 0;
                for (std::size_t j = 0; j < 3; ++j) {
                    const double a = std::abs(transform.values[axis * 4 + j]);
                    products += a * std::abs(d[j]);
                    matrix_sum += a;
                }
                extent[k / 2][axis] = std::max(extent[k / 2][axis], std::abs(baked[axis]));
                error[k / 2][axis] = std::max(error[k / 2][axis], std::abs(w[axis] - baked[axis]) +
                                                                      32 * eps * products +
                                                                      floor * (1 + matrix_sum));
            }
        }
        Vector cross_error{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto j = (axis + 1) % 3, k = (axis + 2) % 3;
            const auto &u = extent[0], &v = extent[1], &eu = error[0], &ev = error[1];
            cross_error[axis] =
                u[j] * ev[k] + u[k] * ev[j] + v[j] * eu[k] + v[k] * eu[j] + eu[j] * ev[k] +
                eu[k] * ev[j] + floor +
                32 * eps * ((u[j] + eu[j]) * (v[k] + ev[k]) + (u[k] + eu[k]) * (v[j] + ev[j]));
        }
        const double uncertainty = std::hypot(cross_error[0], cross_error[1], cross_error[2]);
        if (!((*bounds)[0] - uncertainty >=
              2 * std::sqrt(double{std::numeric_limits<float>::min()})) ||
            !((*bounds)[1] + uncertainty <=
              std::sqrt(double{std::numeric_limits<float>::max()}) / 2)) {
            return "Bilinear patch has uncertified transformed tangent arithmetic";
        }
        // Require representable outward offsets to stay below 1/1024 of
        // the minimum tangent altitude. This explicit numerical-domain bound
        // rejects small sheets at huge normal translations; no geometry-only
        // bound can certify arbitrary separation from another primitive.
        const double tangent_max = std::max(std::hypot(extent[0][0], extent[0][1], extent[0][2]),
                                            std::hypot(extent[1][0], extent[1][1], extent[1][2]));
        const double altitude = ((*bounds)[0] - uncertainty) / tangent_max;
        double offset = 0;
        const auto &matrix = transform.values;
        const bool identity = matrix[3] == 0 && matrix[7] == 0 && matrix[11] == 0 &&
                              matrix[0] == 1 && matrix[1] == 0 && matrix[2] == 0 &&
                              matrix[4] == 0 && matrix[5] == 1 && matrix[6] == 0 &&
                              matrix[8] == 0 && matrix[9] == 0 && matrix[10] == 1;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            float coordinate = 0;
            for (const auto p : world) {
                coordinate = std::max(coordinate, static_cast<float>(std::abs(to_vector(p)[axis])));
            }
            const double ulp =
                double{std::nextafter(coordinate, std::numeric_limits<float>::infinity())} -
                coordinate;
            if (!(ulp <= altitude / 1024)) {
                return "Bilinear patch has insufficient world-coordinate precision for endpoint "
                       "connections";
            }
            const auto j = (axis + 1) % 3, k = (axis + 2) % 3;
            const double normal = std::min(1.0, (extent[0][j] * extent[1][k] +
                                                 extent[0][k] * extent[1][j] + cross_error[axis]) /
                                                    ((*bounds)[0] - uncertainty));
            double products = std::abs(transform.values[axis * 4 + 3]);
            for (std::size_t component = 0; component < 3; ++component) {
                for (const auto index : patch) {
                    products += std::abs(double{transform.values[axis * 4 + component]} *
                                         to_vector(mesh.positions[index])[component]);
                }
            }
            if (products > double{std::numeric_limits<float>::max()} / 16) {
                return "Bilinear patch interpolation exceeds the binary32 rendering range";
            }
            const double traversal = identity ? 0 : 8 * eps * products;
            offset += normal * (4 * ulp + 0x1p-39 * products + traversal + floor);
        }
        if (!(offset <= altitude / 1024)) {
            return "Bilinear patch has insufficient world-coordinate precision for ray offsets";
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            double absolute_sum = 0;
            for (const auto index : patch) {
                absolute_sum += std::abs(to_vector(mesh.positions[index])[axis]);
            }
            if (absolute_sum > double{std::numeric_limits<float>::max()} / 16) {
                return "Bilinear patch interpolation error exceeds the binary32 rendering range";
            }
        }
    }
    return {};
}

std::string validate_area_geometry(const GeometryMesh &mesh, const GeometryTransform &transform) {
    if (auto message = validate_patch_geometry(mesh, transform); !message.empty()) {
        return message;
    }
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

bool instance_uses_texture_context(const RenderScene &scene, std::size_t index) {
    const auto &material = scene.materials[scene.instance_materials[index]];
    const bool emission =
        !scene.instance_emissions.empty() && scene.instance_emissions[index] != no_area_emission &&
        scene.area_emissions[scene.instance_emissions[index]].image_texture != UINT32_MAX;
    return (material.scattering != ScatteringKind::diffuse &&
            material.scattering != ScatteringKind::null_interface) ||
           material.kind == SpectralMaterialKind::image ||
           material.kind == SpectralMaterialKind::texture_graph ||
           material.bump_root != UINT32_MAX || material.normal_map != UINT32_MAX ||
           (!scene.instance_alpha_roots.empty() &&
            scene.instance_alpha_roots[index] != UINT32_MAX) ||
           emission;
}

std::string validate_render_scene(const RenderScene &scene) {
    if (scene.point_lights.size() > max_positional_lights ||
        scene.spot_lights.size() > max_positional_lights - scene.point_lights.size() ||
        scene.goniometric_lights.size() >
            max_positional_lights - scene.point_lights.size() - scene.spot_lights.size() ||
        scene.projection_lights.size() > max_positional_lights - scene.point_lights.size() -
                                             scene.spot_lights.size() -
                                             scene.goniometric_lights.size())
        return "Positional lights exceed the combined limit of 64";
    if ((!scene.point_lights.empty() || !scene.spot_lights.empty() ||
         !scene.goniometric_lights.empty() || !scene.projection_lights.empty()) &&
        scene.model != RenderModel::pbrt)
        return "Positional lights require the PBRT render model";
    for (const auto &point : scene.point_lights) {
        if (!finite(point.position) || !std::ranges::all_of(point.intensity, [](float value) {
                return std::isfinite(value) && value >= 0;
            }))
            return "Point positions and intensities must be finite, with nonnegative intensity";
    }
    for (const auto &spot : scene.spot_lights) {
        if (!finite(spot.position) || !std::ranges::all_of(spot.intensity, [](float value) {
                return std::isfinite(value) && value >= 0;
            }))
            return "Spot positions and intensities must be finite, with nonnegative intensity";
        if (std::fpclassify(spot.position.x) == FP_SUBNORMAL ||
            std::fpclassify(spot.position.y) == FP_SUBNORMAL ||
            std::fpclassify(spot.position.z) == FP_SUBNORMAL)
            return "Spot positions must remain representable on the GPU";
        if (!std::ranges::all_of(spot.light_from_world,
                                 [](float value) { return std::isfinite(value); }) ||
            singular_linear_transform(spot.light_from_world))
            return "Spot direction transforms must be finite and nonsingular";
        float scale = 0;
        for (float value : spot.light_from_world)
            scale = std::max(scale, std::abs(value));
        std::array<float, 9> packed{};
        for (std::size_t i = 0; i < packed.size(); ++i) {
            packed[i] = spot.light_from_world[i] / scale;
            if (spot.light_from_world[i] != 0 &&
                std::abs(packed[i]) < std::numeric_limits<float>::min())
                return "Spot direction transforms lose coefficients in GPU packing";
        }
        if (singular_linear_transform(packed))
            return "Spot direction transforms become singular in GPU packing";
        if (!std::isfinite(spot.cos_falloff_start) || !std::isfinite(spot.cos_falloff_end) ||
            spot.cos_falloff_end < -1 || spot.cos_falloff_end >= 1 ||
            spot.cos_falloff_start < spot.cos_falloff_end || spot.cos_falloff_start > 1)
            return "Spot cosine thresholds must describe a nonempty ordered cone";
        if (std::fpclassify(spot.cos_falloff_start) == FP_SUBNORMAL ||
            std::fpclassify(spot.cos_falloff_end) == FP_SUBNORMAL ||
            (spot.cos_falloff_start != spot.cos_falloff_end &&
             double(spot.cos_falloff_start) - spot.cos_falloff_end <
                 std::numeric_limits<float>::min()))
            return "Spot cosine thresholds or feather interval are not representable on the GPU";
    }
    std::set<const ImagePyramid *> checked_goniometric;
    for (const auto &light : scene.goniometric_lights) {
        if (!finite(light.position) || std::fpclassify(light.position.x) == FP_SUBNORMAL ||
            std::fpclassify(light.position.y) == FP_SUBNORMAL ||
            std::fpclassify(light.position.z) == FP_SUBNORMAL)
            return "Goniometric positions must be finite and representable on the GPU";
        if (!std::ranges::all_of(light.intensity, [](float value) {
                return std::isfinite(value) && value >= 0 && std::fpclassify(value) != FP_SUBNORMAL;
            }))
            return "Goniometric intensity must be nonnegative and representable on the GPU";
        const auto &m = light.light_from_world;
        if (!std::ranges::all_of(m, [](float value) {
                return std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL;
            }))
            return "Goniometric direction transforms must be finite and representable";
        // The checked inverse of a declaration accepted at 64 epsilon can have
        // twice that Gram error after inversion and narrowing to binary32.
        constexpr double tolerance = 128 * std::numeric_limits<float>::epsilon();
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 3; ++col) {
                double gram = 0;
                for (std::size_t k = 0; k < 3; ++k)
                    gram += double(m[3 * row + k]) * m[3 * col + k];
                if (std::abs(gram - (row == col ? 1 : 0)) > tolerance)
                    return "Goniometric direction transforms must have an orthonormal linear part";
            }
        }
        if (light.image_texture >= scene.image_textures.size() ||
            !scene.image_textures[light.image_texture].image)
            return "Goniometric light requires a valid owned scalar image";
        const auto &image = *scene.image_textures[light.image_texture].image;
        if (checked_goniometric.insert(&image).second) {
            if (const auto error = validate_goniometric_image(image); !error.empty())
                return error;
        }
    }
    std::set<const ImagePyramid *> checked_projection;
    for (const auto &light : scene.projection_lights) {
        const auto representable = [](float value) {
            return std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL;
        };
        if (!representable(light.position.x) || !representable(light.position.y) ||
            !representable(light.position.z))
            return "Projection positions must be finite and representable on the GPU";
        if (!representable(light.image_scale) || light.image_scale < 0)
            return "Projection scale must be nonnegative and representable on the GPU";
        if (!std::ranges::all_of(light.light_from_world, representable) ||
            singular_linear_transform(light.light_from_world))
            return "Projection direction transforms must be finite, representable and nonsingular";
        if (!representable(light.inv_tan_half_fov) || !(light.inv_tan_half_fov > 0))
            return "Projection inverse tangent must be positive and representable on the GPU";
        if (light.image_texture >= scene.image_textures.size() ||
            !scene.image_textures[light.image_texture].image)
            return "Projection light requires a valid owned RGB image";
        const auto &image = *scene.image_textures[light.image_texture].image;
        if (checked_projection.insert(&image).second)
            if (const auto error = validate_projection_image(image); !error.empty())
                return error;
        const auto &level = image.levels.front();
        const float aspect = float(level.width) / float(level.height);
        const float half_width = aspect > 1 ? aspect : 1;
        const float half_height = aspect > 1 ? 1 : 1 / aspect;
        if (light.screen_half_width != half_width || light.screen_half_height != half_height ||
            !representable(2 * light.screen_half_width) ||
            !representable(2 * light.screen_half_height))
            return "Projection screen bounds must match the native image aspect ratio";
    }
    if (scene.distant_lights.size() > max_distant_lights)
        return "Distant lights exceed the limit of 64";
    if (!scene.distant_lights.empty()) {
        if (scene.model != RenderModel::pbrt)
            return "Distant lights require the PBRT render model";
        if (!std::ranges::any_of(scene.geometry.instances, [&](const auto &instance) {
                if (instance.mesh_index >= scene.geometry.meshes.size())
                    return false;
                const auto &mesh = scene.geometry.meshes[instance.mesh_index];
                return !mesh.triangles.empty() || !mesh.patches.empty();
            }))
            return "Distant lights require nonempty scene geometry";
        const double diameter = 2 * double(scene.distant_scene_radius);
        if (!std::isfinite(scene.distant_scene_radius) ||
            scene.distant_scene_radius < std::numeric_limits<float>::min() ||
            diameter > std::numeric_limits<float>::max())
            return "Distant scene radius and diameter must be positive representable binary32 "
                   "values";
        for (const auto &light : scene.distant_lights) {
            const auto d = light.direction;
            const double norm = double(d.x) * d.x + double(d.y) * d.y + double(d.z) * d.z;
            if (!finite(d) || std::abs(norm - 1) > 2e-6 || std::fpclassify(d.x) == FP_SUBNORMAL ||
                std::fpclassify(d.y) == FP_SUBNORMAL || std::fpclassify(d.z) == FP_SUBNORMAL)
                return "Distant light directions must be finite representable unit vectors";
            if (!std::ranges::all_of(
                    light.radiance, [](float value) { return std::isfinite(value) && value >= 0; }))
                return "Distant radiance must be finite and nonnegative";
        }
    }
    const auto valid_medium = [&](std::uint32_t id) {
        return id == no_medium || id < scene.media.size();
    };
    if (!valid_medium(scene.camera_medium))
        return "Camera medium index is out of range";
    const bool active_media = scene.camera_medium != no_medium ||
                              std::ranges::any_of(scene.instance_media, [](const auto &m) {
                                  return m.inside != m.outside;
                              });
    if (active_media && (!scene.volume_transport || scene.model != RenderModel::pbrt))
        return "Participating media require PBRT volpath transport";
    if (scene.media.size() > (128ULL * 1024 * 1024) / (3 * 471 + 1))
        return "Homogeneous media exceed the shared GPU data budget";
    for (const auto &medium : scene.media) {
        if (!std::isfinite(medium.g))
            return "Homogeneous medium asymmetry must be finite";
        for (std::size_t i = 0; i < medium.sigma_a.size(); ++i) {
            const float a = medium.sigma_a[i], s = medium.sigma_s[i], e = medium.emission[i];
            if (!std::isfinite(a) || !std::isfinite(s) || !std::isfinite(e) || a < 0 || s < 0 ||
                e < 0 || !std::isfinite(a + s))
                return "Homogeneous coefficients, extinction and emission must be finite and "
                       "nonnegative";
        }
    }
    if (!scene.instance_media.empty() &&
        scene.instance_media.size() != scene.geometry.instances.size())
        return "Medium interfaces must supply one binding per instance";
    for (const auto &binding : scene.instance_media) {
        if (!valid_medium(binding.inside) || !valid_medium(binding.outside))
            return "Instance medium index is out of range";
    }
    if (!valid_pixel_filter(scene.filter)) {
        return "Invalid pixel filter or radius";
    }
    if (scene.model == RenderModel::demo && scene.filter.kind != PixelFilterKind::box) {
        return "Triangle filtering requires the PBRT render model";
    }
    if (auto error = validate_geometry(scene.geometry); !error.empty()) {
        return error;
    }
    std::size_t patch_corners_remaining = render_patch_corner_limit;
    for (const auto &instance : scene.geometry.instances) {
        const auto &mesh = scene.geometry.meshes[instance.mesh_index];
        if (mesh.patches.size() > patch_corners_remaining / 4) {
            return "Expanded patch validation corner limit exceeded";
        }
        patch_corners_remaining -= mesh.patches.size() * 4;
    }
    for (const auto &instance : scene.geometry.instances) {
        if (auto error = validate_patch_geometry(scene.geometry.meshes[instance.mesh_index],
                                                 instance.transform);
            !error.empty()) {
            return error;
        }
    }
    if (scene.instance_materials.size() != scene.geometry.instances.size()) {
        return "Each render instance requires a material index";
    }
    for (const auto material : scene.instance_materials) {
        if (material >= scene.materials.size()) {
            return "Render instance material index is out of range";
        }
    }
    for (std::size_t i = 0; i < scene.instance_materials.size(); ++i) {
        const auto &bound_material = scene.materials[scene.instance_materials[i]];
        if (bound_material.scattering == ScatteringKind::gltf &&
            !scene.geometry.meshes[scene.geometry.instances[i].mesh_index].patches.empty())
            return "glTF materials require triangle geometry";
        const bool null = scene.materials[scene.instance_materials[i]].scattering ==
                          ScatteringKind::null_interface;
        const bool transition = !scene.instance_media.empty() &&
                                scene.instance_media[i].inside != scene.instance_media[i].outside;
        if (null && !scene.instance_emissions.empty() && i < scene.instance_emissions.size() &&
            scene.instance_emissions[i] != no_area_emission)
            return "Null boundaries cannot emit surface light";
        if ((null || transition) && !scene.instance_alpha_roots.empty() &&
            i < scene.instance_alpha_roots.size() && scene.instance_alpha_roots[i] != UINT32_MAX)
            return "Medium boundaries require opaque alpha";
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
        emitter_count += mesh.triangles.size() + mesh.patches.size();
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
                emission.gltf_material == UINT32_MAX && !error.empty()) {
                return "Instance " + std::to_string(i) + ": " + error;
            }
        }
    }
    std::vector<unsigned char> material_state(scene.materials.size());
    std::vector<unsigned> material_depth(scene.materials.size());
    std::vector<std::pair<std::uint32_t, bool>> material_stack;
    for (std::size_t root = 0; root < scene.materials.size(); ++root) {
        if (material_state[root] == 2)
            continue;
        material_stack.emplace_back(static_cast<std::uint32_t>(root), false);
        while (!material_stack.empty()) {
            auto [index, exiting] = material_stack.back();
            material_stack.pop_back();
            if (index >= scene.materials.size())
                return "Mix material references an invalid child";
            if (exiting) {
                const auto &m = scene.materials[index];
                if (m.scattering == ScatteringKind::mix) {
                    material_depth[index] = 1 + std::max(material_depth[m.surface.first_material],
                                                         material_depth[m.surface.second_material]);
                    if (material_depth[index] > 64)
                        return "Mix material nesting exceeds 64 levels";
                }
                material_state[index] = 2;
                continue;
            }
            if (material_state[index] == 2)
                continue;
            if (material_state[index] == 1)
                return "Cyclic mix materials are unsupported";
            material_state[index] = 1;
            material_stack.emplace_back(index, true);
            const auto &m = scene.materials[index];
            if (m.scattering == ScatteringKind::mix) {
                if (m.surface.first_material < scene.materials.size() &&
                    m.surface.second_material < scene.materials.size() &&
                    (scene.materials[m.surface.first_material].scattering ==
                         ScatteringKind::null_interface ||
                     scene.materials[m.surface.second_material].scattering ==
                         ScatteringKind::null_interface))
                    return "Mix materials cannot contain null boundaries";
                material_stack.emplace_back(m.surface.second_material, false);
                material_stack.emplace_back(m.surface.first_material, false);
            }
        }
    }
    if (auto error = validate_image_textures(scene); !error.empty()) {
        return error;
    }
    if (scene.perspective_lens) {
        if (scene.model != RenderModel::pbrt || !scene.raster_camera) {
            return "Perspective lens requires a PBRT raster camera";
        }
        if (const auto error = validate_lens_support(*scene.raster_camera, *scene.perspective_lens,
                                                     PixelFilter{}, 1, 1);
            !error.empty()) {
            return error;
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
        std::set<const BsdfTable *> validated_tables;
        std::size_t table_words = 0;
        for (const auto &material : scene.materials) {
            if (material.table && validated_tables.insert(material.table.get()).second) {
                constexpr std::size_t word_budget = 128ULL * 1024 * 1024;
                if (material.table->words.size() > word_budget - table_words)
                    return "Material table storage exceeds the aggregate budget";
                table_words += material.table->words.size();
                if (auto error = validate_bsdf_table(*material.table); !error.empty())
                    return error;
            }
            if (auto error = validate_pbrt_material(material); !error.empty()) {
                return error;
            }
        }
        return {};
    }
    for (const auto &material : scene.materials) {
        if (material.scattering != ScatteringKind::diffuse ||
            material.kind != SpectralMaterialKind::sampled || material.constant != 0 ||
            material.coefficients != std::array<float, 3>{} || material.temperature_kelvin != 0) {
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

double sampled_spectrum(std::span<const SpectralKnot> knots, double wavelength) {
    if (knots.empty() ||
        !(wavelength >= knots.front().wavelength && wavelength <= knots.back().wavelength)) {
        return 0;
    }
    const auto next = std::ranges::lower_bound(knots, wavelength, {}, &SpectralKnot::wavelength);
    if (next == knots.begin()) {
        return next->value;
    }
    const auto &previous = *(next - 1);
    const double left_distance = wavelength - previous.wavelength;
    const double right_distance = next->wavelength - wavelength;
    const double width = double{next->wavelength} - previous.wavelength;
    // Anchor at the nearer endpoint so a tiny contribution is not lost when
    // the other interpolation fraction rounds to one across a wide interval.
    if (right_distance < left_distance) {
        return std::lerp(double{next->value}, double{previous.value}, right_distance / width);
    }
    return std::lerp(double{previous.value}, double{next->value}, left_distance / width);
}

double blackbody_reflectance(float temperature_kelvin, double wavelength) {
    if (!(wavelength >= wavelength_min && wavelength <= wavelength_max)) {
        return 0;
    }
    if (!(std::isfinite(temperature_kelvin) && temperature_kelvin > 0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double exponent = planck_exponent_nm_kelvin / (wavelength * temperature_kelvin);
    // Normalize to the Wien peak and combine the factors before exponentiating.
    static const double log_peak_factor = std::log(-std::expm1(-wien_exponent));
    const double log_ratio = 5 * std::log(exponent / wien_exponent) + (wien_exponent - exponent) +
                             log_peak_factor - std::log(-std::expm1(-exponent));
    return std::exp(std::min(log_ratio, 0.));
}

float blackbody_exponent_factor(float temperature_kelvin) {
    // At exponent 128 and above, the ratio rounds to zero in binary32.
    // Cap the packed factor so even the smallest positive Kelvin fits.
    return static_cast<float>(
        std::min(planck_exponent_nm_kelvin / temperature_kelvin, 128 * wavelength_max));
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
    case SpectralMaterialKind::blackbody:
        return blackbody_reflectance(material.temperature_kelvin, wavelength);
    default:
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (!material.knots.empty()) {
        return sampled_spectrum(material.knots, wavelength);
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
