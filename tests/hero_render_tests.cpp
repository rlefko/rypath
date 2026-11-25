// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"
#include "hero_spectrum_shared.h"
#include "replay_shared.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
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
    need(validate_render_scene(scene).empty(), "Invalid synthetic hero scene");
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
                 info.completed_evaluations == pixels * ((history ? 7u : 5u) + c.mutation_steps) &&
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
} // namespace
int main() {
    try {
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
