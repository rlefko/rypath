// SPDX-License-Identifier: GPL-3.0-only
// Shared fixed packet shade body; intentionally unguarded and included in kernels.
#if !defined(RYPATH_PACKET_SHADE_SEPARATED) || !defined(RYPATH_PACKET_SHADE_EVENTS) ||             \
    !defined(RYPATH_PACKET_SHADE_PRETRACED)
#error "Select the fixed packet shade policy before including this body"
#endif
#ifndef RYPATH_PACKET_SHADE_HERO
if (rypath_bdpt::hero_enabled(p)) {
#define RYPATH_PACKET_SHADE_HERO 1
#include "packet_shade_body.inc"
#undef RYPATH_PACKET_SHADE_HERO
} else {
#define RYPATH_PACKET_SHADE_HERO 0
#include "packet_shade_body.inc"
#undef RYPATH_PACKET_SHADE_HERO
}
#else
uint slot;
if (!wavefront_slot(a, lane, slot))
    return;
#if RYPATH_PACKET_SHADE_HERO && !RYPATH_PACKET_SHADE_SEPARATED
if (wavefront_enhanced() && wavefront_complete_null(a, slot))
    return;
#endif
#if RYPATH_PACKET_SHADE_HERO && RYPATH_PACKET_SHADE_SEPARATED
auto state = wavefront_load_hero_separated(a, slot);
#elif RYPATH_PACKET_SHADE_HERO
auto state = wavefront_load_hero_packed(a, slot);
#elif RYPATH_PACKET_SHADE_SEPARATED
auto state = wavefront_load_packet_separated(a, slot);
#else
auto state = wavefront_load_packet(a, slot);
#endif
const GraphResources graph{
    graph_nodes,
    graph_knots,
    &graph_p,
    {image_textures, image_levels, image_texels, image_transfer, image_spectra, &texture_p}};
const ray incoming(state.ray_origin, state.ray_direction, state.ray_min_distance,
                   state.ray_max_distance);
SurfaceAlpha alpha{
    materials,     material_data, instances,      positions,        triangles,          patches,
    normals,       texture_uvs,   texture_frames, surface_vertices, surface_transforms, &p,
    &state.sample, graph,         incoming};
const rypath_image_light::Images light_images{image_textures, image_levels,  image_texels,
                                              image_transfer, image_spectra, illuminant,
                                              &texture_p};
rypath_bdpt::Resources resources{
    alpha,           knots,        emissions, emitters, environment,
    environment_cdf, light_images, &tables,   scene,    intersection_table};
rypath_replay::ReplayState replay{};
if (a.config->replay_enabled) {
    if (state.sample.pixel >= a.config->pixel_count) {
        wavefront_fail(a, wf::invalid_sample);
        return;
    }
    replay = a.replay_input[state.sample.pixel];
    if (!rypath_replay::valid(replay)) {
        wavefront_fail(a, wf::invalid_sample);
        return;
    }
}
#if RYPATH_PACKET_SHADE_PRETRACED
const auto traced = a.packet_hit[slot];
const bool needs_hit = wavefront_packet_event_needs_hit(state.complete, state.count, state.beta,
                                                        incoming, p.max_depth + 2);
if (!rypath_packet_event::ready_valid(needs_hit, traced.ready)) {
    wavefront_fail(a, wf::invalid_sample);
    return;
}
rypath_bdpt::packet_pt_step_pretraced(resources, state,
                                      a.config->replay_enabled ? &replay : nullptr, traced.hit);
#elif RYPATH_PACKET_SHADE_EVENTS == 1
#if RYPATH_PACKET_SHADE_HERO && !RYPATH_PACKET_SHADE_SEPARATED
if (wavefront_enhanced()) {
    auto enhanced = reinterpret_cast<device rypath_enhanced_runtime::State *>(a.contribution_state)[slot];
    rypath_enhanced_runtime::Sink sink{enhanced, a.cie, p, *a.config, state.sample.pixel};
    rypath_bdpt::packet_pt_step_enhanced(resources, state, &replay, sink);
    reinterpret_cast<device rypath_enhanced_runtime::State *>(a.contribution_state)[slot] = enhanced;
    if (enhanced.failed) {
        wavefront_fail(a, wf::invalid_sample);
        return;
    }
} else if (wavefront_contribution()) {
    auto contribution = reinterpret_cast<device rypath_contribution_runtime::State *>(a.contribution_state)[slot];
    rypath_contribution_runtime::Sink sink{contribution, a.cie, p, *a.config, state.sample.pixel};
    rypath_bdpt::packet_pt_step_contribution(resources, state, &replay, sink);
    reinterpret_cast<device rypath_contribution_runtime::State *>(a.contribution_state)[slot] = contribution;
    if (contribution.failed) {
        wavefront_fail(a, wf::invalid_sample);
        return;
    }
} else
#endif
    rypath_bdpt::packet_pt_step(resources, state, a.config->replay_enabled ? &replay : nullptr);
#else
#pragma unroll
for (uint event = 0; event < RYPATH_PACKET_SHADE_EVENTS && !state.complete; ++event)
    rypath_bdpt::packet_pt_step(resources, state, a.config->replay_enabled ? &replay : nullptr);
#endif
#if RYPATH_PACKET_SHADE_SEPARATED
wavefront_store_packet_separated(a, slot, state);
#elif RYPATH_PACKET_SHADE_HERO
wavefront_store_hero_packed(a, slot, state);
#else
wavefront_store_packet(a, slot, state);
#endif
if (state.complete) {
    const auto result = rypath_bdpt::packet_pt_result(state, p);
    const uint pixel = result.pixel;
    float3 value;
    if (pixel >= a.config->pixel_count ||
        !rypath_frame_sensor::packet(result, pixel, p.sample_low, p.sample_high, a.cie, value)) {
        wavefront_fail(a, wf::invalid_sample);
        return;
    }
#if RYPATH_PACKET_SHADE_HERO && !RYPATH_PACKET_SHADE_SEPARATED
    if (wavefront_enhanced()) {
        auto enhanced = reinterpret_cast<device rypath_enhanced_runtime::State *>(a.contribution_state)[slot];
        rypath_enhanced_runtime::Sink sink{enhanced, a.cie, p, *a.config, pixel};
        const auto request = a.contribution_evaluation[pixel];
        rypath::contribution_wire::Evaluation evaluated;
        if (!sink.finish(request, value, evaluated)) {
            wavefront_fail(a, wf::invalid_sample);
            return;
        }
        a.contribution_evaluation[pixel] = evaluated;
    } else if (wavefront_contribution()) {
        auto contribution = reinterpret_cast<device rypath_contribution_runtime::State *>(a.contribution_state)[slot];
        rypath_contribution_runtime::Sink sink{contribution, a.cie, p, *a.config, pixel};
        const auto request = a.contribution_evaluation[pixel];
        rypath::contribution_wire::Evaluation evaluated;
        if (!sink.finish(request, value, evaluated)) {
            wavefront_fail(a, wf::invalid_sample);
            return;
        }
        a.contribution_evaluation[pixel] = evaluated;
    }
#endif
    wavefront_publish(a, pixel, value);
    wavefront_append(a, &a.counters->free, a.free_slots, slot);
} else {
    wavefront_append(a, &a.counters->out, a.queue_out, slot);
}

#endif // RYPATH_PACKET_SHADE_HERO
