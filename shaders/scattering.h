// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "image_texture.h"
#include "layered.h"
#include "table_bsdf.h"

namespace rypath_scattering {
using Evaluation = rypath_conductor::Evaluation;
using Sample = rypath_conductor::Sample;
struct Prepared {
    uint kind;
    float reflectance, transmittance, wavelength;
    rypath_conductor::Parameters conductor;
    rypath_dielectric::Parameters dielectric;
    rypath_layered::Parameters layered;
    const device uint *table;
    const device float *rgb_table;
};
inline float spectral_value(float3 value, bool rgb, const thread Prepared &p) {
    return rgb ? rypath_image::unbounded_spectrum(value, p.wavelength, p.rgb_table) : value.x;
}
inline bool delta_only(const thread Prepared &p) {
    return (p.kind == 1 && p.conductor.smooth) || (p.kind == 2 && p.dielectric.smooth) ||
           p.kind == 3;
}
inline bool both_hemispheres(const thread Prepared &p) {
    return p.kind == 2 || p.kind == 3 || p.kind == 4 || p.kind == 8;
}
inline Evaluation evaluate(const thread Prepared &p, float3 wo, float3 wi, uint seed) {
    switch (p.kind) {
    case 0:
        return rypath_transmission::diffuse_evaluate(p.reflectance, 0, wo, wi);
    case 1:
        return rypath_conductor::evaluate(p.conductor, wo, wi);
    case 2:
        return rypath_dielectric::evaluate(p.dielectric, wo, wi);
    case 3:
        return rypath_transmission::thin_evaluate(p.dielectric.eta, wo, wi);
    case 4:
        return rypath_transmission::diffuse_evaluate(p.reflectance, p.transmittance, wo, wi);
    case 5:
    case 6:
        return rypath_layered::evaluate(p.layered, wo, wi, seed);
    case 7:
    case 8: {
        auto e = table_bsdf_evaluate(p.table, wo, wi, p.wavelength);
        float f = spectral_value(e.value, e.rgb, p);
        return {f, e.pdf, e.valid && isfinite(f) && f >= 0};
    }
    }
    return {0, 0, false};
}
inline Sample sample(const thread Prepared &p, float3 wo, float uc, float2 u, uint seed) {
    switch (p.kind) {
    case 0:
        return rypath_transmission::diffuse_sample(p.reflectance, 0, wo, uc, u);
    case 1:
        return rypath_conductor::sample(p.conductor, wo, u);
    case 2:
        return rypath_dielectric::sample(p.dielectric, wo, uc, u);
    case 3:
        return rypath_transmission::thin_sample(p.dielectric.eta, wo, uc, u);
    case 4:
        return rypath_transmission::diffuse_sample(p.reflectance, p.transmittance, wo, uc, u);
    case 5:
    case 6:
        return rypath_layered::sample(p.layered, wo, uc, u, seed);
    case 7:
    case 8: {
        auto e = table_bsdf_sample(p.table, wo, u, p.wavelength);
        if (!e.valid || !e.accepted)
            return {e.wi, 0, e.pdf, false, false, e.valid};
        float f = spectral_value(e.value, e.rgb, p);
        float weight = f * abs(e.wi.z) / e.pdf;
        return {e.wi,
                weight,
                e.pdf,
                false,
                weight > 0,
                isfinite(f) && f >= 0 && isfinite(weight) && weight >= 0 && isfinite(e.pdf) &&
                    e.pdf > 0};
    }
    }
    return {float3(0), 0, 0, false, false, false};
}
} // namespace rypath_scattering
