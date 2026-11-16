// SPDX-License-Identifier: GPL-3.0-only
#include "metal_render.hpp"
#include "qmc_shared.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
void qmc_gpu_probe();
namespace {
using namespace rypath;
void require(bool pass, std::string_view message) {
    if (!pass)
        throw std::runtime_error(std::string(message));
}
RenderScene sky() {
    RenderScene s;
    s.model = RenderModel::pbrt;
    s.raster_camera = RasterCamera{{}, {-.5f, .5f, 1}, {1, 0, 0}, {0, -1, 0}};
    s.environment.fill(1);
    s.max_depth = 0;
    return s;
}
std::unique_ptr<MetalRenderScene> build(RenderScene scene) {
    auto r = build_metal_renderer(scene);
    require(r.status == GeometryStatus::success && r.scene, r.diagnostic);
    return std::move(r.scene);
}
std::vector<RenderSample> sample(MetalRenderScene &native, const RenderRequest &request) {
    auto r = native.sample(request);
    require(r.status == GeometryStatus::success, r.diagnostic);
    require(r.guard_count > 0 &&
                r.samples.size() == std::size_t(request.pixel_count) * request.sample_count,
            "QMC sample accounting/guard");
    return std::move(r.samples);
}
bool same(const RenderSample &a, const RenderSample &b) {
    return a.pixel == b.pixel && a.sample == b.sample && a.wavelength == b.wavelength &&
           a.radiance == b.radiance && a.scatterings == b.scatterings &&
           a.random_words == b.random_words;
}
void primary_and_scheduling() {
    auto native = build(sky());
    for (auto kind : {SamplerKind::halton, SamplerKind::zsobol})
        for (auto mode : {SobolRandomization::none, SobolRandomization::permutedigits,
                          SobolRandomization::fastowen, SobolRandomization::owen}) {
            if (kind == SamplerKind::halton && mode == SobolRandomization::fastowen)
                continue;
            RenderRequest q{.width = 3,
                            .height = 5,
                            .seed = UINT64_MAX,
                            .pixel_count = 15,
                            .sample_count = 64,
                            .total_samples = 64,
                            .threadgroup_width = 32,
                            .sampler = {kind, mode}};
            const auto whole = sample(*native, q);
            for (const auto &s : whole) {
                std::array<unsigned, 4> expected;
                if (kind == SamplerKind::halton) {
                    const auto index = qmc::halton_index(3, 5, s.pixel % 3, s.pixel / 3, s.sample);
                    expected = {
                        qmc::float_bits(qmc::halton_camera(0, 3, index)),
                        qmc::float_bits(qmc::halton_camera(1, 5, index)),
                        qmc::float_bits(qmc::radical(2, index, unsigned(mode), UINT32_MAX)),
                        qmc::float_bits(qmc::radical(3, index, unsigned(mode), UINT32_MAX))};
                } else {
                    const auto index = qmc::morton(s.pixel % 3, s.pixel / 3, 6, s.sample);
                    const auto &t = sobol_tables();
                    expected = {
                        qmc::zword(t, index, 1, 0, true, 3, 6, unsigned(mode), UINT32_MAX),
                        qmc::zword(t, index, 1, 1, true, 3, 6, unsigned(mode), UINT32_MAX),
                        qmc::zword(t, index, 0, 0, false, 3, 6, unsigned(mode), UINT32_MAX),
                        qmc::zword(t, index, 3, 0, false, 3, 6, unsigned(mode), UINT32_MAX)};
                }
                if (kind == SamplerKind::zsobol)
                    require(s.random_words == expected, "ZSobol production primary grouping");
                else
                    for (unsigned i = 0; i < 4; ++i)
                        require(std::abs(qmc::bits_float(s.random_words[i]) -
                                         qmc::bits_float(expected[i])) <=
                                    8 * std::numeric_limits<float>::epsilon() *
                                        std::max(qmc::bits_float(expected[i]),
                                                 std::numeric_limits<float>::min()),
                                "Halton production primary values");
                require(s.radiance == 1 && s.scatterings == 0, "QMC constant sky");
            }
            q.pixel_count = 5;
            q.sample_count = 16;
            q.threadgroup_width = 64;
            for (unsigned pixel : {10u, 0u, 5u})
                for (unsigned begin : {48u, 0u, 32u, 16u}) {
                    q.pixel_begin = pixel;
                    q.sample_begin = begin;
                    for (const auto &s : sample(*native, q))
                        require(same(s, whole[s.sample * 15 + s.pixel]),
                                "QMC reordered schedule changed sample");
                }
            q.pixel_begin = 0;
            q.pixel_count = 1;
            q.sample_begin = UINT64_C(1) << 32;
            q.sample_count = 2;
            q.total_samples = UINT64_C(1) << 33;
            const auto high = sample(*native, q);
            require(high[0].sample == (UINT64_C(1) << 32) && high[1].sample == high[0].sample + 1,
                    "QMC high absolute sample index");
            q.seed = 0xffffffffull;
            require(native->sample(q).status == GeometryStatus::failure,
                    "QMC noncanonical seed accepted");
            q.seed = UINT64_MAX;
            if (kind == SamplerKind::zsobol) {
                q.sample_begin = 64;
                q.total_samples = 64;
                require(native->sample(q).status == GeometryStatus::failure,
                        "ZSobol budget alias accepted");
                q.sample_begin = 0;
                q.total_samples = 0;
                require(native->sample(q).status == GeometryStatus::failure,
                        "ZSobol absent budget accepted");
            }
        }
}
RenderScene edge(bool lens) {
    auto s = sky();
    s.filter = {PixelFilterKind::gaussian, .0625f, .0625f, .01f};
    s.environment.fill(0);
    s.geometry.meshes.push_back(
        {.positions = {{.03125f, -4, 1}, {.03125f, 4, 1}, {4, 4, 1}, {4, -4, 1}},
         .triangles = {{0, 1, 2}, {0, 2, 3}}});
    s.geometry.instances.push_back({});
    s.materials.push_back({.kind = SpectralMaterialKind::constant, .constant = 0});
    s.instance_materials.push_back(0);
    s.area_emissions.emplace_back().radiance.fill(1);
    s.instance_emissions.push_back(0);
    if (lens)
        s.perspective_lens = PerspectiveLens{.1f, 1, {1, 0, 0}, {0, 1, 0}};
    return s;
}
void gaussian_geometry() {
    for (bool lens : {false, true})
        for (auto kind : {SamplerKind::independent, SamplerKind::sobol, SamplerKind::halton,
                          SamplerKind::zsobol}) {
            auto native = build(edge(lens));
            RenderRequest q{.width = 1,
                            .height = 1,
                            .seed = 17,
                            .pixel_count = 1,
                            .sample_count = 4096,
                            .total_samples = 4096,
                            .sampler = {kind, kind == SamplerKind::halton
                                                  ? SobolRandomization::permutedigits
                                                  : SobolRandomization::fastowen}};
            const auto values = sample(*native, q);
            unsigned hits = 0;
            for (const auto &s : values) {
                const float u = kind == SamplerKind::independent
                                    ? float(s.random_words[0] >> 8) * 0x1p-24f
                                : kind == SamplerKind::halton ? qmc::bits_float(s.random_words[0])
                                                              : sobol::uniform(s.random_words[0]);
                require(s.radiance == 0 || s.radiance == 1, "Gaussian emitter sample weight");
                // A ray exactly on the emitter edge can round to either side,
                // particularly after the thin-lens focus construction. Keep every
                // sample in probability, accounting, and partition comparisons.
                if (std::abs(u - .75f) > 1e-6f)
                    require((s.radiance == 1) == (u >= .75f),
                            "Gaussian table edge oracle/camera/lens orientation");
                hits += s.radiance == 1;
            }
            require(std::abs(double(hits) / static_cast<double>(values.size()) - .25) < .05,
                    "Gaussian piecewise table probability (continuous kernel substituted)");
            q.sample_begin = 17;
            q.sample_count = 257;
            const auto part = sample(*native, q);
            for (unsigned i = 0; i < part.size(); ++i)
                require(same(part[i], values[17 + i]),
                        "Gaussian owned CDF or partition changed samples");
        }
}
// Independent dimension expectations use shared algorithms already checked against
// upstream known answers; the rendered path exercises their production call sites.
float variate(SamplerKind kind, SobolRandomization mode, std::uint64_t sample, unsigned dimension,
              bool pair = false, unsigned axis = 0, unsigned log_spp = 12) {
    if (kind == SamplerKind::halton)
        return qmc::radical(dimension + axis, sample, unsigned(mode), 0);
    return sobol::uniform(
        qmc::zword(sobol_tables(), sample, dimension, axis, pair, 0, log_spp, unsigned(mode), 0));
}
void mesh(RenderScene &s, GeometryMesh m, unsigned material, unsigned emission = no_area_emission) {
    const auto id = unsigned(s.geometry.meshes.size());
    s.geometry.meshes.push_back(std::move(m));
    s.geometry.instances.push_back({.mesh_index = id});
    s.instance_materials.push_back(material);
    s.instance_emissions.push_back(emission);
}
RenderRequest transport_request(SamplerKind kind, SobolRandomization mode, unsigned count = 4096) {
    return {.width = 1,
            .height = 1,
            .seed = 0,
            .pixel_count = 1,
            .sample_count = count,
            .total_samples = 4096,
            .sampler = {kind, mode}};
}
void transport_calls() {
    for (auto kind : {SamplerKind::halton, SamplerKind::zsobol})
        for (auto mode : {SobolRandomization::none, SobolRandomization::permutedigits,
                          SobolRandomization::owen}) {
            auto s = sky();
            s.raster_camera = RasterCamera{{}, {-.125f, -.125f, 1}, {.25f, 0, 0}, {0, .25f, 0}};
            s.max_depth = sampler_max_depth({kind}, false);
            s.environment.fill(0);
            s.materials = {{.kind = SpectralMaterialKind::constant, .constant = 1}};
            for (float z : {-1.f, 1.f})
                mesh(s,
                     {.positions =
                          {{-8192, -8192, z}, {-8192, 8192, z}, {8192, 8192, z}, {8192, -8192, z}},
                      .triangles = {{0, 1, 2}, {0, 2, 3}}},
                     0);
            const auto values = sample(*build(s), transport_request(kind, mode, 256));
            unsigned terminated = 0;
            for (const auto &value : values) {
                unsigned expected = 0;
                double travel_bound = 1;
                for (unsigned bounce = 0; bounce < s.max_depth; ++bounce) {
                    ++expected;
                    const double radial = variate(kind, mode, value.sample, 4 + 8 * bounce, true);
                    // Plane spacing plus conservative spawn allowance is below
                    // four. Bound horizontal travel without production trig.
                    travel_bound += 4 * std::sqrt(radial / (1 - radial));
                    if (expected >= 3 &&
                        variate(kind, mode, value.sample, 6 + 8 * bounce) >= .75f) {
                        ++terminated;
                        break;
                    }
                }
                require(travel_bound < 4096, "QMC roulette plane coverage bound failed");
                if (value.scatterings != expected || value.radiance != 0)
                    std::cerr << "roulette mismatch kind=" << unsigned(kind)
                              << " mode=" << unsigned(mode) << " sample=" << value.sample
                              << " observed=" << value.scatterings << " expected=" << expected
                              << " radiance=" << value.radiance << '\n';
                require(value.scatterings == expected && value.radiance == 0,
                        "QMC transport roulette dimension/termination mismatch");
            }
            require(terminated > 100, "QMC roulette did not exercise termination");
            // Three live emitter slots and a fourth null slot, with a BSDF-azimuth
            // subset that cannot hit any emitter through the continuation technique.
            s = sky();
            s.environment.fill(0);
            s.max_depth = 1;
            s.raster_camera =
                RasterCamera{{0, 0, -2}, {-.125f, -.125f, 2}, {.25f, 0, 0}, {0, .25f, 0}};
            s.materials = {{.kind = SpectralMaterialKind::constant, .constant = 0},
                           {.kind = SpectralMaterialKind::constant, .constant = 1}};
            mesh(s,
                 {.positions = {{-1, -1, 0}, {-1, 1, 0}, {1, 1, 0}, {1, -1, 0}},
                  .triangles = {{0, 1, 2}, {0, 2, 3}}},
                 1);
            for (unsigned light = 0; light < 3; ++light) {
                const float y = 1.25f * static_cast<float>(light);
                s.area_emissions.emplace_back().radiance.fill(float(light + 1));
                mesh(s,
                     {.positions = {{.25f, y - .5f, -1}, {1.5f, y - .5f, -1}, {.25f, y + .5f, -1}},
                      .triangles = {{0, 1, 2}}},
                     0, light);
            }
            auto native = build(s);
            auto request = transport_request(kind, mode);
            const auto emitted = sample(*native, request);
            std::array<unsigned, 4> observed{};
            for (const auto &value : emitted) {
                const auto slot = unsigned(variate(kind, mode, value.sample, 7) * 4);
                const auto azimuth = variate(kind, mode, value.sample, 4, true, 1);
                if (azimuth < .2f || azimuth > .8f) {
                    require(slot == 3 ? value.radiance == 0 : value.radiance > 0,
                            "QMC live/null emitter slot mismatch");
                    ++observed[slot];
                }
            }
            require(std::ranges::all_of(observed, [](unsigned n) { return n > 20; }),
                    "QMC emitter test missed a live/null slot");
            request.sample_begin = 113;
            request.sample_count = 257;
            for (const auto &value : sample(*native, request))
                require(same(value, emitted[value.sample]),
                        "QMC finite transport schedule changed sample");
            s.geometry.meshes.resize(2);
            s.geometry.instances.resize(2);
            s.instance_materials.resize(2);
            s.instance_emissions.resize(2);
            s.area_emissions.resize(1);
            const auto single = sample(*build(s), transport_request(kind, mode, 256));
            require(std::ranges::any_of(single, [](const auto &v) { return v.radiance > 0; }),
                    "QMC one-emitter shift produced no light");
        }
}
void dielectric_tail() {
    for (auto kind : {SamplerKind::halton, SamplerKind::zsobol})
        for (auto mode : {SobolRandomization::none, SobolRandomization::permutedigits,
                          SobolRandomization::owen}) {
            auto s = sky();
            s.environment.fill(0);
            s.max_depth = 2;
            s.raster_camera = RasterCamera{{}, {-1e-7f, -1e-7f, 2}, {2e-7f, 0, 0}, {0, 2e-7f, 0}};
            SpectralMaterial glass;
            glass.scattering = ScatteringKind::dielectric;
            glass.eta = {{{360, 1.5f}, {830, 1.5f}}, true};
            s.materials = {glass, {.kind = SpectralMaterialKind::constant, .constant = 0}};
            mesh(s,
                 {.positions = {{-100, -100, 2}, {100, -100, 2}, {100, 100, 2}, {-100, 100, 2}},
                  .triangles = {{0, 2, 1}, {0, 3, 2}}},
                 0);
            mesh(s,
                 {.positions = {{-100, -100, 3}, {100, -100, 3}, {100, 100, 3}, {-100, 100, 3}},
                  .triangles = {{0, 1, 2}, {0, 2, 3}}},
                 0);
            for (float z : {-1.f, 4.f}) {
                const auto e = unsigned(s.area_emissions.size());
                s.area_emissions.emplace_back().radiance.fill(z < 0 ? 2.f : 3.f);
                s.area_emissions.back().two_sided = true;
                mesh(s,
                     {.positions = {{-100, -100, z}, {100, -100, z}, {100, 100, z}, {-100, 100, z}},
                      .triangles = {{0, 1, 2}, {0, 2, 3}}},
                     1, e);
            }
            s.instance_surface_transforms.resize(s.geometry.instances.size());
            s.instance_texture_frames.resize(s.geometry.instances.size());
            for (unsigned i = 0; i < s.geometry.instances.size(); ++i)
                if (instance_uses_texture_context(s, i))
                    require(prepare_texture_triangle_frames(
                                s.geometry.meshes[i], s.instance_surface_transforms[i].shape,
                                s.instance_surface_transforms[i].placement,
                                s.instance_texture_frames[i])
                                .empty(),
                            "QMC slab surface frame");
            const auto paths = sample(*build(s), transport_request(kind, mode));
            std::array<unsigned, 3> branch{};
            for (const auto &value : paths) {
                const auto first = kind == SamplerKind::halton ? 884u : 908u;
                const bool reflect = variate(kind, mode, value.sample, first) < .04f;
                const bool inside_reflect = variate(kind, mode, value.sample, first + 1) < .04f;
                const double expected = reflect ? 2. : inside_reflect ? 0. : 3.;
                require(std::abs(value.radiance - expected) < 5e-5,
                        "QMC dielectric lobe tail/slab eta/emission mismatch");
                ++branch[reflect ? 0 : inside_reflect ? 1 : 2];
            }
            require(branch[0] > 0 && branch[2] > 0,
                    "QMC slab missed entrance reflection or transmission");
            // Unrandomized high-prime Halton dimensions are strongly correlated
            // over a short prefix; separate randomized modes exercise the second
            // interface reflection while all modes check every expected path.
            if (mode != SobolRandomization::none)
                require(branch[1] > 0, "QMC slab missed an interior reflection");
        }
}
void extreme_indices() {
    auto native = build(sky());
    RenderRequest q{.width = 1,
                    .height = 1,
                    .seed = 0,
                    .pixel_count = 1,
                    .sample_begin = UINT64_C(1) << 40,
                    .sample_count = 1,
                    .sampler = {SamplerKind::halton, SobolRandomization::none}};
    auto values = sample(*native, q);
    const float tiny = qmc::bits_float(values[0].random_words[0]);
    require(tiny == 0x1p-41f, "Halton primary float lost sub-32-bit camera fraction");
    q.sample_begin = INT64_MAX;
    q.sample_count = 1;
    values = sample(*native, q);
    require(values[0].sample == std::uint64_t(INT64_MAX), "Halton maximum signed index lost");
    q.sample_begin = std::uint64_t(INT64_MAX) + 1;
    require(native->sample(q).status == GeometryStatus::failure,
            "Halton beyond signed index accepted");
    q.width = 1u << 26;
    q.pixel_begin = (1u << 26) - 1;
    q.sample_begin = 0;
    q.total_samples = 1;
    q.sampler = {SamplerKind::zsobol, SobolRandomization::fastowen};
    values = sample(*native, q);
    require(values[0].pixel == q.pixel_begin, "ZSobol 52-bit Morton index lost");
    q.total_samples = 2;
    require(native->sample(q).status == GeometryStatus::failure,
            "ZSobol over-52-bit Morton index accepted");
}
void depth_limits() {
    for (auto kind : {SamplerKind::halton, SamplerKind::zsobol}) {
        auto s = sky();
        s.max_depth = sampler_max_depth({kind}, false) + 1;
        auto native = build(s);
        RenderRequest q{.width = 1,
                        .height = 1,
                        .pixel_count = 1,
                        .sample_count = 1,
                        .total_samples = 16,
                        .sampler = {kind, SobolRandomization::permutedigits}};
        require(native->sample(q).status == GeometryStatus::failure,
                "QMC unsupported depth reached device");
    }
}
} // namespace
int main() {
    try {
        auto available = build_metal_renderer(sky());
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
        if (available.status == GeometryStatus::no_device) {
            std::cout << "SKIP QMC GPU: " << available.diagnostic << '\n';
            return 77;
        }
#endif
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        if (available.status == GeometryStatus::no_ray_tracing) {
            std::cout << "SKIP QMC GPU: " << available.diagnostic << '\n';
            return 77;
        }
#endif
        require(available.status == GeometryStatus::success, available.diagnostic);
        qmc_gpu_probe();
        primary_and_scheduling();
        gaussian_geometry();
        depth_limits();
        transport_calls();
        dielectric_tail();
        extreme_indices();
        std::cout << "QMC native algorithms, schedules, Gaussian edge/lens contracts passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
