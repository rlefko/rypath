// SPDX-License-Identifier: Apache-2.0
// Resumable vacuum PT scheduling adapted from packet_walk.h's PBRT random walk.
// Copyright (c) 1998–2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt.
#pragma once
// Included inside rypath_bdpt after the shared PT connection helpers.

// GPU-only opaque state: host code allocates sizeof(PacketPTState) bytes per
// active lane. Kernels copy device state into thread storage before calling these
// helpers and write it back afterward. Resources/SurfaceAlpha are reconstructed
// per dispatch; they are never stored here. Vertex Prepared retains immutable
// scene table pointers, which must remain valid through command completion.
#include "hero_current_bsdf.h"
#include "hero_current_vertex.h"
#include "hero_frozen_bsdf.h"
// Older geometry enters represented PDFs only through type/position/normal.
struct HeroNeighbor {
    struct Point {
        float3 position, geometric_normal;
    } point;
    uint type;
};
inline bool infinite(const thread HeroNeighbor &v) { return v.type == environment_vertex; }
template <bool Compact> struct PTCurrent {
    using Type = Vertex;
};
template <> struct PTCurrent<true> {
    using Type = HeroCurrentVertex;
};
template <bool Compact> struct PTNeighbor {
    using Type = Vertex;
};
template <> struct PTNeighbor<true> {
    using Type = HeroNeighbor;
};
#include "hero_pt_helpers.h"

// Owned-light evaluation consumes the original hero technique helpers above.
#include "enhanced_light.h"

// Hybrid direction queries share those BSDF and scaled geometry conventions.
#include "enhanced_hybrid_geometry.h"
template <typename Previous, bool Compact = false,
          typename CurrentType = typename PTCurrent<Compact>::Type>
struct PTWalkState {
    enum { compact_hero = Compact };
    using Neighbor = typename PTNeighbor<Compact>::Type;
    using Current = CurrentType;
    // PT retains only its latest frozen BSDF/frame. The older endpoint enters
    // represented_scattering_pdf only through type and position: a continuing
    // walk contains camera/surface history, never a terminal environment vertex.
    Previous latest;
    float3 preceding_position;
    uint preceding_type;
    wire::Sample sample;
    Wavelengths lambda;
    float4 radiance, beta;
    float3 ray_origin, ray_direction;
    float pdf, ray_min_distance, ray_max_distance;
    rypath_packet_roulette::EtaScale roulette_eta;
    HeroTrack hero;
    ulong index;
    uint count, crossings;
    bool complete;
};
using PacketPTState = PTWalkState<Vertex>;
// Hot tracing/publication state. The full frozen Vertex lives in a separate cold pool.
struct PacketPTHot {
    float3 preceding_position;
    uint preceding_type;
    wire::Sample sample;
    Wavelengths lambda;
    float4 radiance, beta;
    float3 ray_origin, ray_direction;
    float pdf, ray_min_distance, ray_max_distance;
    rypath_packet_roulette::EtaScale roulette_eta;
    HeroTrack hero;
    ulong index;
    uint count, crossings;
    bool complete;
};
struct PacketPTHit {
    SurfaceHit hit;
    uint ready;
};

static_assert(alignof(PacketPTState) <= 16 && alignof(PacketPTHot) <= 16 &&
                  alignof(PacketPTHit) <= 16 &&
                  sizeof(PacketPTHot) + sizeof(Vertex) == sizeof(PacketPTState),
              "Packet PT split-state ABI/alignment");
#include "hero_pt_state.h"
template <typename State>
inline void packet_pt_initialize(thread State &state, const thread wire::Sample &sample,
                                 const thread Wavelengths &lambda, ray incoming, float pdf,
                                 ulong index, constant wire::Parameters &p) {
    state = {};
    state.sample = sample;
    state.lambda = lambda;
    state.index = index;
    state.ray_origin = incoming.origin;
    state.ray_direction = incoming.direction;
    state.pdf = pdf;
    state.ray_min_distance = incoming.min_distance;
    state.ray_max_distance = incoming.max_distance;
    state.beta = 1;
    state.roulette_eta = {};
    state.count = 1;
    typename State::Current endpoint{};
    endpoint.type = camera_vertex;
    endpoint.path_id = 0;
    endpoint.point.position = incoming.origin;
    endpoint.point.geometric_normal = unpack(p.camera_normal);
    endpoint.point.shading_normal = unpack(p.camera_normal);
    endpoint.beta = 1;
    endpoint.connectible = true;
    hero_store_previous(state.latest, endpoint);
    if (sample.status != wire::success || packet_bidirectional_enabled(p) ||
        p.camera_medium != 0xffffffffu || !all(isfinite(incoming.origin)) ||
        !all(isfinite(incoming.direction)) || !isfinite(pdf) || pdf < 0) {
        state.sample.status = wire::invalid_arithmetic;
        state.complete = true;
    }
}

inline SurfaceHit packet_pt_trace(thread Resources &r, thread wire::Sample &sample,
                                  const thread Wavelengths &lambda, ray incoming) {
    const constant auto &p = *r.alpha.parameters;
    MixedIntersector closest;
    closest.set_triangle_cull_mode(triangle_cull_mode::none);
    r.alpha.sample = &sample;
    r.alpha.incoming = incoming;
    sample.wavelength = lambda.lambda[0];
    return p.instance_count == 0 ? SurfaceHit{intersection_type::none, 0, 0, 0, 0, float2(0), true}
           : (native_triangle_hint_mode & 2u)
               ? trace_packet_seeded_closest(incoming, r.scene, all_visible, r.intersection_table,
                                             r.alpha, alpha_enabled)
           : alpha_enabled
               ? trace_masked_surface(incoming, r.scene, all_visible, r.alpha)
               : trace_surface(closest, incoming, r.scene, all_visible, r.intersection_table);
}

// Fixed-chart certification uses precisely the ordinary closest-hit route.
#include "enhanced_hybrid_runtime.h"
#include "enhanced_suffix_capture.h"
#include "enhanced_suffix_lookup.h"

// The ordinary function expands to f0d8494's original body and statement order.
// Only the split variant replaces event tracing; all event arithmetic is shared.
template <typename State>
[[clang::noinline]] inline void
packet_pt_step(thread Resources &r, thread State &state,
               thread const rypath_replay::ReplayState *replay = nullptr) {
#define RYPATH_PACKET_EVENT_PRETRACED 0
#define RYPATH_PACKET_EVENT_REUSE_CURRENT 0
#include "packet_pt_event_body.inc"
#undef RYPATH_PACKET_EVENT_REUSE_CURRENT
#undef RYPATH_PACKET_EVENT_PRETRACED
}
template <typename State>
[[clang::noinline]] inline void
packet_pt_step_pretraced(thread Resources &r, thread State &state,
                         thread const rypath_replay::ReplayState *replay,
                         thread const SurfaceHit &pretraced) {
#define RYPATH_PACKET_EVENT_PRETRACED 1
#define RYPATH_PACKET_EVENT_REUSE_CURRENT 0
#include "packet_pt_event_body.inc"
#undef RYPATH_PACKET_EVENT_REUSE_CURRENT
#undef RYPATH_PACKET_EVENT_PRETRACED
}
// Only compact hero events reuse the local current vertex. The original
// templates remain the unchanged legacy path and explicit out-of-place
// HeroWalkState oracles. Only the compact outer event is inlined; its sampling
// helpers keep their existing call boundaries.
// current starts zeroed and is untouched before either mutually exclusive v site.
template <typename State>
[[clang::always_inline]] inline void
packet_pt_step_compact_inplace(thread Resources &r, thread State &state,
                               thread const rypath_replay::ReplayState *replay) {
    static_assert(State::compact_hero, "In-place event requires compact hero storage");
#define RYPATH_PACKET_EVENT_PRETRACED 0
#define RYPATH_PACKET_EVENT_REUSE_CURRENT 1
#include "packet_pt_event_body.inc"
#undef RYPATH_PACKET_EVENT_REUSE_CURRENT
#undef RYPATH_PACKET_EVENT_PRETRACED
}
template <typename State>
[[clang::always_inline]] inline void
packet_pt_step_pretraced_compact_inplace(thread Resources &r, thread State &state,
                                         thread const rypath_replay::ReplayState *replay,
                                         thread const SurfaceHit &pretraced) {
    static_assert(State::compact_hero, "In-place event requires compact hero storage");
#define RYPATH_PACKET_EVENT_PRETRACED 1
#define RYPATH_PACKET_EVENT_REUSE_CURRENT 1
#include "packet_pt_event_body.inc"
#undef RYPATH_PACKET_EVENT_REUSE_CURRENT
#undef RYPATH_PACKET_EVENT_PRETRACED
}
// Captures individual hero technique contributions without changing their transport
// evaluation or accumulation order. The sink owns independent contribution selection.
template <typename State, typename Sink>
[[clang::always_inline]] inline void
packet_pt_step_contribution(thread Resources &r, thread State &state,
                            thread const rypath_replay::ReplayState *replay, thread Sink &sink) {
    static_assert(State::compact_hero, "Contribution events require hero transport");
#define RYPATH_PACKET_EVENT_PRETRACED 0
#define RYPATH_PACKET_EVENT_REUSE_CURRENT 1
#define RYPATH_PACKET_EVENT_CONTRIBUTION 1
#include "packet_pt_event_body.inc"
#undef RYPATH_PACKET_EVENT_CONTRIBUTION
#undef RYPATH_PACKET_EVENT_REUSE_CURRENT
#undef RYPATH_PACKET_EVENT_PRETRACED
}
// Pretraced differential entry for fixed-atom validation; transport equations
// are shared with the production contribution walker.
template <typename State, typename Sink>
[[clang::always_inline]] inline void
packet_pt_step_contribution_pretraced(thread Resources &r, thread State &state,
                                      thread const rypath_replay::ReplayState *replay,
                                      thread Sink &sink, const thread SurfaceHit &pretraced) {
    static_assert(State::compact_hero, "Contribution events require hero transport");
#define RYPATH_PACKET_EVENT_PRETRACED 1
#define RYPATH_PACKET_EVENT_REUSE_CURRENT 1
#define RYPATH_PACKET_EVENT_CONTRIBUTION 1
#include "packet_pt_event_body.inc"
#undef RYPATH_PACKET_EVENT_CONTRIBUTION
#undef RYPATH_PACKET_EVENT_REUSE_CURRENT
#undef RYPATH_PACKET_EVENT_PRETRACED
}
inline void packet_pt_step(thread Resources &r, thread HeroWalkState &state,
                           thread const rypath_replay::ReplayState *replay = nullptr) {
    packet_pt_step_compact_inplace(r, state, replay);
}
inline void packet_pt_step(thread Resources &r, thread DiffuseWalk &state,
                           thread const rypath_replay::ReplayState *replay = nullptr) {
    packet_pt_step_compact_inplace(r, state, replay);
}
// Canonical Enhanced atoms omit outer transport roulette. Only the fresh sink
// discovers atoms with its independent, accounted roulette law; coating walks
// retain their original internal transport policy.
template <typename State, typename Sink>
[[clang::always_inline]] inline void
packet_pt_step_enhanced(thread Resources &r, thread State &state,
                        thread const rypath_replay::ReplayState *replay, thread Sink &sink) {
    static_assert(State::compact_hero, "Enhanced events require hero transport");
#define RYPATH_PACKET_EVENT_PRETRACED 0
#define RYPATH_PACKET_EVENT_REUSE_CURRENT 1
#define RYPATH_PACKET_EVENT_CONTRIBUTION 1
#define RYPATH_PACKET_EVENT_ENHANCED 1
#include "packet_pt_event_body.inc"
#undef RYPATH_PACKET_EVENT_ENHANCED
#undef RYPATH_PACKET_EVENT_CONTRIBUTION
#undef RYPATH_PACKET_EVENT_REUSE_CURRENT
#undef RYPATH_PACKET_EVENT_PRETRACED
}
inline void packet_pt_step_pretraced(thread Resources &r, thread HeroWalkState &state,
                                     thread const rypath_replay::ReplayState *replay,
                                     thread const SurfaceHit &pretraced) {
    packet_pt_step_pretraced_compact_inplace(r, state, replay, pretraced);
}
template <typename State>
inline storage::Result packet_pt_result(const thread State &state, constant wire::Parameters &p) {
    storage::Result result{};
    result.status = state.complete ? state.sample.status : wire::invalid_arithmetic;
    result.pixel = state.sample.pixel;
    result.sample_low = state.sample.sample_low;
    result.sample_high = state.sample.sample_high;
    result.filter_weight = p.packet_filter_weight;
    for (uint lane = 0; lane < 4; ++lane) {
        result.wavelengths[lane] = state.lambda.lambda[lane];
        result.wavelength_pdfs[lane] = state.lambda.pdf[lane];
        result.radiance[lane] = state.radiance[lane];
    }
    return result;
}
