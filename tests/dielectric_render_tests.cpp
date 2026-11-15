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
    require(translated.scene.has_value(), "Dielectric fixture did not translate");
    if (!translated.scene)
        throw std::runtime_error("Dielectric translation did not return a scene");
    return std::move(translated.scene.value());
}
std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto result = build_metal_renderer(scene);
    require(result.status == GeometryStatus::success && result.scene, result.diagnostic);
    return std::move(result.scene);
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
std::vector<RenderSample> sample(MetalRenderScene &renderer, RenderRequest query) {
    auto result = renderer.sample(query);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == std::size_t{query.pixel_count} * query.sample_count &&
                result.guard_count > 0,
            "Incomplete dielectric sample batch or output guard");
    for (const auto &value : result.samples)
        require(std::isfinite(value.radiance) && value.radiance >= 0,
                "Dielectric generated negative or nonfinite radiance");
    return std::move(result.samples);
}
const std::array samplers{RenderSampler{},
                          RenderSampler{SamplerKind::sobol, SobolRandomization::none},
                          RenderSampler{SamplerKind::sobol, SobolRandomization::permutedigits},
                          RenderSampler{SamplerKind::sobol, SobolRandomization::fastowen},
                          RenderSampler{SamplerKind::sobol, SobolRandomization::owen}};
void partitions(const RenderScene &scene) {
    auto native = build(scene);
    for (const auto sampler : samplers) {
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
                        "Dielectric sample changed across batch/group partitions");
            }
        }
    }
}
// Eta and roughness mirror distinct named material fields at every call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void constant_eta(RenderScene &scene, float eta, float roughness = 0) {
    for (auto &material : scene.materials)
        if (material.scattering == ScatteringKind::dielectric) {
            material.eta = {{{360, eta}, {830, eta}}, true};
            material.u_roughness_root = material.v_roughness_root = UINT32_MAX;
            material.u_roughness = material.v_roughness = roughness;
        }
}
void normal_camera(RenderScene &scene) {
    scene.raster_camera =
        RasterCamera{{0, 0, 0}, {-1e-7f, -1e-7f, 2}, {2e-7f, 0, 0}, {0, 2e-7f, 0}};
}
void analytic_interface(const fs::path &scratch, const fs::path &fixtures) {
    auto scene = load(fixtures / "smooth-environment.pbrt");
    normal_camera(scene);
    scene.max_depth = 1;
    scene.environment.fill(1);
    constant_eta(scene, 1.5f);
    auto query = request();
    query.sample_count = query.total_samples = 16384;
    auto paths = sample(*build(scene), query);
    unsigned reflected = 0;
    for (const auto &value : paths) {
        require(value.scatterings == 1, "Interface must scatter exactly once");
        if (std::abs(value.radiance - 1) < 3e-6)
            ++reflected;
        else
            require(std::abs(value.radiance - 4.0 / 9) < 3e-6,
                    "Entering radiance transmission must carry inverse eta squared");
    }
    // Fresnel branch probability is ((1.5-1)/(1.5+1))^2 = .04.
    // Fixed 16,384-sample independent batch: six binomial standard deviations.
    const auto path_count = static_cast<double>(paths.size());
    require(std::abs(double(reflected) / path_count - .04) < 6 * std::sqrt(.04 * .96 / path_count),
            "Smooth dielectric Fresnel branch probability disagrees with analytic .04");
    // Check the dedicated lobe word independently of production BSDF helpers.
    for (const auto sampler : samplers) {
        auto branch_query = request(sampler);
        const auto branches = sample(*build(scene), branch_query);
        for (const auto &value : branches) {
            const bool independent = sampler.kind == SamplerKind::independent;
            const auto word =
                independent
                    ? sample_words(branch_query.seed, value.pixel, value.sample, 0x80000000u)[0]
                    : sobol::word(sobol_tables(), value.sample, 908,
                                  static_cast<unsigned>(sampler.randomization), branch_query.seed);
            const auto u = independent ? sample_uniform(word) : sobol::uniform(word);
            const double expected = u < .04f ? 1. : 4. / 9;
            require(std::abs(value.radiance - expected) < 3e-6,
                    "Glass branch did not use the dedicated independent/Sobol lobe word");
        }
    }
    // Exercise authored ReverseOrientation through scene translation.
    std::ifstream input(fixtures / "smooth-environment.pbrt");
    std::string source{std::istreambuf_iterator<char>(input), {}};
    source.insert(source.find("Material"), "ReverseOrientation\n");
    const auto reversed = scratch / "reverse-interface.pbrt";
    std::ofstream(reversed) << source;
    scene = load(reversed);
    normal_camera(scene);
    scene.max_depth = 1;
    scene.environment.fill(1);
    constant_eta(scene, 1.5f);
    paths = sample(*build(scene), query);
    unsigned transmitted = 0;
    for (const auto &value : paths) {
        require(std::abs(value.radiance - 1) < 3e-6 || std::abs(value.radiance - 2.25) < 3e-6,
                "Exiting radiance transmission must carry eta squared");
        transmitted += value.radiance > 2;
    }
    require(transmitted > paths.size() / 2, "Exit test did not transmit");
    // Above critical angle inside glass: cos(theta)=.5, eta=1.5.
    scene.raster_camera =
        RasterCamera{{0, 0, 1}, {.8660254f, 0, .5f}, {1e-8f, 0, 0}, {0, 1e-8f, 0}};
    paths = sample(*build(scene), request());
    for (const auto &value : paths)
        require(value.scatterings == 1 && std::abs(value.radiance - 1) < 3e-6,
                "Total internal reflection must retain unit throughput");
    normal_camera(scene);
    for (const float eta : {0.f, 1.f}) {
        constant_eta(scene, eta, .3f);
        paths = sample(*build(scene), request());
        for (const auto &value : paths)
            require(
                value.scatterings == 1 && value.radiance == 1,
                "Eta one (including zero spectrum fallback) must be straight delta transmission");
    }
}
void finite_delta(const fs::path &scratch, const fs::path &fixtures) {
    auto scene = load(fixtures / "smooth-area.pbrt");
    normal_camera(scene);
    scene.max_depth = 1;
    constant_eta(scene, 1.5f);
    for (auto &emission : scene.area_emissions)
        emission.radiance.fill(1);
    const auto paths = sample(*build(scene), request());
    unsigned positive = 0;
    for (const auto &value : paths) {
        require(value.radiance == 0 || std::abs(value.radiance - 4.0 / 9) < 3e-6,
                "Delta transmitted emitter hit must retain unity MIS and inverse eta squared");
        positive += value.radiance > 0;
    }
    require(positive > 0, "Delta transmission never reached finite emitter");
    std::ifstream input(fixtures / "smooth-area.pbrt");
    std::string source{std::istreambuf_iterator<char>(input), {}};
    const std::string original = "[-1.5 -1 4 2.5 -1 4 2.5 1 4 -1.5 1 4]";
    source.replace(source.find(original), original.size(),
                   "[-1.5 -1 -1 2.5 -1 -1 2.5 1 -1 -1.5 1 -1]");
    const auto path = scratch / "reflected-area.pbrt";
    std::ofstream(path) << source;
    scene = load(path);
    normal_camera(scene);
    scene.max_depth = 1;
    for (auto &emission : scene.area_emissions)
        emission.radiance.fill(1);
    const auto reflected = sample(*build(scene), request());
    positive = 0;
    for (const auto &value : reflected) {
        require(value.radiance == 0 || std::abs(value.radiance - 1) < 3e-6,
                "Delta reflected emitter hit must retain unity MIS and branch throughput");
        positive += value.radiance > 0;
    }
    require(positive > 0, "Delta reflection never reached finite emitter");
}
const char *header = R"(Camera "perspective" "float fov" [1]
Film "rgb" "integer xresolution" [1] "integer yresolution" [1]
PixelFilter "box"
Sampler "independent"
Integrator "path" "integer maxdepth" [8]
WorldBegin
)";
const char *interface =
    R"(Shape "trianglemesh" "point3 P" [-100 -100 2 100 -100 2 100 100 2 -100 100 2]
 "integer indices" [0 2 1 0 3 2]
)";
void slab_and_mixed(const fs::path &scratch) {
    const auto path = scratch / "slab.pbrt";
    std::ofstream(path) << header << R"(LightSource "infinite" "float scale" [1]
Material "dielectric" "float eta" [1.5] "float roughness" [0]
)" << interface << R"(Shape "trianglemesh" "point3 P" [-100 -100 3 100 -100 3 100 100 3 -100 100 3]
 "integer indices" [0 1 2 0 2 3]
)";
    auto scene = load(path);
    normal_camera(scene);
    scene.environment.fill(1);
    scene.max_depth = 2;
    const auto paths = sample(*build(scene), request());
    unsigned through_slab = 0;
    for (const auto &value : paths) {
        // Interior reflection at the second interface reaches the depth limit.
        require(value.radiance == 0 || std::abs(value.radiance - 1) < 4e-6,
                "Parallel slab entrance/exit eta factors must cancel");
        through_slab += value.scatterings >= 2;
    }
    require(through_slab > paths.size() / 2, "Slab test did not traverse both interfaces");
    std::ofstream(path) << header
                        << R"(Material "dielectric" "float eta" [1.5] "float roughness" [0]
)" << interface << R"(Material "diffuse" "rgb reflectance" [.5 .5 .5]
Shape "trianglemesh" "point3 P" [-100 -100 4 100 -100 4 100 100 4 -100 100 4]
 "integer indices" [0 2 1 0 3 2]
Material "diffuse" "rgb reflectance" [0 0 0]
AreaLightSource "diffuse" "bool twosided" [true]
Shape "trianglemesh" "point3 P" [3 -1 3 4 -1 3 4 1 3 3 1 3]
 "integer indices" [0 1 2 0 2 3]
)";
    scene = load(path);
    scene.max_depth = 2;
    const auto mixed = sample(*build(scene), request());
    require(std::ranges::any_of(mixed,
                                [](const auto &v) { return v.scatterings == 2 && v.radiance > 0; }),
            "Transmitted glass/diffuse paths lost second-scattering direct light");
    partitions(scene);
    scene.max_depth = 1;
    const auto stopped = sample(*build(scene), request());
    require(std::ranges::all_of(stopped, [](const auto &v) { return v.radiance == 0; }),
            "Mixed transmission path ignored depth cutoff");
}
void hemisphere_lights(const fs::path &scratch) {
    for (const bool transmission : {false, true}) {
        const auto path = scratch / "hemisphere.pbrt";
        const float z = transmission ? 4.f : -1.f;
        std::ofstream(path) << header
                            << R"(Material "dielectric" "float eta" [1.5] "float roughness" [.2]
)" << interface << R"(Material "diffuse" "rgb reflectance" [0 0 0]
AreaLightSource "diffuse" "bool twosided" [true]
Shape "trianglemesh" "point3 P" [-2 -2 )"
                            << z << " 2 -2 " << z << " 2 2 " << z << " -2 2 " << z
                            << R"(] "integer indices" [0 1 2 0 2 3] "float alpha" [0]
)";
        auto scene = load(path);
        normal_camera(scene);
        scene.max_depth = 1;
        const auto paths = sample(*build(scene), request());
        require(std::ranges::any_of(paths, [](const auto &v) { return v.radiance > 0; }),
                transmission ? "Rough glass NEE missed transmission hemisphere"
                             : "Rough glass NEE missed reflection hemisphere");
        // Alpha-zero emitter cannot be reached by the continuation ray. This
        // isolates full-sphere direct-light sampling from BSDF-hit emission.
        constant_eta(scene, 1.5f);
        const auto delta = sample(*build(scene), request());
        require(std::ranges::all_of(delta, [](const auto &v) { return v.radiance == 0; }),
                "Smooth glass incorrectly evaluated continuous NEE");
    }
}
void image_environment(const fs::path &scratch, const fs::path &fixtures) {
    const auto path = scratch / "image-environment.pbrt";
    const auto asset = fs::absolute(fixtures / "../core-textures/assets/normal-flat.png");
    std::ofstream(path) << header << "LightSource \"infinite\" \"string filename\" \""
                        << asset.string() << "\"\nMaterial \"dielectric\" \"float eta\" [1.5]\n"
                        << interface;
    auto scene = load(path);
    normal_camera(scene);
    scene.max_depth = 1;
    const auto paths = sample(*build(scene), request());
    if (!scene.raster_camera)
        throw std::runtime_error("Expected a raster camera");
    scene.raster_camera.value().corner.z = -2;
    scene.max_depth = 0;
    const auto clear = sample(*build(scene), request());
    for (unsigned i = 0; i < paths.size(); ++i) {
        const double a = paths[i].radiance, b = clear[i].radiance;
        require(b > 0 && (std::abs(a - b) < 4e-6 * std::max(1., b) ||
                          std::abs(a - b * 4 / 9) < 4e-6 * std::max(1., b)),
                "Delta image-environment event lost unity MIS or eta throughput");
    }
    normal_camera(scene);
    scene.max_depth = 1;
    constant_eta(scene, 1.5f, .2f);
    const auto rough = sample(*build(scene), request());
    require(std::ranges::any_of(rough, [](const auto &v) { return v.radiance > 0; }),
            "Rough glass image-environment MIS lost all signal");
    partitions(scene);
}
void depth_limits(const fs::path &fixtures) {
    auto scene = load(fixtures / "smooth-environment.pbrt");
    auto query = request({SamplerKind::sobol, SobolRandomization::fastowen});
    query.sample_count = query.total_samples = 1;
    scene.max_depth = 113;
    auto native = build(scene);
    require(native->sample(query).status == GeometryStatus::success,
            "Direct Metal glass depth 113 rejected");
    scene.max_depth = 114;
    native = build(scene);
    require(native->sample(query).status == GeometryStatus::failure,
            "Direct Metal glass depth 114 accepted");
    scene.max_depth = 113;
    RenderSettings settings{.width = 1,
                            .height = 1,
                            .samples = 1,
                            .batch_size = 1,
                            .seed = 1,
                            .sampler = query.sampler};
    require(render_progressive(scene, settings).status == GeometryStatus::success,
            "Progressive glass depth 113 rejected");
    scene.max_depth = 114;
    require(render_progressive(scene, settings).status == GeometryStatus::failure,
            "Progressive glass depth 114 accepted");
    for (auto &material : scene.materials) {
        material = SpectralMaterial{};
        material.kind = SpectralMaterialKind::constant;
        material.constant = .5f;
    }
    scene.instance_texture_frames.clear();
    scene.max_depth = 127;
    const auto legacy = render_progressive(scene, settings);
    require(legacy.status == GeometryStatus::success, legacy.diagnostic);
    native = build(scene);
    scene.max_depth = 127;
    require(native->sample(query).status == GeometryStatus::success,
            "Direct Metal legacy diffuse depth 127 regressed");
}
void film_accounting(const fs::path &fixtures) {
    for (const auto name : {"smooth-environment", "roughness-graph"}) {
        const auto scene = load(fixtures / (std::string(name) + ".pbrt"));
        for (const auto sampler : samplers) {
            RenderSettings settings{.width = 1,
                                    .height = 1,
                                    .samples = 33,
                                    .batch_size = 33,
                                    .seed = 0x43956ab9ULL,
                                    .sampler = sampler};
            const auto whole = render_progressive(scene, settings);
            require(whole.status == GeometryStatus::success, whole.diagnostic);
            require(whole.film.pixels.size() == 1 && whole.film.pixels.front().count == 33,
                    "Dielectric film lost samples");
            for (const unsigned batch : {1U, 7U}) {
                settings.batch_size = batch;
                const auto split = render_progressive(scene, settings);
                require(split.status == GeometryStatus::success, split.diagnostic);
                require(split.film == whole.film,
                        "Dielectric Film changes across batch partitions");
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
    const auto failed = render_progressive(
        load(path),
        {.width = 1, .height = 1, .samples = 64, .batch_size = 64, .seed = 0x43956ab9ULL});
    require(failed.status == GeometryStatus::failure && failed.film.pixels.size() == 1 &&
                failed.film.pixels.front().count == 0 &&
                failed.diagnostic.find("invalid path arithmetic") != std::string::npos,
            "Invalid roughness must reject whole Film batch with no committed samples");
}
} // namespace
int main(int argc, char **argv) {
    try {
        require(argc >= 3, "Expected scratch and dielectric fixture directories");
        bool no_device = false, no_ray_tracing = false;
        for (int i = 3; i < argc; ++i) {
            const std::string_view option(argv[i]);
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown dielectric test option");
            no_device |= option == "--allow-no-device";
            no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        const fs::path scratch(argv[1]), fixtures(argv[2]);
        fs::create_directories(scratch);
        auto capability = build_metal_renderer(load(fixtures / "smooth-environment.pbrt"));
        if ((no_device && capability.status == GeometryStatus::no_device) ||
            (no_ray_tracing && capability.status == GeometryStatus::no_ray_tracing)) {
            std::cout << "SKIP Metal dielectrics: " << capability.diagnostic << '\n';
            return 77;
        }
        require(capability.status == GeometryStatus::success && capability.scene,
                capability.diagnostic);
        analytic_interface(scratch, fixtures);
        finite_delta(scratch, fixtures);
        slab_and_mixed(scratch);
        hemisphere_lights(scratch);
        image_environment(scratch, fixtures);
        depth_limits(fixtures);
        film_accounting(fixtures);
        invalid_dynamic_roughness(scratch, fixtures);
        for (const auto name :
             {"smooth-environment", "smooth-area", "rough-isotropic", "anisotropic",
              "numeric-file-optical", "detail-alpha", "anisotropy-rotated", "roughness-graph"})
            partitions(load(fixtures / (std::string(name) + ".pbrt")));
        std::cout
            << "PASS analytic glass, finite delta transmission, dielectric batch/Film identity\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Dielectric render: " << error.what() << '\n';
        return 1;
    }
}
