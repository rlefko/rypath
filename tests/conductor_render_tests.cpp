// SPDX-License-Identifier: GPL-3.0-only
#include "metal_render.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool ok, std::string_view message) {
    if (!ok)
        throw std::runtime_error(std::string(message));
}
RenderScene load(const fs::path &path) {
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
    for (const auto &issue : translated.diagnostics)
        std::cerr << pbrt::format_diagnostic(issue);
    if (!translated.scene)
        throw std::runtime_error("Conductor fixture did not translate");
    return std::move(*translated.scene);
}
std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto result = build_metal_renderer(scene);
    require(result.status == GeometryStatus::success && result.scene, result.diagnostic);
    return std::move(result.scene);
}
std::vector<RenderSample> sample(MetalRenderScene &renderer, RenderRequest request) {
    auto result = renderer.sample(request);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == request.sample_count && result.guard_count > 0,
            "Incomplete conductor sample batch or output guard");
    for (const auto &value : result.samples)
        require(std::isfinite(value.radiance) && value.radiance >= 0,
                "Conductor generated negative or nonfinite radiance");
    return std::move(result.samples);
}
RenderRequest request(RenderSampler sampler = {}) {
    return {.width = 1,
            .height = 1,
            .seed = 0x43956ab9ULL,
            .pixel_count = 1,
            .sample_count = 257,
            .total_samples = 257,
            .threadgroup_width = 32,
            .sampler = sampler};
}
void partitions(const RenderScene &scene) {
    auto native = build(scene);
    for (const auto sampler :
         {RenderSampler{}, RenderSampler{SamplerKind::sobol, SobolRandomization::fastowen}}) {
        auto query = request(sampler);
        const auto whole = sample(*native, query);
        for (const unsigned begin : {0U, 113U}) {
            query.sample_begin = begin;
            query.sample_count = begin == 0 ? 113 : 144;
            query.threadgroup_width = begin == 0 ? 1 : 64;
            const auto part = sample(*native, query);
            for (unsigned i = 0; i < part.size(); ++i) {
                const auto &a = part[i];
                const auto &b = whole[begin + i];
                require(a.pixel == b.pixel && a.sample == b.sample &&
                            a.wavelength == b.wavelength && a.radiance == b.radiance &&
                            a.scatterings == b.scatterings && a.random_words == b.random_words,
                        "Conductor sample changed across batch/group partitions");
            }
        }
    }
}
void analytic(const fs::path &path, bool area) {
    // Normal incidence Fresnel follows directly from the complex refractive index:
    // |(n-1)/(n+1)|^2. No production Fresnel or BSDF function is used as an oracle.
    auto scene = load(path);
    scene.raster_camera =
        RasterCamera{{0, 0, 0}, {-1e-7f, -1e-7f, 2}, {2e-7f, 0, 0}, {0, 2e-7f, 0}};
    scene.max_depth = 1;
    scene.environment.fill(area ? 0.f : 1.f);
    for (auto &emission : scene.area_emissions)
        emission.radiance.fill(1.f);
    for (auto &material : scene.materials) {
        if (material.scattering != ScatteringKind::conductor)
            continue;
        material.eta = {{{360, 1.5f}, {830, 1.5f}}, true};
        material.k = {{{360, 2.f}, {830, 2.f}}, true};
        material.u_roughness = material.v_roughness = 0;
    }
    const auto paths = sample(*build(scene), request());
    constexpr double reflectance = (0.5 * 0.5 + 2 * 2) / (2.5 * 2.5 + 2 * 2);
    for (const auto &value : paths)
        require(value.scatterings == 1 && std::abs(value.radiance - reflectance) < 3e-6,
                area ? "Delta emitter hit must retain unity MIS and Fresnel throughput"
                     : "Smooth conductor must reflect environment times analytic Fresnel");
    partitions(scene);
}
void image_environment(const fs::path &scratch, const fs::path &fixtures) {
    const auto path = scratch / "image-environment.pbrt";
    std::ifstream input(fixtures / "smooth-cu-environment.pbrt");
    std::string source{std::istreambuf_iterator<char>(input), {}};
    const std::string original = "LightSource \"infinite\" \"float scale\" [1]";
    const auto asset = fs::absolute(fixtures / "../core-textures/assets/normal-flat.png");
    source.replace(source.find(original), original.size(),
                   "LightSource \"infinite\" \"string filename\" \"" + asset.string() + "\"");
    std::ofstream(path) << source;
    auto scene = load(path);
    scene.raster_camera =
        RasterCamera{{0, 0, 0}, {-1e-7f, -1e-7f, 2}, {2e-7f, 0, 0}, {0, 2e-7f, 0}};
    scene.max_depth = 1;
    for (auto &material : scene.materials) {
        if (material.scattering != ScatteringKind::conductor)
            continue;
        material.eta = {{{360, 1.5f}, {830, 1.5f}}, true};
        material.k = {{{360, 2.f}, {830, 2.f}}, true};
    }
    const auto reflected = sample(*build(scene), request());
    scene.raster_camera->corner.z = -2;
    scene.max_depth = 0;
    const auto clear = sample(*build(scene), request());
    constexpr double fresnel = 4.25 / 10.25;
    for (unsigned i = 0; i < clear.size(); ++i)
        require(clear[i].radiance > 0 &&
                    std::abs(reflected[i].radiance - fresnel * clear[i].radiance) <=
                        3e-6 * std::max(1.f, clear[i].radiance),
                "Delta image-environment hit must retain Fresnel times unity MIS");
}
void invisible_emitter(const fs::path &scratch, const fs::path &fixtures) {
    std::ifstream input(fixtures / "smooth-ag-area.pbrt");
    std::string source{std::istreambuf_iterator<char>(input), {}};
    const std::string indices = "\"integer indices\" [0 1 2 0 2 3]";
    source.insert(source.find(indices) + indices.size(), " \"float alpha\" [0]");
    const auto path = scratch / "invisible-emitter.pbrt";
    std::ofstream(path) << source;
    auto scene = load(path);
    const auto smooth = sample(*build(scene), request());
    require(std::ranges::all_of(smooth, [](const auto &v) { return v.radiance == 0; }),
            "Invisible emitter contributed through a delta BSDF hit");
    for (auto &material : scene.materials)
        if (material.scattering == ScatteringKind::conductor)
            material.u_roughness = material.v_roughness = 0.2f;
    const auto rough = sample(*build(scene), request());
    require(std::ranges::any_of(rough, [](const auto &v) { return v.radiance > 0; }),
            "Invisible emitter lost its rough-conductor direct light estimate");
}
void mixed(const fs::path &scratch) {
    const auto path = scratch / "mixed.pbrt";
    std::ofstream file(path);
    file << R"(Camera "perspective" "float fov" 1
Film "rgb" "integer xresolution" 1 "integer yresolution" 1
PixelFilter "box"
Sampler "independent"
Integrator "path" "integer maxdepth" 2
WorldBegin
Material "conductor" "float roughness" 0
Shape "trianglemesh" "point3 P" [-2 -2 2 2 -2 2 2 2 2 -2 2 2]
 "integer indices" [0 1 2 0 2 3]
Material "diffuse" "rgb reflectance" [.5 .5 .5]
Shape "trianglemesh" "point3 P" [-10 -10 -1 10 -10 -1 10 10 -1 -10 10 -1]
 "integer indices" [0 1 2 0 2 3]
Material "diffuse" "rgb reflectance" [0 0 0]
AreaLightSource "diffuse" "bool twosided" true
Shape "trianglemesh" "point3 P" [3 -1 0 4 -1 0 4 1 0 3 1 0]
 "integer indices" [0 1 2 0 2 3]
)";
    file.close();
    require(file.good(), "Could not write mixed delta/diffuse fixture");
    auto scene = load(path);
    const auto paths = sample(*build(scene), request());
    require(std::ranges::any_of(paths,
                                [](const auto &v) { return v.scatterings == 2 && v.radiance > 0; }),
            "Mixed delta/diffuse paths did not reach illuminated second scattering");
    partitions(scene);
    scene.max_depth = 1;
    const auto stopped = sample(*build(scene), request());
    require(std::ranges::all_of(stopped, [](const auto &v) { return v.radiance == 0; }),
            "Delta/diffuse/emitter transport ignored its depth cutoff");
}
void graph_branch_sensitivity(const fs::path &fixtures) {
    auto scene = load(fixtures / "roughness-graph.pbrt");
    auto query = request();
    query.width = query.height = 8;
    query.pixel_count = 64;
    query.sample_count = 16;
    query.total_samples = 4096;
    const auto trace = [&] {
        auto native = build(scene);
        auto result = native->sample(query);
        require(result.status == GeometryStatus::success, result.diagnostic);
        require(result.samples.size() == 1024 && result.guard_count > 0,
                "Incomplete graph branch sensitivity batch");
        return std::move(result.samples);
    };
    const auto graph = trace();
    const auto set_roughness = [&](float roughness) {
        for (auto &material : scene.materials)
            if (material.scattering == ScatteringKind::conductor) {
                material.u_roughness_root = material.v_roughness_root = UINT32_MAX;
                material.u_roughness = material.v_roughness = roughness;
            }
    };
    set_roughness(0);
    const auto smooth = trace();
    set_roughness(.15f);
    const auto rough = trace();
    unsigned smooth_matches = 0, rough_matches = 0;
    for (unsigned i = 0; i < graph.size(); ++i) {
        const auto a = graph[i].radiance, b = smooth[i].radiance, c = rough[i].radiance;
        if (b != c) {
            smooth_matches += a == b;
            rough_matches += a == c;
        }
    }
    require(smooth_matches > 0 && rough_matches > 0,
            "Roughness graph must exercise both smooth and rough response plateaus");
}
void film_accounting(const fs::path &fixtures) {
    for (const auto name : {"smooth-cu-environment", "roughness-graph"}) {
        const auto scene = load(fixtures / (std::string(name) + ".pbrt"));
        for (const auto sampler :
             {RenderSampler{}, RenderSampler{SamplerKind::sobol, SobolRandomization::fastowen}}) {
            RenderSettings settings{.width = 1,
                                    .height = 1,
                                    .samples = 33,
                                    .batch_size = 33,
                                    .seed = 0x43956ab9ULL,
                                    .sampler = sampler};
            const auto whole = render_progressive(scene, settings);
            require(whole.status == GeometryStatus::success, whole.diagnostic);
            require(whole.film.pixels.size() == 1 && whole.film.pixels.front().count == 33,
                    "Conductor film lost samples");
            for (const unsigned batch : {1U, 7U}) {
                settings.batch_size = batch;
                const auto split = render_progressive(scene, settings);
                require(split.status == GeometryStatus::success, split.diagnostic);
                require(split.film == whole.film,
                        "Raw compensated conductor film changes across batch partitions");
            }
        }
    }
}
void invalid_dynamic_roughness(const fs::path &scratch, const fs::path &fixtures) {
    std::ifstream input(fixtures / "roughness-graph.pbrt");
    std::string source{std::istreambuf_iterator<char>(input), {}};
    const std::string original = "\"float tex2\" [0.15]";
    source.replace(source.find(original), original.size(), "\"float tex2\" [-0.15]");
    const auto path = scratch / "invalid-roughness.pbrt";
    std::ofstream(path) << source;
    const auto scene = load(path);
    const auto failed = render_progressive(
        scene, {.width = 1, .height = 1, .samples = 64, .batch_size = 64, .seed = 0x43956ab9ULL});
    std::cerr << "Dynamic roughness failure diagnostic: " << failed.diagnostic << '\n';
    require(failed.status == GeometryStatus::failure && failed.film.pixels.size() == 1 &&
                failed.film.pixels.front().count == 0 &&
                failed.diagnostic.find("invalid path arithmetic") != std::string::npos,
            "Invalid dynamic roughness did not fail GPU arithmetic with zero committed samples");
}
void diffuse_delta_emitter(const fs::path &scratch) {
    const auto path = scratch / "diffuse-delta-emitter.pbrt";
    std::ofstream file(path);
    file << R"(Camera "perspective" "float fov" 1
Film "rgb" "integer xresolution" 1 "integer yresolution" 1
PixelFilter "box"
Sampler "independent"
Integrator "path" "integer maxdepth" 2
WorldBegin
Material "diffuse" "rgb reflectance" [.5 .5 .5]
Shape "trianglemesh" "point3 P" [-2 -2 2 2 -2 2 2 2 2 -2 2 2]
 "integer indices" [0 1 2 0 2 3]
Material "conductor" "float roughness" 0
Shape "trianglemesh" "point3 P" [-100 -100 -1 100 -100 -1 100 100 -1 -100 100 -1]
 "integer indices" [0 1 2 0 2 3]
Material "diffuse" "rgb reflectance" [0 0 0]
AreaLightSource "diffuse"
ReverseOrientation
Shape "trianglemesh" "point3 P" [1 -8 0 2 -8 0 2 8 0 1 8 0]
 "integer indices" [0 1 2 0 2 3]
)";
    file.close();
    require(file.good(), "Could not write diffuse/delta/emitter fixture");
    auto scene = load(path);
    auto query = request();
    query.sample_count = 4096;
    query.total_samples = 4096;
    const auto paths = sample(*build(scene), query);
    require(std::ranges::any_of(paths,
                                [](const auto &v) { return v.scatterings == 2 && v.radiance > 0; }),
            "Diffuse/delta/emitter paths lost finite-emitter contribution");
    // The emitter faces the mirror and is dark from the primary diffuse plane.
    // Cutting off the mirror bounce must remove all of the positive signal.
    scene.max_depth = 1;
    const auto stopped = sample(*build(scene), query);
    require(std::ranges::all_of(stopped, [](const auto &v) { return v.radiance == 0; }),
            "Diffuse/delta/emitter signal bypassed the mirror or depth cutoff");
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 3, "Expected scratch and conductor fixture directories");
        bool no_device = false, no_ray_tracing = false;
        for (int i = 3; i < argc; ++i) {
            const std::string_view option(argv[i]);
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown conductor test option");
            no_device |= option == "--allow-no-device";
            no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        const fs::path scratch(argv[1]), fixtures(argv[2]);
        fs::create_directories(scratch);
        auto capability = build_metal_renderer(load(fixtures / "smooth-cu-environment.pbrt"));
        if ((no_device && capability.status == GeometryStatus::no_device) ||
            (no_ray_tracing && capability.status == GeometryStatus::no_ray_tracing)) {
            std::cout << "SKIP Metal conductors: " << capability.diagnostic << '\n';
            return 77;
        }
        require(capability.status == GeometryStatus::success && capability.scene,
                capability.diagnostic);
        analytic(fixtures / "smooth-cu-environment.pbrt", false);
        analytic(fixtures / "smooth-ag-area.pbrt", true);
        image_environment(scratch, fixtures);
        invisible_emitter(scratch, fixtures);
        mixed(scratch);
        diffuse_delta_emitter(scratch);
        graph_branch_sensitivity(fixtures);
        film_accounting(fixtures);
        invalid_dynamic_roughness(scratch, fixtures);
        for (const auto name : {"rough-au", "anisotropic-cu", "numeric-file-optical",
                                "detail-alpha", "anisotropy-rotated", "roughness-graph"})
            partitions(load(fixtures / (std::string(name) + ".pbrt")));
        std::cout
            << "PASS analytic mirror environment/area, mixed paths, conductor batch identity\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Conductor render: " << error.what() << '\n';
        return 1;
    }
}
