// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table_fixture.hpp"
#include "metal_render.hpp"
#include "pbrt_render.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace rypath;
namespace fs = std::filesystem;
namespace {
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
RenderScene load(const fs::path &path) {
    const auto input = pbrt::check(path);
    require(input.exit_code() == 0, pbrt::report(input));
    auto output = pbrt::translate(input, pbrt::Location{path.string()});
    for (const auto &issue : output.diagnostics)
        std::cerr << pbrt::format_diagnostic(issue);
    require(output.exit_code() == 0 && output.scene.has_value(),
            "Surface fixture translation failed");
    if (!output.scene)
        throw std::runtime_error("Expected translated surface scene");
    return std::move(*output.scene);
}
std::vector<RenderSample> sample(MetalRenderScene &renderer, const RenderRequest &request) {
    auto result = renderer.sample(request);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.guard_count > 0 &&
                result.samples.size() == std::size_t{request.sample_count} * request.pixel_count,
            "Surface sample accounting or output guard failure");
    for (const auto &s : result.samples)
        require(std::isfinite(s.radiance) && s.radiance >= 0,
                "Surface produced invalid raw radiance");
    return std::move(result.samples);
}
void partitions(const RenderScene &scene) {
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    constexpr std::array samplers{
        RenderSampler{}, RenderSampler{SamplerKind::sobol, SobolRandomization::none},
        RenderSampler{SamplerKind::sobol, SobolRandomization::permutedigits},
        RenderSampler{SamplerKind::sobol, SobolRandomization::fastowen},
        RenderSampler{SamplerKind::sobol, SobolRandomization::owen}};
    for (const auto sampler : samplers) {
        RenderRequest query{.width = 1,
                            .height = 1,
                            .seed = 42,
                            .pixel_count = 1,
                            .sample_count = 73,
                            .total_samples = 73,
                            .threadgroup_width = 32,
                            .sampler = sampler};
        const auto whole = sample(*built.scene, query);
        require(std::ranges::any_of(whole, [](const auto &s) { return s.radiance > 0; }),
                "Surface fixture has no transported signal");
        for (unsigned begin : {0u, 31u}) {
            query.sample_begin = begin;
            query.sample_count = begin == 0 ? 31 : 42;
            query.threadgroup_width = begin == 0 ? 1 : 64;
            const auto part = sample(*built.scene, query);
            for (unsigned i = 0; i < part.size(); ++i) {
                const auto &a = part[i], &b = whole[begin + i];
                require(a.pixel == b.pixel && a.sample == b.sample &&
                            a.wavelength == b.wavelength && a.radiance == b.radiance &&
                            a.scatterings == b.scatterings && a.random_words == b.random_words,
                        "Surface sample changed across batch/threadgroup partitions");
            }
        }
    }
}
void mix_detail_endpoints(RenderScene scene) {
    const auto root = scene.instance_materials.front();
    const auto first = scene.materials.at(root).surface.first_material;
    const auto second = scene.materials.at(root).surface.second_material;
    require(scene.materials.at(first).bump_root != UINT32_MAX &&
                scene.materials.at(second).normal_map != UINT32_MAX,
            "Mix endpoint fixture must carry child-owned bump and normal detail");
    for (unsigned endpoint : {0u, 1u}) {
        auto &mix = scene.materials.at(root);
        mix.surface.mix_amount_root = UINT32_MAX;
        mix.surface.mix_amount = static_cast<float>(endpoint);
        scene.instance_materials.front() = root;
        auto mixed = build_metal_renderer(scene);
        require(mixed.status == GeometryStatus::success && mixed.scene, mixed.diagnostic);
        scene.instance_materials.front() = endpoint == 0 ? first : second;
        auto direct = build_metal_renderer(scene);
        require(direct.status == GeometryStatus::success && direct.scene, direct.diagnostic);
        RenderRequest query{.width = 1,
                            .height = 1,
                            .seed = 42,
                            .pixel_count = 1,
                            .sample_count = 73,
                            .total_samples = 73,
                            .threadgroup_width = 32};
        const auto a = sample(*mixed.scene, query), b = sample(*direct.scene, query);
        for (std::size_t i = 0; i < a.size(); ++i)
            require(a[i].radiance == b[i].radiance && a[i].scatterings == b[i].scatterings &&
                        a[i].wavelength == b[i].wavelength,
                    "Mix endpoint changed child bump/normal shading relative to direct binding");
    }
}
void through_light(RenderScene scene, bool thin = false) {
    // The back light covers almost the entire hemisphere. With R=0,T=1,
    // diffuse transmission must carry unit radiance without an eta factor.
    scene.environment.fill(0);
    for (std::size_t i = 0; i < scene.area_emissions.size(); ++i)
        scene.area_emissions[i].radiance.fill(!thin || i == 0 ? 1 : 0);
    scene.max_depth = 2;
    auto &m = scene.materials.at(scene.instance_materials.front());
    if (!thin) {
        m.kind = SpectralMaterialKind::constant;
        m.constant = 0;
        m.texture_root = UINT32_MAX;
        m.surface.transmittance_root = UINT32_MAX;
        m.surface.transmittance = 1;
        m.surface.transmission_scale = 1;
    }
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    RenderRequest query{.width = 1,
                        .height = 1,
                        .seed = 113,
                        .pixel_count = 1,
                        .sample_count = 8192,
                        .total_samples = 8192,
                        .threadgroup_width = 32};
    const auto paths = sample(*built.scene, query);
    double mean = 0;
    for (const auto &s : paths)
        mean += s.radiance;
    mean /= static_cast<double>(paths.size());
    // Thin-sheet multiple internal reflections give T=(1-R)/(1+R),
    // with normal-incidence R=.04. The 0.025 threshold exceeds six binomial
    // standard deviations for 8192 independent samples (0.018).
    const double expected = thin ? .96 / 1.04 : 1;
    require(std::abs(mean - expected) < .025,
            "Surface disagrees with analytic through-light radiance: " + std::to_string(mean));
}
} // namespace
int main(int argc, char **argv) {
    try {
        require(argc >= 3, "Expected scratch and surface fixture directories");
        const fs::path scratch(argv[1]), fixtures(argv[2]);
        fs::create_directories(scratch);
        table_fixture::write(scratch / "tiny.tensor", table_fixture::measured());
        table_fixture::write(scratch / "tiny.bsdf", table_fixture::fourier());
        for (const auto &entry : fs::directory_iterator(fixtures))
            if (entry.path().extension() == ".pbrt" || entry.path().extension() == ".png")
                fs::copy_file(entry.path(), scratch / entry.path().filename(),
                              fs::copy_options::overwrite_existing);
        bool allow_no_device = false, allow_no_ray_tracing = false;
        for (int i = 3; i < argc; ++i) {
            const std::string_view option(argv[i]);
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown surface test option");
            allow_no_device |= option == "--allow-no-device";
            allow_no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        auto capability = build_metal_renderer(load(scratch / "thindielectric.pbrt"));
        if ((allow_no_device && capability.status == GeometryStatus::no_device) ||
            (allow_no_ray_tracing && capability.status == GeometryStatus::no_ray_tracing)) {
            std::cout << "SKIP Metal surfaces: " << capability.diagnostic << '\n';
            return 77;
        }
        require(capability.status == GeometryStatus::success && capability.scene,
                capability.diagnostic);
        for (const auto name : {"coateddiffuse", "coatedconductor", "thindielectric",
                                "diffusetransmission", "measured", "fourier", "mix"})
            partitions(load(scratch / (std::string(name) + ".pbrt")));
        mix_detail_endpoints(load(scratch / "mix.pbrt"));
        through_light(load(scratch / "diffusetransmission.pbrt"));
        through_light(load(scratch / "thindielectric.pbrt"), true);
        std::cout << "PASS surface families, all sampler partitions, mapped mix children, diffuse "
                     "through-light\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Surface render: " << e.what() << '\n';
        return 1;
    }
}
