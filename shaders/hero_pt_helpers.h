// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included inside rypath_bdpt, after the common visibility/light helpers.
struct HeroTrack {
    rypath_hero::Density density = rypath_hero::initial_density();
    rypath_hero::Density previous = rypath_hero::initial_density();
    float4 eta_scale = 1;
    uint active_mask = 15;
    bool roulette_enabled = true;
};
inline bool hero_enabled(constant wire::Parameters &p) { return packet_hero_enabled(p); }
inline Wavelengths packet_wavelengths(constant wire::Parameters &p, float u) {
    if (!hero_enabled(p))
        return rypath_packet_spectrum::sample_visible(u);
    return p.hero_wavelengths == 1 ? rypath_hero::sample_visible(u)
                                   : rypath_hero::sample_uniform(u);
}
inline bool hero_connectible(const thread rypath_packet::Prepared &bsdf) {
    for (uint k = 0; k < 4; ++k)
        if ((bsdf.active_mask & (1u << k)) && !rypath_scattering::delta_only(bsdf.lane[k]))
            return true;
    return false;
}
inline bool hero_connectible(const thread HeroFrozenBSDF &bsdf) {
    return hero_current_connectible(bsdf);
}
inline bool hero_prepare_future(thread HeroTrack &track, thread float4 &beta,
                                thread rypath_packet::Prepared &bsdf) {
    if (rypath_hero_scattering::requires_single_hero(bsdf) && track.active_mask != 1) {
        const auto collapsed = rypath_hero::collapse(track.density, beta);
        if (!collapsed.valid)
            return false;
        track.density = collapsed.density;
        beta = collapsed.beta;
        track.active_mask = 1;
    }
    bsdf.active_mask = track.active_mask;
    return true;
}
inline bool hero_prepare_future(thread HeroTrack &track, thread float4 &beta,
                                thread HeroFrozenBSDF &bsdf) {
    if (hero_current_requires_single(bsdf) && track.active_mask != 1) {
        const auto collapsed = rypath_hero::collapse(track.density, beta);
        if (!collapsed.valid)
            return false;
        track.density = collapsed.density;
        beta = collapsed.beta;
        track.active_mask = 1;
    }
    bsdf.active_mask = track.active_mask;
    return true;
}
// One cyclically invariant gate precedes BOTH the next light sample and BSDF
// continuation. Thus its density is common to the joint techniques and cancels
// from their balance weights. Arrival emission is evaluated before this gate.
inline bool hero_continue(thread float4 &beta, const thread HeroTrack &track, uint depth,
                          float uniform) {
    if (!track.roulette_enabled || depth <= 1)
        return true;
    float maximum = 0;
    for (uint k = 0; k < 4; ++k) {
        const float value = beta[k] * track.eta_scale[k];
        if (!isfinite(value) || value < 0 ||
            (beta[k] > 0 && !rypath_packet_roulette::positive_normal(value)))
            return true;
        maximum = max(maximum, value);
    }
    const rypath_packet_roulette::EtaScale unit{};
    const auto decision = rypath_packet_roulette::prepare(float4(maximum), unit, depth);
    if (!decision.eligible)
        return true;
    const float survival = 1 - decision.termination;
    float4 survivor = beta;
    for (uint k = 0; k < 4; ++k) {
        if (rypath_hero::bits(beta[k]) == 0)
            continue;
        survivor[k] /= survival;
        if (!rypath_packet_roulette::positive_normal(survivor[k]))
            return true;
    }
    if (uniform < decision.termination)
        return false;
    beta = survivor;
    return true;
}
inline void hero_account_eta(thread HeroTrack &track, float4 eta, bool transmission) {
    if (!transmission || !track.roulette_enabled)
        return;
    for (uint k = 0; k < 4; ++k) {
        rypath_packet_roulette::EtaScale lane{track.eta_scale[k], true};
        rypath_packet_roulette::account_transmission(lane, eta[k], true);
        if (!lane.enabled) {
            track.roulette_enabled = false;
            return;
        }
        track.eta_scale[k] = lane.value;
    }
}
inline rypath_hero_scattering::Evaluation
hero_history_evaluate(const thread rypath_packet::Prepared &p, float3 wo, float3 wi, uint seed) {
    return rypath_hero_scattering::evaluate(p, wo, wi, seed);
}
inline rypath_hero_scattering::Evaluation hero_history_evaluate(const thread HeroFrozenBSDF &p,
                                                                float3 wo, float3 wi, uint seed) {
    return hero_frozen_evaluate(p, wo, wi, seed);
}
template <typename V, typename Previous, typename Next>
inline float4 hero_represented_pdf(const thread V &v, const thread Previous &previous,
                                   const thread Next &next) {
    const float3 before = infinite(previous) ? -previous.point.geometric_normal
                                             : previous.point.position - v.point.position;
    const float3 after =
        infinite(next) ? -next.point.geometric_normal : next.point.position - v.point.position;
    if (all(before == 0) || all(after == 0))
        return 0;
    const auto e = hero_history_evaluate(v.bsdf, v.frame.local(normalize(before)),
                                         v.frame.local(normalize(after)), v.seed);
    return e.valid ? e.pdf : float4(NAN);
}
inline float4 hero_weight(thread wire::Sample &sample, const thread rypath_hero::Density &prefix,
                          float4 value, float4 sampled, float4 other) {
    if (!finite_nonnegative(value) || !finite_nonnegative(sampled) || !finite_nonnegative(other)) {
        sample.status = wire::invalid_arithmetic;
        return 0;
    }
    // A represented-geometry proxy may round to zero at a grazing event.
    // With no competing technique the sampled estimator retains weight one,
    // matching the existing PT convention; no undefined 0/0 is evaluated.
    if (rypath_hero::mixture(prefix, other).fraction == 0)
        return value;
    const auto result = rypath_hero::balance_contribution(prefix, value, sampled, other);
    if (!result.valid) {
        sample.status = wire::invalid_arithmetic;
        return 0;
    }
    return result.value;
}
template <typename Previous, typename Light>
inline float hero_history_light_pdf(const thread Resources &r, const thread Previous &from,
                                    const thread Light &light) {
    const float selection =
        rypath_light_sampling::pmf(*r.alpha.parameters, r.alpha.material_data,
                                   {from.point.position, from.point.shading_normal}, light.light);
    if (infinite(light))
        return environment_pdf(r, -light.point.geometric_normal) * selection;
    const float3 delta = light.point.position - from.point.position;
    const float d2 = dot(delta, delta);
    if (!(d2 > 0))
        return 0;
    if (!on_surface(light))
        return 0;
    auto alpha = r.alpha;
    const wire::Emitter emitter{light.instance, light.primitive, light.kind};
    return selection * rypath_light_geometry::pdf(alpha, emitter, from.point.position,
                                                  from.point.shading_normal, light.uv, light.point);
}
template <typename Current, typename Previous, typename Before>
inline float4 hero_endpoint(thread Resources &r, thread wire::Sample &sample,
                            const thread Wavelengths &lambda, const thread Current &pt,
                            const thread Previous &previous, const thread Before &before, uint t,
                            const thread rypath_hero::Density &previous_density) {
    float4 radiance;
    bool emission_valid;
    if (infinite(pt))
        emission_valid = emitted(r, pt, -pt.point.geometric_normal, lambda, radiance);
    else {
        const float3 delta = previous.point.position - pt.point.position;
        if (all(delta == 0)) {
            radiance = 0;
            emission_valid = true;
        } else
            emission_valid = emitted(r, pt, normalize(delta), lambda, radiance);
    }
    if (!emission_valid) {
        sample.status = wire::invalid_arithmetic;
        return 0;
    }
    const float4 value = pt.beta * radiance;
    if (t <= 2 || previous.delta || !nonzero(value))
        return value;
    const float4 bsdf_pdf = hero_represented_pdf(previous, before, pt);
    const float light_pdf = hero_history_light_pdf(r, previous, pt);
    return hero_weight(sample, previous_density, value, bsdf_pdf, float4(light_pdf));
}
template <typename Current, typename Previous>
inline float4 hero_direct(thread Resources &r, thread wire::Sample &sample,
                          const thread Wavelengths &lambda, ulong index, const thread Current &pt,
                          const thread Previous &before, uint t, float4 beta,
                          const thread rypath_hero::Density &prefix,
                          thread const rypath_replay::ReplayState *replay = nullptr) {
    if (pt.type != surface_vertex || !pt.connectible)
        return 0;
    const constant auto &p = *r.alpha.parameters;
    Vertex sampled{};
    float3 direction;
    float sampled_pdf = 0;
    if (!sample_li(r, pt, lambda, uniforms4(p, random(r, sample, index, 133 + t - 2, replay)),
                   sampled, direction, sampled_pdf) ||
        !isfinite(sampled_pdf) || sampled_pdf < 0) {
        sample.status = wire::invalid_arithmetic;
        return 0;
    }
    if (!(sampled_pdf > 0) || !nonzero(sampled.beta))
        return 0;
    const auto evaluation =
        hero_evaluate_current(pt.bsdf, pt.pair_valid ? pt.sampled_wo_local : pt.frame.local(pt.wo),
                              pt.frame.local(direction), pt.seed);
    if (!evaluation.valid) {
        sample.status = wire::invalid_arithmetic;
        return 0;
    }
    const float4 value =
        beta * evaluation.f * sampled.beta * abs(dot(pt.point.shading_normal, direction));
    if (!nonzero(value) || !visible(r, pt, sampled, sample))
        return 0;
    if (sampled.delta_light)
        return value;
    sampled_pdf = direct_pdf(r, pt, sampled);
    const float4 bsdf_pdf = hero_represented_pdf(pt, before, sampled);
    return hero_weight(sample, prefix, value, float4(sampled_pdf), bsdf_pdf);
}
