// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifdef __METAL_VERSION__
#include <metal_stdlib>
namespace hero_scattering_probe {
using Word = metal::uint;
#else
#include <cstdint>
namespace hero_scattering_probe {
using Word = std::uint32_t;
#endif
struct Query {
    Word kind, active_mask, importance, force_rough;
    float eta[4], reflectance[4], transmittance[4], alpha;
    float wo[3], wi[3], uc, u[2];
};
struct Result {
    float wi[3], numerator[4], pdf[4], eta[4], f[4], evaluation_pdf[4];
    float fallback_weight[4], fallback_pdf;
    Word valid, accepted, delta, transmission, fallback, evaluation_valid, requires_single;
    // Legacy controls test fallback plumbing only, not independent BSDF correctness.
    float legacy_weight[4], legacy_pdf, legacy_f[4], legacy_evaluation_pdf;
};
static_assert(sizeof(Query) == 104 && sizeof(Result) == 180, "Hero probe CPU/MSL ABI");
} // namespace hero_scattering_probe
