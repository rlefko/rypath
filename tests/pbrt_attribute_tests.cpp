// SPDX-License-Identifier: GPL-3.0-only

#include "attribute_reference.hpp"
#include "metal_render.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace rypath;
using namespace attribute_reference;

constexpr std::uint32_t paths = 32768;
constexpr std::uint64_t seed = 0x7eab129ff367c4d2ULL;
constexpr std::uint64_t affine_seed = 0x15adb51978ed3909ULL;
constexpr SpectralMaterial black{.kind = SpectralMaterialKind::constant};
constexpr SpectralMaterial diffuse{.kind = SpectralMaterialKind::constant, .constant = reflectance};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

GeometryMesh quad(float z, GeometryPoint normal) {
    GeometryMesh mesh{
        .positions = {{-1, -1, z}, {-1, 1, z}, {1, 1, z}, {1, -1, z}},
        .triangles = {{0, 1, 2}, {0, 2, 3}},
    };
    mesh.normals.assign(mesh.positions.size(), normal);
    mesh.uvs = {{{0, 0}}, {{0, 1}}, {{1, 1}}, {{1, 0}}};
    return mesh;
}

void add_mesh(RenderScene &scene, GeometryMesh mesh, std::uint32_t material,
              std::uint32_t emission = no_area_emission) {
    const auto mesh_index = static_cast<std::uint32_t>(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({.mesh_index = mesh_index, .transform = {}});
    scene.instance_materials.push_back(material);
    scene.instance_emissions.push_back(emission);
}

RenderScene scene_base() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{
        {0, 0, -2}, {-footprint, -footprint, 2}, {2 * footprint, 0, 0}, {0, 2 * footprint, 0}};
    scene.materials = {black, diffuse};
    scene.max_depth = 1;
    return scene;
}

AreaEmission constant_emission(float value) {
    AreaEmission emission;
    emission.radiance.fill(value);
    return emission;
}

RenderScene directional_scene(bool affine) {
    auto scene = scene_base();
    if (affine) {
        GeometryMesh receiver{
            .positions = {{-2, -1, 0}, {-2, 3, 0}, {2, -1, 0}},
            .triangles = {{0, 1, 2}},
        };
        receiver.normals = {{0, 0, -1}, {16, 0, -4}, {-2, 1, -2}};
        receiver.uvs = {{{0, 0}}, {{0, 1}}, {{1, 0}}};
        add_mesh(scene, std::move(receiver), 1);
        scene.geometry.instances.front().transform.values = {2, 0.5F, 0, 0, 0, 1, 0, 0, 0, 0, 3, 0};
    } else {
        add_mesh(scene, quad(0, {3, 0, -1}), 1);
    }
    // Emitter directions reach the shading hemisphere through the back
    // of the receiver's oriented geometric plane. The emitter normals
    // oppose its winding, so both light sampling and BSDF hits must use them.
    scene.area_emissions = {constant_emission(1)};
    add_mesh(scene,
             {.positions = {{light_xmin, light_ymin, light_z},
                            {light_xmin, light_ymax, light_z},
                            {light_xmax, light_ymax, light_z}},
              .triangles = {{0, 2, 1}},
              .normals = {{0, 0, -1}, {1, 0, -1}, {0, 1, -2}}},
             0, 0);
    return scene;
}

std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    return std::move(built.scene);
}

RenderRequest request(std::uint32_t samples, std::uint64_t sample_seed = seed) {
    return {
        .width = 1, .height = 1, .seed = sample_seed, .pixel_count = 1, .sample_count = samples};
}

std::vector<RenderSample> sample(MetalRenderScene &renderer, const RenderRequest &query) {
    auto result = renderer.sample(query);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == query.sample_count && result.guard_count > 0,
            "Expected a complete guarded shading-normal dispatch");
    for (std::size_t i = 0; i < result.samples.size(); ++i) {
        require(result.samples[i].sample == query.sample_begin + i && result.samples[i].pixel == 0,
                "Shading-normal dispatch changed absolute sample indices");
    }
    return std::move(result.samples);
}

Moments quadrature(bool affine) {
    const auto coarse = area_moments(affine, {.raster = 16, .emitter = 192});
    const auto fine = area_moments(affine, {.raster = 24, .emitter = 384});
    require(std::abs(fine.mean - coarse.mean) < 2e-6 &&
                std::abs(fine.second - coarse.second) < 2e-6 && fine.bsdf_mean > 0.005,
            "Directional quadrature did not converge or exercise significant BSDF-hit emission");
    return fine;
}

void check_mean(std::string_view label, const std::vector<RenderSample> &samples, Moments moments) {
    double observed = 0;
    for (const auto &s : samples) {
        require(s.scatterings == 1 && s.radiance >= 0,
                "Directional rays must scatter once at the diffuse receiver");
        observed += s.radiance;
    }
    const double count = static_cast<double>(samples.size());
    observed /= count;
    const double standard_error = std::sqrt((moments.second - moments.mean * moments.mean) / count);
    const double limit = 6 * standard_error + 2e-5 * std::max(1.0, std::abs(moments.mean));
    std::cout << std::setprecision(10) << label << " expected=" << moments.mean
              << " observed=" << observed << " analytic_se=" << standard_error << " limit=" << limit
              << " expected_bsdf_hit=" << moments.bsdf_mean << '\n';
    require(std::abs(observed - moments.mean) <= limit, "Shading-normal radiance mismatch");
}

RasterCamera &raster_camera(RenderScene &scene) {
    if (!scene.raster_camera) {
        throw std::runtime_error("Shading-normal test requires a raster camera");
    }
    return *scene.raster_camera;
}

void furnace() {
    auto scene = scene_base();
    scene.environment.fill(1);
    add_mesh(scene, quad(0, {3, 0, -1}), 1);
    auto &camera = raster_camera(scene);
    for (float side : {-1.0F, 1.0F}) {
        camera.origin.z = side * 2;
        camera.corner.z = -side * 2;
        const auto samples = sample(*build(scene), request(paths));
        check_mean(side < 0 ? "Furnace front" : "Furnace back", samples, furnace_moments());
    }
}

void fallback_and_tangent() {
    auto scene = scene_base();
    scene.environment.fill(1);
    add_mesh(scene, quad(0, {}), 1);
    auto &mesh = scene.geometry.meshes.front();
    mesh.normals.clear();
    mesh.uvs.clear();
    const auto query = request(1024);
    const auto geometric = sample(*build(scene), query);
    mesh.normals.resize(mesh.positions.size());
    mesh.uvs = {{{-7, 11}}, {{3, -8}}, {{0, 25}}, {{9, 6}}};
    const auto fallback = sample(*build(scene), query);
    for (std::size_t i = 0; i < fallback.size(); ++i) {
        require(fallback[i].radiance == geometric[i].radiance && fallback[i].scatterings == 1 &&
                    geometric[i].scatterings == 1,
                "Zero shading normals or unused UVs changed the geometric furnace");
    }

    // Cancel the first raster sample's X direction exactly using its known
    // dyadic jitter. The camera remains a nondegenerate pinhole camera.
    const auto words = sample_words(seed, 0, 0, 0);
    auto &camera = raster_camera(scene);
    camera.corner.x = -camera.dx.x * sample_uniform(words[0]);
    mesh.normals.assign(mesh.positions.size(), {1, 0, 0});
    auto tangent_query = request(1);
    tangent_query.emergency_limit = 1;
    const auto tangent = sample(*build(scene), tangent_query).front();
    require(tangent.scatterings == 0 && tangent.radiance == 0,
            "An exactly tangent shading normal scattered or escaped to the environment");
}

void directional(MetalRenderScene &native) {
    check_mean("Across plane", sample(native, request(paths)), quadrature(false));
    auto blocked = directional_scene(false);
    // Close to the receiver, cover every connection to the light. Primary
    // rays still meet the receiver first. Both MIS techniques must be black.
    add_mesh(blocked, quad(0x1p-15F, {0, 0, -1}), 0);
    const auto shadowed = sample(*build(blocked), request(2048));
    require(std::ranges::all_of(
                shadowed, [](const auto &s) { return s.radiance == 0 && s.scatterings == 1; }),
            "The nearby blocker leaked direct or BSDF-hit emission");
}

void primary_emission() {
    auto scene = scene_base();
    scene.max_depth = 0;
    scene.environment.fill(3);
    scene.area_emissions = {constant_emission(2)};
    add_mesh(scene, quad(0, {0, 0, -1}), 0, 0);
    const auto verify = [&](GeometryPoint normal, bool mirror, bool reverse, bool two_sided,
                            float expected) {
        auto &mesh = scene.geometry.meshes.front();
        mesh.normals.assign(mesh.positions.size(), normal);
        scene.geometry.instances.front().transform.values[0] = mirror ? -1 : 1;
        scene.instance_reverse_orientation = {mirror != reverse ? 1U : 0U};
        scene.area_emissions.front().two_sided = two_sided;
        const auto samples = sample(*build(scene), request(37));
        require(std::ranges::all_of(
                    samples,
                    [&](const auto &s) { return s.scatterings == 0 && s.radiance == expected; }),
                "Depth-zero attributed emitter returned incorrect sidedness or opacity");
    };
    verify({1, 0, -1}, false, false, false, 2);
    verify({1, 0, -1}, false, true, false, 0);
    verify({1, 0, -1}, true, false, false, 2);
    verify({1, 0, -1}, true, true, false, 0);
    verify({1, 0, 1}, false, false, false, 0);
    verify({1, 0, 1}, false, true, false, 2);
    verify({}, false, false, false, 2);
    verify({1, 0, 0}, false, false, false, 2);
    verify({}, false, true, true, 2);
    verify({1, 0, 0}, true, false, true, 2);
    verify({1, 0, 1}, false, false, true, 2);
}

void mixed_sign_emission() {
    auto scene = scene_base();
    scene.max_depth = 0;
    scene.environment.fill(3);
    scene.area_emissions = {constant_emission(2)};
    add_mesh(scene, {.positions = {{-1, -1, 0}, {-1, 3, 0}, {3, -1, 0}}, .triangles = {{0, 1, 2}}},
             0, 0);

    // Sample zero travels along +Z exactly. Translating the pinhole along X
    // moves the hit through the zero of the normal field without changing
    // direction. At the origin, the barycentrics are (1/2, 1/4, 1/4).
    const auto words = sample_words(seed, 0, 0, 0);
    auto &camera = raster_camera(scene);
    camera.corner.x = -camera.dx.x * sample_uniform(words[0]);
    camera.corner.y = -camera.dy.y * sample_uniform(words[1]);
    for (float sign : {-1.0F, 1.0F}) {
        scene.geometry.meshes.front().normals = {{0, 0, sign}, {0, 0, -sign}, {0, 0, -sign}};
        for (float x : {-0.25F, 0.0F, 0.25F}) {
            camera.origin.x = x;
            const float expected = x * sign > 0 ? 2 : 0;
            const auto hit = sample(*build(scene), request(1)).front();
            // The cancellation point falls back to the geometric -Z normal
            // for both sign assignments. A residual would flip one result.
            require(hit.scatterings == 0 && hit.radiance == (x == 0 ? 2 : expected),
                    "Mixed-sign normal interpolation changed emission sidedness or fallback");
        }
    }
}

void batching(MetalRenderScene &native) {
    constexpr std::uint32_t count = 67;
    auto whole_query = request(count, affine_seed);
    whole_query.threadgroup_width = 32;
    const auto whole = sample(native, whole_query);
    require(std::ranges::any_of(whole, [](const auto &s) { return s.radiance > 0; }),
            "Attributed batch had no directional signal");
    for (std::uint32_t begin = 0; begin < count; begin += 7) {
        auto part_query = request(std::min(7U, count - begin), affine_seed);
        part_query.threadgroup_width = 64;
        part_query.sample_begin = begin;
        const auto part = sample(native, part_query);
        for (std::size_t i = 0; i < part.size(); ++i) {
            const auto &a = whole[begin + i];
            const auto &b = part[i];
            require(a.wavelength == b.wavelength && a.radiance == b.radiance &&
                        a.scatterings == b.scatterings && a.random_words == b.random_words,
                    "Attributed sample changed with batch partition or threadgroup width");
        }
    }
}

void cornell_inventory(const std::filesystem::path &directory) {
    const auto path = directory / "cornell-box-attributes.pbrt";
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0 && checked.document.dependencies_complete &&
                checked.document.dependencies.empty() && checked.inventory.meshes == 8 &&
                checked.inventory.triangles == 36,
            pbrt::report(checked));
    const auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
    if (translated.exit_code() != 0 || !translated.scene) {
        throw std::runtime_error("Attributed Cornell fixture failed production translation");
    }
    const auto &scene = *translated.scene;
    const auto &settings = translated.settings;
    require(settings.width == 8 && settings.height == 8 && settings.samples == 4096 &&
                settings.seed == 0 && scene.max_depth == 65 && scene.geometry.meshes.size() == 8 &&
                scene.geometry.instances.size() == 8 && scene.area_emissions.size() == 1 &&
                scene.filter.kind == PixelFilterKind::triangle && scene.filter.x_radius == 1 &&
                scene.filter.y_radius == 1,
            "Unexpected attributed Cornell render settings or shape inventory");
    std::size_t vertices = 0;
    for (const auto &mesh : scene.geometry.meshes) {
        require(mesh.positions.size() == mesh.normals.size() &&
                    mesh.positions.size() == mesh.uvs.size(),
                "Attributed Cornell translation lost normal or UV arrays");
        vertices += mesh.positions.size();
    }
    require(vertices == 72, "Unexpected attributed Cornell vertex count");
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 2, "Expected --reference or --metal");
        if (std::string_view{argv[1]} == "--reference") {
            require(argc == 3, "Expected the PBRT fixture directory");
            cornell_inventory(argv[2]);
            return 0;
        }
        require(std::string_view{argv[1]} == "--metal", "Unknown shading-normal test mode");
        bool allow_no_device = false;
        bool allow_no_ray_tracing = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view option{argv[i]};
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown shading-normal Metal test option");
            allow_no_device |= option == "--allow-no-device";
            allow_no_ray_tracing |= option == "--allow-no-ray-tracing";
        }

        // Destroy caller-owned geometry and attributes before dispatch.
        auto native = build_metal_renderer(directional_scene(false));
        if ((native.status == GeometryStatus::no_device && allow_no_device) ||
            (native.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
            std::cout << "SKIP Metal shading-normal render. " << native.diagnostic << '\n';
            return 77;
        }
        require(native.status == GeometryStatus::success && native.scene, native.diagnostic);
        furnace();
        fallback_and_tangent();
        directional(*native.scene);
        const auto affine = build(directional_scene(true));
        check_mean("Affine smooth receiver", sample(*affine, request(paths, affine_seed)),
                   quadrature(true));
        batching(*affine);
        primary_emission();
        mixed_sign_emission();
    } catch (const std::exception &error) {
        std::cerr << "Shading-normal test failure. " << error.what() << '\n';
        return 1;
    }
}
