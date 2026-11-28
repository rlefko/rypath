// SPDX-License-Identifier: GPL-3.0-only
// Scene-proven diffuse current payload; persistent history remains generic and unchanged.
#pragma once
struct DiffuseBSDF {
    float reflectance[4];
    uint active_mask;
};
struct DiffuseCurrent {
    SurfacePoint point;
    ScatteringFrame frame;
    DiffuseBSDF bsdf;
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
template <> struct HeroDirectEndpoint<DiffuseCurrent> {
    using Type = HeroLightEndpoint;
};
inline bool hero_current_structure(const thread DiffuseBSDF &p) {
    return (p.active_mask & 1u) && !(p.active_mask & ~15u);
}
inline bool hero_current_requires_single(const thread DiffuseBSDF &) { return false; }
inline bool hero_current_connectible(const thread DiffuseBSDF &p) {
    return (p.active_mask & 15u) != 0;
}
inline bool prepare_hero_current(const thread Resources &r, const device wire::Material &material,
                                 graph_wire::Context context, const thread Wavelengths &lambda,
                                 thread DiffuseBSDF &out, uint active_mask = 15) {
    out = {};
    if (material.scattering != 0)
        return false;
    out.active_mask = active_mask;
    for (uint k = 0; k < 4; ++k) {
        if (!(active_mask & (1u << k)))
            continue;
        if (!packet_reflectance(r, material, context, lambda.lambda[k], out.reflectance[k]))
            return false;
    }
    return hero_current_structure(out);
}
inline bool prepare_event_bsdf(const thread Resources &r, const device wire::Material &m,
                               graph_wire::Context c, thread Wavelengths &l, thread float4 &b,
                               thread DiffuseBSDF &out, uint mask) {
    (void)b;
    return prepare_hero_current(r, m, c, l, out, mask);
}
inline rypath_hero_scattering::Evaluation hero_evaluate_current(const thread DiffuseBSDF &p,
                                                                float3 wo, float3 wi, uint seed) {
    using namespace rypath_hero_scattering;
    Evaluation out{float4(0), float4(0), false};
    if (!hero_current_structure(p) || !all(isfinite(wo)) || !all(isfinite(wi)))
        return out;
    out.valid = true;
    for (uint k = 0; k < 4; ++k) {
        if (!(p.active_mask & (1u << k)))
            continue;
        const auto e = rypath_transmission::diffuse_evaluate(p.reflectance[k], 0, wo, wi);
        out.f[k] = e.f * 1.f;
        out.pdf[k] = e.pdf;
        out.valid = out.valid && e.valid;
    }
    out.valid = out.valid && nonnegative(out.f) && nonnegative(out.pdf);
    (void)seed;
    return out;
}
inline rypath_hero_scattering::Sample hero_sample_current(const thread DiffuseBSDF &p, float3 wo,
                                                          float uc, float2 u, uint seed,
                                                          Mode mode) {
    namespace h = rypath_hero_scattering;
    h::Sample out{};
    out.eta = 1;
    if (!hero_current_structure(p) || !all(isfinite(wo)) || !rypath_transmission::randoms(uc, u))
        return out;
    const auto hero = rypath_packet::diffuse_reflection_sample(p.reflectance[0], wo, uc, u);
    out.valid = hero.valid;
    out.wi = hero.wi;
    out.delta = hero.delta;
    out.transmission = rypath_packet_roulette::opposite_hemispheres(wo.z, hero.wi.z);
    if (!hero.valid)
        return out;
    if (!isfinite(hero.pdf) || hero.pdf < 0 || !all(isfinite(hero.wi))) {
        out.valid = false;
        return out;
    }
    if (hero.pdf == 0 || wo.z == 0 || hero.wi.z == 0)
        return out;
    const auto e = hero_evaluate_current(p, wo, hero.wi, seed);
    out.numerator = e.f * abs(hero.wi.z);
    out.pdf = e.pdf;
    out.valid = e.valid;
    out.pdf[0] = hero.pdf;
    out.valid = out.valid && h::nonnegative(out.numerator) && h::nonnegative(out.pdf) &&
                all(isfinite(out.eta)) && all(out.eta > 0);
    out.accepted = out.valid && any(out.numerator > 0);
    (void)mode;
    return out;
}
