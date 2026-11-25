// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included after render.metal's scalar and packet definitions, never compiled separately.
#include "../src/wavefront_wire.h"
#include "frame_sensor.h"
#include "packet_event_gate.h"
namespace wf = rypath::wavefront_wire;
// Optional defaults keep standalone queue probes on the packed/one-event path.
// Session pipelines always provide both values after the contract-6 handshake.
constant uint rypath_wavefront_packet_layout [[function_constant(wf::packet_layout_constant)]];
constant uint rypath_wavefront_packet_events [[function_constant(wf::packet_events_constant)]];
inline uint wavefront_packet_layout() {
    return is_function_constant_defined(rypath_wavefront_packet_layout)
               ? rypath_wavefront_packet_layout
               : 0u;
}
inline uint wavefront_packet_events() {
    return is_function_constant_defined(rypath_wavefront_packet_events)
               ? rypath_wavefront_packet_events
               : 1u;
}

struct WavefrontArguments {
    device const wf::Config *config [[id(wf::config_id)]];
    device uchar *scalar_state [[id(wf::scalar_state_id)]];
    device uchar *packet_state [[id(wf::packet_state_id)]];
    device uint *queue_in [[id(wf::queue_in_id)]];
    device uint *queue_out [[id(wf::queue_out_id)]];
    device uint *free_slots [[id(wf::free_slots_id)]];
    device wf::Counters *counters [[id(wf::counters_id)]];
    device wire::Point *raw_xyz [[id(wf::raw_xyz_id)]];
    device const wire::Point *cie [[id(wf::cie_id)]];
    device atomic_uint *done [[id(wf::done_id)]];
    device wf::DispatchArguments *current_dispatch [[id(wf::current_dispatch_id)]];
    device wf::DispatchArguments *refill_dispatch [[id(wf::refill_dispatch_id)]];
    device rypath_bdpt::PacketPTHot *packet_hot [[id(wf::packet_hot_id)]];
    device rypath_bdpt::Vertex *packet_cold [[id(wf::packet_cold_id)]];
    device rypath_bdpt::PacketPTHit *packet_hit [[id(wf::packet_hit_id)]];
    device const rypath_replay::ReplayState *replay_input [[id(wf::replay_input_id)]];
};
inline void wavefront_fail(constant WavefrontArguments &a, uint failure) {
    atomic_fetch_or_explicit(&a.counters->failure, failure, memory_order_relaxed);
}
inline bool wavefront_failed(constant WavefrontArguments &a) {
    return atomic_load_explicit(&a.counters->failure, memory_order_relaxed) != 0;
}
inline bool wavefront_config_valid(constant WavefrontArguments &a) {
    const auto c = *a.config;
    return c.pixel_count > 0 && c.capacity > 0 && c.capacity <= wf::default_capacity &&
           c.packet <= 2 && (c.model == wire::demo_model || c.model == wire::pbrt_model) &&
           c.group_width > 0 && c.replay_enabled <= 1 &&
           (c.packet_events == 1 || c.packet_events == 2 || c.packet_events == 4) &&
           c.packet_layout <= 2 && (c.packet_layout != 2 || c.packet_events == 1) &&
           (c.packet || (c.packet_events == 1 && c.packet_layout == 0)) &&
           c.packet_layout == wavefront_packet_layout() &&
           c.packet_events == wavefront_packet_events();
}
inline void wavefront_dispatch(device wf::DispatchArguments *args, uint count, uint group) {
    // Division plus remainder avoids the count+group-1 overflow.
    args->x = group == 0 ? 1 : max(1u, count / group + uint(count % group != 0));
    args->y = args->z = 1;
}
inline device rypath_scalar::ScalarState &wavefront_scalar(constant WavefrontArguments &a,
                                                           uint slot) {
    return reinterpret_cast<device rypath_scalar::ScalarState *>(a.scalar_state)[slot];
}
inline device rypath_bdpt::PacketPTState &wavefront_packet(constant WavefrontArguments &a,
                                                           uint slot) {
    return reinterpret_cast<device rypath_bdpt::PacketPTState *>(a.packet_state)[slot];
}
inline rypath_bdpt::PacketPTState wavefront_load_packet_separated(constant WavefrontArguments &a,
                                                                  uint slot) {
    const auto hot = a.packet_hot[slot];
    rypath_bdpt::PacketPTState state;
    state.latest = a.packet_cold[slot];
    state.preceding_position = hot.preceding_position;
    state.preceding_type = hot.preceding_type;
    state.sample = hot.sample;
    state.lambda = hot.lambda;
    state.radiance = hot.radiance;
    state.beta = hot.beta;
    state.ray_origin = hot.ray_origin;
    state.ray_direction = hot.ray_direction;
    state.pdf = hot.pdf;
    state.ray_min_distance = hot.ray_min_distance;
    state.ray_max_distance = hot.ray_max_distance;
    state.roulette_eta = hot.roulette_eta;
    state.hero = hot.hero;
    state.index = hot.index;
    state.count = hot.count;
    state.crossings = hot.crossings;
    state.complete = hot.complete;
    return state;
}
inline void wavefront_store_packet_separated(constant WavefrontArguments &a, uint slot,
                                             const thread rypath_bdpt::PacketPTState &state) {
    rypath_bdpt::PacketPTHot hot;
    hot.preceding_position = state.preceding_position;
    hot.preceding_type = state.preceding_type;
    hot.sample = state.sample;
    hot.lambda = state.lambda;
    hot.radiance = state.radiance;
    hot.beta = state.beta;
    hot.ray_origin = state.ray_origin;
    hot.ray_direction = state.ray_direction;
    hot.pdf = state.pdf;
    hot.ray_min_distance = state.ray_min_distance;
    hot.ray_max_distance = state.ray_max_distance;
    hot.roulette_eta = state.roulette_eta;
    hot.hero = state.hero;
    hot.index = state.index;
    hot.count = state.count;
    hot.crossings = state.crossings;
    hot.complete = state.complete;
    a.packet_hot[slot] = hot;
    a.packet_cold[slot] = state.latest;
}
inline void wavefront_store_packet(constant WavefrontArguments &a, uint slot,
                                   const thread rypath_bdpt::PacketPTState &state) {
    if (wavefront_packet_layout() == 0)
        wavefront_packet(a, slot) = state;
    else
        wavefront_store_packet_separated(a, slot, state);
}
inline bool wavefront_slot(constant WavefrontArguments &a, uint lane, thread uint &slot) {
    if (wavefront_failed(a))
        return false;
    const uint count = atomic_load_explicit(&a.counters->current, memory_order_relaxed);
    if (count > a.config->capacity) {
        wavefront_fail(a, wf::invalid_counters);
        return false;
    }
    if (lane >= count)
        return false;
    slot = a.queue_in[lane];
    if (slot >= a.config->capacity) {
        wavefront_fail(a, wf::invalid_slot);
        return false;
    }
    return true;
}
inline void wavefront_append(constant WavefrontArguments &a, device atomic_uint *counter,
                             device uint *queue, uint slot) {
    const uint position = atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
    if (position >= a.config->capacity) {
        wavefront_fail(a, wf::queue_overflow);
        return;
    }
    queue[position] = slot;
}
inline void wavefront_publish(constant WavefrontArguments &a, uint pixel,
                              const thread float3 &value) {
    if (pixel >= a.config->pixel_count || !all(isfinite(value))) {
        wavefront_fail(a, wf::invalid_sample);
        return;
    }
    if (atomic_exchange_explicit(a.done + pixel, 1u, memory_order_relaxed) != 0) {
        wavefront_fail(a, wf::duplicate_sample);
        return;
    }
    a.raw_xyz[pixel] = {value.x, value.y, value.z};
    atomic_fetch_add_explicit(&a.counters->completed, 1u, memory_order_relaxed);
}
inline bool wavefront_parameters(constant WavefrontArguments &a, constant wire::Parameters &p) {
    if (!wavefront_config_valid(a) || p.pixel_begin != 0 ||
        p.pixel_count != a.config->pixel_count || p.lane_count != a.config->pixel_count ||
        p.width == 0 || p.height == 0 || ulong(p.width) * p.height != a.config->pixel_count ||
        p.packet_transport != a.config->packet || p.model != a.config->model ||
        p.bidirectional != 0 ||
        (a.config->replay_enabled && p.sampler_kind != wire::independent_sampler)) {
        wavefront_fail(a, wf::invalid_config);
        return false;
    }
    return !wavefront_failed(a);
}
inline void wavefront_initialize_owner(constant WavefrontArguments &a, constant wire::Parameters &p,
                                       const device sobol::Tables &tables,
                                       const device float *environment, uint slot, uint pixel) {
    rypath_replay::ReplayState replay{};
    if (a.config->replay_enabled) {
        replay = a.replay_input[pixel];
        if (!rypath_replay::valid(replay)) {
            wavefront_fail(a, wf::invalid_sample);
            return;
        }
    }
    if (a.config->packet) {
        ulong index = 0;
        wire::Sample sample = a.config->replay_enabled
                                  ? begin_replay_sample(p, tables, pixel, replay)
                                  : begin_sample(p, tables, pixel, index);
        if (!a.config->replay_enabled) {
            const auto lens = draw_words(p, tables, sample, index, 1);
            sample.lens_words[0] = lens.x;
            sample.lens_words[1] = lens.y;
        }
        auto lambda = rypath_bdpt::packet_wavelengths(p, sample_uniform(p, sample.random_words[2]));
        sample.wavelength = lambda.lambda[0];
        const ray incoming = primary_ray(p, sample);
        rypath_bdpt::PacketPTState state;
        rypath_bdpt::packet_pt_initialize(
            state, sample, lambda, incoming,
            rypath_bdpt::camera_pdf(p, incoming.origin, incoming.direction), index, p);
        wavefront_store_packet(a, slot, state);
    } else {
        rypath_scalar::ScalarResources resources{};
        resources.p = &p;
        resources.tables = &tables;
        resources.environment = environment;
        resources.replay = a.config->replay_enabled ? &replay : nullptr;
        wavefront_scalar(a, slot) = rypath_scalar::scalar_init(resources, pixel);
    }
}
kernel void rypath_wavefront_contract(device wf::Contract *out [[buffer(0)]]) {
    *out = {wf::contract,
            sizeof(rypath_scalar::ScalarState),
            alignof(rypath_scalar::ScalarState),
            sizeof(rypath_bdpt::PacketPTState),
            alignof(rypath_bdpt::PacketPTState),
            sizeof(wf::Config),
            sizeof(wf::Counters),
            sizeof(wf::DispatchArguments),
            sizeof(rypath_bdpt::PacketPTHot),
            alignof(rypath_bdpt::PacketPTHot),
            sizeof(rypath_bdpt::Vertex),
            alignof(rypath_bdpt::Vertex),
            sizeof(rypath_bdpt::PacketPTHit),
            alignof(rypath_bdpt::PacketPTHit),
            {0, 0}};
}
kernel void rypath_wavefront_reset(constant WavefrontArguments &a [[buffer(1)]],
                                   uint lane [[thread_position_in_grid]]) {
    if (lane < a.config->pixel_count)
        atomic_store_explicit(a.done + lane, 0u, memory_order_relaxed);
    if (lane != 0)
        return;
    const bool valid = wavefront_config_valid(a);
    const uint count = valid ? min(a.config->pixel_count, a.config->capacity) : 0u;
    atomic_store_explicit(&a.counters->current, count, memory_order_relaxed);
    atomic_store_explicit(&a.counters->out, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->free, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->next_pixel, count, memory_order_relaxed);
    atomic_store_explicit(&a.counters->completed, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->failure, valid ? 0u : wf::invalid_config,
                          memory_order_relaxed);
    atomic_store_explicit(&a.counters->rounds, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->refill_begin, 0u, memory_order_relaxed);
    wavefront_dispatch(a.current_dispatch, count, a.config->group_width);
    wavefront_dispatch(a.refill_dispatch, 0, a.config->group_width);
}
kernel void rypath_wavefront_initialize(constant wire::Parameters &p [[buffer(0)]],
                                        constant WavefrontArguments &a [[buffer(1)]],
                                        const device float *environment [[buffer(7)]],
                                        const device sobol::Tables &tables [[buffer(12)]],
                                        uint lane [[thread_position_in_grid]]) {
    if (!wavefront_parameters(a, p))
        return;
    const uint count = min(a.config->capacity, a.config->pixel_count);
    if (lane >= count)
        return;
    wavefront_initialize_owner(a, p, tables, environment, lane, lane);
    a.queue_in[lane] = lane;
}

#define RYPATH_WAVEFRONT_PARAMETERS(TableType)                                                     \
    constant wire::Parameters &p [[buffer(0)]], constant WavefrontArguments &a [[buffer(1)]],      \
        instance_acceleration_structure scene [[buffer(2)]],                                       \
        const device wire::Instance *instances [[buffer(3)]],                                      \
        const device wire::Point *positions [[buffer(4)]],                                         \
        const device wire::Triangle *triangles [[buffer(5)]],                                      \
        const device wire::Material *materials [[buffer(6)]],                                      \
        const device float *environment [[buffer(7)]],                                             \
        const device wire::SpectralKnot *knots [[buffer(8)]],                                      \
        const device wire::AreaEmission *emissions [[buffer(9)]],                                  \
        const device wire::Emitter *emitters [[buffer(10)]],                                       \
        const device wire::Point *normals [[buffer(11)]],                                          \
        const device sobol::Tables &tables [[buffer(12)]],                                         \
        const device wire::Patch *patches [[buffer(13)]],                                          \
        TableType intersection_table [[buffer(14)]],                                               \
        const device wire::Point2 *texture_uvs [[buffer(15)]],                                     \
        const device wire::TextureFrame *texture_frames [[buffer(16)]],                            \
        const device wire::ImageTexture *image_textures [[buffer(17)]],                            \
        const device wire::TextureLevel *image_levels [[buffer(18)]],                              \
        const device wire::Word *image_texels [[buffer(19)]],                                      \
        const device float *image_transfer [[buffer(20)]],                                         \
        const device float *image_spectra [[buffer(21)]],                                          \
        constant wire::TextureParameters &texture_p [[buffer(22)]],                                \
        const device graph_wire::Node *graph_nodes [[buffer(23)]],                                 \
        const device wire::SpectralKnot *graph_knots [[buffer(24)]],                               \
        constant wire::TextureGraphParameters &graph_p [[buffer(25)]],                             \
        const device wire::SurfaceVertex *surface_vertices [[buffer(26)]],                         \
        const device wire::SurfaceTransform *surface_transforms [[buffer(27)]],                    \
        const device float *environment_cdf [[buffer(28)]],                                        \
        const device float *illuminant [[buffer(29)]],                                             \
        const device uint *material_data [[buffer(30)]], uint lane [[thread_position_in_grid]]

kernel void rypath_wavefront_trace_scalar(RYPATH_WAVEFRONT_PARAMETERS(MixedIntersectionTable)) {
    uint slot;
    if (!wavefront_slot(a, lane, slot))
        return;
    rypath_scalar::ScalarResources resources{&p,
                                             scene,
                                             instances,
                                             positions,
                                             triangles,
                                             materials,
                                             environment,
                                             knots,
                                             emissions,
                                             emitters,
                                             normals,
                                             &tables,
                                             patches,
                                             intersection_table,
                                             texture_uvs,
                                             texture_frames,
                                             image_textures,
                                             image_levels,
                                             image_texels,
                                             image_transfer,
                                             image_spectra,
                                             &texture_p,
                                             graph_nodes,
                                             graph_knots,
                                             &graph_p,
                                             surface_vertices,
                                             surface_transforms,
                                             environment_cdf,
                                             illuminant,
                                             material_data};
    auto state = wavefront_scalar(a, slot);
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
        resources.replay = &replay;
    }
    rypath_scalar::scalar_trace(resources, state);
    wavefront_scalar(a, slot) = state;
}

kernel void rypath_wavefront_shade_scalar(RYPATH_WAVEFRONT_PARAMETERS(MixedIntersectionTable)) {
    uint slot;
    if (!wavefront_slot(a, lane, slot))
        return;
    rypath_scalar::ScalarResources resources{&p,
                                             scene,
                                             instances,
                                             positions,
                                             triangles,
                                             materials,
                                             environment,
                                             knots,
                                             emissions,
                                             emitters,
                                             normals,
                                             &tables,
                                             patches,
                                             intersection_table,
                                             texture_uvs,
                                             texture_frames,
                                             image_textures,
                                             image_levels,
                                             image_texels,
                                             image_transfer,
                                             image_spectra,
                                             &texture_p,
                                             graph_nodes,
                                             graph_knots,
                                             &graph_p,
                                             surface_vertices,
                                             surface_transforms,
                                             environment_cdf,
                                             illuminant,
                                             material_data};
    auto state = wavefront_scalar(a, slot);
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
        resources.replay = &replay;
    }
    rypath_scalar::scalar_shade(resources, state);
    wavefront_scalar(a, slot) = state;
    if (state.finished) {
        float3 value;
        const uint pixel = state.sample.pixel;
        if (pixel >= a.config->pixel_count ||
            !rypath_frame_sensor::scalar(state.sample, pixel, p.sample_low, p.sample_high, p.model,
                                         a.cie, value)) {
            wavefront_fail(a, wf::invalid_sample);
            return;
        }
        wavefront_publish(a, pixel, value);
        wavefront_append(a, &a.counters->free, a.free_slots, slot);
    } else {
        wavefront_append(a, &a.counters->out, a.queue_out, slot);
    }
}

inline bool wavefront_packet_event_needs_hit(bool complete, uint count, float4 beta, ray incoming,
                                             uint capacity) {
    return rypath_packet_event::needs_hit(complete, count, capacity, beta, incoming.origin,
                                          incoming.direction);
}

kernel void
rypath_wavefront_trace_packet(RYPATH_WAVEFRONT_PARAMETERS(instance_acceleration_structure)) {
    uint slot;
    if (!wavefront_slot(a, lane, slot))
        return;
    if (!wavefront_parameters(a, p) || !a.config->packet || wavefront_packet_layout() != 2)
        return;
    auto state = a.packet_hot[slot];
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
    rypath_bdpt::PacketPTHit traced{};
    if (wavefront_packet_event_needs_hit(state.complete, state.count, state.beta, incoming,
                                         p.max_depth + 2)) {
        traced.hit = rypath_bdpt::packet_pt_trace(resources, state.sample, state.lambda, incoming);
        traced.ready = 1;
    }
    a.packet_hot[slot] = state;
    a.packet_hit[slot] = traced;
}

kernel void
rypath_wavefront_shade_packet(RYPATH_WAVEFRONT_PARAMETERS(instance_acceleration_structure)) {
#define RYPATH_PACKET_SHADE_SEPARATED 0
#define RYPATH_PACKET_SHADE_EVENTS 1
#define RYPATH_PACKET_SHADE_PRETRACED 0
#include "packet_shade_body.inc"
#undef RYPATH_PACKET_SHADE_PRETRACED
#undef RYPATH_PACKET_SHADE_EVENTS
#undef RYPATH_PACKET_SHADE_SEPARATED
}

kernel void rypath_wavefront_shade_packet_packed_2(
    RYPATH_WAVEFRONT_PARAMETERS(instance_acceleration_structure)) {
#define RYPATH_PACKET_SHADE_SEPARATED 0
#define RYPATH_PACKET_SHADE_EVENTS 2
#define RYPATH_PACKET_SHADE_PRETRACED 0
#include "packet_shade_body.inc"
#undef RYPATH_PACKET_SHADE_PRETRACED
#undef RYPATH_PACKET_SHADE_EVENTS
#undef RYPATH_PACKET_SHADE_SEPARATED
}

kernel void rypath_wavefront_shade_packet_packed_4(
    RYPATH_WAVEFRONT_PARAMETERS(instance_acceleration_structure)) {
#define RYPATH_PACKET_SHADE_SEPARATED 0
#define RYPATH_PACKET_SHADE_EVENTS 4
#define RYPATH_PACKET_SHADE_PRETRACED 0
#include "packet_shade_body.inc"
#undef RYPATH_PACKET_SHADE_PRETRACED
#undef RYPATH_PACKET_SHADE_EVENTS
#undef RYPATH_PACKET_SHADE_SEPARATED
}

kernel void rypath_wavefront_shade_packet_hot_cold_1(
    RYPATH_WAVEFRONT_PARAMETERS(instance_acceleration_structure)) {
#define RYPATH_PACKET_SHADE_SEPARATED 1
#define RYPATH_PACKET_SHADE_EVENTS 1
#define RYPATH_PACKET_SHADE_PRETRACED 0
#include "packet_shade_body.inc"
#undef RYPATH_PACKET_SHADE_PRETRACED
#undef RYPATH_PACKET_SHADE_EVENTS
#undef RYPATH_PACKET_SHADE_SEPARATED
}

kernel void rypath_wavefront_shade_packet_hot_cold_2(
    RYPATH_WAVEFRONT_PARAMETERS(instance_acceleration_structure)) {
#define RYPATH_PACKET_SHADE_SEPARATED 1
#define RYPATH_PACKET_SHADE_EVENTS 2
#define RYPATH_PACKET_SHADE_PRETRACED 0
#include "packet_shade_body.inc"
#undef RYPATH_PACKET_SHADE_PRETRACED
#undef RYPATH_PACKET_SHADE_EVENTS
#undef RYPATH_PACKET_SHADE_SEPARATED
}

kernel void rypath_wavefront_shade_packet_hot_cold_4(
    RYPATH_WAVEFRONT_PARAMETERS(instance_acceleration_structure)) {
#define RYPATH_PACKET_SHADE_SEPARATED 1
#define RYPATH_PACKET_SHADE_EVENTS 4
#define RYPATH_PACKET_SHADE_PRETRACED 0
#include "packet_shade_body.inc"
#undef RYPATH_PACKET_SHADE_PRETRACED
#undef RYPATH_PACKET_SHADE_EVENTS
#undef RYPATH_PACKET_SHADE_SEPARATED
}

kernel void
rypath_wavefront_shade_packet_split(RYPATH_WAVEFRONT_PARAMETERS(instance_acceleration_structure)) {
#define RYPATH_PACKET_SHADE_SEPARATED 1
#define RYPATH_PACKET_SHADE_EVENTS 1
#define RYPATH_PACKET_SHADE_PRETRACED 1
#include "packet_shade_body.inc"
#undef RYPATH_PACKET_SHADE_PRETRACED
#undef RYPATH_PACKET_SHADE_EVENTS
#undef RYPATH_PACKET_SHADE_SEPARATED
}

#undef RYPATH_WAVEFRONT_PARAMETERS

kernel void rypath_wavefront_prepare_refill(constant WavefrontArguments &a [[buffer(1)]],
                                            uint lane [[thread_position_in_grid]]) {
    if (lane != 0)
        return;
    const uint free = atomic_load_explicit(&a.counters->free, memory_order_relaxed);
    const uint out = atomic_load_explicit(&a.counters->out, memory_order_relaxed);
    const uint current = atomic_load_explicit(&a.counters->current, memory_order_relaxed);
    const uint next = atomic_load_explicit(&a.counters->next_pixel, memory_order_relaxed);
    const uint completed = atomic_load_explicit(&a.counters->completed, memory_order_relaxed);
    if (!wavefront_config_valid(a))
        wavefront_fail(a, wf::invalid_config);
    if (free > a.config->capacity || out > a.config->capacity || current > a.config->capacity ||
        free + out != current || next > a.config->pixel_count || completed > next ||
        out != next - completed)
        wavefront_fail(a, wf::invalid_counters);
    if (wavefront_failed(a)) {
        wavefront_dispatch(a.refill_dispatch, 0u, a.config->group_width);
        return;
    }
    // The previous dispatch has finished producing the free list. Reserve one
    // contiguous owner range here; lane i owns begin+i without a global CAS
    // retry loop. Queue order changes only slot ownership, never sample identity.
    const uint count = min(free, a.config->pixel_count - next);
    atomic_store_explicit(&a.counters->refill_begin, next, memory_order_relaxed);
    atomic_store_explicit(&a.counters->free, count, memory_order_relaxed);
    atomic_store_explicit(&a.counters->next_pixel, next + count, memory_order_relaxed);
    wavefront_dispatch(a.refill_dispatch, count, a.config->group_width);
}
kernel void rypath_wavefront_refill(constant wire::Parameters &p [[buffer(0)]],
                                    constant WavefrontArguments &a [[buffer(1)]],
                                    const device float *environment [[buffer(7)]],
                                    const device sobol::Tables &tables [[buffer(12)]],
                                    uint lane [[thread_position_in_grid]]) {
    if (wavefront_failed(a))
        return;
    const uint count = atomic_load_explicit(&a.counters->free, memory_order_relaxed);
    const uint begin = atomic_load_explicit(&a.counters->refill_begin, memory_order_relaxed);
    const uint next = atomic_load_explicit(&a.counters->next_pixel, memory_order_relaxed);
    if (count > a.config->capacity || begin > a.config->pixel_count ||
        count > a.config->pixel_count - begin || next != begin + count) {
        wavefront_fail(a, wf::invalid_counters);
        return;
    }
    if (lane >= count)
        return;
    const uint slot = a.free_slots[lane];
    if (slot >= a.config->capacity) {
        wavefront_fail(a, wf::invalid_slot);
        return;
    }
    const uint pixel = begin + lane;
    wavefront_initialize_owner(a, p, tables, environment, slot, pixel);
    wavefront_append(a, &a.counters->out, a.queue_out, slot);
}
kernel void rypath_wavefront_advance(constant WavefrontArguments &a [[buffer(1)]],
                                     uint lane [[thread_position_in_grid]]) {
    if (lane != 0)
        return;
    uint count = atomic_load_explicit(&a.counters->out, memory_order_relaxed);
    const uint free = atomic_load_explicit(&a.counters->free, memory_order_relaxed);
    const uint completed = atomic_load_explicit(&a.counters->completed, memory_order_relaxed);
    const uint next = atomic_load_explicit(&a.counters->next_pixel, memory_order_relaxed);
    if (count > a.config->capacity || free > a.config->capacity ||
        completed > a.config->pixel_count || next > a.config->pixel_count ||
        (count == 0 && completed != a.config->pixel_count))
        wavefront_fail(a, wf::invalid_counters);
    if (wavefront_failed(a))
        count = 0;
    atomic_store_explicit(&a.counters->current, count, memory_order_relaxed);
    atomic_store_explicit(&a.counters->out, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->free, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->refill_begin, 0u, memory_order_relaxed);
    atomic_fetch_add_explicit(&a.counters->rounds, 1u, memory_order_relaxed);
    wavefront_dispatch(a.current_dispatch, count, a.config->group_width);
    wavefront_dispatch(a.refill_dispatch, 0u, a.config->group_width);
}
