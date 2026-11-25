// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "packet_scattering.h"

// Hero-conditioned BSDF proposals for Wilkie et al. (2014), Sections 3.2-4.1.
// This helper exposes each wavelength's density on the SAME sampled direction.
// The caller must combine complete path densities, including light techniques;
// dividing every channel by its own BSDF PDF is not the hero estimator.
// Continuous solid-angle densities and Dirac branch masses never mix. A zero
// companion density at a hero Dirac means incompatible support, not a clamped
// contribution. No wavelength termination/probability compensation occurs here.
#if defined(RYPATH_PACKET_RENDER)
namespace rypath_hero_scattering_renderer {
namespace self = ::rypath_hero_scattering_renderer;
#else
namespace rypath_hero_scattering {
namespace self = ::rypath_hero_scattering;
#endif
namespace packet = rypath_packet;
using Prepared = packet::Prepared;
using TransportMode = packet::TransportMode;
struct Evaluation {
    float4 f, pdf;
    bool valid;
};
struct Sample {
    float3 wi;
    float4 numerator, pdf, eta;
    bool delta, transmission, accepted, valid;
    float4 fallback_weight;
    float fallback_pdf;
    bool fallback;
};
inline bool requires_single_hero(const thread Prepared &p) {
    for (uint i = 0; i < 4; ++i)
        if ((p.active_mask & (1u << i)) && (p.lane[i].kind == 5 || p.lane[i].kind == 6))
            return true;
    return false;
}
inline bool valid_structure(const thread Prepared &p) {
    if (!(p.active_mask & 1u) || (p.active_mask & ~15u))
        return false;
    const uint kind = p.lane[0].kind;
    if (kind != 0 && kind != 1 && kind != 2 && kind != 3 && kind != 4 && kind != 5 && kind != 6 &&
        kind != 7 && kind != 8 && kind != 11)
        return false;
    for (uint i = 1; i < 4; ++i)
        if ((p.active_mask & (1u << i)) && p.lane[i].kind != kind)
            return false;
    // Roughness and eta may differ: unlike packet::compatible(), these are
    // per-wavelength proposals, not one wavelength-independent proposal.
    return true;
}
inline bool nonnegative(float4 v) { return all(isfinite(v)) && all(v >= 0); }
inline float event_eta(const thread rypath_scattering::Prepared &p, float3 wo, float3 wi) {
    if (!rypath_packet_roulette::opposite_hemispheres(wo.z, wi.z))
        return 1;
    if (p.kind == 2)
        return wo.z > 0 ? p.dielectric.eta : 1 / p.dielectric.eta;
    if (packet::has_fourier() && (p.kind == 7 || p.kind == 8) &&
        p.table[0] == RYPATH_TABLE_FOURIER) {
        const float eta = tb_f(p.table, 4);
        return wo.z > 0 ? eta : 1 / eta;
    }
    return 1;
}
inline Evaluation evaluate(const thread Prepared &p, float3 wo, float3 wi, uint seed,
                           TransportMode mode = TransportMode::Radiance) {
    Evaluation out{float4(0), float4(0), false};
    if (!valid_structure(p) || !all(isfinite(wo)) || !all(isfinite(wi)))
        return out;
    if (requires_single_hero(p)) {
        // A layered walk's joint hidden-path density is unavailable. The caller
        // must collapse BEFORE both direct evaluation and sampling. Its legacy
        // directional PDF is allowed only as a complementary MIS proxy.
        if (p.active_mask != 1)
            return out;
        const auto legacy = packet::evaluate(p, wo, wi, seed, mode);
        out.f = legacy.f;
        out.pdf[0] = legacy.pdf;
        out.valid = legacy.valid && nonnegative(out.f) && nonnegative(out.pdf);
        return out;
    }
    out.valid = true;
    for (uint i = 0; i < 4; ++i)
        if (p.active_mask & (1u << i)) {
            auto lane = p.lane[i];
            if (lane.kind == 2 && lane.dielectric.eta == 1)
                lane.dielectric.smooth = true;
            const auto e = packet::scalar_evaluate(lane, wo, wi);
            out.f[i] = e.f * packet::mode_scale(p.lane[i], wo, wi, mode);
            out.pdf[i] = e.pdf;
            out.valid = out.valid && e.valid;
        }
    out.valid = out.valid && nonnegative(out.f) && nonnegative(out.pdf);
    return out;
}
// Evaluate the physical mass and the scalar sampler's branch probability for
// a Dirac event. A different represented direction has zero common support;
// reflection remains aligned even across dispersive indices. Refraction uses
// the same checked Snell construction as scalar sampling, including eta=1/TIR.
inline bool dirac_lane(const thread rypath_scattering::Prepared &p, float3 wo, float3 wi, float2 u,
                       TransportMode mode, thread float &mass, thread float &pdf,
                       thread float &eta) {
    mass = pdf = 0;
    eta = 1;
    if (wo.z == 0 || wi.z == 0)
        return true;
    const bool transmission = rypath_packet_roulette::opposite_hemispheres(wo.z, wi.z);
    if (packet::has_conductor() && p.kind == 1 && p.conductor.smooth) {
        if (transmission || !all(wi == float3(-wo.x, -wo.y, wo.z)))
            return true;
        pdf = 1;
        return rypath_conductor::fresnel(abs(wo.z), p.conductor.eta, p.conductor.k, mass);
    }
    if (packet::has_dielectric() && p.kind == 2 && (p.dielectric.smooth || p.dielectric.eta == 1)) {
        float F, ep, ct;
        bool tir;
        if (!rypath_dielectric::optics(wo.z, p.dielectric.eta, F, ep, ct, tir))
            return false;
        const float probability = transmission ? 1 - F : F;
        if (probability == 0)
            return true;
        // uc==F chooses transmission; uc==0 chooses supported reflection.
        float sampled_eta;
        auto lane = p;
        if (lane.dielectric.eta == 1)
            lane.dielectric.smooth = true;
        const auto candidate =
            packet::scalar_sample(lane, wo, transmission ? F : 0.f, u, sampled_eta);
        if (!candidate.valid)
            return false;
        if (!(candidate.pdf > 0) || !all(candidate.wi == wi))
            return true;
        pdf = candidate.pdf;
        mass = candidate.weight * pdf * packet::mode_scale(p, wo, wi, mode);
        eta = sampled_eta;
        return isfinite(mass) && mass >= 0;
    }
    if (packet::has_surface() && packet::has_thin_dielectric() && p.kind == 3) {
        const float3 direction = transmission ? -wo : float3(-wo.x, -wo.y, wo.z);
        if (!all(wi == direction))
            return true;
        float F;
        if (!rypath_dielectric::fresnel(abs(wo.z), p.dielectric.eta, F))
            return false;
        const float R = 2 * F / (1 + F);
        mass = pdf = transmission ? 1 - R : R;
        return true;
    }
    if (packet::has_gltf() && p.kind == 11 && p.gltf.alpha == 0 && wo.z > 0 && !transmission &&
        all(wi == float3(-wo.x, -wo.y, wo.z))) {
        pdf = p.gltf.specular_probability;
        mass = pdf > 0 ? rypath_gltf::fresnel(p.gltf, wo.z) : 0;
    }
    return true;
}
[[clang::noinline]] inline Sample sample(const thread Prepared &p, float3 wo, float uc, float2 u,
                                         uint seed, TransportMode mode = TransportMode::Radiance) {
    Sample out{};
    out.eta = 1;
    out.valid = false;
    if (!valid_structure(p) || !all(isfinite(wo)) || !rypath_transmission::randoms(uc, u))
        return out;
    if (requires_single_hero(p)) {
        if (p.active_mask != 1)
            return out;
        const auto legacy = packet::sample(p, wo, uc, u, seed, mode);
        out.wi = legacy.wi;
        out.delta = legacy.delta;
        out.transmission = legacy.transmission;
        out.accepted = legacy.accepted;
        out.valid = legacy.valid;
        out.eta = legacy.eta;
        out.fallback_weight = legacy.weight;
        out.fallback_pdf = legacy.pdf;
        out.fallback = true;
        // numerator/pdf remain zero: fallback_pdf is NOT a joint likelihood.
        return out;
    }
    float hero_eta;
    auto hero_lane = p.lane[0];
    // Matched indices transmit deterministically even if a manually prepared
    // state retains roughness; do not feed eta=1 into the rough Snell Jacobian.
    if (hero_lane.kind == 2 && hero_lane.dielectric.eta == 1)
        hero_lane.dielectric.smooth = true;
    const auto hero = packet::scalar_sample(hero_lane, wo, uc, u, hero_eta);
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
    // A real proposal rejection has zero mass/density. accepted=false with a
    // positive density may instead mean zero hero ENERGY (e.g. a black smooth
    // conductor/glTF lobe); keep that direction for contributing companions.
    if (hero.pdf == 0 || wo.z == 0 || hero.wi.z == 0)
        return out;
    if (hero.delta) {
        for (uint i = 0; i < 4; ++i)
            if (p.active_mask & (1u << i)) {
                float mass, density, eta;
                out.valid =
                    dirac_lane(p.lane[i], wo, hero.wi, u, mode, mass, density, eta) && out.valid;
                out.numerator[i] = mass;
                out.pdf[i] = density;
                out.eta[i] = eta;
            }
    } else {
        const auto e = self::evaluate(p, wo, hero.wi, seed, mode);
        out.numerator = e.f * abs(hero.wi.z);
        out.pdf = e.pdf;
        out.valid = e.valid;
        for (uint i = 0; i < 4; ++i)
            if ((p.active_mask & (1u << i)) && out.pdf[i] > 0)
                out.eta[i] = event_eta(p.lane[i], wo, hero.wi);
    }
    // The sampled hero PDF is the actual generation density, not a density
    // reconstructed from the rounded direction. Companion densities are queried
    // at that direction. The two can differ by floating-point roundoff.
    out.pdf[0] = hero.pdf;
    out.eta[0] = p.lane[0].kind == 2 ? hero_eta : event_eta(p.lane[0], wo, hero.wi);
    out.valid = out.valid && nonnegative(out.numerator) && nonnegative(out.pdf) &&
                all(isfinite(out.eta)) && all(out.eta > 0);
    out.accepted = out.valid && any(out.numerator > 0);
    return out;
}
} // namespace hero implementation
#if defined(RYPATH_PACKET_RENDER)
namespace rypath_hero_scattering = rypath_hero_scattering_renderer;
#endif
