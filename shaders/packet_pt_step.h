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
struct PacketPTState {
    // PT retains only its latest frozen BSDF/frame. The older endpoint enters
    // represented_scattering_pdf only through type and position: a continuing
    // walk contains camera/surface history, never a terminal environment vertex.
    Vertex latest;
    float3 preceding_position;
    uint preceding_type;
    wire::Sample sample;
    Wavelengths lambda;
    float4 radiance, beta;
    float3 ray_origin, ray_direction;
    float pdf, ray_min_distance, ray_max_distance;
    rypath_packet_roulette::EtaScale roulette_eta;
    ulong index;
    uint count, crossings;
    bool complete;
};
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
inline void packet_pt_initialize(thread PacketPTState &state, const thread wire::Sample &sample,
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
    thread auto &endpoint = state.latest;
    endpoint.type = camera_vertex;
    endpoint.path_id = 0;
    endpoint.point.position = incoming.origin;
    endpoint.point.geometric_normal = unpack(p.camera_normal);
    endpoint.point.shading_normal = unpack(p.camera_normal);
    endpoint.beta = 1;
    endpoint.connectible = true;
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

// The ordinary function expands to f0d8494's original body and statement order.
// Only the split variant replaces event tracing; all event arithmetic is shared.
[[clang::noinline]] inline void
packet_pt_step(thread Resources &r, thread PacketPTState &state,
               thread const rypath_replay::ReplayState *replay = nullptr) {
#define RYPATH_PACKET_EVENT_PRETRACED 0
#include "packet_pt_event_body.inc"
#undef RYPATH_PACKET_EVENT_PRETRACED
}
[[clang::noinline]] inline void
packet_pt_step_pretraced(thread Resources &r, thread PacketPTState &state,
                         thread const rypath_replay::ReplayState *replay,
                         thread const SurfaceHit &pretraced) {
#define RYPATH_PACKET_EVENT_PRETRACED 1
#include "packet_pt_event_body.inc"
#undef RYPATH_PACKET_EVENT_PRETRACED
}
inline storage::Result packet_pt_result(const thread PacketPTState &state,
                                        constant wire::Parameters &p) {
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
