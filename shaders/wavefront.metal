// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included after render.metal's scalar and packet definitions, never compiled separately.
#include "../src/wavefront_wire.h"
#include "contribution_sink.h"
#include "enhanced_sink.h"
#include "frame_sensor.h"
#include "packet_event_gate.h"
namespace wf = rypath::wavefront_wire;
// Optional defaults keep standalone queue probes on the packed/one-event path.
// Session pipelines always provide both values after the contract-6 handshake.
constant uint rypath_wavefront_packet_layout [[function_constant(wf::packet_layout_constant)]];
constant uint rypath_wavefront_packet_events [[function_constant(wf::packet_events_constant)]];
constant bool rypath_wavefront_contribution [[function_constant(wf::contribution_mode_constant)]];
constant bool rypath_wavefront_enhanced [[function_constant(wf::enhanced_mode_constant)]];
inline bool wavefront_enhanced() {
    return is_function_constant_defined(rypath_wavefront_enhanced) ? rypath_wavefront_enhanced
                                                                   : false;
}
inline bool wavefront_contribution() {
    return is_function_constant_defined(rypath_wavefront_contribution)
               ? rypath_wavefront_contribution
               : false;
}
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
    device uchar *hero_state [[id(wf::hero_state_id)]];
    device uchar *hero_cold [[id(wf::hero_cold_id)]];
    device rypath::contribution_wire::Evaluation *contribution_evaluation
        [[id(wf::contribution_evaluation_id)]];
    device uchar *contribution_state [[id(wf::contribution_state_id)]];
    device rypath::enhanced_wire::Record *enhanced_records [[id(wf::enhanced_records_id)]];
    device rypath::enhanced_wire::Evaluation *enhanced_evaluation
        [[id(wf::enhanced_evaluation_id)]];
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
           c.packet_events == wavefront_packet_events() &&
           c.contribution_replay <= rypath::contribution_wire::prefix_replay &&
           c.reserved2[0] == 0 && c.reserved2[1] == 0 &&
           (!wavefront_enhanced() || wavefront_contribution()) &&
           c.contribution_mode == (wavefront_enhanced() ? 2u : uint(wavefront_contribution())) &&
           (wavefront_contribution()
                ? (c.packet == 2 && c.replay_enabled == 1 && c.packet_layout == 0 &&
                   c.packet_events == 1 && c.contribution_depth <= 65 &&
                   rypath_reservoir::positive_normal(c.contribution_target_scale))
                : (c.contribution_replay == rypath::contribution_wire::full_replay &&
                   c.contribution_depth == 0 && as_type<uint>(c.contribution_target_scale) == 0 &&
                   c.contribution_serial_low == 0 && c.contribution_serial_high == 0));
}
inline void wavefront_dispatch(device wf::DispatchArguments *args, uint count, uint group) {
    // Division plus remainder avoids the count+group-1 overflow.
    args->x = group == 0 ? 1 : max(1u, count / group + uint(count % group != 0));
    args->y = args->z = 1;
}
// Union-containing payloads use dedicated opaque buffers: MSL argument-buffer
// reflection rejects unions even behind a typed pointer. Never alias packet pools.
inline device rypath_bdpt::HeroPTState &wavefront_hero_persistent(constant WavefrontArguments &a,
                                                                  uint slot) {
    return reinterpret_cast<device rypath_bdpt::HeroPTState *>(a.hero_state)[slot];
}
inline device rypath_bdpt::HeroPrevious &wavefront_hero_cold(constant WavefrontArguments &a,
                                                             uint slot) {
    return reinterpret_cast<device rypath_bdpt::HeroPrevious *>(a.hero_cold)[slot];
}
inline device rypath_scalar::ScalarState &wavefront_scalar(constant WavefrontArguments &a,
                                                           uint slot) {
    return reinterpret_cast<device rypath_scalar::ScalarState *>(a.scalar_state)[slot];
}
inline device rypath_bdpt::PacketPTState &wavefront_packet(constant WavefrontArguments &a,
                                                           uint slot) {
    return reinterpret_cast<device rypath_bdpt::PacketPTState *>(a.packet_state)[slot];
}
template <typename State>
inline rypath_bdpt::PacketPTHot wavefront_packet_hot(const thread State &state) {
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
    return hot;
}
inline rypath_bdpt::PacketPTState wavefront_load_packet(constant WavefrontArguments &a, uint slot) {
    return wavefront_packet(a, slot);
}
template <bool Separated>
inline rypath_bdpt::HeroWalkState wavefront_load_hero_layout(constant WavefrontArguments &a,
                                                             uint slot) {
    const auto hot = Separated ? a.packet_hot[slot] : wavefront_hero_persistent(a, slot).hot;
    rypath_bdpt::HeroWalkState state;
    state.latest =
        Separated ? wavefront_hero_cold(a, slot) : wavefront_hero_persistent(a, slot).latest;
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
inline rypath_bdpt::HeroWalkState wavefront_load_hero_packed(constant WavefrontArguments &a,
                                                             uint slot) {
    return wavefront_load_hero_layout<false>(a, slot);
}
inline rypath_bdpt::HeroWalkState wavefront_load_hero_separated(constant WavefrontArguments &a,
                                                                uint slot) {
    return wavefront_load_hero_layout<true>(a, slot);
}
// Initialization/refill pipelines receive policy constants. Fixed shade entries
// select the explicit helpers instead, since their layout constants are unset.
inline rypath_bdpt::HeroWalkState wavefront_load_hero(constant WavefrontArguments &a, uint slot) {
    return wavefront_packet_layout() == 0 ? wavefront_load_hero_packed(a, slot)
                                          : wavefront_load_hero_separated(a, slot);
}
inline void wavefront_store_hero_packed(constant WavefrontArguments &a, uint slot,
                                        const thread rypath_bdpt::HeroWalkState &state) {
    wavefront_hero_persistent(a, slot) = {state.latest, wavefront_packet_hot(state)};
}
inline void wavefront_store_packet_separated(constant WavefrontArguments &a, uint slot,
                                             const thread rypath_bdpt::HeroWalkState &state) {
    a.packet_hot[slot] = wavefront_packet_hot(state);
    wavefront_hero_cold(a, slot) = state.latest;
}
inline void wavefront_store_packet(constant WavefrontArguments &a, uint slot,
                                   const thread rypath_bdpt::HeroWalkState &state) {
    if (wavefront_packet_layout() == 0)
        wavefront_store_hero_packed(a, slot, state);
    else
        wavefront_store_packet_separated(a, slot, state);
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
    const auto hot = wavefront_packet_hot(state);
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
inline bool wavefront_complete_null(constant WavefrontArguments &a, uint slot) {
    const auto pending =
        reinterpret_cast<device rypath_enhanced_runtime::State *>(a.contribution_state)[slot];
    if (pending.requested_slot == rypath::contribution_wire::enhanced_null_slot) {
        const uint pixel = pending.pixel;
        if (pending.failed || pixel >= a.config->pixel_count || pending.next_slot != 0 ||
            pending.rr_cut != 0 || !rypath_enhanced_path::valid_state(pending.selection) ||
            !rypath_enhanced_runtime::unstarted_hybrid(pending.hybrid)) {
            wavefront_fail(a, wf::invalid_sample);
            return true;
        }
        const auto request = a.contribution_evaluation[pixel];
        if (request.ready != 0 || request.requested_slot != pending.requested_slot ||
            request.selected_slot != rypath_enhanced_path::fresh_slot ||
            as_type<uint>(request.normalizer) != 0) {
            wavefront_fail(a, wf::invalid_sample);
            return true;
        }
        auto owned = a.enhanced_evaluation[pixel];
        if (!rypath_enhanced_runtime::pending_owned(owned, a.config->pixel_count, pixel, false)) {
            wavefront_fail(a, wf::invalid_sample);
            return true;
        }
        owned.target_h_fraction = .5f;
        owned.target_h_exponent = 1;
        owned.ready = 1;
        if (owned.source.bank != owned.result.bank || owned.source.pixel != owned.result.pixel)
            a.enhanced_records[ulong(owned.result.bank) * a.config->pixel_count +
                               owned.result.pixel] = rypath::enhanced_wire::empty_record();
        a.enhanced_evaluation[pixel] = owned;
        a.contribution_evaluation[pixel] = {pending.requested_slot, pending.requested_slot, 0.f, 1};
        wavefront_publish(a, pixel, float3(0));
        if (wavefront_failed(a))
            return true;
        atomic_fetch_add_explicit(&a.counters->null_completed, 1u, memory_order_relaxed);
        wavefront_append(a, &a.counters->free, a.free_slots, slot);
        return true;
    }
    return false;
}
inline bool wavefront_parameters(constant WavefrontArguments &a, constant wire::Parameters &p) {
    if (!wavefront_config_valid(a) || p.pixel_begin != 0 ||
        p.pixel_count != a.config->pixel_count || p.lane_count != a.config->pixel_count ||
        p.width == 0 || p.height == 0 || ulong(p.width) * p.height != a.config->pixel_count ||
        p.packet_transport != a.config->packet || p.model != a.config->model ||
        p.bidirectional != 0 ||
        (a.config->replay_enabled && p.sampler_kind != wire::independent_sampler) ||
        (wavefront_contribution() && p.max_depth != a.config->contribution_depth)) {
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
    if (wavefront_enhanced()) {
        const auto request = a.contribution_evaluation[pixel];
        const auto owned = a.enhanced_evaluation[pixel];
        const auto state = rypath_enhanced_runtime::begin(request, *a.config, pixel, owned);
        reinterpret_cast<device rypath_enhanced_runtime::State *>(a.contribution_state)[slot] =
            state;
        if (state.failed) {
            wavefront_fail(a, wf::invalid_sample);
            return;
        }
        if (request.requested_slot == rypath::contribution_wire::enhanced_null_slot)
            return; // Queued owner, completed by shade without reading ray/BSDF storage.
    } else if (wavefront_contribution()) {
        const auto request = a.contribution_evaluation[pixel];
        const auto state = rypath_contribution_runtime::begin(request, *a.config);
        reinterpret_cast<device rypath_contribution_runtime::State *>(a.contribution_state)[slot] =
            state;
        if (state.failed) {
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
        if (a.config->packet == 2) {
            rypath_bdpt::HeroWalkState state;
            rypath_bdpt::packet_pt_initialize(
                state, sample, lambda, incoming,
                rypath_bdpt::camera_pdf(p, incoming.origin, incoming.direction), index, p);
            wavefront_store_packet(a, slot, state);
        } else {
            rypath_bdpt::PacketPTState state;
            rypath_bdpt::packet_pt_initialize(
                state, sample, lambda, incoming,
                rypath_bdpt::camera_pdf(p, incoming.origin, incoming.direction), index, p);
            wavefront_store_packet(a, slot, state);
        }
    } else {
        rypath_scalar::ScalarResources resources{};
        resources.p = &p;
        resources.tables = &tables;
        resources.environment = environment;
        resources.replay = a.config->replay_enabled ? &replay : nullptr;
        wavefront_scalar(a, slot) = rypath_scalar::scalar_init(resources, pixel);
    }
}
// Version the entry when its output grows: an older host must fail function
// lookup before binding its shorter contract buffer to this kernel.
kernel void rypath_wavefront_contract_v19(device wf::Contract *out [[buffer(0)]]) {
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
            sizeof(rypath_bdpt::HeroPTState),
            alignof(rypath_bdpt::HeroPTState),
            sizeof(rypath_bdpt::PacketPTHot),
            alignof(rypath_bdpt::PacketPTHot),
            sizeof(rypath_bdpt::HeroPrevious),
            alignof(rypath_bdpt::HeroPrevious),
            sizeof(rypath_contribution_runtime::State),
            alignof(rypath_contribution_runtime::State),
            sizeof(rypath_enhanced_runtime::State),
            alignof(rypath_enhanced_runtime::State),
            sizeof(rypath::enhanced_wire::Record),
            sizeof(rypath::enhanced_wire::Evaluation)};
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
    atomic_store_explicit(&a.counters->null_completed, 0u, memory_order_relaxed);
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
    const uint null_completed =
        atomic_load_explicit(&a.counters->null_completed, memory_order_relaxed);
    if (!wavefront_config_valid(a))
        wavefront_fail(a, wf::invalid_config);
    if (free > a.config->capacity || out > a.config->capacity || current > a.config->capacity ||
        free + out != current || next > a.config->pixel_count || completed > next ||
        out != next - completed || null_completed > completed ||
        (!wavefront_enhanced() && null_completed != 0))
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
    const uint null_completed =
        atomic_load_explicit(&a.counters->null_completed, memory_order_relaxed);
    const uint next = atomic_load_explicit(&a.counters->next_pixel, memory_order_relaxed);
    if (count > a.config->capacity || free > a.config->capacity ||
        completed > a.config->pixel_count || next > a.config->pixel_count ||
        (count == 0 && completed != a.config->pixel_count) || null_completed > completed ||
        (!wavefront_enhanced() && null_completed != 0))
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

// Exercise the same pre-traversal publication helper with poisoned ray/BSDF pools.
kernel void rypath_wavefront_null_probe(constant WavefrontArguments &a [[buffer(1)]],
                                        uint lane [[thread_position_in_grid]]) {
    uint slot;
    if (!wavefront_slot(a, lane, slot))
        return;
    if (!wavefront_enhanced() || !wavefront_complete_null(a, slot))
        wavefront_fail(a, wf::invalid_sample);
}

// Test-only layout witness lets the host inspect persisted scalar bytes
// independently of the thread-side aggregate comparison and union accesses.
kernel void rypath_hero_history_layout(device uint *out [[buffer(0)]]) {
    using namespace rypath_bdpt;
    out[0] = 0x48535435u;
    out[1] = __builtin_offsetof(HeroPTState, latest);
    out[2] = __builtin_offsetof(HeroPrevious, bsdf);
    out[3] = __builtin_offsetof(HeroFrozenBSDF, payload);
    out[4] = __builtin_offsetof(HeroFrozenBSDF, kind);
    out[5] = __builtin_offsetof(HeroFrozenBSDF, active_mask);
    out[6] = sizeof(rypath_dielectric::Parameters);
    out[7] = __builtin_offsetof(rypath_dielectric::Parameters, eta);
    out[8] = __builtin_offsetof(rypath_dielectric::Parameters, alpha_x);
    out[9] = __builtin_offsetof(rypath_dielectric::Parameters, alpha_y);
    out[10] = 3 * sizeof(uint); // The wire stores a canonical 0/1 word.
    out[11] = sizeof(uint);
}

// Deterministic ABI probe: use actual shader Prepared (including frozen table
// pointers), frames and history conversion, without scene/material preparation.
inline bool hero_history_same_conductor(const thread rypath_conductor::Parameters &a,
                                        const thread rypath_conductor::Parameters &b) {
    return a.eta == b.eta && a.k == b.k && a.alpha_x == b.alpha_x && a.alpha_y == b.alpha_y &&
           a.smooth == b.smooth;
}
inline bool hero_history_same_dielectric(const thread rypath_dielectric::Parameters &a,
                                         const thread rypath_dielectric::Parameters &b) {
    return a.eta == b.eta && a.alpha_x == b.alpha_x && a.alpha_y == b.alpha_y &&
           a.smooth == b.smooth;
}
template <typename State>
inline State hero_history_sentinel(uint slot, device const uint *table,
                                   device const float *rgb_table) {
    using namespace rypath_bdpt;
    const uint mask = slot == 1 ? 1u : slot == 3 ? 3u : 15u;
    State state{};
    Vertex original{};
    original.point.position = float3(1.25f, -2.5f, 3.75f) + float(slot);
    original.point.geometric_normal = float3(.1f, .2f, .3f);
    original.point.shading_normal = float3(.4f, .5f, .6f);
    original.frame = {float3(1, 2, 3), float3(4, 5, 6), float3(7, 8, 9)};
    original.type = slot == 1 ? camera_vertex : surface_vertex;
    original.seed = 0xfedcba98u + slot;
    original.path_id = 65 + slot;
    original.delta = slot != 3;
    original.bsdf.active_mask = mask;
    for (uint k = 0; k < 4; ++k) {
        thread auto &p = original.bsdf.lane[k];
        const float f = float(k + slot * 16) + .125f;
        p.kind = slot == 1 ? 0u : slot == 3 ? 2u : 7u;
        p.reflectance = f;
        p.transmittance = f + 1;
        p.wavelength = f + 400;
        p.conductor = {f + 2, f + 3, f + 4, f + 5, bool(k & 1)};
        p.dielectric = {f + 6, f + 7, f + 8, bool(k & 2)};
        p.layered = {p.dielectric, p.conductor, bool(k & 1), f + 9,  f + 10,
                     f + 11,       f + 12,      k + 16,      k + 32, int(k) - 9};
        p.gltf = {f + 13, f + 14, f + 15, f + 16};
        p.table = table + k;
        p.rgb_table = rgb_table + k;
    }
    hero_store_previous(state.latest, original);
    state.preceding_position = float3(slot + 1, slot + 2, slot + 3);
    state.preceding_type = slot == 1 ? surface_vertex : camera_vertex;
    state.sample.status = slot + 20;
    state.sample.pixel = slot + 30;
    state.sample.sample_low = slot + 40;
    state.sample.sample_high = slot + 50;
    state.sample.wavelength = slot + 60.25f;
    state.sample.radiance = slot + 70.25f;
    state.sample.scatterings = slot + 80;
    for (uint k = 0; k < 4; ++k) {
        state.sample.random_words[k] = 0xfedcba98u + slot + k;
        state.lambda.lambda[k] = 400 + slot + k * 10;
        state.lambda.pdf[k] = .125f + float(slot + k);
        state.hero.density.lane[k] = {.25f + .125f * float(k), int(slot + k)};
        state.hero.previous.lane[k] = {.125f + .125f * float(k), -int(slot + k)};
        state.hero.eta_scale[k] = float(slot + k + 2);
    }
    for (uint k = 0; k < 2; ++k) {
        state.sample.lens_words[k] = 0x87654321u + slot + k;
        state.sample.filter_offsets[k] = float(slot + k) + .5f;
    }
    state.radiance = float4(1, 2, 3, 4) + float(slot);
    state.beta = float4(5, 6, 7, 8) + float(slot);
    state.ray_origin = float3(9, 10, 11) + float(slot);
    state.ray_direction = float3(12, 13, 14) + float(slot);
    state.pdf = float(slot) + .25f;
    state.ray_min_distance = float(slot) + .5f;
    state.ray_max_distance = float(slot) + 100.75f;
    state.roulette_eta = {float(slot) + 1.25f, slot != 3};
    state.hero.active_mask = slot == 1 ? 15u : slot == 3 ? 1u : 3u;
    state.hero.roulette_enabled = slot != 5;
    state.index = (ulong(0xfedcba98u) << 32) | ulong(0x87654321u + slot);
    state.count = 65 + slot;
    state.crossings = 100 + slot;
    state.complete = slot != 1;
    return state;
}
inline bool hero_frozen_fields_match(const thread rypath_packet::Prepared &original,
                                     const thread rypath_bdpt::HeroFrozenBSDF &restored) {
    // Derive the expected tag independently instead of freezing another union
    // aggregate inside the comparison. Every active payload field is checked
    // directly against its original scalar Prepared below.
    const uint kind = original.lane[0].kind;
    const bool coated = kind == 5 || kind == 6;
    const bool unsupported =
        !rypath_hero_scattering::valid_structure(original) ||
        (coated && (original.active_mask != 1 || !rypath_packet::compatible(original)));
    const uint expected_kind = unsupported ? 0xffffffffu : kind;
    if (restored.kind != expected_kind || restored.active_mask != original.active_mask)
        return false;
    bool valid = true;
    for (uint k = 0; k < 4; ++k) {
        if (!(original.active_mask & (1u << k)))
            continue;
        const thread auto &a = original.lane[k];
        switch (restored.kind) {
        case 0:
            valid = valid && a.reflectance == rypath_bdpt::hero_frozen_diffuse(restored, k);
            break;
        case 1:
            valid = valid && hero_history_same_conductor(
                                 a.conductor, rypath_bdpt::hero_frozen_conductor(restored, k));
            break;
        case 2:
            valid = valid && hero_history_same_dielectric(
                                 a.dielectric, rypath_bdpt::hero_frozen_dielectric(restored, k));
            break;
        case 3:
            valid = valid && a.dielectric.eta == rypath_bdpt::hero_frozen_thin_eta(restored, k);
            break;
        case 4: {
            const auto b = rypath_bdpt::hero_frozen_transmission(restored, k);
            valid = valid && a.reflectance == b.reflectance && a.transmittance == b.transmittance;
            break;
        }
        case 5:
        case 6: {
            const auto b = rypath_bdpt::hero_frozen_coated(restored);
            valid = valid && hero_history_same_conductor(a.layered.conductor, b.conductor) &&
                    hero_history_same_dielectric(a.layered.interface, b.interface) &&
                    a.layered.conductor_base == b.conductor_base &&
                    a.layered.reflectance == b.reflectance && a.layered.thickness == b.thickness &&
                    a.layered.albedo == b.albedo && a.layered.g == b.g &&
                    a.layered.max_depth == b.max_depth && a.layered.samples == b.samples &&
                    a.layered.material_seed == b.material_seed;
            break;
        }
        case 7:
        case 8: {
            const auto b = rypath_bdpt::hero_frozen_table(restored, k);
            valid = valid && a.table == b.table && a.rgb_table == b.rgb_table &&
                    a.wavelength == b.wavelength;
            break;
        }
        case 11: {
            const auto b = rypath_bdpt::hero_frozen_gltf(restored, k);
            valid = valid && a.gltf.color == b.color && a.gltf.metallic == b.metallic &&
                    a.gltf.alpha == b.alpha &&
                    a.gltf.specular_probability == b.specular_probability;
            break;
        }
        }
    }
    return valid;
}
// Twelve words per owner: failure groups, density bits, material tags/mask,
// and the first mismatching dielectric lane/field with its original values.
inline void hero_history_diagnostics(const thread rypath_bdpt::PacketPTState &expected,
                                     const thread rypath_bdpt::HeroWalkState &actual,
                                     device uint *out) {
    using namespace rypath_bdpt;
    const thread auto &original = expected.latest;
    const thread auto &restored = actual.latest;
    uint failures = 0;
#define RYPATH_HISTORY_CHECK(bit, condition)                                                       \
    if (!(condition))                                                                              \
    failures |= 1u << bit
    RYPATH_HISTORY_CHECK(0, all(restored.point.position == original.point.position));
    RYPATH_HISTORY_CHECK(1,
                         all(restored.point.geometric_normal == original.point.geometric_normal));
    RYPATH_HISTORY_CHECK(2, all(restored.point.shading_normal == original.point.shading_normal));
    RYPATH_HISTORY_CHECK(3, all(restored.frame.x == original.frame.x) &&
                                all(restored.frame.y == original.frame.y) &&
                                all(restored.frame.z == original.frame.z));
    RYPATH_HISTORY_CHECK(4, restored.type == original.type && restored.seed == original.seed &&
                                restored.path_id == original.path_id &&
                                restored.delta == original.delta &&
                                restored.bsdf.active_mask == original.bsdf.active_mask);
    RYPATH_HISTORY_CHECK(5, hero_frozen_fields_match(original.bsdf, restored.bsdf));
    // Both area-density directions require the previous geometric normal;
    // represented scattering also retains the preceding position in hot state.
    Vertex current{};
    current.type = surface_vertex;
    current.point.position = float3(4, 8, 12);
    current.point.geometric_normal = float3(.7f, .8f, .9f);
    const float forward_expected = convert_density(.25f, original, current);
    const float forward_actual = convert_density(.25f, restored, current);
    const float reverse_expected = convert_density(.75f, current, original);
    const float reverse_actual = convert_density(.75f, current, restored);
    RYPATH_HISTORY_CHECK(6, forward_actual == forward_expected);
    RYPATH_HISTORY_CHECK(7, reverse_actual == reverse_expected);
    RYPATH_HISTORY_CHECK(8, all(expected.preceding_position == actual.preceding_position) &&
                                expected.preceding_type == actual.preceding_type);
    RYPATH_HISTORY_CHECK(9, expected.sample.status == actual.sample.status &&
                                expected.sample.pixel == actual.sample.pixel &&
                                expected.sample.sample_low == actual.sample.sample_low &&
                                expected.sample.sample_high == actual.sample.sample_high);
    RYPATH_HISTORY_CHECK(10, expected.sample.wavelength == actual.sample.wavelength &&
                                 expected.sample.radiance == actual.sample.radiance &&
                                 expected.sample.scatterings == actual.sample.scatterings);
    for (uint k = 0; k < 4; ++k) {
        RYPATH_HISTORY_CHECK(11, expected.sample.random_words[k] == actual.sample.random_words[k]);
        RYPATH_HISTORY_CHECK(12, expected.lambda.lambda[k] == actual.lambda.lambda[k] &&
                                     expected.lambda.pdf[k] == actual.lambda.pdf[k]);
        RYPATH_HISTORY_CHECK(
            13, expected.hero.density.lane[k].fraction == actual.hero.density.lane[k].fraction &&
                    expected.hero.density.lane[k].exponent == actual.hero.density.lane[k].exponent);
        RYPATH_HISTORY_CHECK(14, expected.hero.previous.lane[k].fraction ==
                                         actual.hero.previous.lane[k].fraction &&
                                     expected.hero.previous.lane[k].exponent ==
                                         actual.hero.previous.lane[k].exponent);
    }
    for (uint k = 0; k < 2; ++k)
        RYPATH_HISTORY_CHECK(15, expected.sample.lens_words[k] == actual.sample.lens_words[k] &&
                                     expected.sample.filter_offsets[k] ==
                                         actual.sample.filter_offsets[k]);
    RYPATH_HISTORY_CHECK(16, all(expected.radiance == actual.radiance));
    RYPATH_HISTORY_CHECK(17, all(expected.beta == actual.beta));
    RYPATH_HISTORY_CHECK(18, all(expected.ray_origin == actual.ray_origin));
    RYPATH_HISTORY_CHECK(19, all(expected.ray_direction == actual.ray_direction));
    RYPATH_HISTORY_CHECK(20, expected.pdf == actual.pdf);
    RYPATH_HISTORY_CHECK(21, expected.ray_min_distance == actual.ray_min_distance &&
                                 expected.ray_max_distance == actual.ray_max_distance);
    RYPATH_HISTORY_CHECK(22, expected.roulette_eta.value == actual.roulette_eta.value &&
                                 expected.roulette_eta.enabled == actual.roulette_eta.enabled);
    RYPATH_HISTORY_CHECK(23, all(expected.hero.eta_scale == actual.hero.eta_scale) &&
                                 expected.hero.active_mask == actual.hero.active_mask &&
                                 expected.hero.roulette_enabled == actual.hero.roulette_enabled);
    RYPATH_HISTORY_CHECK(24, expected.index == actual.index);
    RYPATH_HISTORY_CHECK(25,
                         expected.count == actual.count && expected.crossings == actual.crossings);
    RYPATH_HISTORY_CHECK(26, expected.complete == actual.complete);
#undef RYPATH_HISTORY_CHECK
    out[0] = failures;
    out[1] = as_type<uint>(forward_expected);
    out[2] = as_type<uint>(forward_actual);
    out[3] = as_type<uint>(reverse_expected);
    out[4] = as_type<uint>(reverse_actual);
    out[5] = hero_freeze_bsdf(original.bsdf).kind;
    out[6] = restored.bsdf.kind;
    out[7] = restored.bsdf.active_mask;
    uint material_field = 0, material_expected = 0, material_actual = 0;
    if (original.bsdf.lane[0].kind == 2 && restored.bsdf.kind == 2) {
        for (uint k = 0; k < 4; ++k) {
            if (!(original.bsdf.active_mask & (1u << k)))
                continue;
            const thread auto &a = original.bsdf.lane[k].dielectric;
            const auto b = hero_frozen_dielectric(restored.bsdf, k);
            const uint expected_words[4] = {as_type<uint>(a.eta), as_type<uint>(a.alpha_x),
                                            as_type<uint>(a.alpha_y), uint(a.smooth)};
            const uint actual_words[4] = {as_type<uint>(b.eta), as_type<uint>(b.alpha_x),
                                          as_type<uint>(b.alpha_y), uint(b.smooth)};
            for (uint field = 0; field < 4; ++field)
                if (expected_words[field] != actual_words[field] && material_field == 0) {
                    material_field = 1 + k * 4 + field;
                    material_expected = expected_words[field];
                    material_actual = actual_words[field];
                }
        }
    }
    out[8] = material_field;
    out[9] = material_expected;
    out[10] = material_actual;
    out[11] = 0x48535435u;
}
// Separate dispatches force publication through real typed device pools. Odd
// slots leave a full guarded owner before, between and after the three writes.
kernel void rypath_hero_history_write(constant WavefrontArguments &a [[buffer(1)]],
                                      uint lane [[thread_position_in_grid]]) {
    if (lane >= 3)
        return;
    const uint slot = 2 * lane + 1;
    const auto state = hero_history_sentinel<rypath_bdpt::HeroWalkState>(
        slot, a.queue_in, reinterpret_cast<device const float *>(a.queue_out));
    wavefront_store_packet(a, slot, state);
}
kernel void rypath_hero_history_read(constant WavefrontArguments &a [[buffer(1)]],
                                     device uint *out [[buffer(0)]],
                                     uint lane [[thread_position_in_grid]]) {
    if (lane >= 3)
        return;
    const uint slot = 2 * lane + 1;
    const auto expected = hero_history_sentinel<rypath_bdpt::PacketPTState>(
        slot, a.queue_in, reinterpret_cast<device const float *>(a.queue_out));
    const auto actual = wavefront_load_hero(a, slot);
    hero_history_diagnostics(expected, actual, out + 12 * lane);
}

template <bool Separated>
inline void hero_history_write_fixed(constant WavefrontArguments &a, uint lane) {
    if (lane >= 3)
        return;
    const uint slot = 2 * lane + 1;
    const auto state = hero_history_sentinel<rypath_bdpt::HeroWalkState>(
        slot, a.queue_in, reinterpret_cast<device const float *>(a.queue_out));
    if (Separated)
        wavefront_store_packet_separated(a, slot, state);
    else
        wavefront_store_hero_packed(a, slot, state);
}
template <bool Separated>
inline void hero_history_read_fixed(constant WavefrontArguments &a, device uint *out, uint lane) {
    if (lane >= 3)
        return;
    const uint slot = 2 * lane + 1;
    const auto expected = hero_history_sentinel<rypath_bdpt::PacketPTState>(
        slot, a.queue_in, reinterpret_cast<device const float *>(a.queue_out));
    const auto actual =
        Separated ? wavefront_load_hero_separated(a, slot) : wavefront_load_hero_packed(a, slot);
    hero_history_diagnostics(expected, actual, out + 12 * lane);
}
kernel void rypath_hero_history_write_packed(constant WavefrontArguments &a [[buffer(1)]],
                                             uint lane [[thread_position_in_grid]]) {
    hero_history_write_fixed<false>(a, lane);
}
kernel void rypath_hero_history_read_packed(constant WavefrontArguments &a [[buffer(1)]],
                                            device uint *out [[buffer(0)]],
                                            uint lane [[thread_position_in_grid]]) {
    hero_history_read_fixed<false>(a, out, lane);
}
kernel void rypath_hero_history_write_separated(constant WavefrontArguments &a [[buffer(1)]],
                                                uint lane [[thread_position_in_grid]]) {
    hero_history_write_fixed<true>(a, lane);
}
kernel void rypath_hero_history_read_separated(constant WavefrontArguments &a [[buffer(1)]],
                                               device uint *out [[buffer(0)]],
                                               uint lane [[thread_position_in_grid]]) {
    hero_history_read_fixed<true>(a, out, lane);
}

// Differential reference uses the original full Prepared evaluator; compact
// queries call native families directly and preserve validity as well as PDFs.
kernel void rypath_hero_frozen_differential(device uint *out [[buffer(0)]],
                                            device const uint *measured [[buffer(1)]],
                                            device const uint *fourier [[buffer(2)]],
                                            device const float *rgb [[buffer(3)]],
                                            uint index [[thread_position_in_grid]]) {
    using namespace rypath_bdpt;
    if (index >= 480)
        return;
    const uint kinds[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 11};
    const uint kind = kinds[index / 48];
    const uint mask_index = (index / 16) % 3;
    const uint mask = mask_index == 0 ? 1u : mask_index == 1 ? 3u : 15u;
    const uint variant = index % 8;
    const Mode mode = (index & 8u) ? Mode::Importance : Mode::Radiance;
    Vertex v{};
    v.type = surface_vertex;
    v.point.position = float3(1, 2, 3);
    v.point.geometric_normal = float3(0, 0, 1);
    v.point.shading_normal = float3(0, 0, 1);
    v.frame = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
    v.seed = 0xfedcba98u + index;
    v.bsdf.active_mask = mask;
    for (uint k = 0; k < 4; ++k) {
        thread auto &p = v.bsdf.lane[k];
        p.kind = kind;
        p.reflectance = .125f * float(k + 1);
        p.transmittance = .0625f * float(4 - k);
        p.wavelength = 410 + 80 * k;
        p.conductor = {1.25f + float(k), 2.5f + float(k), .2f + .03f * float(k), .3f, variant == 1};
        p.dielectric = {variant == 2 ? 1.f : 1.25f + .125f * float(k), .2f, .3f, variant == 1};
        p.layered = {{1.5f, .2f, .3f, false},
                     {1.5f, 2.5f, .2f, .3f, false},
                     kind == 6,
                     .5f,
                     .1f,
                     variant == 3 ? .25f : 0.f,
                     .125f,
                     3,
                     2,
                     79};
        // The legacy coated compatibility check rejects NaN roughness even
        // at tangent directions where the layered evaluator returns early.
        if ((kind == 5 || kind == 6) && variant == 6)
            p.layered.interface.alpha_x = NAN;
        p.gltf = {.125f * float(k + 1), .25f, variant == 1 ? 0.f : .2f, .5f};
        p.table = kind == 7 ? measured : fourier;
        p.rgb_table = rgb;
    }
    const auto frozen = hero_freeze_previous(v);
    float3 wo = normalize(float3(.3f, .2f, 1));
    float3 wi = normalize(float3(-.2f, .1f, 1));
    if (variant == 3)
        wi.z = -wi.z;
    if (variant == 4)
        wo = normalize(float3(1, .1f, 0x1p-20f));
    if (variant == 5) {
        wo.z = -wo.z;
        wi.z = -wi.z;
    }
    if (variant == 6)
        wo = float3(1, 0, 0);
    if (variant == 7)
        wi = -wo;
    const auto reference = rypath_hero_scattering::evaluate(v.bsdf, wo, wi, v.seed, mode);
    const auto compact = hero_frozen_evaluate(frozen.bsdf, wo, wi, v.seed, mode);
    bool valid = hero_frozen_fields_match(v.bsdf, frozen.bsdf) && reference.valid == compact.valid;
    if (kind == 7 || kind == 8) {
        // These address-only sentinels are never dereferenced: verify distinct
        // per-lane pointers independently of the valid shared-table queries.
        auto pointers = v.bsdf;
        for (uint k = 0; k < 4; ++k) {
            pointers.lane[k].table += k;
            pointers.lane[k].rgb_table += k;
        }
        const auto frozen_pointers = hero_freeze_bsdf(pointers);
        valid = valid && hero_frozen_fields_match(pointers, frozen_pointers);
    }
    if (reference.valid)
        valid = valid && all(as_type<uint4>(reference.f) == as_type<uint4>(compact.f)) &&
                all(as_type<uint4>(reference.pdf) == as_type<uint4>(compact.pdf));
    Vertex before{}, next{};
    before.type = surface_vertex;
    before.point.position = v.point.position + wo;
    next.type = variant == 7 ? environment_vertex : surface_vertex;
    next.point.position = v.point.position + wi;
    next.point.geometric_normal = -wi;
    const auto original_pdf = hero_represented_pdf(v, before, next);
    const auto frozen_pdf = hero_represented_pdf(frozen, before, next);
    valid = valid && ((all(isnan(original_pdf)) && all(isnan(frozen_pdf))) ||
                      all(as_type<uint4>(original_pdf) == as_type<uint4>(frozen_pdf)));
    out[index] = uint(valid);
}
