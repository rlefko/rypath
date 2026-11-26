// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "hero_current_bsdf.h"
// Included inside rypath_bdpt after the qualified current-BSDF helpers.
// Keep the original event's two copies and all small scalar/pair fields. Only
// the unused four-lane scalar Prepared storage changes in this execution step.
struct HeroCurrentVertex {
    SurfacePoint point;
    ScatteringFrame frame;
    HeroFrozenBSDF bsdf;
    float4 beta;
    float3 wo;
    float2 uv;
    float pdf_fwd, pdf_rev;
    uint type, instance, primitive, kind, light, seed;
    uint path_id, sampled_prev_id, sampled_next_id;
    bool pair_valid;
    float3 sampled_wo_local, sampled_wi_local;
    bool delta, connectible, delta_light;
};
static_assert(sizeof(HeroCurrentVertex) == 496 && alignof(HeroCurrentVertex) == 16 &&
                  sizeof(HeroCurrentVertex) + 512 == sizeof(Vertex),
              "Hero current event removes only the full Prepared payload");
inline bool prepare_event_bsdf(const thread Resources &r, const device wire::Material &material,
                               graph_wire::Context context, thread Wavelengths &lambda,
                               thread float4 &beta, thread rypath_packet::Prepared &out,
                               uint active_mask) {
    return prepare_packet(r, material, context, lambda, beta, out, active_mask);
}
inline bool prepare_event_bsdf(const thread Resources &r, const device wire::Material &material,
                               graph_wire::Context context, thread Wavelengths &lambda,
                               thread float4 &beta, thread HeroFrozenBSDF &out, uint active_mask) {
    (void)beta; // Hero preparation never mutates physical throughput or wavelengths.
    return prepare_hero_current(r, material, context, lambda, out, active_mask);
}
inline rypath_hero_scattering::Sample hero_sample_current(const thread rypath_packet::Prepared &p,
                                                          float3 wo, float uc, float2 u, uint seed,
                                                          Mode mode) {
    return rypath_hero_scattering::sample(p, wo, uc, u, seed, mode);
}
inline rypath_hero_scattering::Sample hero_sample_current(const thread HeroFrozenBSDF &p, float3 wo,
                                                          float uc, float2 u, uint seed,
                                                          Mode mode) {
    return hero_current_sample(p, wo, uc, u, seed, mode);
}
inline rypath_hero_scattering::Evaluation
hero_evaluate_current(const thread rypath_packet::Prepared &p, float3 wo, float3 wi, uint seed) {
    return rypath_hero_scattering::evaluate(p, wo, wi, seed);
}
inline rypath_hero_scattering::Evaluation hero_evaluate_current(const thread HeroFrozenBSDF &p,
                                                                float3 wo, float3 wi, uint seed) {
    return hero_current_evaluate(p, wo, wi, seed);
}
