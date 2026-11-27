// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"
#include "frame_session_test_access.hpp"
#include "hero_spectrum_shared.h"
#include "replay_shared.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
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
void enhanced_mean_contrast(const RenderScene &scene, std::string_view label,
                            std::optional<float> analytic_radiance = {}) {
    // Replication is by32 independent run seeds, never by correlated pixels.
    // Same predeclared5 empiricalSE +128epsilon criterion for both contrasts.
    constexpr unsigned repeats = 32;
    std::array<std::array<double, 3>, repeats> differences{};
    std::array<double, 3> reference_mean{}, candidate_mean{};
    std::array<std::array<double, 3>, repeats> reference_samples{}, candidate_samples{};
    for (unsigned n = 0; n < repeats; ++n) {
        auto c = enhanced_config();
        c.seed = seed + 7919ull * n;
        c.mutation_steps = 0;
        c.cache_temporal_evaluations = false;
        auto control = c;
        control.area_path_domain = FrameAreaPathDomain::summed;
        auto reference = session(scene, control), candidate = session(scene, c);
        const auto a = contribution_capture(*reference, control, scene.max_depth, 0, high_sample);
        const auto b = enhanced_capture(*candidate, c, scene.max_depth, 0, high_sample);
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
        need(argc == 1 || contribution_only || enhanced_only || enhanced_nee,
             "Expected optional --contribution-only, --enhanced or --enhanced-nee");
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
