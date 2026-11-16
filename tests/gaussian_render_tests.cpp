// SPDX-License-Identifier: GPL-3.0-only
#include "gaussian_filter.hpp"
#include "metal_render.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
constexpr std::uint32_t paths = 32768;
constexpr std::uint64_t seed = 0x36b2adcf517ee289ULL;
constexpr PixelFilter coarse{PixelFilterKind::gaussian, .0625f, .0625f, .01f};

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

RenderScene sky(PixelFilter filter) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{{}, {-.5f, .5f, 1}, {1, 0, 0}, {0, -1, 0}};
    scene.filter = filter;
    scene.max_depth = 0;
    return scene;
}

RenderScene edge(PixelFilter filter, float x, float y = -4) {
    auto scene = sky(filter);
    scene.geometry.meshes.push_back({.positions = {{x, y, 1}, {x, 4, 1}, {4, 4, 1}, {4, y, 1}},
                                     .triangles = {{0, 1, 2}, {0, 2, 3}}});
    scene.geometry.instances.push_back({});
    scene.materials.push_back({.kind = SpectralMaterialKind::constant, .constant = 0});
    scene.instance_materials.push_back(0);
    scene.area_emissions.emplace_back().radiance.fill(1);
    scene.instance_emissions.push_back(0);
    return scene;
}

std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    return std::move(built.scene);
}

std::vector<RenderSample> samples(MetalRenderScene &renderer, RenderRequest request) {
    auto result = renderer.sample(request);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == std::size_t{request.pixel_count} * request.sample_count &&
                result.guard_count > 0,
            "Gaussian dispatch omitted samples or damaged its output guard");
    for (std::size_t i = 0; i < result.samples.size(); ++i)
        require(result.samples[i].pixel == request.pixel_begin + i % request.pixel_count &&
                    result.samples[i].sample == request.sample_begin + i / request.pixel_count,
                "Gaussian dispatch changed absolute sample identity");
    return std::move(result.samples);
}

// Independent double-precision midpoint quadrature: this integrates the step
// function, including a partial cell, rather than the continuous Gaussian.
// Arguments name the filter's radius and sigma before the integration threshold.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double tail(double radius, double sigma, double threshold) {
    const auto count = static_cast<unsigned>(32 * radius);
    const double boundary = std::exp(-radius * radius / (2 * sigma * sigma));
    double total = 0, selected = 0;
    for (unsigned i = 0; i < count; ++i) {
        const double lo = -radius + 2 * radius * i / count;
        const double hi = -radius + 2 * radius * (i + 1) / count;
        const double midpoint = (lo + hi) / 2;
        const double value =
            std::max(0., std::exp(-midpoint * midpoint / (2 * sigma * sigma)) - boundary);
        total += value * (hi - lo);
        selected += value * std::clamp(hi - threshold, 0., hi - lo);
    }
    return selected / total;
}

void coverage(PixelFilter filter, float min_x, float min_y, double expected) {
    // Construction temporaries are destroyed before the first dispatch. This
    // exercises session ownership of the compiled Gaussian table and geometry.
    auto renderer = build(edge(filter, min_x, min_y));
    const auto result =
        samples(*renderer,
                {.width = 1, .height = 1, .seed = seed, .pixel_count = 1, .sample_count = paths});
    const auto compiled = compile_gaussian_filter(filter);
    require(compiled.diagnostic.empty(), compiled.diagnostic);
    unsigned hits = 0;
    for (const auto &sample : result) {
        require((sample.radiance == 0 || sample.radiance == 1) && sample.scatterings == 0,
                "Gaussian depth-zero emitter changed its unit radiance weight");
        hits += sample.radiance == 1;
        const auto offset = sample_gaussian_filter(compiled.distribution,
                                                   float(sample.random_words[0] >> 8) * 0x1p-24f,
                                                   float(sample.random_words[1] >> 8) * 0x1p-24f);
        // Film y runs opposite world y. Exclude only the narrow geometric
        // comparison boundary; all paths still contribute to the mean above.
        if (std::abs(offset[0] - min_x) > 1e-6f && std::abs(-offset[1] - min_y) > 1e-6f)
            require((sample.radiance == 1) == (offset[0] > min_x && -offset[1] > min_y),
                    "Production Gaussian camera ray disagrees with its CPU table sample");
    }
    const double observed = double(hits) / paths;
    // Set before measurement: six binomial standard errors plus a bound for
    // pinned PBRT's float exponential approximation versus independent exp().
    const double limit = 6 * std::sqrt(expected * (1 - expected) / paths) + .00015;
    std::cout << "Gaussian edge (" << min_x << ',' << min_y << "): " << observed << " expected "
              << expected << " limit " << limit << '\n';
    require(std::abs(observed - expected) <= limit,
            "Gaussian coverage differs from its independently integrated midpoint table");
}

void normalization() {
    for (const auto kind :
         {SamplerKind::independent, SamplerKind::sobol, SamplerKind::halton, SamplerKind::zsobol}) {
        auto scene = sky({PixelFilterKind::gaussian, 1.5f, .75f, .5f});
        scene.environment.fill(.625f);
        auto gaussian = build(scene);
        scene.filter = {};
        auto box = build(scene);
        scene = {}; // Neither renderer may borrow the construction storage.
        RenderRequest request{.width = 2,
                              .height = 3,
                              .seed = 19,
                              .pixel_count = 6,
                              .sample_count = 64,
                              .total_samples = 64,
                              .sampler = {kind, kind == SamplerKind::halton
                                                    ? SobolRandomization::permutedigits
                                                    : SobolRandomization::fastowen}};
        const auto actual = samples(*gaussian, request);
        const auto control = samples(*box, request);
        for (std::size_t i = 0; i < actual.size(); ++i)
            require(actual[i].radiance == .625f && actual[i].radiance == control[i].radiance &&
                        actual[i].wavelength == control[i].wavelength &&
                        actual[i].random_words == control[i].random_words &&
                        actual[i].scatterings == 0,
                    "Gaussian sky changed energy, wavelength or sampler streams");
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 2 && std::string_view(argv[1]) == "--metal", "Expected --metal");
        bool allow_no_device = false, allow_no_ray_tracing = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view option{argv[i]};
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown Gaussian Metal availability option");
            allow_no_device |= option == "--allow-no-device";
            allow_no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        const auto invalid =
            build_metal_renderer(sky({PixelFilterKind::gaussian, .02f, 1.5f, .5f}));
        require(invalid.status == GeometryStatus::failure && !invalid.scene &&
                    !invalid.diagnostic.empty(),
                "Invalid Gaussian table must fail renderer construction");
        const auto native = build_metal_renderer(edge(coarse, .03125f));
        if ((native.status == GeometryStatus::no_device && allow_no_device) ||
            (native.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
            std::cout << "SKIP Metal Gaussian filter. " << native.diagnostic << '\n';
            return 77;
        }
        require(native.status == GeometryStatus::success && native.scene, native.diagnostic);
        coverage(coarse, .03125f, -4, .25);
        constexpr PixelFilter isotropic{PixelFilterKind::gaussian, 1.5f, 1.5f, .5f};
        coverage(isotropic, .75f, -4, tail(1.5, .5, .75));
        constexpr PixelFilter anisotropic{PixelFilterKind::gaussian, 1.5f, .75f, .5f};
        coverage(anisotropic, .75f, .25f, tail(1.5, .5, .75) * tail(.75, .5, .25));
        normalization();
    } catch (const std::exception &error) {
        std::cerr << "Gaussian Metal test failure: " << error.what() << '\n';
        return 1;
    }
}
