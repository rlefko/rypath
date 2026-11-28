// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "hero_frozen_bsdf.h"
// Included inside rypath_bdpt after packet_walk.h and hero_frozen_bsdf.h.
static_assert(sizeof(rypath_scattering::Prepared) == 152 &&
                  sizeof(rypath_packet_layered::Parameters) == 276,
              "Compact hero uses one scalar lane or packet coating scratch");
// Current-event storage differs from terminal history: coatings retain their
// material tag and full active mask until the caller explicitly collapses.
inline bool hero_current_structure(const thread HeroFrozenBSDF &p) {
    return (p.active_mask & 1u) && !(p.active_mask & ~15u) &&
           (p.kind == 0 || p.kind == 1 || p.kind == 2 || p.kind == 3 || p.kind == 4 ||
            p.kind == 5 || p.kind == 6 || p.kind == 7 || p.kind == 8 || p.kind == 11);
}
inline bool hero_current_requires_single(const thread HeroFrozenBSDF &p) {
    return (p.active_mask & 15u) && (p.kind == 5 || p.kind == 6) && !hero_frozen_shared_coating(p);
}
inline void hero_current_store_lane(thread HeroFrozenBSDF &out, uint k,
                                    const thread rypath_scattering::Prepared &s) {
    hero_frozen_store_lane(out, k, s);
#if RYPATH_HERO_SHARED_COATED_DIFFUSE
    if (out.kind == 5) {
        if (k == 0)
            out.payload[20] = hero_shared_coating_controls(s.layered, s.layered) ? 0x53434431u : 0u;
        else {
            hero_frozen_store_float(out, 16u + k, s.layered.reflectance);
            if (!hero_shared_coating_controls(hero_frozen_coated(out), s.layered))
                out.payload[20] = 0;
        }
    }
#endif
}
// One scalar staging value at a time; never reconstruct the four-lane Prepared.
inline rypath_scattering::Prepared hero_current_scalar(const thread HeroFrozenBSDF &p, uint k) {
    rypath_scattering::Prepared s{};
    s.kind = p.kind;
    switch (p.kind) {
    case 0:
        s.reflectance = hero_frozen_diffuse(p, k);
        break;
    case 1:
        s.conductor = hero_frozen_conductor(p, k);
        break;
    case 2:
        s.dielectric = hero_frozen_dielectric(p, k);
        break;
    case 3:
        s.dielectric.eta = hero_frozen_thin_eta(p, k);
        break;
    case 4: {
        const auto t = hero_frozen_transmission(p, k);
        s.reflectance = t.reflectance;
        s.transmittance = t.transmittance;
        break;
    }
    case 5:
    case 6:
        s.layered = hero_frozen_coated(p);
        if (k && hero_frozen_shared_coating(p))
            s.layered.reflectance = hero_frozen_float(p, 16u + k);
        break;
    case 7:
    case 8: {
        const auto t = hero_frozen_table(p, k);
        s.table = t.table;
        s.rgb_table = t.rgb_table;
        s.wavelength = t.wavelength;
        break;
    }
    case 11:
        s.gltf = hero_frozen_gltf(p, k);
        break;
    }
    return s;
}
inline bool hero_current_connectible(const thread HeroFrozenBSDF &p) {
    for (uint k = 0; k < 4; ++k)
        if ((p.active_mask & (1u << k)) &&
            !rypath_scattering::delta_only(hero_current_scalar(p, k)))
            return true;
    return false;
}
inline bool prepare_hero_current(const thread Resources &r, const device wire::Material &material,
                                 graph_wire::Context context, const thread Wavelengths &lambda,
                                 thread HeroFrozenBSDF &out, uint active_mask = 15) {
    const constant auto &p = *r.alpha.parameters;
    if (material.scattering != 0 &&
        !(conductor_enabled || dielectric_enabled || surface_materials_enabled || gltf_enabled))
        return false;
    const device auto *aux =
        material_auxiliary(material, r.alpha.material_data, p.material_data_words);
    out = {};
    out.kind = material.scattering;
    out.active_mask = active_mask;
    bool shared_coating = RYPATH_HERO_SHARED_COATED_DIFFUSE && material.scattering == 5;
    for (uint k = 0; k < 4; ++k) {
        if (!(active_mask & (1u << k)))
            continue;
        float rho;
        if (!packet_reflectance(r, material, context, lambda.lambda[k], rho))
            return false;
        rypath_scattering::Prepared lane{};
        if (material.scattering == 0) {
            lane.kind = 0;
            lane.reflectance = rho;
            lane.wavelength = lambda.lambda[k];
            lane.rgb_table = r.alpha.graph.images.spectra;
        } else if (!prepare_scattering(material, aux, rho, r.knots, p, r.alpha.graph, context,
                                       lambda.lambda[k], r.alpha.material_data,
                                       r.alpha.graph.images.spectra, lane)) {
            return false;
        }
        // Every active companion is prepared and validated even for coatings,
        // although only hero0's coating survives in the tagged payload.
        if (shared_coating) {
            const auto reference = k ? hero_frozen_coated(out) : lane.layered;
            shared_coating = hero_shared_coating_controls(reference, lane.layered);
        }
        hero_current_store_lane(out, k, lane);
    }
#if RYPATH_HERO_SHARED_COATED_DIFFUSE
    if (shared_coating)
        out.payload[20] = 0x53434431u;
#endif
    return hero_current_structure(out);
}
inline bool hero_current_coated_compatible(const thread HeroFrozenBSDF &p) {
    const auto x = hero_frozen_coated(p);
    // packet::compatible compares these values even to themselves in mask1.
    // Do not replace with isfinite: infinite roughness compares equal there.
    return !isnan(x.interface.alpha_x) && !isnan(x.interface.alpha_y) &&
           (!x.conductor_base || (!isnan(x.conductor.alpha_x) && !isnan(x.conductor.alpha_y))) &&
           rypath_layered::valid_parameters(x);
}
inline rypath_hero_scattering::Evaluation hero_current_evaluate(const thread HeroFrozenBSDF &p,
                                                                float3 wo, float3 wi, uint seed,
                                                                Mode mode = Mode::Radiance) {
    if (!hero_current_structure(p) ||
        ((p.kind == 5 || p.kind == 6) && ((p.active_mask != 1 && !hero_frozen_shared_coating(p)) ||
                                          !hero_current_coated_compatible(p))))
        return {float4(0), float4(0), false};
    return hero_frozen_evaluate(p, wo, wi, seed, mode);
}
[[clang::noinline]] inline rypath_hero_scattering::Sample
hero_current_sample(const thread HeroFrozenBSDF &p, float3 wo, float uc, float2 u, uint seed,
                    Mode mode = Mode::Radiance) {
    namespace h = rypath_hero_scattering;
    namespace packet = rypath_packet;
    h::Sample out{};
    out.eta = 1;
    if (!hero_current_structure(p) || !all(isfinite(wo)) || !rypath_transmission::randoms(uc, u))
        return out;
    if (p.kind == 5 || p.kind == 6) {
        if (p.active_mask != 1 && !hero_frozen_shared_coating(p))
            return out;
        out.fallback = true;
        if (!hero_current_coated_compatible(p) || !packet::has_surface() || !packet::has_coated())
            return out;
        const auto q = hero_frozen_layered_packet(p);
        const auto a = rypath_packet_layered::sample(q, wo, uc, u, seed, mode == Mode::Importance);
        out.wi = a.wi;
        out.delta = a.delta;
        out.transmission = rypath_packet_roulette::opposite_hemispheres(wo.z, out.wi.z);
        out.fallback_weight = a.weight;
        out.fallback_pdf = a.pdf;
        float reverse = a.pdf;
        if (!out.delta) {
            const auto density = rypath_packet_layered::directional_pdf(q, out.wi, wo);
            reverse = density.valid ? density.pdf : NAN;
        }
        // Legacy packet coating has eta=1, even for hidden interface refraction.
        out.valid = a.valid && all(isfinite(a.weight)) && all(a.weight >= 0) && isfinite(a.pdf) &&
                    a.pdf >= 0 && isfinite(reverse) && reverse >= 0;
        out.accepted = a.accepted && out.valid && packet::maximum(a.weight) > 0;
        return out;
    }
    float hero_eta;
    rypath_scattering::Sample hero;
    {
        auto lane = hero_current_scalar(p, 0);
        if (lane.kind == 2 && lane.dielectric.eta == 1)
            lane.dielectric.smooth = true;
        hero = packet::scalar_sample(lane, wo, uc, u, hero_eta);
    }
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
    if (hero.delta) {
        for (uint k = 0; k < 4; ++k)
            if (p.active_mask & (1u << k)) {
                const auto lane = hero_current_scalar(p, k);
                float mass, density, eta;
                out.valid =
                    h::dirac_lane(lane, wo, hero.wi, u, mode, mass, density, eta) && out.valid;
                out.numerator[k] = mass;
                out.pdf[k] = density;
                out.eta[k] = eta;
            }
    } else {
        const auto e = hero_current_evaluate(p, wo, hero.wi, seed, mode);
        out.numerator = e.f * abs(hero.wi.z);
        out.pdf = e.pdf;
        out.valid = e.valid;
        for (uint k = 0; k < 4; ++k)
            if ((p.active_mask & (1u << k)) && out.pdf[k] > 0)
                out.eta[k] = h::event_eta(hero_current_scalar(p, k), wo, hero.wi);
    }
    out.pdf[0] = hero.pdf;
    out.eta[0] = p.kind == 2 ? hero_eta : h::event_eta(hero_current_scalar(p, 0), wo, hero.wi);
    out.valid = out.valid && h::nonnegative(out.numerator) && h::nonnegative(out.pdf) &&
                all(isfinite(out.eta)) && all(out.eta > 0);
    out.accepted = out.valid && any(out.numerator > 0);
    return out;
}
