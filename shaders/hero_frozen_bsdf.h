// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included inside rypath_bdpt. Only immutable, evaluated material values live here.
struct HeroFrozenTable {
    const device uint *table;
    const device float *rgb_table;
    float wavelength;
};
struct HeroFrozenTransmission {
    float reflectance, transmittance;
};
// A union aggregate is unsafe for persistence: Metal lowers it through the
// largest member's fields and can omit bytes that are padding in that member
// but live data in another. Every payload word here has an initialized value.
struct alignas(8) HeroFrozenBSDF {
    uint kind, active_mask;
    uint payload[24];
};
static_assert(sizeof(HeroFrozenTable) == 24 && sizeof(HeroFrozenBSDF) == 104 &&
                  alignof(HeroFrozenBSDF) == 8 && sizeof(const device uint *) == 8,
              "Frozen hero material ABI");
inline float hero_frozen_float(const thread HeroFrozenBSDF &p, uint word) {
    return as_type<float>(p.payload[word]);
}
inline void hero_frozen_store_float(thread HeroFrozenBSDF &p, uint word, float value) {
    p.payload[word] = as_type<uint>(value);
}
// Unsigned-character access copies the pointer's object representation. It
// neither converts the address numerically nor narrows it to a FLOAT or uint.
template <typename T>
inline void hero_frozen_store_pointer(thread uint *words, const device T *value) {
    const thread uchar *bytes = reinterpret_cast<const thread uchar *>(&value);
    words[0] = words[1] = 0;
    for (uint i = 0; i < sizeof(value); ++i)
        words[i / 4] |= uint(bytes[i]) << (8 * (i % 4));
}
template <typename T> inline const device T *hero_frozen_pointer(const thread uint *words) {
    const device T *value = nullptr;
    thread uchar *bytes = reinterpret_cast<thread uchar *>(&value);
    for (uint i = 0; i < sizeof(value); ++i)
        bytes[i] = uchar(words[i / 4] >> (8 * (i % 4)));
    return value;
}
inline float hero_frozen_diffuse(const thread HeroFrozenBSDF &p, uint k) {
    return hero_frozen_float(p, k);
}
inline rypath_conductor::Parameters hero_frozen_conductor(const thread HeroFrozenBSDF &p, uint k) {
    const uint b = 5 * k;
    return {hero_frozen_float(p, b), hero_frozen_float(p, b + 1), hero_frozen_float(p, b + 2),
            hero_frozen_float(p, b + 3), p.payload[b + 4] != 0};
}
inline rypath_dielectric::Parameters hero_frozen_dielectric(const thread HeroFrozenBSDF &p,
                                                            uint k) {
    const uint b = 4 * k;
    return {hero_frozen_float(p, b), hero_frozen_float(p, b + 1), hero_frozen_float(p, b + 2),
            p.payload[b + 3] != 0};
}
inline float hero_frozen_thin_eta(const thread HeroFrozenBSDF &p, uint k) {
    return hero_frozen_float(p, k);
}
inline HeroFrozenTransmission hero_frozen_transmission(const thread HeroFrozenBSDF &p, uint k) {
    return {hero_frozen_float(p, 2 * k), hero_frozen_float(p, 2 * k + 1)};
}
inline rypath_layered::Parameters hero_frozen_coated(const thread HeroFrozenBSDF &p) {
    return {{hero_frozen_float(p, 0), hero_frozen_float(p, 1), hero_frozen_float(p, 2),
             p.payload[3] != 0},
            {hero_frozen_float(p, 4), hero_frozen_float(p, 5), hero_frozen_float(p, 6),
             hero_frozen_float(p, 7), p.payload[8] != 0},
            p.payload[9] != 0,
            hero_frozen_float(p, 10),
            hero_frozen_float(p, 11),
            hero_frozen_float(p, 12),
            hero_frozen_float(p, 13),
            p.payload[14],
            p.payload[15],
            as_type<int>(p.payload[16])};
}
inline HeroFrozenTable hero_frozen_table(const thread HeroFrozenBSDF &p, uint k) {
    const uint b = 6 * k;
    return {hero_frozen_pointer<uint>(p.payload + b), hero_frozen_pointer<float>(p.payload + b + 2),
            hero_frozen_float(p, b + 4)};
}
inline rypath_gltf::Parameters hero_frozen_gltf(const thread HeroFrozenBSDF &p, uint k) {
    const uint b = 4 * k;
    return {hero_frozen_float(p, b), hero_frozen_float(p, b + 1), hero_frozen_float(p, b + 2),
            hero_frozen_float(p, b + 3)};
}
inline void hero_frozen_store_lane(thread HeroFrozenBSDF &out, uint k,
                                   const thread rypath_scattering::Prepared &s) {
    switch (out.kind) {
    case 0:
        hero_frozen_store_float(out, k, s.reflectance);
        break;
    case 1: {
        const uint b = 5 * k;
        hero_frozen_store_float(out, b, s.conductor.eta);
        hero_frozen_store_float(out, b + 1, s.conductor.k);
        hero_frozen_store_float(out, b + 2, s.conductor.alpha_x);
        hero_frozen_store_float(out, b + 3, s.conductor.alpha_y);
        out.payload[b + 4] = uint(s.conductor.smooth);
        break;
    }
    case 2: {
        const uint b = 4 * k;
        hero_frozen_store_float(out, b, s.dielectric.eta);
        hero_frozen_store_float(out, b + 1, s.dielectric.alpha_x);
        hero_frozen_store_float(out, b + 2, s.dielectric.alpha_y);
        out.payload[b + 3] = uint(s.dielectric.smooth);
        break;
    }
    case 3:
        hero_frozen_store_float(out, k, s.dielectric.eta);
        break;
    case 4:
        hero_frozen_store_float(out, 2 * k, s.reflectance);
        hero_frozen_store_float(out, 2 * k + 1, s.transmittance);
        break;
    case 5:
    case 6:
        if (k != 0)
            break;
        hero_frozen_store_float(out, 0, s.layered.interface.eta);
        hero_frozen_store_float(out, 1, s.layered.interface.alpha_x);
        hero_frozen_store_float(out, 2, s.layered.interface.alpha_y);
        out.payload[3] = uint(s.layered.interface.smooth);
        hero_frozen_store_float(out, 4, s.layered.conductor.eta);
        hero_frozen_store_float(out, 5, s.layered.conductor.k);
        hero_frozen_store_float(out, 6, s.layered.conductor.alpha_x);
        hero_frozen_store_float(out, 7, s.layered.conductor.alpha_y);
        out.payload[8] = uint(s.layered.conductor.smooth);
        out.payload[9] = uint(s.layered.conductor_base);
        hero_frozen_store_float(out, 10, s.layered.reflectance);
        hero_frozen_store_float(out, 11, s.layered.thickness);
        hero_frozen_store_float(out, 12, s.layered.albedo);
        hero_frozen_store_float(out, 13, s.layered.g);
        out.payload[14] = s.layered.max_depth;
        out.payload[15] = s.layered.samples;
        out.payload[16] = as_type<uint>(s.layered.material_seed);
        break;
    case 7:
    case 8: {
        const uint b = 6 * k;
        hero_frozen_store_pointer(out.payload + b, s.table);
        hero_frozen_store_pointer(out.payload + b + 2, s.rgb_table);
        hero_frozen_store_float(out, b + 4, s.wavelength);
        break;
    }
    case 11: {
        const uint b = 4 * k;
        hero_frozen_store_float(out, b, s.gltf.color);
        hero_frozen_store_float(out, b + 1, s.gltf.metallic);
        hero_frozen_store_float(out, b + 2, s.gltf.alpha);
        hero_frozen_store_float(out, b + 3, s.gltf.specular_probability);
        break;
    }
    }
}
inline HeroFrozenBSDF hero_freeze_bsdf(const thread rypath_packet::Prepared &p) {
    HeroFrozenBSDF f{};
    f.kind = p.lane[0].kind;
    f.active_mask = p.active_mask;
    if (!rypath_hero_scattering::valid_structure(p)) {
        f.kind = 0xffffffffu;
        return f;
    }
    // Multi-lane coatings occur only in terminal history (before collapse).
    // Their legacy hero query is invalid; retain that result without four coats.
    if ((f.kind == 5 || f.kind == 6) && (f.active_mask != 1 || !rypath_packet::compatible(p))) {
        f.kind = 0xffffffffu;
        return f;
    }
    for (uint k = 0; k < 4; ++k)
        if (f.active_mask & (1u << k))
            hero_frozen_store_lane(f, k, p.lane[k]);
    return f;
}
inline rypath_hero_scattering::Evaluation hero_frozen_evaluate(const thread HeroFrozenBSDF &p,
                                                               float3 wo, float3 wi, uint seed,
                                                               Mode mode = Mode::Radiance) {
    using namespace rypath_hero_scattering;
    Evaluation out{float4(0), float4(0), false};
    if (!(p.active_mask & 1u) || (p.active_mask & ~15u) || !all(isfinite(wo)) || !all(isfinite(wi)))
        return out;
    if (p.kind == 5 || p.kind == 6) {
        if (p.active_mask != 1 || !rypath_packet::has_surface() || !rypath_packet::has_coated())
            return out;
        const auto coated = hero_frozen_coated(p);
        if (!rypath_layered::valid_parameters(coated))
            return out;
        // Reuse the packet float4 estimator and both directional validations;
        // scalar layered evaluation has a different arithmetic/RNG contract.
        rypath_packet_layered::Parameters q;
        q.active_mask = 1;
        for (uint k = 0; k < 4; ++k)
            q.lane[k] = coated;
        auto forward = rypath_packet_layered::directional_pdf(q, wo, wi);
        auto reverse = rypath_packet_layered::directional_pdf(q, wi, wo);
        auto e = rypath_packet_layered::evaluate(q, wo, wi, seed, mode == Mode::Importance);
        out.f = e.f;
        out.pdf[0] = forward.pdf;
        out.valid = e.valid && forward.valid && reverse.valid && isfinite(forward.pdf) &&
                    forward.pdf >= 0 && isfinite(reverse.pdf) && reverse.pdf >= 0 &&
                    nonnegative(out.f) && nonnegative(out.pdf);
        return out;
    }
    if (p.kind != 0 && p.kind != 1 && p.kind != 2 && p.kind != 3 && p.kind != 4 && p.kind != 7 &&
        p.kind != 8 && p.kind != 11)
        return out;
    out.valid = true;
    for (uint k = 0; k < 4; ++k) {
        if (!(p.active_mask & (1u << k)))
            continue;
        rypath_scattering::Evaluation e{0, 0, false};
        float scale = 1;
        switch (p.kind) {
        case 0:
            e = rypath_transmission::diffuse_evaluate(hero_frozen_diffuse(p, k), 0, wo, wi);
            break;
        case 1:
            if (rypath_packet::has_conductor())
                e = rypath_conductor::evaluate(hero_frozen_conductor(p, k), wo, wi);
            break;
        case 2: {
            auto d = hero_frozen_dielectric(p, k);
            if (d.eta == 1)
                d.smooth = true;
            if (rypath_packet::has_dielectric())
                e = rypath_dielectric::evaluate(d, wo, wi);
            if (mode != Mode::Radiance && wo.z * wi.z < 0)
                scale = wo.z > 0 ? d.eta * d.eta : 1 / (d.eta * d.eta);
            break;
        }
        case 3:
            if (rypath_packet::has_surface() && rypath_packet::has_thin_dielectric())
                e = rypath_transmission::thin_evaluate(hero_frozen_thin_eta(p, k), wo, wi);
            break;
        case 4:
            if (rypath_packet::has_surface() && rypath_packet::has_diffuse_transmission())
                e = rypath_transmission::diffuse_evaluate(
                    hero_frozen_transmission(p, k).reflectance,
                    hero_frozen_transmission(p, k).transmittance, wo, wi);
            break;
        case 7:
        case 8: {
            const auto t = hero_frozen_table(p, k);
            if (rypath_packet::has_surface()) {
                const auto a = rypath_packet::table_evaluate(t.table, wo, wi, t.wavelength);
                const float value =
                    a.rgb ? rypath_image::unbounded_spectrum(a.value, t.wavelength, t.rgb_table)
                          : a.value.x;
                e = {value, a.pdf, a.valid && isfinite(value) && value >= 0};
            }
            if (mode != Mode::Radiance && wo.z * wi.z < 0 && rypath_packet::has_fourier() &&
                t.table[0] == RYPATH_TABLE_FOURIER) {
                const float eta = tb_f(t.table, 4);
                scale = wi.z < 0 ? eta * eta : 1 / (eta * eta);
            }
            break;
        }
        case 11:
            if (rypath_packet::has_gltf())
                e = rypath_gltf::evaluate(hero_frozen_gltf(p, k), wo, wi);
            break;
        }
        out.f[k] = e.f * scale;
        out.pdf[k] = e.pdf;
        out.valid = out.valid && e.valid;
    }
    out.valid = out.valid && nonnegative(out.f) && nonnegative(out.pdf);
    return out;
}
