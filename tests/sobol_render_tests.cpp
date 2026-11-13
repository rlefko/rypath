// SPDX-License-Identifier: GPL-3.0-only
#include "metal_render.hpp"
#include "sobol_shared.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>

void sobol_gpu_probe();

namespace {
using namespace rypath;
constexpr std::array modes{SobolRandomization::none, SobolRandomization::permutedigits,
                           SobolRandomization::fastowen, SobolRandomization::owen};
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}
RenderScene sky() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera =
        RasterCamera{{0, 0, -2}, {-0.125F, -0.125F, 2}, {0.25F, 0, 0}, {0, 0.25F, 0}};
    scene.max_depth = 0;
    scene.environment.fill(1);
    return scene;
}
std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto native = build_metal_renderer(scene);
    require(native.status == GeometryStatus::success && native.scene, native.diagnostic);
    return std::move(native.scene);
}
std::vector<RenderSample> sample(MetalRenderScene &native, const RenderRequest &request) {
    auto result = native.sample(request);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.guard_count > 0 &&
                result.samples.size() == std::size_t{request.pixel_count} * request.sample_count,
            "Incomplete Sobol dispatch or missing output guard");
    return std::move(result.samples);
}
RenderRequest request(SobolRandomization mode, std::uint32_t count = 4096) {
    return {.width = 1,
            .height = 1,
            .seed = 0,
            .pixel_count = 1,
            .sample_count = count,
            .threadgroup_width = 32,
            .sampler = {SamplerKind::sobol, mode}};
}
bool same(const RenderSample &a, const RenderSample &b) {
    return a.pixel == b.pixel && a.sample == b.sample && a.wavelength == b.wavelength &&
           a.radiance == b.radiance && a.scatterings == b.scatterings &&
           a.random_words == b.random_words;
}

void identity_and_limits(MetalRenderScene &native) {
    const auto &tables = sobol_tables();
    for (const auto mode : modes) {
        auto query = request(mode, 33);
        query.width = 3;
        query.height = 5;
        query.pixel_count = 15;
        query.seed = UINT64_MAX; // PBRT signed seed -1.
        const auto whole = sample(native, query);
        for (const auto &s : whole) {
            const auto index = sobol::index(tables, 3, s.pixel % 3, s.pixel / 3, s.sample);
            for (std::uint32_t dimension = 0; dimension < 4; ++dimension) {
                const auto strategy = dimension < 2 ? 0U : static_cast<std::uint32_t>(mode);
                require(s.random_words[dimension] ==
                            sobol::word(tables, index, dimension, strategy, UINT32_MAX),
                        "Sobol production camera or wavelength word differs from CPU");
            }
        }
        query.threadgroup_width = 64;
        query.pixel_count = 5;
        query.sample_count = 11;
        for (const auto first_pixel : {10U, 0U, 5U}) {
            query.pixel_begin = first_pixel;
            for (const auto first_sample : {0U, 11U, 22U}) {
                query.sample_begin = first_sample;
                const auto part = sample(native, query);
                for (const auto &s : part) {
                    require(same(s, whole[s.sample * 15 + s.pixel]),
                            "Sobol sample changed after tile/group/batch reordering");
                }
            }
        }
        query = request(mode, 2);
        query.sample_begin = (std::uint64_t{1} << 32) - 1;
        const auto carry = sample(native, query);
        require(carry[1].sample == std::uint64_t{1} << 32, "Sobol absolute index carry failed");
        const auto check_high_words = [&](const auto &values) {
            for (const auto &s : values) {
                for (std::uint32_t dimension = 0; dimension < 4; ++dimension) {
                    const auto strategy = dimension < 2 ? 0U : static_cast<std::uint32_t>(mode);
                    require(s.random_words[dimension] ==
                                sobol::word(tables, s.sample, dimension, strategy, 0),
                            "Production Sobol sampler lost high absolute index bits");
                }
            }
        };
        check_high_words(carry);
        query.sample_begin = (std::uint64_t{1} << 52) - 1;
        query.sample_count = 1;
        check_high_words(sample(native, query));
        query.sample_count = 2;
        require(native.sample(query).status == GeometryStatus::failure,
                "Sobol direct request crossed its interval index limit");
        query = request(mode, 1);
        query.seed = 0xffffffffULL;
        require(native.sample(query).status == GeometryStatus::failure,
                "Sobol accepted a noncanonical signed seed");
        query.seed = 0;
        query.width = (1U << 25) + 1;
        require(native.sample(query).status == GeometryStatus::failure,
                "Sobol accepted a film beyond its interval table");
    }
    auto too_deep = sky();
    too_deep.max_depth = 128;
    auto renderer = build(too_deep);
    require(renderer->sample(request(SobolRandomization::fastowen)).status ==
                GeometryStatus::failure,
            "Sobol accepted an unsupported scene depth");
    auto settings = RenderSettings{.width = 1,
                                   .height = 1,
                                   .samples = 1,
                                   .sampler = {SamplerKind::sobol, SobolRandomization::fastowen}};
    require(render_progressive(too_deep, settings).status == GeometryStatus::failure &&
                render_progressive(make_demo_scene(), settings).status == GeometryStatus::failure,
            "Progressive Sobol model/depth validation failed");
}

void spectral_quadrature(MetalRenderScene &native) {
    // The PBRT observer uses nearest-nm plateaus, with half-width endpoints.
    // Its exact integral is trapezoidal. One point per 1/N interval bounds
    // integration error by 470 * total_variation / N. Float wavelength mapping
    // displaces every step by at most 2^-13 nm (including uniform conversion
    // and separate multiply/add), adding total_variation * 2^-13 to the bound.
    std::array<double, 3> expected{}, variation{};
    for (int nm = 360; nm < 830; ++nm) {
        const auto a = pbrt_cie_xyz(nm);
        const auto b = pbrt_cie_xyz(nm + 1);
        for (std::size_t c = 0; c < 3; ++c) {
            expected[c] += 0.5 * (a[c] + b[c]);
            const auto delta = std::abs(b[c] - a[c]);
            variation[c] += delta;
        }
    }
    for (const auto mode : modes) {
        constexpr std::uint32_t count = 65536;
        const auto samples = sample(native, request(mode, count));
        auto film = make_film(1, 1, RenderModel::pbrt);
        FilmBatch batch(film, count);
        require(batch.add(samples).empty() && batch.commit(film).empty(),
                "Sobol spectral batch failed");
        const auto observed = film_mean(film.pixels[0]);
        for (std::size_t c = 0; c < 3; ++c) {
            const double bound = variation[c] * (470.0 / count + 0x1p-13) + 1e-9;
            std::cout << "Sobol " << static_cast<int>(mode) << " XYZ " << c
                      << " delta=" << observed[c] - expected[c] << " bound=" << bound << '\n';
            require(std::abs(observed[c] - expected[c]) <= bound,
                    "Sobol spectral integral exceeds deterministic variation bound");
        }
    }
}

void add_mesh(RenderScene &scene, GeometryMesh mesh, std::uint32_t material,
              std::uint32_t emission = no_area_emission) {
    const auto index = static_cast<std::uint32_t>(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({.mesh_index = index});
    scene.instance_materials.push_back(material);
    scene.instance_emissions.push_back(emission);
}
RenderScene receiver() {
    auto scene = sky();
    scene.max_depth = 1;
    scene.materials = {{.kind = SpectralMaterialKind::constant, .constant = 0},
                       {.kind = SpectralMaterialKind::constant, .constant = 1}};
    add_mesh(scene,
             {.positions = {{-1, -1, 0}, {-1, 1, 0}, {1, 1, 0}, {1, -1, 0}},
              .triangles = {{0, 1, 2}, {0, 2, 3}}},
             1);
    return scene;
}
void randomized_furnace() {
    auto native = build(receiver());
    for (const auto mode : {SobolRandomization::permutedigits, SobolRandomization::fastowen,
                            SobolRandomization::owen}) {
        std::array<double, 16> replicates{};
        for (std::size_t seed = 0; seed < replicates.size(); ++seed) {
            auto query = request(mode, 8192);
            query.seed = seed;
            const auto samples = sample(*native, query);
            for (const auto &s : samples) {
                require(s.scatterings == 1, "Furnace did not scatter exactly once");
                replicates[seed] += s.radiance / static_cast<double>(samples.size());
            }
        }
        const auto mean =
            std::accumulate(replicates.begin(), replicates.end(), 0.0) / replicates.size();
        double sum_squared = 0;
        for (const auto value : replicates) {
            sum_squared += (value - mean) * (value - mean);
        }
        const auto se = std::sqrt(sum_squared / (replicates.size() * (replicates.size() - 1)));
        const auto bound = 6 * se + 2e-5;
        std::cout << "Sobol furnace " << static_cast<int>(mode) << " mean=" << mean
                  << " replicate_se=" << se << " bound=" << bound << '\n';
        require(std::abs(mean - 1) <= bound, "Randomized Sobol furnace mean mismatch");
    }
}

void emitter_slots_and_batches() {
    auto scene = receiver();
    scene.environment.fill(0);
    for (std::uint32_t light = 0; light < 3; ++light) {
        const float y = 1.25F * static_cast<float>(light);
        scene.area_emissions.emplace_back().radiance.fill(static_cast<float>(light + 1));
        add_mesh(scene,
                 {.positions = {{0.25F, y - 0.5F, -1}, {1.5F, y - 0.5F, -1}, {0.25F, y + 0.5F, -1}},
                  .triangles = {{0, 1, 2}}},
                 0, light);
    }
    auto native = build(scene);
    const auto &tables = sobol_tables();
    for (const auto mode : modes) {
        const auto samples = sample(*native, request(mode, 32768));
        std::array<std::uint32_t, 4> away{};
        for (const auto &s : samples) {
            const auto bits = sobol::word(tables, s.sample, 7, static_cast<std::uint32_t>(mode), 0);
            const auto slot = bits >> 30;
            const auto azimuth = sobol::uniform(
                sobol::word(tables, s.sample, 5, static_cast<std::uint32_t>(mode), 0));
            if (azimuth < 0.2F || azimuth > 0.8F) {
                require(slot == 3 ? s.radiance == 0 : s.radiance > 0,
                        "Sobol emitter selection disagrees with dyadic high-bit PMF");
                ++away[slot];
            }
        }
        require(std::ranges::all_of(away, [](auto count) { return count > 100; }),
                "Sobol failed to exercise every live and null emitter slot");
        RenderSettings settings{.width = 3,
                                .height = 5,
                                .samples = 33,
                                .batch_size = 1,
                                .seed = 0,
                                .sampler = {SamplerKind::sobol, mode}};
        const auto one = render_progressive(scene, settings);
        settings.batch_size = 17;
        const auto many = render_progressive(scene, settings);
        require(one.status == GeometryStatus::success && many.status == GeometryStatus::success &&
                    one.film == many.film,
                "Sobol raw film differs across batch partitions");
    }
    // Exercise the one-slot shift guard with actual finite-light transport.
    scene.geometry.meshes.resize(2);
    scene.geometry.instances.resize(2);
    scene.instance_materials.resize(2);
    scene.instance_emissions.resize(2);
    scene.area_emissions.resize(1);
    for (const auto mode : modes) {
        const auto samples = sample(*build(scene), request(mode));
        require(std::ranges::any_of(samples, [](const auto &s) { return s.radiance > 0; }),
                "Single Sobol emitter produced no light");
    }
}

void multibounce_roulette() {
    auto scene = sky();
    scene.raster_camera =
        RasterCamera{{0, 0, 0}, {-0.125F, -0.125F, 1}, {0.25F, 0, 0}, {0, 0.25F, 0}};
    scene.max_depth = sobol_max_depth;
    scene.environment.fill(0);
    scene.materials = {{.kind = SpectralMaterialKind::constant, .constant = 1}};
    // For the first 256 unrandomized points, each BSDF coordinate has an
    // eight-bit fraction. cos(theta) >= 1/16 bounds travel between these planes
    // by 32; 127 scatters cannot reach their 8192-unit edges.
    for (const float z : {-1.0F, 1.0F}) {
        add_mesh(
            scene,
            {.positions = {{-8192, -8192, z}, {-8192, 8192, z}, {8192, 8192, z}, {8192, -8192, z}},
             .triangles = {{0, 1, 2}, {0, 2, 3}}},
            0);
    }
    const auto values = sample(*build(scene), request(SobolRandomization::none, 256));
    bool saw_roulette = false;
    bool saw_depth = false;
    for (const auto &s : values) {
        std::uint32_t expected = 0;
        for (std::uint32_t bounce = 0; bounce < sobol_max_depth; ++bounce) {
            ++expected;
            const auto rr =
                sobol::word(sobol_tables(), s.sample, 6 + 8 * bounce, sobol::no_randomization, 0);
            if (expected >= 3 && rr >= 0xc0000000U) {
                saw_roulette = true;
                break;
            }
        }
        saw_depth |= expected == sobol_max_depth;
        require(s.scatterings == expected && s.radiance == 0,
                "Sobol multibounce roulette consumed the wrong dimension or skipped a path");
    }
    require(saw_roulette && saw_depth, "Sobol roulette test missed a termination branch");
}
} // namespace

int main() {
    try {
        auto built = build_metal_renderer(sky());
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
        if (built.status == GeometryStatus::no_device) {
            std::cout << "SKIP Sobol GPU: " << built.diagnostic << '\n';
            return 77;
        }
#endif
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        if (built.status == GeometryStatus::no_ray_tracing) {
            std::cout << "SKIP Sobol GPU: " << built.diagnostic << '\n';
            return 77;
        }
#endif
        require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
        sobol_gpu_probe();
        identity_and_limits(*built.scene);
        spectral_quadrature(*built.scene);
        randomized_furnace();
        emitter_slots_and_batches();
        multibounce_roulette();
    } catch (const std::exception &error) {
        std::cerr << "Sobol render test: " << error.what() << '\n';
        return 1;
    }
}
