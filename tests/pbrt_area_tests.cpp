// SPDX-License-Identifier: GPL-3.0-only

#include "area_reference.hpp"
#include "metal_render.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace rypath;
constexpr std::uint32_t paths = 32768;
constexpr std::uint64_t seed = 0x7589bda9aa812393ULL;
constexpr SpectralMaterial black{.kind = SpectralMaterialKind::constant, .constant = 0};
constexpr SpectralMaterial white{.kind = SpectralMaterialKind::constant, .constant = 1};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

GeometryMesh quad(float xmin, float xmax, float ymin, float ymax, float z) {
    return {
        .positions = {{xmin, ymin, z}, {xmin, ymax, z}, {xmax, ymax, z}, {xmax, ymin, z}},
        .triangles = {{0, 1, 2}, {0, 2, 3}},
    };
}

void add_mesh(RenderScene &scene, GeometryMesh mesh, std::uint32_t material,
              std::uint32_t emission = no_area_emission) {
    const auto mesh_index = static_cast<std::uint32_t>(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({.mesh_index = mesh_index, .transform = {}});
    scene.instance_materials.push_back(material);
    scene.instance_emissions.push_back(emission);
}

AreaEmission constant_emission(float radiance) {
    AreaEmission emission;
    emission.radiance.fill(radiance);
    return emission;
}

RenderScene scene_base() {
    constexpr float footprint = 0.125F;
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{
        {0, 0, -2}, {-footprint, -footprint, 2}, {2 * footprint, 0, 0}, {0, 2 * footprint, 0}};
    scene.materials = {black, white};
    scene.max_depth = 1;
    return scene;
}

GeometryMesh off_axis_triangle() {
    return {
        .positions = {{0.25F, -0.5F, -1}, {1.5F, -0.5F, -1}, {0.25F, 0.5F, -1}},
        .triangles = {{0, 1, 2}},
    };
}

RenderScene oracle_scene() {
    auto scene = scene_base();
    scene.materials.push_back({});
    for (std::size_t i = 0; i < area_reference::wavelengths.size(); ++i) {
        scene.materials.back().knots.push_back(
            {area_reference::wavelengths[i], area_reference::reflectance[i]});
    }
    AreaEmission light;
    for (std::size_t i = 0; i < light.radiance.size(); ++i) {
        light.radiance[i] = static_cast<float>(area_reference::emission(360 + static_cast<int>(i)));
    }
    scene.area_emissions.push_back(light);
    add_mesh(scene, quad(-0.125F, 0.125F, -0.125F, 0.125F, 0), 2);
    add_mesh(scene, off_axis_triangle(), 0, 0);
    return scene;
}

void area_geometry_preflight() {
    auto scene = oracle_scene();
    require(validate_render_scene(scene).empty(), "Valid world-space emitter was rejected");
    // The exact object-space cross product is nonzero. Subtracting in
    // binary32 rounds the second edge to (-1,-1,0) and collapses the area.
    auto &mesh = scene.geometry.meshes.back();
    mesh.positions = {{1, 0, 0}, {2, 1, 0}, {1e-8F, -1, 0}};
    require(validate_geometry(scene.geometry).empty() && !validate_render_scene(scene).empty(),
            "Area preflight accepted a triangle collapsed by shader edge rounding");
    mesh.positions = {{0, 0, 2}, {4, 4, 2}, {4, 4, 3}};
    scene.geometry.instances.back().transform.values = {1e38F, -1e38F, 0, 0, 1, 0,
                                                        0,     0,      0, 0, 1, 0};
    require(validate_geometry(scene.geometry).empty() && !validate_render_scene(scene).empty(),
            "Area preflight accepted cancelling overflowing transform intermediates");
}

std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    return std::move(built.scene);
}

RenderRequest request(std::uint32_t samples) {
    return {.width = 1, .height = 1, .seed = seed, .pixel_count = 1, .sample_count = samples};
}

std::vector<RenderSample> sample(MetalRenderScene &renderer, const RenderRequest &query) {
    auto result = renderer.sample(query);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == query.sample_count && result.guard_count > 0,
            "Expected a complete guarded area-light dispatch");
    for (std::size_t i = 0; i < result.samples.size(); ++i) {
        require(result.samples[i].sample == query.sample_begin + i && result.samples[i].pixel == 0,
                "Area-light dispatch changed absolute sample indices");
    }
    return std::move(result.samples);
}

area_reference::AngularMoments angular_oracle() {
    const auto angular = area_reference::angular_moments();
    std::cout << std::setprecision(15) << "Area angular mean=" << angular.mean
              << " Duffy mean=" << angular.duffy_mean << " variance=" << angular.variance << '\n';
    require(std::abs(angular.mean - angular.duffy_mean) <= 1e-9,
            "Duffy and spherical area quadrature disagree on the directional mean");
    require(std::abs(angular.mean - 0.0930025220483664) <= 1e-9 &&
                std::abs(angular.variance - 0.00170612894641743) <= 1e-9,
            "Area quadrature did not reproduce the predeclared directional moments");
    return angular;
}

void cornell_inventory(const std::filesystem::path &fixture_directory) {
    const auto path = fixture_directory / "cornell-box-adapted.pbrt";
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0 && checked.document.dependencies_complete &&
                checked.document.dependencies.empty() && checked.inventory.named_materials == 8 &&
                checked.inventory.meshes == 8 && checked.inventory.triangles == 36 &&
                checked.inventory.instances == 0,
            pbrt::report(checked));
    const auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
    if (translated.exit_code() != 0 || !translated.scene) {
        throw std::runtime_error("Adapted Cornell fixture failed production translation");
    }
    const auto &scene = *translated.scene;
    const auto &settings = translated.settings;
    require(scene.max_depth == 65 && scene.exposure == 1 && settings.width == 8 &&
                settings.height == 8 && settings.samples == 4096 && settings.seed == 0 &&
                scene.geometry.instances.size() == 8 && scene.area_emissions.size() == 1 &&
                scene.instance_emissions.size() == 8 && scene.instance_emissions.back() == 0 &&
                std::ranges::all_of(scene.instance_emissions.begin(),
                                    scene.instance_emissions.end() - 1,
                                    [](auto e) { return e == no_area_emission; }) &&
                !scene.area_emissions.front().two_sided,
            "Unexpected adapted Cornell camera, film, material, or area-light inventory");
    std::size_t vertices = 0;
    for (const auto &mesh : scene.geometry.meshes) {
        vertices += mesh.positions.size();
    }
    const auto &emitter = scene.geometry.meshes[scene.geometry.instances.back().mesh_index];
    require(vertices == 72 && emitter.positions.size() == 4 && emitter.triangles.size() == 2,
            "Unexpected adapted Cornell vertex or emissive triangle count");
}

void analytic(MetalRenderScene &renderer) {
    const auto moments = area_reference::xyz_moments(angular_oracle());
    const auto samples = sample(renderer, request(paths));
    require(std::ranges::all_of(samples, [](const auto &s) { return s.scatterings == 1; }),
            "Analytic rays must scatter once at the receiver");
    auto film = make_film(1, 1, RenderModel::pbrt);
    FilmBatch batch(film, paths);
    require(batch.add(samples).empty() && batch.commit(film).empty(),
            "Analytic area samples failed to commit");
    const auto observed = film_mean(film.pixels.front());
    for (std::size_t c = 0; c < observed.size(); ++c) {
        const double expected = moments.mean[c];
        const double variance = moments.second[c] - expected * expected;
        const double standard_error = std::sqrt(variance / paths);
        const double limit = 6 * standard_error + 2e-5 * std::max(1.0, std::abs(expected));
        std::cout << "Area XYZ[" << c << "] expected=" << expected << " observed=" << observed[c]
                  << " analytic_se=" << standard_error << " limit=" << limit << '\n';
        require(std::abs(observed[c] - expected) <= limit, "Analytic area-light XYZ mismatch");
    }
}

void primary_emission() {
    auto scene = scene_base();
    scene.max_depth = 0;
    scene.environment.fill(3);
    scene.area_emissions = {constant_emission(2)};
    add_mesh(scene, quad(-1, 1, -1, 1, 0), 0, 0);
    const auto verify = [&](float expected) {
        const auto samples = sample(*build(scene), request(37));
        require(std::ranges::all_of(
                    samples,
                    [&](const auto &s) { return s.scatterings == 0 && s.radiance == expected; }),
                "Depth-zero emitter returned incorrect sidedness, winding, or opacity");
    };
    verify(2);
    scene.geometry.instances.front().transform.values[0] = -1;
    verify(0);
    // The translator combines authored ReverseOrientation with handedness.
    scene.instance_reverse_orientation = {1};
    verify(2);
    scene.instance_reverse_orientation = {0};
    scene.area_emissions.front().two_sided = true;
    verify(2);
}

void null_slots_and_last_hit() {
    auto scene = scene_base();
    add_mesh(scene, quad(-1, 1, -1, 1, 0), 1);
    // Interleave an empty native instance before two lights to exercise original-to-native
    // remapping.
    add_mesh(scene, {}, 0);
    for (const auto radiance : {1.0F, 2.0F, 3.0F}) {
        const auto index = static_cast<std::uint32_t>(scene.area_emissions.size());
        auto emitter = off_axis_triangle();
        for (auto &p : emitter.positions) {
            p.y += 1.25F * static_cast<float>(index);
        }
        scene.area_emissions.push_back(constant_emission(radiance));
        add_mesh(scene, std::move(emitter), 0, index);
    }
    const auto samples = sample(*build(scene), request(paths));
    std::array<std::size_t, 4> away_slots{};
    std::size_t null_slot_hits = 0;
    for (const auto &s : samples) {
        require(s.scatterings == 1, "Expected the receiver to scatter once");
        const auto words = sample_words(seed, 0, s.sample, 1);
        const auto slot = words[3] & 3U;
        const auto azimuth = sample_uniform(words[1]);
        // For a -Z normal, this azimuth interval travels towards -X, away
        // from all emitters at positive X. Only NEE can contribute.
        if (azimuth < 0.2F || azimuth > 0.8F) {
            require(slot == 3 ? s.radiance == 0 : s.radiance > 0,
                    "The three emitters must use three live slots and one null slot");
            ++away_slots[slot];
        }
        if (slot == 3 && s.radiance > 0) {
            ++null_slot_hits;
        }
    }
    require(std::ranges::all_of(away_slots, [](std::size_t count) { return count > 100; }) &&
                null_slot_hits > 100,
            "Missing null-slot BSDF hits at authored maxdepth");
}

RenderScene shadow_scene(float scale) {
    const float extent = scale / 1024;
    auto scene = scene_base();
    scene.raster_camera = RasterCamera{
        {0, 0, -2 * scale}, {-extent, -extent, 2 * scale}, {2 * extent, 0, 0}, {0, 2 * extent, 0}};
    scene.area_emissions = {constant_emission(1)};
    add_mesh(scene, quad(-scale, scale, -scale, scale, 0), 1);
    GeometryMesh light{
        .positions = {{0.4375F * scale, -0.0625F * scale, -scale},
                      {0.5625F * scale, -0.0625F * scale, -scale},
                      {0.4375F * scale, 0.0625F * scale, -scale}},
        .triangles = {{0, 1, 2}},
    };
    add_mesh(scene, std::move(light), 0, 0);
    return scene;
}

void finite_segments(float scale, bool anisotropic_parameterization = false,
                     bool shared_mesh = false) {
    std::cout << "Area visibility scale=" << scale
              << " anisotropic_parameterization=" << anisotropic_parameterization
              << " shared_mesh=" << shared_mesh << std::endl;
    auto scene = shadow_scene(scale);
    if (anisotropic_parameterization) {
        // Preserve the world-space emitter while giving its object-space
        // bounds a large extent. Sampled endpoints have no traversal error.
        for (auto &p : scene.geometry.meshes.back().positions) {
            p.y *= 1e6F;
        }
        scene.geometry.instances.back().transform.values[5] = 1e-6F;
    }
    if (shared_mesh) {
        auto other = scene.geometry.instances.back();
        other.transform.values[11] = 1024;
        scene.geometry.instances.push_back(other);
        scene.instance_materials.push_back(0);
        scene.instance_emissions.push_back(no_area_emission);
    }
    const auto baseline = sample(*build(scene), request(1024));
    require(std::ranges::all_of(baseline, [](const auto &s) { return s.radiance > 0; }),
            "Source or emitter incorrectly blocked the open segment");

    const auto verify = [&](float z, float xmin, float xmax, bool visible) {
        auto with_blocker = scene;
        add_mesh(with_blocker, quad(xmin * scale, xmax * scale, -scale, scale, z * scale), 0);
        const auto samples = sample(*build(with_blocker), request(1024));
        for (std::size_t i = 0; i < samples.size(); ++i) {
            require(samples[i].scatterings == 1,
                    "Shadow blocker crossed the primary camera footprint");
            require(samples[i].radiance == (visible ? baseline[i].radiance : 0),
                    "Area shadow segment mishandled a source, emitter, or beyond-end blocker");
        }
    };
    verify(-0.015625F, 0.005F, 0.011F, false);
    verify(-0.984375F, 0.42F, 0.57F, false);
    // This gap exceeds the endpoint error allowance but is less than a
    // fixed 1e-4 segment clip, including at the long-segment scale.
    verify(-(1.0F - 0x1p-16F), 0.42F, 0.57F, false);
    verify(-1.015625F, 0.43F, 0.59F, true);
}

void batches_and_failure() {
    auto scene = oracle_scene();
    auto settings =
        RenderSettings{.width = 2, .height = 3, .samples = 259, .batch_size = 1, .seed = seed};
    const auto one = render_progressive(scene, settings);
    settings.batch_size = 257;
    const auto many = render_progressive(scene, settings);
    require(one.status == GeometryStatus::success && many.status == GeometryStatus::success &&
                one.film == many.film &&
                std::ranges::all_of(one.film.pixels,
                                    [&](const auto &p) { return p.count == settings.samples; }),
            "Area-light raw film changed between batch sizes 1 and 257");

    scene.max_depth = 2;
    scene.instance_materials.back() = 1;
    auto native = build(scene);
    auto film = make_film(1, 1, RenderModel::pbrt);
    const auto first = sample(*native, request(1));
    {
        FilmBatch commit(film, 1);
        require(commit.add(first).empty() && commit.commit(film).empty(),
                "Could not establish the previously committed area-light film");
    }
    const auto saved = film;
    auto query = request(1);
    query.sample_begin = 1;
    const auto prefix = sample(*native, query);
    query = request(1024);
    query.sample_begin = 2;
    {
        FilmBatch uncommitted(film, 1 + query.sample_count);
        require(uncommitted.add(prefix).empty(), "Could not stage first area-light tile");
        query.emergency_limit = 1;
        const auto failed = native->sample(query);
        require(failed.status == GeometryStatus::failure && failed.samples.empty() &&
                    failed.diagnostic.find("emergency") != std::string::npos,
                "An emitter continuation failed to reject the entire native dispatch");
        require(!uncommitted.commit(film).empty() && film == saved,
                "A failed area-light dispatch altered the previously committed film");
    }
    query.emergency_limit = render_emergency_limit;
    const auto recovered = sample(*native, query);
    FilmBatch retry(film, 1 + query.sample_count);
    require(retry.add(prefix).empty() && retry.add(recovered).empty() &&
                retry.commit(film).empty() && film.pixels.front().count == 2 + query.sample_count,
            "The same native scene could not retry a failed area-light dispatch");
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 2, "Expected --reference or Metal availability options");
        if (std::string_view{argv[1]} == "--reference") {
            require(argc == 3, "Expected the PBRT fixture directory");
            static_cast<void>(angular_oracle());
            area_geometry_preflight();
            cornell_inventory(argv[2]);
            return 0;
        }
        require(std::string_view{argv[1]} == "--metal", "Unknown area-light test mode");
        bool allow_no_device = false;
        bool allow_no_ray_tracing = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view option{argv[i]};
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown area-light Metal test option");
            allow_no_device |= option == "--allow-no-device";
            allow_no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        RenderBuildResult native;
        {
            // Destroy caller-owned geometry, materials and emission before sampling.
            const auto scene = oracle_scene();
            native = build_metal_renderer(scene);
        }
        if ((native.status == GeometryStatus::no_device && allow_no_device) ||
            (native.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
            std::cout << "SKIP Metal area-light render. " << native.diagnostic << '\n';
            return 77;
        }
        require(native.status == GeometryStatus::success && native.scene, native.diagnostic);
        analytic(*native.scene);
        primary_emission();
        null_slots_and_last_hit();
        finite_segments(1);
        finite_segments(65536);
        finite_segments(1, true);
        finite_segments(1, true, true);
        batches_and_failure();
    } catch (const std::exception &error) {
        std::cerr << "Area-light test failure. " << error.what() << '\n';
        return 1;
    }
}
