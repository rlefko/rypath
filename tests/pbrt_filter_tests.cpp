// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace rypath;
constexpr std::uint32_t paths = 32768;
constexpr std::uint64_t edge_seed = 0x36b2adcf517ee289ULL;
constexpr std::uint64_t corner_seed = 0xd825e489e71fb063ULL;
constexpr PixelFilter default_triangle{PixelFilterKind::triangle, 2, 2};
constexpr PixelFilter isotropic{PixelFilterKind::triangle, 1, 1};
constexpr PixelFilter anisotropic{PixelFilterKind::triangle, 1.5F, 0.75F};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

double tent_cdf(double offset) {
    // Integral of the unit-radius tent density, evaluated independently
    // of the inverse CDF and after normalizing the sampled offset by radius.
    if (offset < 0) {
        const double left = 1 + offset;
        return 0.5 * left * left;
    }
    const double right = 1 - offset;
    return 1 - 0.5 * right * right;
}

void portable_cdf() {
    constexpr auto tolerance = 8 * std::numeric_limits<float>::epsilon();
    for (const float radius : {0.75F, 1.0F, 1.5F, 2.0F}) {
        require(triangle_filter_offset(0, radius) == -radius &&
                    triangle_filter_offset(0.5F, radius) == 0 &&
                    triangle_filter_offset(1, radius) == radius,
                "Tent support endpoints or center are incorrect");
        float previous = -radius;
        for (std::uint32_t i = 0; i <= 65536; ++i) {
            const float u = static_cast<float>(i) / 65536;
            const float offset = triangle_filter_offset(u, radius);
            require(offset >= previous && offset <= radius &&
                        std::abs(tent_cdf(double{offset} / radius) - u) <= tolerance,
                    "Tent inverse is nonmonotone or disagrees with the integrated density");
            previous = offset;
        }
        for (const float u : {0x1p-24F, 0.5F - 0x1p-24F, 0.5F + 0x1p-24F, 1 - 0x1p-24F}) {
            const float offset = triangle_filter_offset(u, radius);
            require(offset > -radius && offset < radius && ((offset < 0) == (u < 0.5F)) &&
                        std::abs(tent_cdf(double{offset} / radius) - u) <= tolerance,
                    "Tent mapping lost interior support near an endpoint or center");
        }
    }
}

void fixture_translation(const std::filesystem::path &directory) {
    const auto path = directory / "triangle-filter.pbrt";
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0 && checked.document.dependencies_complete &&
                checked.document.dependencies.empty() && checked.inventory.meshes == 2 &&
                checked.inventory.triangles == 4,
            pbrt::report(checked));
    const auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
    if (translated.exit_code() != 0 || !translated.scene) {
        throw std::runtime_error("Triangle filter comparison fixture failed translation");
    }
    const auto &scene = *translated.scene;
    require(scene.filter.kind == anisotropic.kind &&
                scene.filter.x_radius == anisotropic.x_radius &&
                scene.filter.y_radius == anisotropic.y_radius && scene.max_depth == 0 &&
                translated.settings.width == 8 && translated.settings.height == 8 &&
                translated.settings.samples == 4096 && translated.settings.seed == 0,
            "Triangle filter comparison fixture settings differ from the pinned contract");
}

RenderScene sky(PixelFilter filter) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{{}, {-0.5F, 0.5F, 1}, {1, 0, 0}, {0, -1, 0}};
    scene.filter = filter;
    scene.max_depth = 0;
    return scene;
}

RenderScene emitter_edge(PixelFilter filter, float min_y) {
    auto scene = sky(filter);
    constexpr float x = 0.75F;
    scene.geometry.meshes.push_back(
        {.positions = {{x, min_y, 1}, {x, 4, 1}, {4, 4, 1}, {4, min_y, 1}},
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

std::vector<RenderSample> sample(MetalRenderScene &renderer, const RenderRequest &query) {
    auto result = renderer.sample(query);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == std::size_t{query.pixel_count} * query.sample_count &&
                result.guard_count > 0,
            "Expected all samples and an intact Metal output guard");
    for (std::size_t i = 0; i < result.samples.size(); ++i) {
        require(result.samples[i].pixel == query.pixel_begin + i % query.pixel_count &&
                    result.samples[i].sample == query.sample_begin + i / query.pixel_count,
                "Filter dispatch changed absolute sample identity");
    }
    return std::move(result.samples);
}

struct OracleCase {
    std::string_view name;
    PixelFilter filter;
    float min_y;
    std::uint64_t seed;
    double probability;
};

void analytic(MetalRenderScene &renderer, const OracleCase &test) {
    const auto query = RenderRequest{.width = 1,
                                     .height = 1,
                                     .seed = test.seed,
                                     .pixel_count = 1,
                                     .sample_count = paths,
                                     .threadgroup_width = 32};
    const auto samples = sample(renderer, query);
    std::uint32_t hits = 0;
    for (const auto &s : samples) {
        require((s.radiance == 0 || s.radiance == 1) && s.scatterings == 0,
                "Depth-zero emitter must return either unit radiance or black");
        hits += s.radiance == 1 ? 1U : 0U;
    }
    const double observed = static_cast<double>(hits) / paths;
    const double se = std::sqrt(test.probability * (1 - test.probability) / paths);
    const double limit = 6 * se + 2e-5;
    std::cout << std::setprecision(12) << test.name << " hits=" << hits << '/' << paths
              << " observed=" << observed << " expected=" << test.probability
              << " analytic_se=" << se << " limit=" << limit << '\n';
    require(std::abs(observed - test.probability) <= limit,
            "Triangle-filter coverage falls outside the analytic binomial bound");

    const auto box = sample(*build(emitter_edge({}, test.min_y)), query);
    for (std::size_t i = 0; i < box.size(); ++i) {
        require(box[i].radiance == 0 && box[i].scatterings == 0 &&
                    box[i].random_words == samples[i].random_words &&
                    box[i].wavelength == samples[i].wavelength,
                "Half-box control reached the emitter or changed the Philox/wavelength stream");
    }
}

void constant_sky() {
    auto scene = sky(default_triangle);
    scene.environment.fill(0.625F);
    const auto query = RenderRequest{
        .width = 2, .height = 3, .seed = edge_seed, .pixel_count = 6, .sample_count = 29};
    const auto triangle = sample(*build(scene), query);
    scene.filter = {};
    const auto box = sample(*build(scene), query);
    for (std::size_t i = 0; i < triangle.size(); ++i) {
        require(triangle[i].radiance == 0.625F && triangle[i].radiance == box[i].radiance &&
                    triangle[i].wavelength == box[i].wavelength && triangle[i].scatterings == 0 &&
                    triangle[i].random_words == box[i].random_words,
                "Default-radius triangle changed constant-sky energy or sample streams");
    }
}

void batch_identity() {
    const auto scene = emitter_edge(anisotropic, 0.25F);
    auto settings =
        RenderSettings{.width = 2, .height = 3, .samples = 259, .batch_size = 1, .seed = edge_seed};
    const auto one = render_progressive(scene, settings);
    require(one.status == GeometryStatus::success, one.diagnostic);
    settings.batch_size = 257;
    const auto split = render_progressive(scene, settings);
    require(split.status == GeometryStatus::success, split.diagnostic);
    require(
        one.film == split.film &&
            std::ranges::all_of(one.film.pixels,
                                [&](const auto &pixel) { return pixel.count == settings.samples; }),
        "Triangle filtering changed the raw film across progressive batch sizes");
    require(
        std::ranges::any_of(one.film.pixels, [](const auto &pixel) { return pixel.sum[1] > 0; }),
        "Filtered batch identity fixture never reached the emitter");
}

void failed_batch() {
    auto scene = sky({PixelFilterKind::triangle, 3, 1});
    scene.environment.fill(0.625F);
    constexpr RasterCamera wide{{}, {0, 0.5F, 1}, {1e38F, 0, 0}, {0, -1, 0}};
    scene.raster_camera = wide;
    // The 4-pixel raster is representable; width 1 reaches normalized x=3.5,
    // whose product with the camera raster step exceeds binary32.
    require(validate_raster_support(wide, scene.filter, 4, 1).empty() &&
                !validate_raster_support(wide, scene.filter, 1, 1).empty(),
            "Raster validation did not distinguish finite and overflowing triangle support");
    auto renderer = build(scene);
    auto film = make_film(4, 1, RenderModel::pbrt);
    auto query = RenderRequest{
        .width = 4, .height = 1, .seed = edge_seed, .pixel_count = 4, .sample_count = 1};
    {
        FilmBatch batch(film, 1);
        const auto first = sample(*renderer, query);
        require(batch.add(first).empty() && batch.commit(film).empty(),
                "Failed to establish a committed filtered film");
    }
    const auto saved = film;
    query.sample_begin = 1;
    const auto prefix = sample(*renderer, query);
    {
        FilmBatch batch(film, 2);
        require(batch.add(prefix).empty(), "Failed to stage the first filtered tile");
        const auto bad = RenderRequest{.width = 1,
                                       .height = 1,
                                       .seed = edge_seed,
                                       .pixel_count = 1,
                                       .sample_begin = 2,
                                       .sample_count = 1};
        const auto failed = renderer->sample(bad);
        require(failed.status == GeometryStatus::failure && failed.samples.empty() &&
                    failed.guard_count == 0 && !failed.diagnostic.empty(),
                "Overflowing filter support submitted or returned a partial dispatch");
        require(!batch.commit(film).empty() && film == saved,
                "Failed filter dispatch altered a previously committed film");
    }

    query.sample_count = 2;
    const auto recovered = sample(*renderer, query);
    FilmBatch retry(film, 2);
    require(
        retry.add(recovered).empty() && retry.commit(film).empty() &&
            std::ranges::all_of(film.pixels, [](const auto &pixel) { return pixel.count == 3; }),
        "Failed filter dispatch prevented recovery at valid dimensions");
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 2, "Expected --reference or Metal availability options");
        if (std::string_view{argv[1]} == "--reference") {
            require(argc == 3, "Expected the PBRT fixture directory");
            portable_cdf();
            fixture_translation(argv[2]);
            return 0;
        }
        require(std::string_view{argv[1]} == "--metal", "Unknown filter test mode");
        bool allow_no_device = false;
        bool allow_no_ray_tracing = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view option{argv[i]};
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown Metal filter test option");
            allow_no_device |= option == "--allow-no-device";
            allow_no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        constexpr auto edge = OracleCase{"Tent edge", isotropic, -4, edge_seed, 1.0 / 32};
        constexpr auto corner =
            OracleCase{"Tent corner", anisotropic, 0.25F, corner_seed, (1.0 / 8) * (2.0 / 9)};
        const auto native = build_metal_renderer(emitter_edge(edge.filter, edge.min_y));
        if ((native.status == GeometryStatus::no_device && allow_no_device) ||
            (native.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
            std::cout << "SKIP Metal triangle filter. " << native.diagnostic << '\n';
            return 77;
        }
        require(native.status == GeometryStatus::success && native.scene, native.diagnostic);
        // For t >= 0, the normalized tent tail is (1-t/r)^2 / 2.
        // Film y maps to world -y, so the upper quadrant uses the symmetric lower tail.
        analytic(*native.scene, edge);
        analytic(*build(emitter_edge(corner.filter, corner.min_y)), corner);
        constant_sky();
        batch_identity();
        failed_batch();
    } catch (const std::exception &error) {
        std::cerr << "Triangle filter test failure. " << error.what() << '\n';
        return 1;
    }
}
