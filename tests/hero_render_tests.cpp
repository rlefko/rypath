// SPDX-License-Identifier: GPL-3.0-only
#include "area_stencil_shared.h"
#include "enhanced_chart_shared.h"
#include "frame_session.hpp"
#include "frame_session_test_access.hpp"
#include "hero_spectrum_shared.h"
#include "replay_shared.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace {
using namespace rypath;
constexpr std::uint64_t high_sample = (1ull << 32) + 29;
constexpr std::uint64_t seed = 51001;
constexpr float emitted = .375f;
void need(bool ok, std::string_view why) {
    if (!ok)
        throw std::runtime_error(std::string(why));
}
void plane(RenderScene &scene, float z, bool positive, unsigned material,
           unsigned emission = no_area_emission) {
    GeometryMesh mesh;
    mesh.positions = {{-32, -32, z}, {32, -32, z}, {32, 32, z}, {-32, 32, z}};
    mesh.triangles = positive ? std::vector<std::array<unsigned, 3>>{{0, 1, 2}, {0, 2, 3}}
                              : std::vector<std::array<unsigned, 3>>{{0, 2, 1}, {0, 3, 2}};
    const auto index = unsigned(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({index, {}});
    scene.instance_materials.push_back(material);
    scene.instance_emissions.push_back(emission);
    scene.instance_surface_transforms.emplace_back();
}
void frames(RenderScene &scene) {
    bool needs_frames = false;
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i)
        needs_frames = needs_frames || instance_uses_texture_context(scene, i);
    if (needs_frames)
        scene.instance_texture_frames.resize(scene.geometry.instances.size());
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        if (!instance_uses_texture_context(scene, i))
            continue;
        const auto &transform = scene.instance_surface_transforms[i];
        const auto error = prepare_texture_triangle_frames(
            scene.geometry.meshes[scene.geometry.instances[i].mesh_index], transform.shape,
            transform.placement, scene.instance_texture_frames[i]);
        need(error.empty(), error);
    }
    const auto diagnostic = validate_render_scene(scene);
    need(diagnostic.empty(), "Invalid synthetic hero scene: " + diagnostic);
}
RenderScene base() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = SpectralTransport::hero;
    scene.max_depth = 5;
    scene.raster_camera = RasterCamera{{}, {-.05f, -.05f, 1}, {.1f, 0, 0}, {0, .1f, 0}};
    return scene;
}
SpectralMaterial glass(bool thin = false, bool matched = false) {
    SpectralMaterial material;
    material.kind = SpectralMaterialKind::constant;
    material.scattering = thin ? ScatteringKind::thin_dielectric : ScatteringKind::dielectric;
    material.eta.knots = matched
                             ? std::vector<SpectralKnot>{{360, 1}, {830, 1}}
                             : std::vector<SpectralKnot>{{360, 1.7f}, {500, 1.52f}, {830, 1.4f}};
    material.eta.extend_endpoints = true;
    return material;
}
RenderScene diffuse() {
    auto scene = base();
    SpectralMaterial material;
    material.kind = SpectralMaterialKind::constant;
    material.constant = .6f;
    scene.materials.push_back(material);
    plane(scene, 3, false, 0);
    scene.environment.fill(.2f);
    PointLight light;
    light.position = {.75f, .5f, 1};
    light.intensity.fill(4);
    scene.point_lights.push_back(light);
    frames(scene);
    return scene;
}
RenderScene dispersive() {
    auto scene = base();
    scene.max_depth = 6;
    scene.materials.push_back(glass());
    SpectralMaterial black;
    black.kind = SpectralMaterialKind::constant;
    scene.materials.push_back(black);
    AreaEmission source;
    source.radiance.fill(.7f);
    source.two_sided = false;
    scene.area_emissions.push_back(source);
    plane(scene, 2, false, 0);
    plane(scene, 2.25f, true, 0);
    plane(scene, 5, false, 1, 0);
    frames(scene);
    return scene;
}
// Analytic early endpoint: the first one-sided emitter is a matched-index thin
// sheet. Every camera path receives E before continuing to dispersive glass.
// A reflected path sees the emitter's nonemitting BACK face; all other lights
// are black. Thus every wavelength's entire path radiance is exactly E. This
// catches retroactively applying a later single-hero mask/PDF to earlier light.
RenderScene early_emission(bool later_glass) {
    auto scene = base();
    scene.max_depth = later_glass ? 4 : 0;
    scene.materials.push_back(glass(true, true));
    AreaEmission source;
    source.radiance.fill(emitted);
    source.two_sided = false;
    scene.area_emissions.push_back(source);
    plane(scene, 1, false, 0, 0);
    if (later_glass) {
        scene.materials.push_back(glass());
        plane(scene, 2, false, 1);
    }
    frames(scene);
    return scene;
}
RenderScene deep(unsigned depth) {
    auto scene = base();
    scene.max_depth = depth;
    scene.materials.push_back(glass(true, true));
    SpectralMaterial black;
    black.kind = SpectralMaterialKind::constant;
    scene.materials.push_back(black);
    AreaEmission source;
    source.radiance.fill(emitted);
    source.two_sided = false;
    scene.area_emissions.push_back(source);
    // 65 deterministic straight-through scatterings, then the only emitter.
    // A depth-64 control cannot reach it; merely admitting the mode flag fails.
    for (unsigned i = 1; i <= 65; ++i)
        plane(scene, float(i), false, 0);
    plane(scene, 66, false, 1, 0);
    frames(scene);
    return scene;
}
std::unique_ptr<MetalRenderScene> offline(const RenderScene &scene) {
    auto result =
        build_metal_renderer(scene, {}, {.intersection_profile = IntersectionProfile::native});
    need(result.status == GeometryStatus::success && result.scene, result.diagnostic);
    return std::move(result.scene);
}
RenderSamplesResult sample(MetalRenderScene &renderer, std::uint64_t index, unsigned width = 3,
                           unsigned height = 2,
                           HeroWavelengthSampling mode = HeroWavelengthSampling::visible) {
    auto result = renderer.sample({.width = width,
                                   .height = height,
                                   .seed = seed,
                                   .material_seed = -11,
                                   .pixel_count = width * height,
                                   .sample_begin = index,
                                   .sample_count = 1,
                                   .total_samples = index + 1});
    need(result.status == GeometryStatus::success, result.diagnostic);
    need(result.samples.empty() && result.packet_samples.size() == width * height &&
             result.packet_splats.empty() && result.guard_count > 0,
         "Hero packet ownership/guards");
    for (unsigned i = 0; i < result.packet_samples.size(); ++i) {
        const auto &owner = result.packet_samples[i];
        need(owner.pixel == i && owner.sample == index && owner.splat_count == 0,
             "Hero full-width sample identity");
        for (unsigned k = 0; k < 4; ++k) {
            const float expected_pdf =
                mode == HeroWavelengthSampling::visible
                    ? rypath_packet_spectrum::visible_pdf(owner.wavelengths[k])
                    : 1.f / 470.f;
            need(std::isfinite(owner.radiance[k]) && owner.radiance[k] >= 0 &&
                     std::isfinite(owner.wavelengths[k]) && owner.wavelengths[k] >= 360 &&
                     owner.wavelengths[k] <= 830 && owner.wavelength_pdfs[k] > 0 &&
                     std::abs(owner.wavelength_pdfs[k] - expected_pdf) <= 3e-6 * expected_pdf,
                 "Original four hero sensor PDFs retained for the selected wavelength policy");
        }
    }
    return result;
}
FrameConfig config(FrameEstimator estimator = FrameEstimator::independent) {
    FrameConfig c;
    c.width = 3;
    c.height = 2;
    c.seed = seed;
    c.material_seed = -11;
    c.estimator = estimator;
    c.mutation_steps = estimator == FrameEstimator::area_replay ? 1 : 0;
    c.capacity = 2;
    c.quantum = 2;
    return c;
}
std::unique_ptr<FrameSession> session(const RenderScene &scene, const FrameConfig &c) {
    auto built = build_frame_session(scene, c);
    need(built.status == GeometryStatus::success && built.session, built.diagnostic);
    return std::move(built.session);
}
FrameCapture capture(FrameSession &session, const FrameConfig &c, std::uint64_t index,
                     bool history) {
    auto submitted = session.submit({.sample_index = index, .epoch = 73});
    need(submitted.status == GeometryStatus::success && submitted.frame && !submitted.busy,
         submitted.diagnostic);
    const auto info = submitted.frame->wait();
    const std::uint64_t pixels = std::uint64_t(c.width) * c.height;
    need(info.status == FrameStatus::complete, info.diagnostic);
    need(!info.enhanced_pair_compaction && info.compacted_null_evaluations == 0 &&
             !info.enhanced_temporal_reprojection && !info.enhanced_temporal_reprojection_used &&
             info.enhanced_geometry_generated == 0 && info.enhanced_geometry_reused == 0 &&
             info.enhanced_motion_routes == std::array<std::uint64_t, 4>{},
         "Enhanced execution features remain inactive outside Enhanced Area");
    need(info.sample_index == index && info.epoch == 73 && info.completed_samples == pixels &&
             info.fresh_candidates == pixels && info.command_count > 0,
         "Hero frame identity/accounting");
    if (c.estimator == FrameEstimator::area_replay) {
        need(info.history_used == history && info.temporal_candidates == (history ? 2u : 1u) &&
                 info.spatial_candidates == 5 && info.history_generation == (history ? 2u : 1u) &&
                 info.completed_evaluations + info.cached_evaluations ==
                     pixels * ((history ? 7u : 5u) + c.mutation_steps) &&
                 info.mutation_attempts == pixels * c.mutation_steps &&
                 info.mutation_accepts <= info.mutation_attempts,
             "Hero Area replay/mutation accounting");
    } else {
        need(!info.history_used && info.completed_evaluations == pixels &&
                 info.mutation_attempts == 0,
             "Hero independent accounting");
    }
    auto image = submitted.frame->capture();
    need(image.status == GeometryStatus::success && image.width == c.width &&
             image.height == c.height && image.raw_xyz.size() == pixels,
         image.diagnostic);
    for (const auto &xyz : image.raw_xyz)
        for (float v : xyz)
            need(std::isfinite(v), "Finite raw hero XYZ");
    return image;
}
void compare_sensor(const FrameCapture &image, const RenderSamplesResult &samples) {
    need(image.raw_xyz.size() == samples.packet_samples.size(), "Matched sensor dimensions");
    for (std::size_t i = 0; i < image.raw_xyz.size(); ++i) {
        const auto &owner = samples.packet_samples[i];
        // Independent host CIE/sensor reduction, not hero likelihood helpers.
        const auto expected = packet_sensor_xyz(owner, owner.radiance);
        for (unsigned c = 0; c < 3; ++c)
            need(std::abs(double(image.raw_xyz[i][c]) - expected[c]) <=
                     3e-5 * std::abs(expected[c]) + 3e-6,
                 "Offline/wavefront matched hero sensor");
    }
}
void exact(const FrameCapture &a, const FrameCapture &b) {
    need(a.raw_xyz.size() == b.raw_xyz.size(), "Scheduling comparison dimensions");
    for (std::size_t i = 0; i < a.raw_xyz.size(); ++i)
        for (unsigned c = 0; c < 3; ++c)
            need(std::bit_cast<std::uint32_t>(a.raw_xyz[i][c]) ==
                     std::bit_cast<std::uint32_t>(b.raw_xyz[i][c]),
                 "Scheduling/replay raw XYZ bits");
}
void offline_wavefront(const RenderScene &scene) {
    auto direct = offline(scene);
    const auto c = config();
    auto frames = session(scene, c);
    for (auto index : {std::uint64_t(0), high_sample}) {
        const auto samples = sample(*direct, index, 3, 2, scene.hero_wavelengths);
        compare_sensor(capture(*frames, c, index, false), samples);
    }
}
void scheduling(const RenderScene &scene) {
    struct Mode {
        FramePacketLayout layout;
        unsigned events;
    };
    const std::array<Mode, 7> modes{{{FramePacketLayout::packed, 1},
                                     {FramePacketLayout::packed, 2},
                                     {FramePacketLayout::packed, 4},
                                     {FramePacketLayout::hot_cold, 1},
                                     {FramePacketLayout::hot_cold, 2},
                                     {FramePacketLayout::hot_cold, 4},
                                     {FramePacketLayout::split_trace, 1}}};
    for (auto estimator : {FrameEstimator::independent, FrameEstimator::area_replay}) {
        std::array<FrameCapture, 2> reference;
        for (std::size_t mode = 0; mode < modes.size(); ++mode) {
            auto c = config(estimator);
            c.mutation_policy = FrameMutationPolicy::spectral_refresh;
            c.area_stencil = FrameAreaStencil::reciprocal16;
            c.packet_layout = modes[mode].layout;
            c.packet_events = modes[mode].events;
            c.capacity = mode % 2 ? 5 : 2;
            c.quantum = mode % 2 ? 1 : 2;
            auto frames = session(scene, c);
            for (unsigned f = 0; f < 2; ++f) {
                auto image = capture(*frames, c, high_sample + f,
                                     f != 0 && estimator == FrameEstimator::area_replay);
                if (mode == 0)
                    reference[f] = std::move(image);
                else
                    exact(image, reference[f]);
            }
        }
    }
}
void visible_mapping_and_area0() {
    auto scene = early_emission(false);
    auto direct = offline(scene);
    auto scalar_scene = scene;
    scalar_scene.spectral_transport = SpectralTransport::scalar;
    auto scalar = offline(scalar_scene);
    // Scalar diagnostic words expose the same initial wavelength coordinate,
    // independently of the hero implementation and later transport choices.
    const auto diagnostic = scalar->sample({.width = 3,
                                            .height = 2,
                                            .seed = seed,
                                            .material_seed = -11,
                                            .pixel_count = 6,
                                            .sample_begin = high_sample,
                                            .sample_count = 1,
                                            .total_samples = high_sample + 1});
    need(diagnostic.status == GeometryStatus::success && diagnostic.samples.size() == 6,
         "Scalar wavelength-coordinate diagnostic");
    const auto result = sample(*direct, high_sample);
    for (unsigned pixel = 0; pixel < 6; ++pixel) {
        const auto mapped = rypath_hero::sample_visible(
            rypath_replay::uniform(diagnostic.samples[pixel].random_words[2]));
        for (unsigned k = 0; k < 4; ++k) {
            need(std::abs(result.packet_samples[pixel].wavelengths[k] - mapped.lambda[k]) <= .001f,
                 "CPU/MSL visible inverse-CDF mapping at identical renderer coordinates");
            need(std::abs(result.packet_samples[pixel].wavelength_pdfs[k] - mapped.pdf[k]) <=
                     3e-5 * mapped.pdf[k],
                 "CPU/MSL visible per-lane sensor PDF mapping");
        }
    }
    auto c = config(FrameEstimator::area_replay);
    c.width = 35;
    c.height = 19;
    c.capacity = 128;
    c.mutation_steps = 0;
    c.area_stencil = FrameAreaStencil::reciprocal16;
    auto area = session(scene, c);
    auto submitted = area->submit({.sample_index = high_sample, .epoch = 73});
    need(submitted.status == GeometryStatus::success && submitted.frame, submitted.diagnostic);
    const auto info = submitted.frame->wait();
    need(info.status == FrameStatus::complete && !info.history_used &&
             info.completed_evaluations == std::uint64_t(c.width) * c.height * 5 &&
             info.mutation_attempts == 0,
         "Hero-visible Area0 full replay accounting");
    const auto image = submitted.frame->capture();
    need(image.status == GeometryStatus::success, image.diagnostic);
    auto analytic = [&](unsigned pixel) {
        const auto payload =
            rypath_replay::fresh({pixel, unsigned(high_sample), unsigned(high_sample >> 32),
                                  unsigned(seed), unsigned(seed >> 32)});
        const auto wavelengths =
            rypath_hero::sample_visible(rypath_replay::uniform(payload.wavelength));
        std::array<double, 3> xyz{};
        for (unsigned k = 0; k < 4; ++k) {
            const auto cie = pbrt_cie_xyz(wavelengths.lambda[k]);
            for (unsigned channel = 0; channel < 3; ++channel)
                xyz[channel] += cie[channel] * emitted / (4 * wavelengths.pdf[k]);
        }
        return xyz;
    };
    const auto d = info.area_stencil_offsets;
    for (unsigned pixel = 0; pixel < c.width * c.height; ++pixel) {
        auto expected = analytic(pixel);
        for (auto &v : expected)
            v *= .5;
        for (unsigned slot = 0; slot < 4; ++slot) {
            const int sign = slot % 2 ? -1 : 1;
            const int x = int(pixel % c.width) + sign * d[slot / 2 * 2];
            const int y = int(pixel / c.width) + sign * d[slot / 2 * 2 + 1];
            const unsigned neighbor = x >= 0 && y >= 0 && x < int(c.width) && y < int(c.height)
                                          ? unsigned(y) * c.width + unsigned(x)
                                          : pixel;
            const auto value = analytic(neighbor);
            for (unsigned channel = 0; channel < 3; ++channel)
                expected[channel] += .125 * value[channel];
        }
        for (unsigned channel = 0; channel < 3; ++channel)
            need(std::abs(image.raw_xyz[pixel][channel] - expected[channel]) <=
                     3e-5 * std::abs(expected[channel]) + 3e-6,
                 "Analytic constant-emitter hero-visible sensor integral and cold Area0 mapping");
    }
}
void transmission_witness() {
    auto scene = dispersive();
    // The complete camera cone has positive x/y components: even filter-edge
    // rays are strictly off normal. Each quarter-shifted wavelength lies on a
    // strictly decreasing eta curve (360:1.7, 500:1.52, 830:1.4), so the first
    // smooth transmission has mutually distinct Snell directions. Only the
    // hero has Dirac support on the sampled direction. The only light is the
    // emitter BEHIND BOTH glass planes; neither glass reflects/emits light of
    // its own and the environment is black. Positive output therefore proves
    // actual transmission, rather than just backend agreement on black paths.
    scene.raster_camera = RasterCamera{{}, {.1f, .1f, 1}, {.01f, 0, 0}, {0, .01f, 0}};
    auto direct = offline(scene);
    auto c = config();
    c.width = c.height = 1;
    auto wavefront = session(scene, c);
    unsigned positive = 0, positive_wavefront = 0;
    // Fixed in advance; do not search, retry or replace failed/null paths.
    for (unsigned i = 0; i < 8; ++i) {
        const auto index = high_sample + i;
        const auto result = sample(*direct, index, 1, 1);
        const auto &owner = result.packet_samples[0];
        if (owner.radiance[0] > 0) {
            ++positive;
            // On every successful off-normal slab path, only the hero has
            // spectral Dirac support. The spectral mixture contributes factor4;
            // entrance/exit radiance eta factors cancel, and every Fresnel
            // branch mass cancels its own sampling probability. The black
            // environment adds nothing: the surviving hero MUST carry4*.7.
            const float expected = 4 * .7f;
            need(std::abs(owner.radiance[0] - expected) <=
                     64 * std::numeric_limits<float>::epsilon() * expected,
                 "Successful dispersive slab has analytic4*.7 hero radiance");
            for (unsigned k = 1; k < 4; ++k)
                need(owner.radiance[k] == 0,
                     "Off-normal smooth dispersion retains only compatible Dirac support");
        } else {
            for (float value : owner.radiance)
                need(value == 0, "Glass reflection/null paths cannot illuminate the rear emitter");
        }
        // sample() separately requires all FOUR original sensor PDFs, even
        // when the path's physical support has become hero-only.
        const auto image = capture(*wavefront, c, index, false);
        compare_sensor(image, result);
        positive_wavefront += std::ranges::any_of(image.raw_xyz[0], [](float v) { return v > 0; });
    }
    need(positive > 0 && positive_wavefront > 0,
         "Predeclared dispersive-glass samples must include real positive transmission");

    // Independent analytic control: two solid matched-index interfaces have
    // Fresnel R=0, T=1 and eta scaling 1 at every wavelength and incidence.
    // All four lanes must reach the constant .7 emitter with exactly .7.
    scene.materials[0] = glass(false, true);
    frames(scene);
    auto matched = offline(scene);
    const auto result = sample(*matched, high_sample, 1, 1);
    for (float value : result.packet_samples[0].radiance)
        need(value == .7f, "Matched solid-glass transmission has analytic unit throughput");

    // Nontrivial eta, shared by all lanes: no spectral support is lost. Every
    // successful path through the parallel slab has eta_in*eta_out=1 and
    // throughput1, including any internal Fresnel reflections. Each lane must
    // carry .7 rather than the dispersive hero-only4*.7. Fix the same8 indices;
    // reflection/null paths remain recorded black outcomes, never retried.
    scene.materials[0].eta.knots = {{360, 1.5f}, {830, 1.5f}};
    frames(scene);
    auto constant_eta = offline(scene);
    unsigned positive_constant = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const auto sample_result = sample(*constant_eta, high_sample + i, 1, 1);
        const auto &owner = sample_result.packet_samples[0];
        if (owner.radiance[0] > 0) {
            ++positive_constant;
            for (float value : owner.radiance)
                need(std::abs(value - .7f) <= 64 * std::numeric_limits<float>::epsilon() * .7f,
                     "Successful constant-eta slab retains analytic .7 in all four lanes");
        } else {
            for (float value : owner.radiance)
                need(value == 0, "Constant-eta slab reflection/null remains black");
        }
    }
    need(positive_constant > 0, "Fixed constant-eta slab samples include actual transmission");
}
void earlier_emission() {
    auto control = offline(early_emission(false));
    const auto scene = early_emission(true);
    auto later = offline(scene);
    auto c = config();
    c.width = c.height = 1;
    auto frames = session(scene, c);
    for (unsigned i = 0; i < 8; ++i) {
        const auto index = high_sample + i;
        const auto first = sample(*control, index, 1, 1), after = sample(*later, index, 1, 1);
        const auto &a = first.packet_samples[0], &b = after.packet_samples[0];
        need(a.wavelengths == b.wavelengths && a.wavelength_pdfs == b.wavelength_pdfs,
             "A later dispersive event cannot rewrite earlier spectral sampling");
        for (unsigned k = 0; k < 4; ++k)
            need(a.radiance[k] == emitted && b.radiance[k] == emitted,
                 "Earlier analytic emission remains E in all four original lanes");
        compare_sensor(capture(*frames, c, index, false), first);
    }
}
void depth65() {
    auto reaches = deep(65), stops = deep(64);
    auto direct = offline(reaches), control = offline(stops);
    const auto full = sample(*direct, high_sample, 1, 1),
               shortened = sample(*control, high_sample, 1, 1);
    for (unsigned k = 0; k < 4; ++k) {
        need(full.packet_samples[0].radiance[k] == emitted, "Depth65 actually reaches the emitter");
        need(shortened.packet_samples[0].radiance[k] == 0,
             "Depth64 negative control remains black");
    }
    auto c = config();
    c.width = c.height = 1;
    auto frames = session(reaches, c);
    compare_sensor(capture(*frames, c, high_sample, false), full);
}
struct ContributionCapture {
    FrameCapture image;
    FrameInfo info;
};
ContributionCapture contribution_capture(FrameSession &session, const FrameConfig &c,
                                         unsigned depth, unsigned ordinal,
                                         std::uint64_t user_sample) {
    auto submitted = session.submit({.sample_index = user_sample, .epoch = 73});
    need(submitted.status == GeometryStatus::success && submitted.frame && !submitted.busy,
         submitted.diagnostic);
    auto info = submitted.frame->wait();
    need(info.status == FrameStatus::complete, info.diagnostic);
    const auto pixels = std::uint64_t(c.width) * c.height;
    const bool history = ordinal != 0;
    const auto logical = pixels * ((history ? 7u : 5u) + c.mutation_steps);
    const auto cached = history && c.cache_temporal_evaluations ? 2 * pixels : 0;
    need(info.width == c.width && info.height == c.height && info.sample_index == user_sample &&
             info.epoch == 73 && info.area_serial == ordinal &&
             info.history_generation == std::uint64_t(ordinal) + 1 &&
             info.area_path_domain == c.area_path_domain &&
             info.contribution_replay == (c.area_path_domain == FrameAreaPathDomain::contribution
                                              ? c.contribution_replay
                                              : FrameContributionReplay::full) &&
             info.contribution_depth ==
                 (c.area_path_domain == FrameAreaPathDomain::contribution ? depth : 0u) &&
             info.history_used == history && info.temporal_candidates == (history ? 2u : 1u) &&
             info.spatial_candidates == 5 && info.completed_samples == pixels &&
             info.fresh_candidates == pixels && info.logical_evaluations == logical &&
             info.completed_evaluations == logical - cached && info.cached_evaluations == cached &&
             info.temporal_evaluation_cache_used == (cached != 0) &&
             info.mutation_policy == c.mutation_policy && info.area_stencil == c.area_stencil &&
             info.mutation_attempts == pixels * c.mutation_steps &&
             info.mutation_accepts <= info.mutation_attempts && info.command_count > 0,
         "Contribution Area identity/domain/serial/cache accounting");
    auto image = submitted.frame->capture();
    need(image.status == GeometryStatus::success && image.width == c.width &&
             image.height == c.height && image.raw_xyz.size() == pixels,
         image.diagnostic);
    for (const auto &xyz : image.raw_xyz)
        for (float value : xyz)
            need(std::isfinite(value) && value >= 0, "Finite nonnegative contribution XYZ");
    return {std::move(image), std::move(info)};
}
FrameConfig contribution_config() {
    auto c = config(FrameEstimator::area_replay);
    c.width = 4;
    c.height = 2; // Capacity 7 must remain distinct from capacity 2.
    c.area_path_domain = FrameAreaPathDomain::contribution;
    c.contribution_replay = FrameContributionReplay::full;
    c.mutation_policy = FrameMutationPolicy::spectral_refresh;
    c.area_stencil = FrameAreaStencil::reciprocal16;
    // Keep traversal deterministic when comparing scheduling and estimator domains.
    c.intersection_profile = IntersectionProfile::compensated;
    return c;
}
RenderScene contribution_coating() {
    auto scene = early_emission(false);
    scene.max_depth = 4;
    scene.materials[0].scattering = ScatteringKind::coated_diffuse;
    scene.materials[0].constant = .6f;
    scene.materials[0].eta.knots = {{360, 1.5f}, {830, 1.5f}};
    scene.materials[0].surface.albedo = .2f;
    scene.materials[0].surface.samples = 2;
    frames(scene);
    return scene;
}
RenderScene contribution_crossings() {
    auto scene = diffuse();
    SpectralMaterial boundary;
    boundary.scattering = ScatteringKind::null_interface;
    scene.materials.push_back(boundary);
    plane(scene, 1, false, 1);
    plane(scene, 2, false, 0);
    scene.area_emissions.emplace_back();
    scene.area_emissions.back().radiance.fill(0);
    scene.instance_emissions[2] = 0;
    scene.texture_graph.nodes.emplace_back();
    scene.instance_alpha_roots = {UINT32_MAX, UINT32_MAX, 0};
    scene.instance_invisible_emitters = {0, 0, 1};
    frames(scene);
    return scene;
}
void contribution_rejections() {
    const auto reject = [](const RenderScene &scene, const FrameConfig &c) {
        const auto result = build_frame_session(scene, c);
        need(result.status == GeometryStatus::failure && !result.session &&
                 !result.diagnostic.empty(),
             "Invalid contribution configuration must fail explicitly");
    };
    auto c = contribution_config();
    auto too_deep = diffuse();
    too_deep.max_depth = 66;
    reject(too_deep, c);
    for (auto spectral : {SpectralTransport::scalar, SpectralTransport::packet}) {
        auto scene = diffuse();
        scene.spectral_transport = spectral;
        reject(scene, c);
    }
    auto independent = c;
    independent.estimator = FrameEstimator::independent;
    reject(diffuse(), independent);
    auto mega = c;
    mega.execution = FrameExecution::megakernel;
    reject(diffuse(), mega);
    for (auto layout : {FramePacketLayout::hot_cold, FramePacketLayout::split_trace}) {
        auto invalid = c;
        invalid.packet_layout = layout;
        reject(diffuse(), invalid);
    }
    for (auto events : {2u, 4u}) {
        auto invalid = c;
        invalid.packet_events = events;
        reject(diffuse(), invalid);
    }
}
void contribution_scheduling() {
    for (const auto &scene : {diffuse(), dispersive(), early_emission(true), contribution_coating(),
                              contribution_crossings()}) {
        auto reference_config = contribution_config();
        reference_config.cache_temporal_evaluations = false;
        auto cached_config = reference_config;
        cached_config.cache_temporal_evaluations = true;
        auto wide_config = cached_config;
        wide_config.capacity = 7;
        wide_config.quantum = 1;
        auto reference = session(scene, reference_config), cached = session(scene, cached_config),
             wide = session(scene, wide_config);
        const std::uint64_t labels[] = {high_sample, high_sample, high_sample + 1};
        for (unsigned ordinal = 0; ordinal < 3; ++ordinal) {
            const auto a = contribution_capture(*reference, reference_config, scene.max_depth,
                                                ordinal, labels[ordinal]);
            const auto b = contribution_capture(*cached, cached_config, scene.max_depth, ordinal,
                                                labels[ordinal]);
            const auto d =
                contribution_capture(*wide, wide_config, scene.max_depth, ordinal, labels[ordinal]);
            exact(a.image, b.image);
            exact(a.image, d.image);
            need(a.info.area_stencil_offsets == b.info.area_stencil_offsets &&
                     a.info.area_stencil_offsets == d.info.area_stencil_offsets &&
                     a.info.mutation_accepts == b.info.mutation_accepts &&
                     a.info.mutation_accepts == d.info.mutation_accepts,
                 "Contribution queue/cache changes cannot change selection or mutation decisions");
            need(std::any_of(a.image.raw_xyz.begin(), a.image.raw_xyz.end(),
                             [](const auto &xyz) { return xyz[1] > 0; }),
                 "Contribution fixture must retain positive physical radiance");
        }
    }
}
RenderScene contribution_nee_only(bool coated = false, bool textured = false) {
    auto scene = diffuse();
    // No emissive surface or environment: a positive result witnesses NEE.
    scene.environment.fill(0);
    if (coated) {
        scene.materials[0].scattering = ScatteringKind::coated_diffuse;
        scene.materials[0].eta.knots = {{360, 1.5f}, {830, 1.5f}};
        scene.materials[0].surface.albedo = .2f;
        scene.materials[0].surface.samples = 2;
    }
    if (textured) {
        namespace graph = texture_graph_wire;
        // Same bounded procedural receiver pattern as area_frame_tests. This
        // exercises texture coordinates and footprints without external assets.
        scene.texture_graph.nodes = {
            {.kind = graph::constant_node, .type = graph::spectrum, .values = {.2f}},
            {.kind = graph::constant_node, .type = graph::spectrum, .values = {.8f}},
            {.kind = graph::checkerboard, .type = graph::spectrum, .children = {0, 1}}};
        auto &mapping = scene.texture_graph.nodes.back().mapping;
        mapping.kind = graph::spatial;
        mapping.texture_from_render[0] = 1.f / 1.75f;
        mapping.texture_from_render[5] = 1.f / 2.5f;
        mapping.texture_from_render[11] = .125f;
        scene.materials[0].kind = SpectralMaterialKind::texture_graph;
        scene.materials[0].constant = 0;
        scene.materials[0].texture_root = 2;
    }
    frames(scene);
    return scene;
}
void contribution_prefix_scheduling() {
    const std::array scenes{
        contribution_nee_only(), contribution_nee_only(true), dispersive(),
        early_emission(true),    contribution_crossings(),    contribution_nee_only(false, true)};
    for (std::size_t fixture = 0; fixture < scenes.size(); ++fixture) {
        std::cout << "Hero contribution: prefix fixture " << fixture << std::endl;
        const auto &scene = scenes[fixture];
        auto full_config = contribution_config();
        full_config.cache_temporal_evaluations = false;
        auto prefix_config = full_config;
        prefix_config.contribution_replay = FrameContributionReplay::prefix;
        auto cached_config = prefix_config;
        cached_config.cache_temporal_evaluations = true;
        cached_config.capacity = 7;
        cached_config.quantum = 1;
        auto full = session(scene, full_config), prefix = session(scene, prefix_config),
             cached = session(scene, cached_config);
        const std::uint64_t labels[] = {high_sample, high_sample, high_sample + 1};
        for (unsigned ordinal = 0; ordinal < 3; ++ordinal) {
            const auto a =
                contribution_capture(*full, full_config, scene.max_depth, ordinal, labels[ordinal]);
            const auto b = contribution_capture(*prefix, prefix_config, scene.max_depth, ordinal,
                                                labels[ordinal]);
            const auto d = contribution_capture(*cached, cached_config, scene.max_depth, ordinal,
                                                labels[ordinal]);
            exact(a.image, b.image);
            exact(a.image, d.image);
            need(a.info.area_stencil_offsets == b.info.area_stencil_offsets &&
                     a.info.area_stencil_offsets == d.info.area_stencil_offsets &&
                     a.info.mutation_accepts == b.info.mutation_accepts &&
                     a.info.mutation_accepts == d.info.mutation_accepts &&
                     a.info.completed_evaluations == b.info.completed_evaluations &&
                     a.info.cached_evaluations == b.info.cached_evaluations &&
                     a.info.logical_evaluations == d.info.logical_evaluations,
                 "Selected-prefix replay preserves imports, mutation and cache accounting");
            need(std::any_of(a.image.raw_xyz.begin(), a.image.raw_xyz.end(),
                             [](const auto &xyz) { return xyz[1] > 0; }),
                 "Full/prefix fixture retains positive endpoint or direct radiance");
        }
        std::cout << "contribution_prefix_fixture=" << fixture
                  << " exact_full_prefix_cache_queue_frames=3\n";
    }
}
void contribution_single_endpoint() {
    // D0 has exactly one slot, hence no local WRS selection noise. The longer
    // witnesses also have only one nonzero atom. Their positive target floor is
    // reduced in BOTH arms to isolate retained transport from rare zero-slot
    // selections: 2^-40*1e-6 remains normal but is below FLOAT score resolution.
    // The 64 epsilon gate covers the additional bounded score/readout roundings;
    // it is a numerical allowance, not a statistical or percent-bias allowance.
    for (const auto &scene : {early_emission(false), deep(65), early_emission(true)}) {
        auto c = contribution_config();
        c.width = c.height = 1;
        c.target_scale = scene.max_depth == 0 ? 1.f : 0x1p-40f;
        need(std::isnormal(c.target_scale) && std::isnormal(c.target_scale * 1e-6f),
             "Analytic witness retains a normal positive support floor");
        auto full_config = c;
        full_config.area_path_domain = FrameAreaPathDomain::summed;
        auto full = session(scene, full_config), atoms = session(scene, c);
        for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
            const auto a =
                contribution_capture(*full, full_config, scene.max_depth, ordinal, high_sample);
            const auto b = contribution_capture(*atoms, c, scene.max_depth, ordinal, high_sample);
            for (unsigned channel = 0; channel < 3; ++channel) {
                const double x = a.image.raw_xyz[0][channel], y = b.image.raw_xyz[0][channel];
                const double tolerance = 64 * double(std::numeric_limits<float>::epsilon()) *
                                         std::max(std::abs(x), std::abs(y));
                need(std::abs(x - y) <= tolerance,
                     "Single-endpoint contribution retains summed spectral radiance");
            }
            need(a.image.raw_xyz[0][1] > 0 && b.image.raw_xyz[0][1] > 0,
                 "Depth0/depth65/earlier-emission contribution witness must be positive");
        }
    }
}
void contribution_multileaf_mean() {
    // One diffuse bounce has distinct direct-point and environment atoms. Give
    // the point source a different spectrum so selected atoms are not trivially
    // proportional XYZ. Replication is over 32 INDEPENDENT RUN SEEDS, not the
    // spatially correlated pixels or temporal frames. The predeclared 5 empirical
    // SE gate (per XYZ channel) plus 128 epsilon rounding allowance is a bounded
    // bias smoke test; neither normality nor an unbiasedness proof is claimed.
    constexpr unsigned repeats = 32;
    auto scene = diffuse();
    scene.max_depth = 1;
    for (unsigned i = 0; i < scene.point_lights[0].intensity.size(); ++i)
        scene.point_lights[0].intensity[i] = i < 200 ? 4.f : .25f;
    frames(scene);
    std::array<std::array<double, 3>, repeats> differences{};
    std::array<double, 3> summed_mean{}, contribution_mean{};
    std::cout << std::setprecision(17);
    for (unsigned n = 0; n < repeats; ++n) {
        auto c = contribution_config();
        c.seed = seed + 7919ull * n;
        need(c.target_scale == 1.f, "Multileaf smoke keeps the default target scale");
        auto full_config = c;
        full_config.area_path_domain = FrameAreaPathDomain::summed;
        auto full = session(scene, full_config), atoms = session(scene, c);
        const auto a = contribution_capture(*full, full_config, scene.max_depth, 0, high_sample);
        const auto b = contribution_capture(*atoms, c, scene.max_depth, 0, high_sample);
        std::array<double, 3> x{}, y{};
        for (std::size_t p = 0; p < a.image.raw_xyz.size(); ++p)
            for (unsigned k = 0; k < 3; ++k) {
                x[k] += double(a.image.raw_xyz[p][k]) / double(a.image.raw_xyz.size());
                y[k] += double(b.image.raw_xyz[p][k]) / double(b.image.raw_xyz.size());
            }
        for (unsigned k = 0; k < 3; ++k) {
            differences[n][k] = y[k] - x[k];
            summed_mean[k] += x[k] / repeats;
            contribution_mean[k] += y[k] / repeats;
        }
        std::cout << "contribution_seed seed=" << c.seed << " summed_XYZ=" << x[0] << ',' << x[1]
                  << ',' << x[2] << " contribution_XYZ=" << y[0] << ',' << y[1] << ',' << y[2]
                  << " signed_delta_XYZ=" << differences[n][0] << ',' << differences[n][1] << ','
                  << differences[n][2] << '\n';
    }
    for (unsigned k = 0; k < 3; ++k) {
        double delta = 0, variance = 0;
        for (const auto &d : differences)
            delta += d[k] / repeats;
        for (const auto &d : differences)
            variance += (d[k] - delta) * (d[k] - delta) / double(repeats - 1);
        const double se = std::sqrt(variance / repeats);
        const double arithmetic =
            128 * double(std::numeric_limits<float>::epsilon()) *
            std::max(std::abs(summed_mean[k]), std::abs(contribution_mean[k]));
        const double bound = 5 * se + arithmetic;
        std::cout << "contribution_mean replicates=" << repeats << " channel=" << k
                  << " summed=" << summed_mean[k] << " contribution=" << contribution_mean[k]
                  << " signed_delta=" << delta << " empirical_SE=" << se << " bound=" << bound
                  << '\n';
        need(std::isfinite(se) && std::abs(delta) <= bound,
             "Independent-seed multileaf summed/contribution mean smoke gate");
    }
}
FrameConfig enhanced_config() {
    auto c = contribution_config();
    c.area_path_domain = FrameAreaPathDomain::enhanced;
    return c;
}
ContributionCapture enhanced_capture(FrameSession &frames, const FrameConfig &c, unsigned depth,
                                     unsigned ordinal, std::uint64_t label,
                                     std::optional<FrameCamera> camera = {},
                                     std::unique_ptr<Frame> *retained = nullptr,
                                     std::optional<bool> expected_cache = {}) {
    auto submitted =
        frames.submit({.sample_index = label, .epoch = 73, .camera = std::move(camera)});
    need(submitted.status == GeometryStatus::success && submitted.frame && !submitted.busy,
         submitted.diagnostic);
    auto info = submitted.frame->wait();
    need(info.status == FrameStatus::complete, info.diagnostic);
    const auto pixels = std::uint64_t(c.width) * c.height;
    const bool history = ordinal != 0;
    const auto logical = pixels * ((history ? 7u : 5u) + c.mutation_steps);
    const bool cache_used = expected_cache.value_or(history && c.cache_temporal_evaluations);
    const auto cached = cache_used ? 2 * pixels : 0;
    need(!cache_used || (history && c.cache_temporal_evaluations), "Cache expectation permission");
    need(info.width == c.width && info.height == c.height && info.sample_index == label &&
             info.epoch == 73 && info.area_serial == ordinal &&
             info.history_generation == std::uint64_t(ordinal) + 1 &&
             info.area_path_domain == FrameAreaPathDomain::enhanced &&
             info.contribution_replay == c.contribution_replay &&
             info.contribution_depth == depth && info.canonical_transport_version == 4 &&
             info.fresh_discovery_version == 1 && info.history_used == history &&
             info.temporal_candidates == (history ? 2u : 1u) && info.spatial_candidates == 5 &&
             info.completed_samples == pixels && info.fresh_candidates == pixels &&
             info.logical_evaluations == logical && info.completed_evaluations >= pixels &&
             info.completed_evaluations + info.analytic_null_evaluations +
                     info.cached_evaluations ==
                 logical &&
             info.cached_evaluations == cached &&
             info.temporal_evaluation_cache_used == cache_used &&
             info.mutation_policy == c.mutation_policy && info.area_stencil == c.area_stencil &&
             info.mutation_attempts == pixels * c.mutation_steps &&
             info.mutation_accepts <= info.mutation_attempts && info.command_count > 0,
         "Enhanced identity, canonical law, exact cache permission and physical/null accounting");
    auto image = submitted.frame->capture();
    need(image.status == GeometryStatus::success && image.width == c.width &&
             image.height == c.height && image.raw_xyz.size() == pixels,
         image.diagnostic);
    for (const auto &xyz : image.raw_xyz)
        for (float value : xyz)
            need(std::isfinite(value) && value >= 0, "Enhanced finite physical XYZ");
    if (retained)
        *retained = std::move(submitted.frame);
    return {std::move(image), std::move(info)};
}
void enhanced_black() {
    for (unsigned depth : {0u, 65u}) {
        auto scene = depth == 0 ? early_emission(false) : deep(65);
        scene.area_emissions[0].radiance.fill(0);
        frames(scene);
        auto full_config = enhanced_config();
        full_config.cache_temporal_evaluations = false;
        auto prefix_config = full_config;
        prefix_config.contribution_replay = FrameContributionReplay::prefix;
        prefix_config.cache_temporal_evaluations = true;
        prefix_config.capacity = 7;
        prefix_config.quantum = 1;
        auto full = session(scene, full_config), prefix = session(scene, prefix_config);
        for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
            const auto a = enhanced_capture(*full, full_config, depth, ordinal, high_sample);
            const auto b = enhanced_capture(*prefix, prefix_config, depth, ordinal, high_sample);
            exact(a.image, b.image);
            const auto pixels = std::uint64_t(full_config.width) * full_config.height;
            need(a.info.completed_evaluations == pixels && b.info.completed_evaluations == pixels &&
                     a.info.analytic_null_evaluations == a.info.logical_evaluations - pixels &&
                     b.info.analytic_null_evaluations ==
                         b.info.logical_evaluations - pixels - b.info.cached_evaluations &&
                     a.info.mutation_accepts == 0 && b.info.mutation_accepts == 0,
                 "All-black still walks fresh, keeps structural history and null mutation self");
            for (const auto &xyz : a.image.raw_xyz)
                for (float value : xyz)
                    need(std::bit_cast<std::uint32_t>(value) == 0,
                         "All-black null publishes canonical positive zero");
        }
    }
}
void enhanced_scheduling() {
    const std::array scenes{
        contribution_nee_only(), contribution_nee_only(true), dispersive(),
        early_emission(true),    contribution_crossings(),    contribution_nee_only(false, true)};
    for (std::size_t fixture = 0; fixture < scenes.size(); ++fixture) {
        std::cout << "Enhanced full/prefix fixture=" << fixture << std::endl;
        const auto &scene = scenes[fixture];
        auto full_config = enhanced_config();
        full_config.cache_temporal_evaluations = false;
        auto prefix_config = full_config;
        prefix_config.contribution_replay = FrameContributionReplay::prefix;
        auto wide_config = prefix_config;
        wide_config.cache_temporal_evaluations = true;
        wide_config.capacity = 7;
        wide_config.quantum = 1;
        auto full = session(scene, full_config), prefix = session(scene, prefix_config),
             wide = session(scene, wide_config);
        const std::uint64_t labels[] = {high_sample, high_sample, high_sample + 1};
        for (unsigned ordinal = 0; ordinal < 3; ++ordinal) {
            const auto a =
                enhanced_capture(*full, full_config, scene.max_depth, ordinal, labels[ordinal]);
            const auto b =
                enhanced_capture(*prefix, prefix_config, scene.max_depth, ordinal, labels[ordinal]);
            const auto d =
                enhanced_capture(*wide, wide_config, scene.max_depth, ordinal, labels[ordinal]);
            exact(a.image, b.image);
            exact(a.image, d.image);
            need(
                a.info.area_stencil_offsets == b.info.area_stencil_offsets &&
                    a.info.area_stencil_offsets == d.info.area_stencil_offsets &&
                    a.info.mutation_accepts == b.info.mutation_accepts &&
                    a.info.mutation_accepts == d.info.mutation_accepts &&
                    a.info.completed_evaluations == b.info.completed_evaluations &&
                    a.info.analytic_null_evaluations == b.info.analytic_null_evaluations &&
                    d.info.completed_evaluations <= a.info.completed_evaluations &&
                    d.info.analytic_null_evaluations <= a.info.analytic_null_evaluations &&
                    (a.info.completed_evaluations - d.info.completed_evaluations) +
                            (a.info.analytic_null_evaluations - d.info.analytic_null_evaluations) ==
                        d.info.cached_evaluations,
                "Enhanced execution, queue and requested-cache controls retain decisions");
            need(std::any_of(a.image.raw_xyz.begin(), a.image.raw_xyz.end(),
                             [](const auto &xyz) { return xyz[1] > 0; }),
                 "Enhanced fixture exercises positive endpoint or NEE");
        }
    }
}
void enhanced_single_endpoint() {
    // D0 has one atom. Deep matched-eta1 sheets have beta1 and no outer gate;
    // the later-dispersion fixture's only emitter precedes collapse. These
    // isolate canonical atom preservation from discovery/WRS variance.
    for (const auto &scene : {early_emission(false), deep(65), early_emission(true)}) {
        auto c = enhanced_config();
        c.width = c.height = 1;
        c.mutation_steps = 0;
        c.cache_temporal_evaluations = false;
        auto control = c;
        control.area_path_domain = FrameAreaPathDomain::summed;
        auto summed = session(scene, control), enhanced = session(scene, c);
        const auto a = contribution_capture(*summed, control, scene.max_depth, 0, high_sample);
        const auto b = enhanced_capture(*enhanced, c, scene.max_depth, 0, high_sample);
        for (unsigned k = 0; k < 3; ++k) {
            const double x = a.image.raw_xyz[0][k], y = b.image.raw_xyz[0][k];
            const double tolerance = 64 * double(std::numeric_limits<float>::epsilon()) *
                                     std::max(std::abs(x), std::abs(y));
            need(std::abs(x - y) <= tolerance,
                 "Enhanced single emitter retains depth and pre-collapse radiance");
        }
        need(a.image.raw_xyz[0][1] > 0 && b.image.raw_xyz[0][1] > 0,
             "Enhanced single-emitter witness is positive");
    }
}
void enhanced_null_transitions() {
    const auto scene = early_emission(false);
    auto c = enhanced_config();
    c.mutation_steps = 0;
    auto prefix_config = c;
    prefix_config.contribution_replay = FrameContributionReplay::prefix;
    prefix_config.capacity = 7;
    auto full = session(scene, c), prefix = session(scene, prefix_config);
    for (unsigned ordinal = 0; ordinal < 4; ++ordinal) {
        const bool black = ordinal == 0 || ordinal == 3;
        FrameCamera camera{.camera = scene.camera,
                           .camera_medium = scene.camera_medium,
                           .raster_camera = scene.raster_camera,
                           .perspective_lens = scene.perspective_lens,
                           .texture_camera = scene.texture_camera};
        need(camera.raster_camera.has_value(), "Null-transition camera is explicit");
        camera.raster_camera->origin.x = black ? 100.f : 0.f;
        const auto a =
            enhanced_capture(*full, c, 0, ordinal, high_sample, camera, nullptr, ordinal == 2);
        const auto b = enhanced_capture(*prefix, prefix_config, 0, ordinal, high_sample, camera,
                                        nullptr, ordinal == 2);
        exact(a.image, b.image);
        const auto pixels = std::uint64_t(c.width) * c.height;
        if (ordinal == 1)
            need(a.info.completed_evaluations == 6 * pixels &&
                     a.info.analytic_null_evaluations == pixels,
                 "History-null keeps fresh-real reverse evaluation structurally active");
        if (ordinal == 3)
            need(a.info.completed_evaluations == 2 * pixels &&
                     a.info.analytic_null_evaluations == 5 * pixels,
                 "Fresh-null preserves real-history forward evaluation");
        for (const auto &xyz : a.image.raw_xyz)
            need(black ? xyz[0] == 0 && xyz[1] == 0 && xyz[2] == 0 : xyz[1] > 0,
                 "Camera miss/emitter transitions actually change canonical support");
    }
}
template <typename T> bool hybrid_same_words(const T &a, const T &b) {
    static_assert(sizeof(T) % sizeof(std::uint32_t) == 0);
    using Words = std::array<std::uint32_t, sizeof(T) / sizeof(std::uint32_t)>;
    return std::bit_cast<Words>(a) == std::bit_cast<Words>(b);
}
struct HybridWitnesses {
    std::uint64_t fresh = 0, nonself_edges = 0, imported = 0;
};
HybridWitnesses hybrid_witnesses(const EnhancedSessionSnapshot &snapshot, const FrameInfo &info,
                                 unsigned depth) {
    need(snapshot.status == GeometryStatus::success, snapshot.diagnostic);
    const auto pixels = std::uint32_t(info.width * info.height);
    need(snapshot.width == 17 && snapshot.height == 17 && snapshot.width == info.width &&
             snapshot.height == info.height && snapshot.serial == info.area_serial &&
             snapshot.generation == info.history_generation && snapshot.epoch == info.epoch &&
             snapshot.history.size() == pixels && snapshot.temporal.size() == pixels &&
             snapshot.records.size() == std::size_t(pixels) * enhanced_wire::bank_count &&
             !info.history_used && info.mutation_attempts == 0 && info.cached_evaluations == 0,
         "Hybrid witness requires an exact cold publication with immutable fresh temporal tuples");
    const auto positive = [](float fraction, int exponent) {
        return std::isfinite(fraction) && fraction >= .5f && fraction < 1.f && exponent >= -4096 &&
               exponent <= 4096;
    };
    const auto record = [&](const auto &tuple) -> const enhanced_wire::Record * {
        if (!positive(tuple.normalizer, tuple.normalizer_exponent) ||
            !positive(tuple.target, tuple.target_exponent) ||
            !enhanced_wire::valid(tuple.record, pixels))
            return nullptr;
        const auto &value =
            snapshot.records[std::size_t(tuple.record.bank) * pixels + tuple.record.pixel];
        if (value.tag != rypath_enhanced_chart::interior_zomega)
            return nullptr;
        need(std::isfinite(tuple.xyz.x) && std::isfinite(tuple.xyz.y) &&
                 std::isfinite(tuple.xyz.z) && tuple.xyz.x >= 0 && tuple.xyz.y >= 0 &&
                 tuple.xyz.z >= 0 && (tuple.xyz.x > 0 || tuple.xyz.y > 0 || tuple.xyz.z > 0),
             "Positive interior witness retains finite nonnegative nonzero physical XYZ");
        need(rypath_enhanced_chart::validate_chart(value, tuple.selected_slot, depth) ==
                 rypath_enhanced_chart::Status::ok,
             "Positive interior witness must have a structurally valid owned record");
        return &value;
    };
    const auto d = info.area_stencil_offsets;
    const rypath_area_stencil::Offsets offsets{{d[0], d[1]}, {d[2], d[3]}};
    HybridWitnesses result;
    for (std::uint32_t p = 0; p < pixels; ++p) {
        const auto &local = snapshot.temporal[p];
        if (record(local)) {
            need(local.record.bank == enhanced_wire::temporal_bank && local.record.pixel == p,
                 "Fresh witness resolves the immutable per-pixel temporal bank3 record");
            ++result.fresh;
        }
        const auto &destination = snapshot.history[p];
        const auto *selected = destination.valid == 1 ? record(destination) : nullptr;
        if (selected)
            need(destination.record.bank == snapshot.front_bank && destination.record.pixel == p,
                 "Published witness resolves the receiver-owned front history record");
        bool selected_donor = false;
        for (unsigned slot = 1; slot <= 4; ++slot) {
            const auto q = rypath_area_stencil::neighbor(p, info.width, info.height, offsets, slot);
            if (q == p)
                continue;
            ++result.nonself_edges;
            need(rypath_area_stencil::neighbor(q, info.width, info.height, offsets,
                                               rypath_area_stencil::inverse_slot(slot)) == p,
                 "Hybrid donor witness follows an actual reciprocal nonself edge");
            const auto &donor = snapshot.temporal[q];
            const auto *source = record(donor);
            if (selected && source && !hybrid_same_words(destination.payload, local.payload) &&
                hybrid_same_words(destination.payload, donor.payload) &&
                destination.selected_slot == donor.selected_slot &&
                hybrid_same_words(*selected, *source))
                selected_donor = true;
        }
        if (selected_donor)
            ++result.imported; // Unique destination pixels, even for duplicate stencil slots.
    }
    return result;
}
void enhanced_mean_contrast(const RenderScene &scene, std::string_view label,
                            std::optional<float> analytic_radiance = {}, bool hybrid = false) {
    // Replication is by32 independent run seeds, never by correlated pixels.
    // Same predeclared5 empiricalSE +128epsilon criterion for both contrasts.
    constexpr unsigned repeats = 32;
    std::array<std::array<double, 3>, repeats> differences{};
    std::array<double, 3> reference_mean{}, candidate_mean{};
    std::array<std::array<double, 3>, repeats> reference_samples{}, candidate_samples{};
    HybridWitnesses witnesses;
    for (unsigned n = 0; n < repeats; ++n) {
        auto c = enhanced_config();
        c.seed = seed + 7919ull * n;
        c.mutation_steps = 0;
        c.cache_temporal_evaluations = false;
        if (hybrid) {
            c.width = c.height = 17;
            c.capacity = 289;
        }
        auto control = c;
        control.area_path_domain = FrameAreaPathDomain::summed;
        auto reference = session(scene, control), candidate = session(scene, c);
        if (hybrid && n == 0)
            need(FrameSessionTestAccess::snapshot_enhanced(*candidate, 0).status ==
                     GeometryStatus::failure,
                 "Unsubmitted Enhanced snapshot must be rejected");
        const auto a = contribution_capture(*reference, control, scene.max_depth, 0, high_sample);
        const auto b = enhanced_capture(*candidate, c, scene.max_depth, 0, high_sample);
        if (hybrid) {
            const auto found = hybrid_witnesses(
                FrameSessionTestAccess::snapshot_enhanced(*candidate, b.info.area_serial), b.info,
                scene.max_depth);
            witnesses.fresh += found.fresh;
            witnesses.nonself_edges += found.nonself_edges;
            witnesses.imported += found.imported;
            std::cout << "hybrid_witness seed=" << c.seed << " positive_fresh_tag5=" << found.fresh
                      << " nonself_edges=" << found.nonself_edges
                      << " positive_selected_donor_pixels=" << found.imported << std::endl;
            if (n == 0)
                need(FrameSessionTestAccess::snapshot_enhanced(*candidate, 1).status ==
                         GeometryStatus::failure,
                     "Mismatched Enhanced snapshot serial must be rejected");
        }
        std::array<double, 3> x{}, y{};
        for (std::size_t p = 0; p < a.image.raw_xyz.size(); ++p)
            for (unsigned k = 0; k < 3; ++k) {
                x[k] += double(a.image.raw_xyz[p][k]) / double(a.image.raw_xyz.size());
                y[k] += double(b.image.raw_xyz[p][k]) / double(b.image.raw_xyz.size());
            }
        for (unsigned k = 0; k < 3; ++k) {
            differences[n][k] = y[k] - x[k];
            reference_samples[n][k] = x[k];
            candidate_samples[n][k] = y[k];
            reference_mean[k] += x[k] / repeats;
            candidate_mean[k] += y[k] / repeats;
        }
        std::cout << std::setprecision(17) << "enhanced_contrast=" << label << " seed=" << c.seed
                  << " summed_XYZ=" << x[0] << ',' << x[1] << ',' << x[2]
                  << " enhanced_XYZ=" << y[0] << ',' << y[1] << ',' << y[2] << std::endl;
    }
    for (unsigned k = 0; k < 3; ++k) {
        double delta = 0, variance = 0;
        for (const auto &d : differences)
            delta += d[k] / repeats;
        for (const auto &d : differences)
            variance += (d[k] - delta) * (d[k] - delta) / double(repeats - 1);
        const double se = std::sqrt(variance / repeats);
        const double arithmetic =
            128 * double(std::numeric_limits<float>::epsilon()) *
            std::max(std::abs(reference_mean[k]), std::abs(candidate_mean[k]));
        const double bound = 5 * se + arithmetic;
        std::cout << "enhanced_mean contrast=" << label << " replicates=" << repeats
                  << " channel=" << k << " summed=" << reference_mean[k]
                  << " enhanced=" << candidate_mean[k] << " signed_delta=" << delta
                  << " empirical_SE=" << se << " bound=" << bound << std::endl;
        need(std::isfinite(se) && std::abs(delta) <= bound,
             "Enhanced independent-seed multileaf matched-model mean smoke gate");
    }
    if (analytic_radiance) {
        std::array<double, 3> expected{};
        // Sensor uses nearest-integer-nm CIE, not a linear interpolant. Endpoint
        // bins cover half a nanometer on the360..830 continuous interval.
        for (unsigned wavelength = 360; wavelength <= 830; ++wavelength) {
            const auto cie = pbrt_cie_xyz(wavelength);
            const double width = wavelength == 360 || wavelength == 830 ? .5 : 1;
            for (unsigned k = 0; k < 3; ++k)
                expected[k] += width * double(float(cie[k])) * *analytic_radiance;
        }
        for (unsigned arm = 0; arm < 2; ++arm)
            for (unsigned k = 0; k < 3; ++k) {
                const auto &values = arm == 0 ? reference_samples : candidate_samples;
                const double mean = arm == 0 ? reference_mean[k] : candidate_mean[k];
                double variance = 0;
                for (const auto &value : values)
                    variance += (value[k] - mean) * (value[k] - mean) / double(repeats - 1);
                const double se = std::sqrt(variance / repeats);
                const double arithmetic = 128 * double(std::numeric_limits<float>::epsilon()) *
                                          std::max(std::abs(mean), std::abs(expected[k]));
                const double bound = 5 * se + arithmetic;
                // D2 is an analytic truncation negative, not a third renderer
                // contrast or a claim that32 replicates always resolve its gap.
                const double D2 = expected[k] * (.21875 / .234375);
                std::cout << "enhanced_analytic contrast=" << label
                          << " arm=" << (arm == 0 ? "summed" : "enhanced") << " channel=" << k
                          << " mean=" << mean << " expected_D3=" << expected[k]
                          << " negative_D2=" << D2 << " empirical_SE=" << se << " bound=" << bound
                          << std::endl;
                need(std::isfinite(se) && std::abs(mean - expected[k]) <= bound,
                     "Closed-cavity mean agrees with finite-depth analytic transport");
            }
    }
    if (hybrid) {
        std::cout << "hybrid_witness_summary seeds=32 positive_fresh_tag5=" << witnesses.fresh
                  << " nonself_edges=" << witnesses.nonself_edges
                  << " positive_selected_donor_pixels=" << witnesses.imported << std::endl;
        need(witnesses.fresh > 0 && witnesses.nonself_edges > 0 && witnesses.imported > 0,
             "Hybrid mean screen must exercise positive fresh and selected nonself imported tag5 "
             "atoms");
    }
}
RenderScene enhanced_cavity() {
    auto scene = base();
    scene.max_depth = 3;
    SpectralMaterial material;
    material.kind = SpectralMaterialKind::constant;
    material.constant = .5f;
    scene.materials.push_back(material);
    AreaEmission source;
    source.radiance.fill(.125f);
    source.two_sided = true;
    scene.area_emissions.push_back(source);
    // Six EXACTLY closed quads, all inward normals, camera strictly inside.
    // The cyclic tangent basis has positive axis normal before winding reversal.
    for (unsigned axis = 0; axis < 3; ++axis)
        for (int sign : {-1, 1}) {
            GeometryMesh mesh;
            constexpr int corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto &corner : corners) {
                std::array<float, 3> point{};
                point[axis] = float(sign) * 4;
                point[(axis + 1) % 3] = float(corner[0]) * 4;
                point[(axis + 2) % 3] = float(corner[1]) * 4;
                mesh.positions.push_back({point[0], point[1], point[2]});
            }
            mesh.triangles = sign < 0 ? std::vector<std::array<unsigned, 3>>{{0, 1, 2}, {0, 2, 3}}
                                      : std::vector<std::array<unsigned, 3>>{{0, 2, 1}, {0, 3, 2}};
            // Independent winding guard: cross(edge1,edge2) points to origin.
            const auto &triangle = mesh.triangles[0];
            const auto &a = mesh.positions[triangle[0]], &b = mesh.positions[triangle[1]],
                       &c = mesh.positions[triangle[2]];
            const std::array<float, 3> u{b.x - a.x, b.y - a.y, b.z - a.z};
            const std::array<float, 3> v{c.x - a.x, c.y - a.y, c.z - a.z};
            const std::array<float, 3> n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                         u[0] * v[1] - u[1] * v[0]};
            need(n[axis] * float(sign) < 0, "Cavity face normal must point inward");
            const auto index = unsigned(scene.geometry.meshes.size());
            scene.geometry.meshes.push_back(std::move(mesh));
            scene.geometry.instances.push_back({index, {}});
            scene.instance_materials.push_back(0);
            scene.instance_emissions.push_back(0);
            scene.instance_surface_transforms.emplace_back();
        }
    frames(scene);
    return scene;
}
void enhanced_snapshot_failed_attempt() {
    // Same real nonfinite sensor failure used by the wavefront failure tests.
    // D0 primary emission initially hides the finite, overflowing environment.
    auto scene = early_emission(false);
    // D0 performs no NEE; pin the unused proposal policy explicitly.
    scene.light_sampler = LightSamplerKind::uniform;
    // The light table builds alias PMFs even for a uniform proposal. Keep its
    // area/environment ratio in normal range while the visible emitter's
    // sensor value is finite by many orders of magnitude.
    scene.area_emissions[0].radiance.fill(1e30f);
    scene.environment.fill(std::numeric_limits<float>::max());
    frames(scene);
    auto c = enhanced_config();
    c.width = c.height = 17;
    c.capacity = 289;
    c.mutation_steps = 0;
    c.cache_temporal_evaluations = false;
    auto candidate = session(scene, c);
    std::unique_ptr<Frame> saved;
    const auto first = enhanced_capture(*candidate, c, 0, 0, high_sample, {}, &saved);
    const auto committed = FrameSessionTestAccess::snapshot_enhanced(*candidate, 0);
    need(committed.status == GeometryStatus::success, committed.diagnostic);
    FrameCamera miss{.camera = scene.camera,
                     .camera_medium = scene.camera_medium,
                     .raster_camera = scene.raster_camera,
                     .perspective_lens = scene.perspective_lens,
                     .texture_camera = scene.texture_camera};
    need(miss.raster_camera.has_value(), "Failure camera has explicit raster rays");
    miss.raster_camera->origin.x = 100.f;
    auto failed = candidate->submit({.sample_index = high_sample, .epoch = 73, .camera = miss});
    need(failed.status == GeometryStatus::success && failed.frame && !failed.busy,
         failed.diagnostic);
    const auto bad = failed.frame->wait();
    need(bad.status == FrameStatus::failed && !bad.diagnostic.empty() && bad.area_serial == 1 &&
             bad.history_generation == first.info.history_generation && bad.command_count > 0,
         "Accepted sensor failure consumes a serial without committing history");
    need(FrameSessionTestAccess::snapshot_enhanced(*candidate, 1).status ==
                 GeometryStatus::failure &&
             FrameSessionTestAccess::snapshot_enhanced(*candidate, 0).status ==
                 GeometryStatus::failure,
         "Failed attempt cannot alias old committed history or expose mismatched scratch");
    const auto recaptured = saved->capture();
    need(recaptured.status == GeometryStatus::success, recaptured.diagnostic);
    exact(first.image, recaptured);
    failed.frame.reset();
    auto recovered = candidate->submit({.sample_index = high_sample, .epoch = 73});
    need(recovered.status == GeometryStatus::success && recovered.frame && !recovered.busy,
         recovered.diagnostic);
    const auto good = recovered.frame->wait();
    need(good.status == FrameStatus::complete && good.area_serial == 2 && good.history_used &&
             good.history_generation == first.info.history_generation + 1,
         "Recovery commits exactly once after the failed attempt");
    const auto snapshot = FrameSessionTestAccess::snapshot_enhanced(*candidate, 2);
    need(snapshot.status == GeometryStatus::success && snapshot.serial == 2 &&
             snapshot.generation == good.history_generation,
         snapshot.diagnostic);
}
void enhanced_hybrid_controls() {
    enhanced_snapshot_failed_attempt();
    auto scene = enhanced_cavity();
    auto c = enhanced_config();
    c.width = c.height = 17;
    c.seed = seed + 7919ull * 4; // Fixed predeclared seed82677: cold offset(+16,0).
    c.mutation_steps = 1;
    c.cache_temporal_evaluations = false;
    std::array<SharedSceneFrameSessions, 2> pairs;
    std::array<std::array<FrameConfig, 2>, 2> configurations;
    for (unsigned replay = 0; replay < 2; ++replay) {
        auto &settings = configurations[replay];
        settings[0] = c;
        settings[0].capacity = replay == 0 ? 37 : 289;
        settings[0].contribution_replay =
            replay == 0 ? FrameContributionReplay::full : FrameContributionReplay::prefix;
        settings[1] = settings[0];
        settings[1].cache_temporal_evaluations = true;
        pairs[replay] = FrameSessionTestAccess::build_shared_scene(scene, settings[0], settings[1]);
        need(pairs[replay].status == GeometryStatus::success && pairs[replay].shared_renderer &&
                 pairs[replay].independent_history && pairs[replay].independent_owned_records,
             pairs[replay].diagnostic);
    }
    for (unsigned ordinal = 0; ordinal < 3; ++ordinal) {
        std::array<std::array<ContributionCapture, 2>, 2> captures;
        for (unsigned replay = 0; replay < 2; ++replay) {
            for (unsigned cached = 0; cached < 2; ++cached)
                captures[replay][cached] = enhanced_capture(*pairs[replay].sessions[cached],
                                                            configurations[replay][cached],
                                                            scene.max_depth, ordinal, high_sample);
            const auto &a = captures[replay][0];
            const auto &b = captures[replay][1];
            exact(a.image, b.image);
            need(a.info.area_serial == b.info.area_serial &&
                     a.info.history_generation == b.info.history_generation &&
                     a.info.area_stencil_offsets == b.info.area_stencil_offsets &&
                     a.info.mutation_accepts == b.info.mutation_accepts &&
                     a.info.completed_evaluations + a.info.analytic_null_evaluations ==
                         b.info.completed_evaluations + b.info.analytic_null_evaluations +
                             b.info.cached_evaluations,
                 "Hybrid shared-AS full/prefix cacheoff/on retains history, accounting and "
                 "decisions");
            if (ordinal == 1)
                need(FrameSessionTestAccess::snapshot_enhanced(*pairs[replay].sessions[0], 0)
                             .status == GeometryStatus::failure,
                     "Superseded completed Enhanced snapshot serial must be rejected");
        }
        // Each cache pair shares one AS. This additional full/prefix comparison
        // spans the two independently built pairs, without claiming a shared AS.
        exact(captures[0][0].image, captures[1][0].image);
        need(captures[0][0].info.mutation_accepts == captures[1][0].info.mutation_accepts,
             "Hybrid full/prefix retains corrected mutation decisions");
    }
    // Exactly the old fixed32-seed ensemble and unchanged5SE+128epsilon gates.
    enhanced_mean_contrast(scene, "D3-hybrid-17x17", .234375f, true);
}
EnhancedSuffixContextState suffix_state(FrameSession &session) {
    auto state = FrameSessionTestAccess::suffix_context_state(session);
    need(state.status == GeometryStatus::success, state.diagnostic);
    return state;
}
void enhanced_suffix_context_controls() {
    auto scene = enhanced_cavity();
    auto c = enhanced_config();
    c.width = c.height = 17;
    c.capacity = 289;
    c.mutation_steps = 0;
    c.cache_temporal_evaluations = false;
    c.contribution_replay = FrameContributionReplay::prefix;
    auto candidate = session(scene, c);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    need(FrameSessionTestAccess::seed_next_suffix_context(*candidate, 0).status ==
             GeometryStatus::failure,
         "Zero suffix context seed is rejected without consuming pristine state");
    auto seeded = FrameSessionTestAccess::seed_next_suffix_context(*candidate, maximum - 1);
    need(seeded.status == GeometryStatus::success && seeded.next_context == maximum - 1 &&
             !seeded.exhausted && seeded.next_serial == 0 && !seeded.committed_serial,
         "Pristine suffix allocator accepts MAX-1 exactly");
    need(FrameSessionTestAccess::seed_next_suffix_context(*candidate, maximum).status ==
             GeometryStatus::failure,
         "Suffix allocator cannot be reseeded");
    FrameCamera moved{.camera = scene.camera,
                      .camera_medium = scene.camera_medium,
                      .raster_camera = scene.raster_camera,
                      .perspective_lens = scene.perspective_lens,
                      .texture_camera = scene.texture_camera};
    need(moved.raster_camera.has_value(), "Allocator fixture has explicit raster rays");
    moved.raster_camera->origin.x += .0625f;
    for (unsigned ordinal = 0; ordinal < 4; ++ordinal) {
        enhanced_capture(*candidate, c, scene.max_depth, ordinal, high_sample,
                         ordinal < 2 ? std::optional<FrameCamera>{} : moved);
        const auto state = suffix_state(*candidate);
        need(state.contexts[state.front] == (ordinal < 2 ? maximum - 1 : maximum) &&
                 state.next_context == maximum && state.exhausted == (ordinal >= 2) &&
                 state.generation == ordinal + 1 && state.next_serial == ordinal + 1 &&
                 state.committed_serial == ordinal,
             "Static A/B contexts reuse exact identities across allocator exhaustion");
    }
    auto third = moved;
    third.raster_camera->origin.x += .0625f;
    auto rejected = candidate->submit({.sample_index = high_sample, .epoch = 73, .camera = third});
    need(rejected.status == GeometryStatus::failure && !rejected.frame && !rejected.busy &&
             rejected.diagnostic.find("suffix context identity exhausted") != std::string::npos,
         "New C context fails visibly before accepting a serial at exhaustion");
    const auto exhausted = suffix_state(*candidate);
    need(exhausted.valid && exhausted.exhausted && exhausted.contexts[exhausted.front] == maximum &&
             exhausted.next_context == maximum && exhausted.next_serial == 4 &&
             exhausted.pending_serial == 3 && exhausted.committed_serial == 3 &&
             exhausted.generation == 4,
         "Rejected context cannot wrap the token or advance serial/history publication");
    enhanced_capture(*candidate, c, scene.max_depth, 4, high_sample, moved);
    const auto reused = suffix_state(*candidate);
    need(reused.contexts[reused.front] == maximum && reused.exhausted &&
             reused.next_context == maximum && reused.next_serial == 5 && reused.generation == 5,
         "Committed B remains usable after rejecting a new exhausted context");
    std::cout << "suffix_context overflow/static-reuse controls passed\n";
}
void enhanced_suffix_failed_context() {
    // Reuse the accepted asynchronous sensor-overflow fixture, not a preflight error.
    auto scene = early_emission(false);
    scene.light_sampler = LightSamplerKind::uniform;
    scene.area_emissions[0].radiance.fill(1e30f);
    scene.environment.fill(std::numeric_limits<float>::max());
    frames(scene);
    auto c = enhanced_config();
    c.width = c.height = 17;
    c.capacity = 289;
    c.mutation_steps = 0;
    c.cache_temporal_evaluations = false;
    c.contribution_replay = FrameContributionReplay::prefix;
    auto candidate = session(scene, c);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto seeded = FrameSessionTestAccess::seed_next_suffix_context(*candidate, maximum - 1);
    need(seeded.status == GeometryStatus::success, seeded.diagnostic);
    const auto first = enhanced_capture(*candidate, c, 0, 0, high_sample);
    const auto before = suffix_state(*candidate);
    need(before.contexts[before.front] == maximum - 1 && !before.exhausted,
         "Successful A owns MAX-1 before asynchronous failure");
    FrameCamera miss{.camera = scene.camera,
                     .camera_medium = scene.camera_medium,
                     .raster_camera = scene.raster_camera,
                     .perspective_lens = scene.perspective_lens,
                     .texture_camera = scene.texture_camera};
    need(miss.raster_camera.has_value(), "Failure context has explicit raster rays");
    miss.raster_camera->origin.x = 100.f;
    auto failed = candidate->submit({.sample_index = high_sample, .epoch = 73, .camera = miss});
    need(failed.status == GeometryStatus::success && failed.frame && !failed.busy,
         failed.diagnostic);
    const auto bad = failed.frame->wait();
    need(bad.status == FrameStatus::failed && !bad.diagnostic.empty() && bad.command_count > 0 &&
             bad.area_serial == 1 && bad.history_generation == first.info.history_generation,
         "Actual asynchronous B failure consumes its serial without committing history");
    const auto after = suffix_state(*candidate);
    need(after.front == before.front && after.valid && after.exhausted &&
             after.contexts[after.front] == maximum - 1 &&
             after.contexts[after.front ^ 1] == maximum && after.next_context == maximum &&
             after.next_serial == 2 && after.pending_serial == 1 && after.committed_serial == 0 &&
             after.generation == before.generation,
         "Failed B consumes MAX exactly and retains successful front A identity");
    need(FrameSessionTestAccess::snapshot_enhanced(*candidate, 0).status ==
                 GeometryStatus::failure &&
             FrameSessionTestAccess::snapshot_enhanced(*candidate, 1).status ==
                 GeometryStatus::failure,
         "Allocator diagnostics do not weaken failed-attempt full-snapshot rejection");
    failed.frame.reset();
    auto retry = candidate->submit({.sample_index = high_sample, .epoch = 73, .camera = miss});
    need(retry.status == GeometryStatus::failure && !retry.frame && !retry.busy &&
             retry.diagnostic.find("suffix context identity exhausted") != std::string::npos,
         "Failed B retry cannot reuse an uncommitted identity after exhaustion");
    const auto rejected = suffix_state(*candidate);
    need(rejected.next_serial == 2 && rejected.pending_serial == 1 &&
             rejected.committed_serial == 0 && rejected.generation == 1 && rejected.exhausted &&
             rejected.contexts[rejected.front] == maximum - 1,
         "Rejected retry preserves allocator exhaustion and committed publication");
    auto recovered = candidate->submit({.sample_index = high_sample, .epoch = 73});
    need(recovered.status == GeometryStatus::success && recovered.frame && !recovered.busy,
         recovered.diagnostic);
    const auto good = recovered.frame->wait();
    need(good.status == FrameStatus::complete && good.area_serial == 2 && good.history_used &&
             good.history_generation == 2,
         "Original A recovers with one new successful generation after failed B");
    const auto final = suffix_state(*candidate);
    need(final.contexts[final.front] == maximum - 1 && final.exhausted &&
             final.next_context == maximum && final.next_serial == 3 && final.generation == 2 &&
             final.committed_serial == 2,
         "Recovery reuses committed A without wrapping or reminting a context token");
    const auto snapshot = FrameSessionTestAccess::snapshot_enhanced(*candidate, 2);
    need(snapshot.status == GeometryStatus::success && snapshot.suffix_context == maximum - 1,
         snapshot.diagnostic);
    std::cout << "suffix_context asynchronous-failure/exhaustion controls passed\n";
}
void enhanced_suffix_full_inactive() {
    const auto scene = enhanced_cavity();
    auto off = enhanced_config();
    off.width = off.height = 17;
    off.capacity = 289;
    off.contribution_replay = FrameContributionReplay::full;
    off.cache_temporal_evaluations = false;
    off.enhanced_suffix_cache = false;
    auto on = off;
    on.enhanced_suffix_cache = true;
    auto pair = FrameSessionTestAccess::build_shared_scene(scene, off, on);
    need(pair.status == GeometryStatus::success && pair.shared_renderer &&
             pair.independent_history && pair.independent_owned_records &&
             pair.independent_suffix_storage,
         pair.diagnostic);
    need(FrameSessionTestAccess::seed_next_suffix_context(*pair.sessions[1], 7).status ==
                 GeometryStatus::failure &&
             FrameSessionTestAccess::suffix_context_state(*pair.sessions[1]).status ==
                 GeometryStatus::failure,
         "Full oracle cannot enable the prefix allocator through its test bridge");
    for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
        auto a = enhanced_capture(*pair.sessions[0], off, scene.max_depth, ordinal, high_sample);
        auto b = enhanced_capture(*pair.sessions[1], on, scene.max_depth, ordinal, high_sample);
        exact(a.image, b.image);
        need(!a.info.enhanced_suffix_cache && !b.info.enhanced_suffix_cache &&
                 a.info.enhanced_suffix_hits == std::array<std::uint64_t, 3>{} &&
                 b.info.enhanced_suffix_hits == std::array<std::uint64_t, 3>{} &&
                 a.info.enhanced_suffix_misses == 0 && b.info.enhanced_suffix_misses == 0 &&
                 a.info.mutation_accepts == b.info.mutation_accepts,
             "Full replay keeps requested suffix on inactive with zero cache work");
        const auto x = FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[0], ordinal, true);
        const auto y = FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[1], ordinal, true);
        need(x.status == GeometryStatus::success && y.status == GeometryStatus::success,
             x.diagnostic + y.diagnostic);
        need(!x.suffix_cache_enabled && !y.suffix_cache_enabled && x.suffix_context == 0 &&
                 y.suffix_context == 0 && x.enhanced_suffix.empty() && y.enhanced_suffix.empty() &&
                 x.history.size() == y.history.size() && x.temporal.size() == y.temporal.size(),
             "Both full replay arms expose token zero and no suffix tape");
        need(std::memcmp(x.history.data(), y.history.data(),
                         x.history.size() * sizeof(area_wire::History)) == 0 &&
                 std::memcmp(x.temporal.data(), y.temporal.data(),
                             x.temporal.size() * sizeof(area_wire::Temporal)) == 0,
             "Inactive full-mode flag preserves exact published history and temporal tuples");
    }
    std::cout << "suffix_full inactive on/off shared-AS controls passed\n";
}
void enhanced_suffix_conductor() {
    auto scene = enhanced_cavity();
    scene.max_depth = 6;
    auto &material = scene.materials[0];
    material = SpectralMaterial{};
    material.kind = SpectralMaterialKind::constant;
    material.scattering = ScatteringKind::conductor;
    material.eta.knots = {{360, .8f}, {500, 1.2f}, {830, 1.8f}};
    material.k.knots = {{360, 3.5f}, {500, 2.5f}, {830, 1.5f}};
    material.eta.extend_endpoints = material.k.extend_endpoints = true;
    material.u_roughness = material.v_roughness = .5f;
    material.remap_roughness = false;
    // Rough conductor evaluation uses the authored surface frame table even
    // though eta/k are constant across each wall's spatial coordinates.
    frames(scene);
    auto off = enhanced_config();
    off.width = off.height = 17;
    off.capacity = 37;
    off.seed = 51001;
    off.mutation_steps = 1;
    off.contribution_replay = FrameContributionReplay::prefix;
    off.cache_temporal_evaluations = false;
    off.enhanced_suffix_cache = false;
    auto on = off;
    on.enhanced_suffix_cache = true;
    auto pair = FrameSessionTestAccess::build_shared_scene(scene, off, on);
    need(pair.status == GeometryStatus::success && pair.shared_renderer &&
             pair.independent_history && pair.independent_owned_records &&
             pair.independent_suffix_storage,
         pair.diagnostic);
    FrameCamera moved{.camera = scene.camera,
                      .camera_medium = scene.camera_medium,
                      .raster_camera = scene.raster_camera,
                      .perspective_lens = scene.perspective_lens,
                      .texture_camera = scene.texture_camera};
    need(moved.raster_camera.has_value(), "Conductor suffix fixture has a raster camera");
    moved.raster_camera->origin.x += .0625f;
    constexpr unsigned pixels = 17u * 17u;
    std::array<std::uint64_t, 3> hits{};
    std::uint64_t misses = 0, records = 0, previous_context = 0;
    for (unsigned frame = 0; frame < 4; ++frame) {
        const auto camera =
            frame >= 2 ? std::optional<FrameCamera>(moved) : std::optional<FrameCamera>{};
        std::cout << "suffix_conductor_begin seed=" << off.seed << " frame=" << frame << " arm=off"
                  << std::endl;
        const auto a =
            enhanced_capture(*pair.sessions[0], off, scene.max_depth, frame, high_sample, camera);
        std::cout << "suffix_conductor_begin seed=" << off.seed << " frame=" << frame << " arm=on"
                  << std::endl;
        const auto b =
            enhanced_capture(*pair.sessions[1], on, scene.max_depth, frame, high_sample, camera);
        exact(a.image, b.image);
        need(!a.info.enhanced_suffix_cache && b.info.enhanced_suffix_cache &&
                 !a.info.temporal_evaluation_cache_used && !b.info.temporal_evaluation_cache_used &&
                 a.info.enhanced_suffix_hits == std::array<std::uint64_t, 3>{} &&
                 a.info.enhanced_suffix_misses == 0 &&
                 a.info.mutation_attempts == b.info.mutation_attempts &&
                 a.info.mutation_accepts == b.info.mutation_accepts &&
                 a.info.completed_evaluations == b.info.completed_evaluations &&
                 a.info.analytic_null_evaluations == b.info.analytic_null_evaluations &&
                 a.info.logical_evaluations == b.info.logical_evaluations &&
                 a.info.area_stencil_offsets == b.info.area_stencil_offsets,
             "Rough conductor suffix on/off preserves mutation and evaluator accounting");
        const auto x = FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[0], frame);
        const auto y = FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[1], frame);
        need(x.status == GeometryStatus::success && y.status == GeometryStatus::success,
             x.diagnostic + y.diagnostic);
        need(x.width == 17 && x.height == 17 && y.width == 17 && y.height == 17 &&
                 x.serial == frame && y.serial == frame &&
                 x.generation == a.info.history_generation &&
                 y.generation == b.info.history_generation && x.epoch == a.info.epoch &&
                 y.epoch == b.info.epoch && x.front_bank == y.front_bank &&
                 x.history.size() == pixels && y.history.size() == pixels &&
                 x.temporal.size() == pixels && y.temporal.size() == pixels &&
                 x.records.size() == enhanced_wire::bank_count * pixels &&
                 y.records.size() == enhanced_wire::bank_count * pixels &&
                 !x.suffix_cache_enabled && x.suffix_context == 0 && y.suffix_cache_enabled &&
                 y.suffix_context != 0 && x.enhanced_suffix.empty() && y.enhanced_suffix.empty(),
             "Rough conductor snapshots retain exact publication identity and actual cache state");
        need(std::memcmp(x.history.data(), y.history.data(), pixels * sizeof(area_wire::History)) ==
                     0 &&
                 std::memcmp(x.temporal.data(), y.temporal.data(),
                             pixels * sizeof(area_wire::Temporal)) == 0,
             "Rough conductor suffix reuse preserves complete history and temporal tuples");
        const auto compare_record = [&](enhanced_wire::Reference ref) {
            if (enhanced_wire::absent(ref))
                return;
            need(enhanced_wire::valid(ref, pixels), "Conductor owned-record reference is valid");
            const auto index = std::size_t(ref.bank) * pixels + ref.pixel;
            need(std::memcmp(&x.records[index], &y.records[index], sizeof(enhanced_wire::Record)) ==
                     0,
                 "Rough conductor suffix reuse preserves the referenced initialized record");
            ++records;
        };
        // Unreferenced arena bytes are not initialized semantic witnesses.
        for (const auto &h : x.history)
            if (h.valid && h.selected_slot != contribution_wire::enhanced_null_slot)
                compare_record(h.record);
        for (const auto &t : x.temporal)
            if (t.normalizer > 0 && t.target > 0 &&
                t.selected_slot != contribution_wire::enhanced_null_slot)
                compare_record(t.record);
        if (frame == 2)
            need(y.suffix_context > previous_context,
                 "Moved conductor camera receives a new suffix context");
        else if (frame != 0)
            need(y.suffix_context == previous_context,
                 "Static conductor camera retains its suffix context");
        previous_context = y.suffix_context;
        for (unsigned k = 0; k < 3; ++k)
            hits[k] += b.info.enhanced_suffix_hits[k];
        misses += b.info.enhanced_suffix_misses;
        std::cout << "suffix_conductor_exact seed=" << off.seed << " frame=" << frame
                  << " context=" << y.suffix_context << " hits=" << b.info.enhanced_suffix_hits[0]
                  << ',' << b.info.enhanced_suffix_hits[1] << ',' << b.info.enhanced_suffix_hits[2]
                  << " misses=" << b.info.enhanced_suffix_misses << '\n';
    }
    need(hits[0] + hits[1] + hits[2] > 0 && misses > 0 && records > 0,
         "Fixed-seed rough conductor run must witness actual suffix hits, fallback and records");
    std::cout << "suffix_conductor_exact totals hits=" << hits[0] << ',' << hits[1] << ','
              << hits[2] << " misses=" << misses << " referenced_records=" << records << '\n';
}
RenderScene motion_disocclusion_scene() {
    auto scene = base();
    scene.max_depth = 0;
    scene.raster_camera = RasterCamera{{}, {-1, 1, 1}, {2, 0, 0}, {0, -2, 0}};
    SpectralMaterial material;
    material.kind = SpectralMaterialKind::constant;
    material.constant = .5f;
    scene.materials.push_back(material);
    AreaEmission emission;
    emission.radiance.fill(emitted);
    emission.two_sided = true;
    scene.area_emissions.push_back(emission);
    plane(scene, 8, false, 0, 0);
    GeometryMesh blocker;
    blocker.positions = {{-.75f, -4, 2}, {.75f, -4, 2}, {.75f, 4, 2}, {-.75f, 4, 2}};
    blocker.triangles = {{0, 2, 1}, {0, 3, 2}};
    const auto mesh = unsigned(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(blocker));
    scene.geometry.instances.push_back({mesh, {}});
    scene.instance_materials.push_back(0);
    scene.instance_emissions.push_back(no_area_emission);
    scene.instance_surface_transforms.emplace_back();
    frames(scene);
    return scene;
}
FrameCamera motion_pan(const RenderScene &scene) {
    FrameCamera camera{.camera = scene.camera,
                       .camera_medium = scene.camera_medium,
                       .raster_camera = scene.raster_camera,
                       .perspective_lens = scene.perspective_lens,
                       .texture_camera = scene.texture_camera};
    need(camera.raster_camera.has_value(), "Motion fixture has raster camera");
    camera.raster_camera->origin.x += 1;
    return camera;
}
ContributionCapture motion_capture(FrameSession &session, std::optional<FrameCamera> camera = {},
                                   std::uint64_t epoch = 73,
                                   std::unique_ptr<Frame> *retained = nullptr) {
    auto submitted =
        session.submit({.sample_index = high_sample, .epoch = epoch, .camera = camera});
    need(submitted.status == GeometryStatus::success && submitted.frame, submitted.diagnostic);
    auto info = submitted.frame->wait();
    need(info.status == FrameStatus::complete, info.diagnostic);
    auto image = submitted.frame->capture();
    need(image.status == GeometryStatus::success, image.diagnostic);
    if (retained)
        *retained = std::move(submitted.frame);
    return {std::move(image), std::move(info)};
}
void enhanced_motion_static_controls() {
    auto scene = motion_disocclusion_scene();
    auto off = enhanced_config();
    off.width = off.height = 7;
    off.capacity = 37;
    off.mutation_steps = 1;
    off.contribution_replay = FrameContributionReplay::prefix;
    off.cache_temporal_evaluations = true;
    auto on = off;
    on.enhanced_temporal_reprojection = true;
    auto pair = FrameSessionTestAccess::build_shared_scene(scene, off, on);
    need(pair.status == GeometryStatus::success && pair.shared_renderer && pair.independent_history,
         pair.diagnostic);
    std::vector<enhanced_motion_wire::Geometry> first_geometry;
    unsigned geometry_front = 0;
    for (unsigned frame = 0; frame < 4; ++frame) {
        auto a = motion_capture(*pair.sessions[0]);
        auto b = motion_capture(*pair.sessions[1]);
        exact(a.image, b.image);
        need(a.info.mutation_accepts == b.info.mutation_accepts &&
                 a.info.completed_evaluations == b.info.completed_evaluations &&
                 a.info.cached_evaluations == b.info.cached_evaluations &&
                 a.info.analytic_null_evaluations == b.info.analytic_null_evaluations &&
                 !a.info.enhanced_temporal_reprojection && b.info.enhanced_temporal_reprojection &&
                 !b.info.enhanced_temporal_reprojection_used,
             "Static identity motion preserves exact raw, mutation and transport accounting");
        const auto x = FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[0], frame);
        const auto y =
            FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[1], frame, false, true);
        need(x.status == GeometryStatus::success && y.status == GeometryStatus::success,
             x.diagnostic + y.diagnostic);
        need(
            y.motion_enabled && y.geometry_valid &&
                y.geometry_generation == b.info.history_generation && y.geometry.size() == 49 &&
                y.motion_map_generated == (frame == 0) &&
                y.donors.size() == (frame == 0 ? 49u : 0u) &&
                std::memcmp(x.history.data(), y.history.data(),
                            x.history.size() * sizeof(area_wire::History)) == 0 &&
                std::memcmp(x.temporal.data(), y.temporal.data(),
                            x.temporal.size() * sizeof(area_wire::Temporal)) == 0,
            "Static identity exposes paired geometry generation and exact history/temporal tuples");
        if (frame == 0) {
            geometry_front = y.geometry_front;
            first_geometry = y.geometry;
            need(b.info.enhanced_geometry_generated == 49 && b.info.enhanced_geometry_reused == 0 &&
                     b.info.enhanced_motion_routes == std::array<std::uint64_t, 4>{49, 0, 0, 0},
                 "Cold motion generates one representative query per pixel");
        } else {
            need(y.geometry_front == geometry_front && b.info.enhanced_geometry_generated == 0 &&
                     b.info.enhanced_geometry_reused == 49 &&
                     b.info.enhanced_motion_routes == std::array<std::uint64_t, 4>{} &&
                     std::memcmp(first_geometry.data(), y.geometry.data(),
                                 49 * sizeof(enhanced_motion_wire::Geometry)) == 0,
                 "Static identity reuses immutable geometry without inventing donor work");
        }
        const auto compare_record = [&](enhanced_wire::Reference ref) {
            if (enhanced_wire::absent(ref))
                return;
            const auto index = std::size_t(ref.bank) * 49 + ref.pixel;
            need(index < x.records.size() && index < y.records.size() &&
                     std::memcmp(&x.records[index], &y.records[index],
                                 sizeof(enhanced_wire::Record)) == 0,
                 "Static motion preserves referenced owned records");
        };
        for (const auto &h : x.history)
            if (h.valid && h.selected_slot != contribution_wire::enhanced_null_slot)
                compare_record(h.record);
        for (const auto &t : x.temporal)
            if (t.normalizer > 0 && t.target > 0 &&
                t.selected_slot != contribution_wire::enhanced_null_slot)
                compare_record(t.record);
    }
}
void enhanced_motion_lifetime_controls() {
    const auto scene = motion_disocclusion_scene();
    const auto moved = motion_pan(scene);
    auto c = enhanced_config();
    c.width = 32;
    c.height = 16;
    c.capacity = 37;
    c.mutation_steps = 0;
    c.enhanced_temporal_reprojection = true;
    c.compact_enhanced_pairs = true;
    c.contribution_replay = FrameContributionReplay::prefix;
    auto candidate = session(scene, c);
    std::unique_ptr<Frame> retained;
    const auto a = motion_capture(*candidate, {}, 73, &retained);
    const auto initial = FrameSessionTestAccess::motion_state(*candidate);
    need(initial.status == GeometryStatus::success && initial.geometry_valid &&
             initial.history_valid,
         initial.diagnostic);
    const auto snapshot_a =
        FrameSessionTestAccess::snapshot_enhanced(*candidate, a.info.area_serial, false, true);
    need(snapshot_a.status == GeometryStatus::success && snapshot_a.geometry.size() == 512,
         snapshot_a.diagnostic);
    need(FrameSessionTestAccess::force_next_motion_publication_failure(*candidate).empty(),
         "Arm failure after GPU work and before joint motion/reservoir publication");
    auto failed = candidate->submit({.sample_index = high_sample, .epoch = 73, .camera = moved});
    need(failed.status == GeometryStatus::success && failed.frame, failed.diagnostic);
    need(failed.frame->wait().status == FrameStatus::failed,
         "Injected changed-camera publication fails");
    const auto after_failure = FrameSessionTestAccess::motion_state(*candidate);
    need(after_failure.status == GeometryStatus::success && after_failure.geometry_valid &&
             after_failure.history_valid &&
             after_failure.geometry_front == initial.geometry_front &&
             after_failure.geometry_generation == initial.geometry_generation &&
             after_failure.history_generation == initial.history_generation &&
             after_failure.committed_serial == initial.committed_serial,
         "Same-epoch failure preserves the committed geometry/reservoir pair");
    need(FrameSessionTestAccess::snapshot_enhanced(*candidate, a.info.area_serial, false, true)
                 .status == GeometryStatus::failure,
         "Latest failed attempt cannot expose stale full GPU snapshots");
    failed.frame.reset();
    const auto b = motion_capture(*candidate, moved);
    const auto snapshot_b =
        FrameSessionTestAccess::snapshot_enhanced(*candidate, b.info.area_serial, false, true);
    // Actual representative hits must round-trip to their owner at pixel
    // centers under an independent double pinhole projection, not a matrix
    // helper or the physical stochastic primary sample.
    for (const auto *snapshot : {&snapshot_a, &snapshot_b})
        for (unsigned owner = 0; owner < snapshot->geometry.size(); ++owner) {
            const auto &g = snapshot->geometry[owner];
            if (g.status != enhanced_motion_wire::finite)
                continue;
            const auto p = g.camera_relative_position;
            need(p.z > 0 && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),
                 "Actual representative geometry is finite camera-relative data");
            const double x = (double(p.x) / p.z + 1) * 16;
            const double y = (1 - double(p.y) / p.z) * 8;
            need(x >= 0 && x < 32 && y >= 0 && y < 16 &&
                     unsigned(std::floor(y)) * 32 + unsigned(std::floor(x)) == owner,
                 "Actual center geometry independently round-trips to its owner");
            need(std::abs(p.z - (g.instance == 0 ? 8.f : 2.f)) <=
                         32 * std::numeric_limits<float>::epsilon() * p.z &&
                     g.geometric_normal.z == -1.f && g.pixel_footprint > 0,
                 "Actual representative hit agrees with independent opaque plane geometry");
        }
    need(snapshot_b.status == GeometryStatus::success && snapshot_b.geometry_valid &&
             snapshot_b.motion_map_generated &&
             snapshot_b.geometry_front != snapshot_a.geometry_front &&
             snapshot_b.geometry_generation == b.info.history_generation &&
             snapshot_b.donors.size() == 512 && b.info.enhanced_temporal_reprojection_used &&
             b.info.enhanced_geometry_generated == 512 && b.info.enhanced_geometry_reused == 0 &&
             b.info.enhanced_motion_routes[2] > 0,
         "Actual translated foreground/background scene produces positive dual donor work after "
         "recovery");
    std::array<std::uint64_t, 4> routes{};
    for (const auto &donor : snapshot_b.donors) {
        need(donor.source_pixel < 512 && donor.classification < 4 &&
                 donor.serial_low == std::uint32_t(b.info.area_serial) &&
                 donor.serial_high == std::uint32_t(b.info.area_serial >> 32),
             "Published donor map retains owner bounds and successful serial");
        ++routes[donor.classification];
    }
    need(routes == b.info.enhanced_motion_routes,
         "Actual geometry donor counts match immutable map readback");
    const auto hold = motion_capture(*candidate, moved);
    const auto held =
        FrameSessionTestAccess::snapshot_enhanced(*candidate, hold.info.area_serial, false, true);
    need(held.status == GeometryStatus::success &&
             held.geometry_front == snapshot_b.geometry_front && !held.motion_map_generated &&
             held.donors.empty() && hold.info.enhanced_geometry_reused == 512,
         "Hold B reuses geometry independently of reservoir bank alternation");
    need(FrameSessionTestAccess::force_next_motion_publication_failure(*candidate).empty(),
         "Arm failed cut");
    auto cut_failed = candidate->submit({.sample_index = high_sample, .epoch = 74});
    need(cut_failed.status == GeometryStatus::success && cut_failed.frame &&
             cut_failed.frame->wait().status == FrameStatus::failed,
         "Epoch cut can fail before joint publication");
    const auto cut_state = FrameSessionTestAccess::motion_state(*candidate);
    need(cut_state.status == GeometryStatus::success && !cut_state.geometry_valid &&
             !cut_state.history_valid,
         "Failed cut invalidates both history and geometry eligibility");
    cut_failed.frame.reset();
    const auto recovered = motion_capture(*candidate, {}, 74);
    need(!recovered.info.history_used && recovered.info.enhanced_geometry_generated == 512 &&
             !recovered.info.enhanced_temporal_reprojection_used,
         "Recovery from failed cut recaptures a cold geometry/reservoir pair");
    candidate.reset();
    exact(a.image, retained->capture());
}
void enhanced_motion_mean_controls() {
    // Fixed 32 independent run seeds, declared before execution. Compare raw XYZ
    // means using five empirical standard errors plus128epsilon rounding. This
    // is an estimator smoke test, not a universal image-identity requirement.
    constexpr unsigned repeats = 32;
    const auto scene = motion_disocclusion_scene();
    const auto moved = motion_pan(scene);
    std::array<std::array<double, 3>, repeats> differences{};
    std::array<double, 3> first_mean{}, second_mean{};
    for (unsigned run = 0; run < repeats; ++run) {
        auto off = enhanced_config();
        off.width = 8;
        off.height = 4;
        off.capacity = 7;
        off.mutation_steps = 0;
        off.seed = seed + 7919ull * run;
        off.contribution_replay = FrameContributionReplay::full;
        off.cache_temporal_evaluations = false;
        auto on = off;
        on.enhanced_temporal_reprojection = true;
        auto pair = FrameSessionTestAccess::build_shared_scene(scene, off, on);
        need(pair.status == GeometryStatus::success, pair.diagnostic);
        motion_capture(*pair.sessions[0]);
        motion_capture(*pair.sessions[1]);
        const auto a = motion_capture(*pair.sessions[0], moved),
                   b = motion_capture(*pair.sessions[1], moved);
        std::array<double, 3> x{}, y{};
        for (unsigned p = 0; p < a.image.raw_xyz.size(); ++p)
            for (unsigned k = 0; k < 3; ++k) {
                need(std::isfinite(a.image.raw_xyz[p][k]) && std::isfinite(b.image.raw_xyz[p][k]),
                     "Moving motion raw values finite");
                x[k] += double(a.image.raw_xyz[p][k]) / a.image.raw_xyz.size();
                y[k] += double(b.image.raw_xyz[p][k]) / b.image.raw_xyz.size();
            }
        for (unsigned k = 0; k < 3; ++k) {
            differences[run][k] = y[k] - x[k];
            first_mean[k] += x[k] / repeats;
            second_mean[k] += y[k] / repeats;
        }
    }
    for (unsigned k = 0; k < 3; ++k) {
        double delta = 0, variance = 0;
        for (const auto &d : differences)
            delta += d[k] / repeats;
        for (const auto &d : differences)
            variance += (d[k] - delta) * (d[k] - delta) / double(repeats - 1);
        const double se = std::sqrt(variance / repeats);
        const double bound =
            5 * se + 128 * std::numeric_limits<float>::epsilon() *
                         std::max(std::abs(first_mean[k]), std::abs(second_mean[k]));
        std::cout << "motion_mean channel=" << k << " off=" << first_mean[k]
                  << " on=" << second_mean[k] << " delta=" << delta << " SE=" << se
                  << " bound=" << bound << '\n';
        need(std::isfinite(se) && std::abs(delta) <= bound,
             "Fixed-seed moving motion raw mean smoke gate");
    }
}
void enhanced_motion_controls() {
    need(!FrameConfig{}.enhanced_temporal_reprojection, "Motion reprojection defaults off");
    enhanced_motion_static_controls();
    enhanced_motion_lifetime_controls();
    enhanced_motion_mean_controls();
}
// Pair compaction changes execution only: exact output and published payloads
// are compared on one shared AS at fixed seeds, including changed cameras.
void enhanced_pair_compaction_controls() {
    need(!FrameConfig{}.compact_enhanced_pairs, "Pair compaction is opt-in by default");
    for (const auto domain : {FrameAreaPathDomain::summed, FrameAreaPathDomain::contribution}) {
        auto c = contribution_config();
        c.area_path_domain = domain;
        c.compact_enhanced_pairs = true;
        c.enhanced_temporal_reprojection = true;
        auto frames = session(diffuse(), c);
        // These modes never publish analytic-null completions. The ordinary
        // capture helper also checks effective-off metadata on every frame.
        if (domain == FrameAreaPathDomain::summed)
            capture(*frames, c, high_sample, false);
        else {
            auto submitted = frames->submit({.sample_index = high_sample, .epoch = 73});
            need(submitted.status == GeometryStatus::success && submitted.frame,
                 submitted.diagnostic);
            const auto info = submitted.frame->wait();
            need(info.status == FrameStatus::complete && !info.enhanced_pair_compaction &&
                     info.compacted_null_evaluations == 0 && !info.enhanced_temporal_reprojection &&
                     info.enhanced_geometry_generated == 0 && info.enhanced_geometry_reused == 0,
                 "Contribution domain retains requested pair compaction as effective off");
        }
    }
    auto independent = config();
    independent.compact_enhanced_pairs = true;
    independent.enhanced_temporal_reprojection = true;
    auto independent_session = session(diffuse(), independent);
    capture(*independent_session, independent, high_sample, false);
    for (const bool black : {true, false}) {
        auto scene = black ? early_emission(false) : enhanced_cavity();
        if (black)
            scene.area_emissions[0].radiance.fill(0);
        frames(scene);
        for (const auto replay : {FrameContributionReplay::prefix, FrameContributionReplay::full})
            for (const unsigned mutations : {0u, 1u})
                for (const unsigned capacity : {1u, 37u, 49u}) {
                    auto off = enhanced_config();
                    off.width = off.height = 7;
                    off.capacity = capacity;
                    off.mutation_steps = mutations;
                    off.contribution_replay = replay;
                    off.cache_temporal_evaluations = false;
                    off.enhanced_suffix_cache = true;
                    off.compact_enhanced_pairs = false;
                    auto on = off;
                    on.compact_enhanced_pairs = true;
                    auto pair = FrameSessionTestAccess::build_shared_scene(scene, off, on);
                    need(pair.status == GeometryStatus::success && pair.shared_renderer &&
                             pair.independent_history && pair.independent_owned_records,
                         pair.diagnostic);
                    FrameCamera moved{.camera = scene.camera,
                                      .camera_medium = scene.camera_medium,
                                      .raster_camera = scene.raster_camera,
                                      .perspective_lens = scene.perspective_lens,
                                      .texture_camera = scene.texture_camera};
                    need(moved.raster_camera.has_value(),
                         "Pair fixture has explicit raster camera");
                    moved.raster_camera->origin.x += .0625f;
                    std::uint64_t compacted = 0, physical = 0;
                    for (unsigned frame = 0; frame < 4; ++frame) {
                        const auto camera = frame >= 2 ? std::optional<FrameCamera>(moved)
                                                       : std::optional<FrameCamera>{};
                        auto a = enhanced_capture(*pair.sessions[0], off, scene.max_depth, frame,
                                                  high_sample, camera);
                        auto b = enhanced_capture(*pair.sessions[1], on, scene.max_depth, frame,
                                                  high_sample, camera);
                        exact(a.image, b.image);
                        need(
                            !a.info.enhanced_pair_compaction && b.info.enhanced_pair_compaction &&
                                a.info.compacted_null_evaluations == 0 &&
                                b.info.compacted_null_evaluations ==
                                    b.info.analytic_null_evaluations &&
                                a.info.completed_evaluations == b.info.completed_evaluations &&
                                a.info.analytic_null_evaluations ==
                                    b.info.analytic_null_evaluations &&
                                a.info.cached_evaluations == b.info.cached_evaluations &&
                                a.info.logical_evaluations == b.info.logical_evaluations &&
                                a.info.area_stencil_offsets == b.info.area_stencil_offsets &&
                                a.info.mutation_accepts == b.info.mutation_accepts,
                            "Compaction preserves raw, selection, mutation and logical accounting");
                        compacted += b.info.compacted_null_evaluations;
                        physical += b.info.completed_evaluations;
                        const auto x =
                            FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[0], frame);
                        const auto y =
                            FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[1], frame);
                        need(x.status == GeometryStatus::success &&
                                 y.status == GeometryStatus::success,
                             x.diagnostic + y.diagnostic);
                        need(x.front_bank == y.front_bank && x.serial == y.serial &&
                                 x.generation == y.generation && x.epoch == y.epoch &&
                                 x.history.size() == y.history.size() &&
                                 x.temporal.size() == y.temporal.size() &&
                                 std::memcmp(x.history.data(), y.history.data(),
                                             x.history.size() * sizeof(area_wire::History)) == 0 &&
                                 std::memcmp(x.temporal.data(), y.temporal.data(),
                                             x.temporal.size() * sizeof(area_wire::Temporal)) == 0,
                             "Compaction preserves exact published history and temporal tuples");
                        const auto compare_record = [&](enhanced_wire::Reference ref) {
                            if (enhanced_wire::absent(ref))
                                return;
                            need(enhanced_wire::valid(ref, 49), "Pair record reference bounds");
                            const auto index = std::size_t(ref.bank) * 49 + ref.pixel;
                            need(index < x.records.size() && index < y.records.size() &&
                                     std::memcmp(&x.records[index], &y.records[index],
                                                 sizeof(enhanced_wire::Record)) == 0,
                                 "Compaction preserves referenced owned records exactly");
                        };
                        for (const auto &h : x.history)
                            if (h.valid && h.selected_slot != contribution_wire::enhanced_null_slot)
                                compare_record(h.record);
                        for (const auto &t : x.temporal)
                            if (t.normalizer > 0 && t.target > 0 &&
                                t.selected_slot != contribution_wire::enhanced_null_slot)
                                compare_record(t.record);
                        if (!black)
                            need(std::ranges::any_of(b.image.raw_xyz,
                                                     [](const auto &xyz) { return xyz[1] > 0; }),
                                 "Emitting pair fixture must retain positive real transport");
                    }
                    need(physical > 0, "Compaction never removes fresh transport owners");
                    if (black)
                        need(compacted > 0,
                             "Black fixture must execute compacted selected-null work");
                }
    }
    std::cout << "Enhanced pair compaction exact shared-AS controls passed\n";
}
void enhanced_suffix_controls() {
    namespace tape = rypath_enhanced_suffix_tape;
    auto scene = enhanced_cavity();
    scene.max_depth = 6;
    scene.materials[0].constant = .9f; // Exercise later cached atoms, not just first E.
    std::array<std::uint64_t, 3> hits{};
    std::uint64_t misses = 0, published = 0;
    for (const unsigned seed_index : {0u, 4u, 11u}) {
        auto off = enhanced_config();
        off.width = off.height = 17;
        off.capacity = seed_index == 4 ? 289 : 37;
        off.seed = seed + 7919ull * seed_index;
        off.mutation_steps = 1;
        off.contribution_replay = FrameContributionReplay::prefix;
        off.cache_temporal_evaluations = false; // Force both temporal directions.
        off.enhanced_suffix_cache = false;
        auto on = off;
        on.enhanced_suffix_cache = true;
        auto pair = FrameSessionTestAccess::build_shared_scene(scene, off, on);
        need(pair.status == GeometryStatus::success && pair.shared_renderer &&
                 pair.independent_history && pair.independent_owned_records &&
                 pair.independent_suffix_storage,
             pair.diagnostic);
        FrameCamera moved{.camera = scene.camera,
                          .camera_medium = scene.camera_medium,
                          .raster_camera = scene.raster_camera,
                          .perspective_lens = scene.perspective_lens,
                          .texture_camera = scene.texture_camera};
        need(moved.raster_camera.has_value(), "Suffix fixture has an explicit raster camera");
        moved.raster_camera->origin.x += .0625f;
        std::uint64_t previous_context = 0;
        for (unsigned frame = 0; frame < 8; ++frame) {
            const auto camera = frame == 2 || frame == 3 ? std::optional<FrameCamera>(moved)
                                                         : std::optional<FrameCamera>{};
            std::cout << "suffix_begin seed=" << off.seed << " frame=" << frame << " arm=off"
                      << std::endl;
            auto a = enhanced_capture(*pair.sessions[0], off, scene.max_depth, frame, high_sample,
                                      camera);
            std::cout << "suffix_begin seed=" << off.seed << " frame=" << frame << " arm=on"
                      << std::endl;
            auto b = enhanced_capture(*pair.sessions[1], on, scene.max_depth, frame, high_sample,
                                      camera);
            exact(a.image, b.image);
            need(!a.info.enhanced_suffix_cache && b.info.enhanced_suffix_cache &&
                     a.info.enhanced_suffix_hits == std::array<std::uint64_t, 3>{} &&
                     a.info.enhanced_suffix_misses == 0 &&
                     a.info.mutation_accepts == b.info.mutation_accepts &&
                     a.info.completed_evaluations == b.info.completed_evaluations &&
                     a.info.analytic_null_evaluations == b.info.analytic_null_evaluations,
                 "Suffix on/off preserves exact output, mutation and owner accounting");
            const auto x =
                FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[0], frame, true);
            const auto y =
                FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[1], frame, true);
            need(x.status == GeometryStatus::success && y.status == GeometryStatus::success,
                 x.diagnostic + y.diagnostic);
            need(x.enhanced_suffix.empty() && x.suffix_context == 0 &&
                     y.enhanced_suffix.size() == 4u * 17u * 17u && y.suffix_context != 0 &&
                     x.history.size() == y.history.size() && x.temporal.size() == y.temporal.size(),
                 "Disabled cache has a sentinel; enabled snapshot owns all four banks");
            need(std::memcmp(x.history.data(), y.history.data(),
                             x.history.size() * sizeof(area_wire::History)) == 0 &&
                     std::memcmp(x.temporal.data(), y.temporal.data(),
                                 x.temporal.size() * sizeof(area_wire::Temporal)) == 0,
                 "Suffix reuse preserves complete published reservoir tuples");
            if (frame == 2 || frame == 4)
                need(y.suffix_context > previous_context,
                     "Changed camera receives a new context identity, including A-B-A");
            else if (frame != 0)
                need(y.suffix_context == previous_context,
                     "Exactly matching current contexts retain their identity");
            previous_context = y.suffix_context;
            for (unsigned k = 0; k < 3; ++k)
                hits[k] += b.info.enhanced_suffix_hits[k];
            misses += b.info.enhanced_suffix_misses;
            for (const auto &h : y.history) {
                if (!h.valid || h.selected_slot == contribution_wire::enhanced_null_slot)
                    continue;
                const auto index = std::size_t(h.record.bank) * 289 + h.record.pixel;
                need(index < y.enhanced_suffix.size(), "Published suffix reference is in bounds");
                const auto &entry = y.enhanced_suffix[index];
                if (entry.version == 0)
                    continue;
                tape::Input identity{};
                identity.selected_slot = h.selected_slot;
                identity.pivot_event = y.records[index].event_ordinal;
                identity.context_low = entry.context_low;
                identity.context_high = entry.context_high;
                need(tape::header(entry, identity) == tape::Status::ok &&
                         y.records[index].tag == rypath_enhanced_chart::interior_zomega,
                     "Published tape remains associated with its owned chart and selected slot");
                ++published;
            }
            std::cout << "suffix_exact seed=" << off.seed << " frame=" << frame
                      << " context=" << y.suffix_context
                      << " hits=" << b.info.enhanced_suffix_hits[0] << ','
                      << b.info.enhanced_suffix_hits[1] << ',' << b.info.enhanced_suffix_hits[2]
                      << " misses=" << b.info.enhanced_suffix_misses << '\n';
        }
    }
    need(published > 0 && hits[0] > 0 && hits[1] > 0 && hits[2] > 0 && misses > 0,
         "Every bounded tape length hits and unsupported/changed contexts fall back");
    std::cout << "suffix_exact totals hits=" << hits[0] << ',' << hits[1] << ',' << hits[2]
              << " misses=" << misses << " published=" << published << '\n';
    enhanced_suffix_conductor();
    enhanced_suffix_context_controls();
    enhanced_suffix_failed_context();
    enhanced_suffix_full_inactive();
}
void enhanced_mean() {
    auto scene = diffuse();
    scene.max_depth = 1;
    for (unsigned i = 0; i < scene.point_lights[0].intensity.size(); ++i)
        scene.point_lights[0].intensity[i] = i < 200 ? 4.f : .25f;
    frames(scene);
    enhanced_mean_contrast(scene, "D1-point-environment");
    // Every interior ray hits another diffuse wall. At second surface beta=.5
    // with eta1, so an effective discovery gate is possible; later atoms remain
    // positive. Mean agreement cannot establish that this gate actually ran.
    // Ideal radiance: L*(1+rho+rho^2+rho^3)=.234375; D2=.21875.
    std::cout << "enhanced_cavity rho=.5 emission=.125 D3_radiance=.234375"
                 " D2_negative=.21875 gate_execution_requires_separate_probe"
              << std::endl;
    enhanced_mean_contrast(enhanced_cavity(), "D3-closed-cavity", .234375f);
}
void enhanced_shared_scene_isolation() {
    std::cout << "Enhanced shared-AS owned storage isolation" << std::endl;
    auto c = enhanced_config();
    c.contribution_replay = FrameContributionReplay::prefix;
    c.cache_temporal_evaluations = false;
    auto cached_config = c;
    cached_config.cache_temporal_evaluations = true;
    auto scene = contribution_nee_only();
    scene.light_sampler = LightSamplerKind::uniform;
    auto other_light = scene.point_lights[0];
    other_light.position.x = -other_light.position.x;
    scene.point_lights.push_back(other_light);
    frames(scene); // Exactlytwo point masses: every positive selected N has h=.5.
    auto pair = FrameSessionTestAccess::build_shared_scene(scene, c, cached_config);
    need(pair.status == GeometryStatus::success && pair.sessions[0] && pair.sessions[1],
         pair.diagnostic);
    need(pair.shared_renderer && pair.independent_history && pair.independent_owned_records &&
             pair.enhanced_record_arenas[0] != 0 && pair.enhanced_record_arenas[1] != 0 &&
             pair.enhanced_record_arenas[0] != pair.enhanced_record_arenas[1] &&
             pair.enhanced_evaluations[0] != 0 && pair.enhanced_evaluations[1] != 0 &&
             pair.enhanced_evaluations[0] != pair.enhanced_evaluations[1],
         "Shared AS must retain independent Enhanced record/evaluation/history owners");
    std::unique_ptr<Frame> saved;
    FrameCapture saved_image;
    for (unsigned ordinal = 0; ordinal < 3; ++ordinal) {
        const auto a = enhanced_capture(*pair.sessions[0], c, scene.max_depth, ordinal, high_sample,
                                        {}, ordinal == 0 ? &saved : nullptr);
        const auto b = enhanced_capture(*pair.sessions[1], cached_config, scene.max_depth, ordinal,
                                        high_sample);
        exact(a.image, b.image);
        need(
            a.info.area_serial == b.info.area_serial &&
                a.info.history_generation == b.info.history_generation &&
                a.info.area_stencil_offsets == b.info.area_stencil_offsets &&
                b.info.completed_evaluations <= a.info.completed_evaluations &&
                b.info.analytic_null_evaluations <= a.info.analytic_null_evaluations &&
                (a.info.completed_evaluations - b.info.completed_evaluations) +
                        (a.info.analytic_null_evaluations - b.info.analytic_null_evaluations) ==
                    b.info.cached_evaluations &&
                a.info.mutation_accepts == b.info.mutation_accepts,
            "Interleaved shared-AS cacheoff/on retains nonunit-h selected responses and decisions");
        if (ordinal == 0)
            saved_image = a.image;
        need(saved != nullptr, "First completed frame lease retained");
        const auto recaptured = saved->capture();
        need(recaptured.status == GeometryStatus::success, recaptured.diagnostic);
        exact(saved_image, recaptured);
        const auto info = saved->poll();
        need(info.status == FrameStatus::complete && info.area_serial == 0 &&
                 info.history_generation == 1 && info.sample_index == high_sample,
             "Completed Frame output/receipt survives another session/frame advancing");
    }
}
RenderScene enhanced_point_triangle_scene(LightSamplerKind sampler) {
    auto scene = base();
    scene.max_depth = 1;
    scene.light_sampler = sampler;
    SpectralMaterial receiver;
    receiver.kind = SpectralMaterialKind::constant;
    receiver.constant = .6f;
    scene.materials.push_back(receiver);
    SpectralMaterial black;
    black.kind = SpectralMaterialKind::constant;
    scene.materials.push_back(black);
    const auto quad = [&](std::array<GeometryPoint, 4> points, unsigned material) {
        GeometryMesh mesh;
        mesh.positions.assign(points.begin(), points.end());
        mesh.triangles = {{0, 2, 1}, {0, 3, 2}}; // Faces toward the camera / -Z.
        const auto index = unsigned(scene.geometry.meshes.size());
        scene.geometry.meshes.push_back(std::move(mesh));
        scene.geometry.instances.push_back({index, {}});
        scene.instance_materials.push_back(material);
        scene.instance_emissions.push_back(no_area_emission);
        scene.instance_surface_transforms.emplace_back();
    };
    quad({{{-1, -2, 3}, {1, -2, 3}, {1, 2, 3}, {-1, 2, 3}}}, 0);
    // A distinct geometric normal, not a changed shading/normal-map model.
    quad({{{3, -2, 2.75f}, {5, -2, 3.25f}, {5, 2, 3.25f}, {3, 2, 2.75f}}}, 0);
    // Opaque black blocker hides both lights from the tilted panel. Camera rays
    // at x=0 and x=4 miss it; the original flat panel sees both lights.
    quad({{{2, -3, 2}, {3.4f, -3, 2}, {3.4f, 3, 2}, {2, 3, 2}}}, 1);
    AreaEmission emitter;
    emitter.radiance.fill(.75f);
    emitter.two_sided = false;
    scene.area_emissions.push_back(emitter);
    GeometryMesh triangle;
    triangle.positions = {{1, -1, 1}, {2, -1, 1}, {1, 1, 1}};
    triangle.triangles = {{0, 1, 2}}; // +Z, toward the receiver, outside camera.
    const auto index = unsigned(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(triangle));
    scene.geometry.instances.push_back({index, {}});
    scene.instance_materials.push_back(1);
    scene.instance_emissions.push_back(0);
    scene.instance_surface_transforms.emplace_back();
    PointLight light;
    light.position = {.75f, .5f, 1};
    light.intensity.fill(4);
    scene.point_lights.push_back(light);
    frames(scene);
    return scene;
}
void enhanced_nee_visibility() {
    for (auto sampler :
         {LightSamplerKind::uniform, LightSamplerKind::power, LightSamplerKind::bvh}) {
        std::cout << "Enhanced point/triangle visibility sampler=" << unsigned(sampler)
                  << std::endl;
        const auto scene = enhanced_point_triangle_scene(sampler);
        auto full_config = enhanced_config();
        full_config.mutation_steps = 0;
        full_config.cache_temporal_evaluations = false;
        auto prefix_config = full_config;
        prefix_config.contribution_replay = FrameContributionReplay::prefix;
        prefix_config.cache_temporal_evaluations = true;
        prefix_config.capacity = 7;
        prefix_config.quantum = 1;
        auto full = session(scene, full_config), prefix = session(scene, prefix_config);
        for (unsigned ordinal = 0; ordinal < 5; ++ordinal) {
            FrameCamera camera{.camera = scene.camera,
                               .camera_medium = scene.camera_medium,
                               .raster_camera = scene.raster_camera,
                               .perspective_lens = scene.perspective_lens,
                               .texture_camera = scene.texture_camera};
            need(camera.raster_camera.has_value(), "NEE transition camera explicit");
            const bool occluded = ordinal == 2 || ordinal == 3;
            const bool same_context = ordinal == 1 || ordinal == 3;
            camera.raster_camera->origin.x = occluded ? 4.f : 0.f;
            const auto a = enhanced_capture(*full, full_config, 1, ordinal, high_sample, camera,
                                            nullptr, false);
            const auto b = enhanced_capture(*prefix, prefix_config, 1, ordinal, high_sample, camera,
                                            nullptr, same_context);
            exact(a.image, b.image);
            need(
                a.info.mutation_accepts == 0 && b.info.mutation_accepts == 0 &&
                    b.info.completed_evaluations <= a.info.completed_evaluations &&
                    b.info.analytic_null_evaluations <= a.info.analytic_null_evaluations &&
                    (a.info.completed_evaluations - b.info.completed_evaluations) +
                            (a.info.analytic_null_evaluations - b.info.analytic_null_evaluations) ==
                        b.info.cached_evaluations,
                "Forced-NEE full/prefix retains visibility/null decisions");
            for (const auto &xyz : a.image.raw_xyz)
                need(occluded ? xyz[0] == 0 && xyz[1] == 0 && xyz[2] == 0 : xyz[1] > 0,
                     "Point/triangle NEE actually changes positive to occluded and back");
            if (ordinal == 2)
                need(a.info.history_used &&
                         a.info.completed_evaluations >
                             std::uint64_t(full_config.width) * full_config.height &&
                         a.info.analytic_null_evaluations > 0,
                     "Real history is evaluated at an occluded destination, not deleted");
        }
    }
}
void enhanced_nee_controls() {
    enhanced_nee_visibility();
    // Each proposal policy gets its OWN32 independent seeds and predeclared
    // 5SE+128epsilon gate. Never pool opposite signed biases across samplers.
    for (auto sampler :
         {LightSamplerKind::uniform, LightSamplerKind::power, LightSamplerKind::bvh}) {
        const auto label = sampler == LightSamplerKind::uniform ? "D1-point-triangle-uniform"
                           : sampler == LightSamplerKind::power ? "D1-point-triangle-power-alias"
                                                                : "D1-point-triangle-BVH";
        enhanced_mean_contrast(enhanced_point_triangle_scene(sampler), label);
    }
}

void enhanced_rejections() {
    const auto reject = [](const RenderScene &scene, const FrameConfig &c) {
        const auto result = build_frame_session(scene, c);
        need(result.status == GeometryStatus::failure && !result.session &&
                 !result.diagnostic.empty(),
             "Unsupported Enhanced configuration fails before frame work");
    };
    auto c = enhanced_config();
    auto depth = diffuse();
    depth.max_depth = 66;
    reject(depth, c);
    for (auto spectral : {SpectralTransport::scalar, SpectralTransport::packet}) {
        auto scene = diffuse();
        scene.spectral_transport = spectral;
        reject(scene, c);
    }
    auto invalid = c;
    invalid.estimator = FrameEstimator::independent;
    reject(diffuse(), invalid);
    invalid = c;
    invalid.execution = FrameExecution::megakernel;
    reject(diffuse(), invalid);
    for (auto layout : {FramePacketLayout::hot_cold, FramePacketLayout::split_trace}) {
        invalid = c;
        invalid.packet_layout = layout;
        reject(diffuse(), invalid);
    }
    for (unsigned events : {2u, 4u}) {
        invalid = c;
        invalid.packet_events = events;
        reject(diffuse(), invalid);
    }
}
void enhanced_controls() {
    enhanced_rejections();
    enhanced_shared_scene_isolation();
    enhanced_black();
    enhanced_single_endpoint();
    enhanced_null_transitions();
    enhanced_scheduling();
    enhanced_mean();
}

void contribution_controls() {
    contribution_rejections();
    contribution_single_endpoint();
    contribution_scheduling();
    contribution_prefix_scheduling();
    contribution_multileaf_mean();
}
} // namespace
int main(int argc, char **argv) {
    try {
        const bool contribution_only =
            argc == 2 && std::string_view(argv[1]) == "--contribution-only";
        const bool enhanced_only = argc == 2 && std::string_view(argv[1]) == "--enhanced";
        const bool enhanced_nee = argc == 2 && std::string_view(argv[1]) == "--enhanced-nee";
        const bool enhanced_motion = argc == 2 && std::string_view(argv[1]) == "--enhanced-motion";
        const bool enhanced_pairs = argc == 2 && std::string_view(argv[1]) == "--enhanced-pairs";
        const bool enhanced_suffix = argc == 2 && std::string_view(argv[1]) == "--enhanced-suffix";
        const bool enhanced_hybrid = argc == 2 && std::string_view(argv[1]) == "--enhanced-hybrid";
        need(argc == 1 || contribution_only || enhanced_only || enhanced_nee || enhanced_hybrid ||
                 enhanced_suffix || enhanced_pairs || enhanced_motion,
             "Expected optional --contribution-only, --enhanced, --enhanced-nee or "
             "--enhanced-hybrid, --enhanced-suffix, --enhanced-pairs or --enhanced-motion");
        // Device availability is checked once. Unsupported arithmetic/scene
        // failures below are failures, never reclassified as hardware skips.
        std::cout << "Hero native: capability and cold pipelines" << std::endl;
        auto capability = build_metal_renderer(diffuse());
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
        if (capability.status == GeometryStatus::no_device)
            return 77;
#endif
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
        if (capability.status == GeometryStatus::no_ray_tracing)
            return 77;
#endif
        need(capability.status == GeometryStatus::success && capability.scene,
             capability.diagnostic);
        capability.scene.reset();
        if (enhanced_motion) {
            enhanced_motion_controls();
            return 0;
        }
        if (enhanced_pairs) {
            enhanced_pair_compaction_controls();
            return 0;
        }
        if (enhanced_suffix) {
            enhanced_suffix_controls();
            std::cout << "Enhanced sequential suffix exact controls passed\n";
            return 0;
        }
        if (enhanced_hybrid) {
            enhanced_hybrid_controls();
            std::cout << "Enhanced hybrid positive fresh/nonself/mean/cache controls passed\n";
            return 0;
        }
        if (enhanced_nee) {
            enhanced_nee_controls();
            std::cout
                << "Enhanced point/triangle proposal-density and visibility controls passed\n";
            return 0;
        }
        if (enhanced_only) {
            enhanced_controls();
            std::cout << "Enhanced canonical null/support/physical/scheduling controls passed\n";
            return 0;
        }
        if (contribution_only) {
            contribution_controls();
            std::cout
                << "Hero contribution-domain accounting/physical/scheduling controls passed\n";
            return 0;
        }
        std::cout << "Hero native: diffuse backend comparison" << std::endl;
        offline_wavefront(diffuse());
        auto uniform = diffuse();
        uniform.hero_wavelengths = HeroWavelengthSampling::uniform;
        offline_wavefront(uniform);
        visible_mapping_and_area0();
        std::cout << "Hero native: dispersive backend comparison" << std::endl;
        offline_wavefront(dispersive());
        std::cout << "Hero native: seven schedules and replay" << std::endl;
        scheduling(dispersive());
        std::cout << "Hero native: positive solid transmission" << std::endl;
        transmission_witness();
        std::cout << "Hero native: earlier emission" << std::endl;
        earlier_emission();
        std::cout << "Hero native: depth 65" << std::endl;
        depth65();
        std::cout << "Hero native: matched offline/wavefront, seven schedules, Area replay, "
                     "positive glass transmission, early emission and actual depth65 passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
