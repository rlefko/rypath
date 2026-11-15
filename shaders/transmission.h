// SPDX-License-Identifier: Apache-2.0
// PBRT v4 b0bdf461 thin-sheet and diffuse transmission equations, adapted for Metal.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt.
#pragma once
#include "dielectric.h"
namespace rypath_transmission {
using Evaluation = rypath_conductor::Evaluation;
using Sample = rypath_conductor::Sample;
inline bool randoms(float uc, float2 u) {
    return isfinite(uc) && uc >= 0 && uc < 1 && all(isfinite(u)) && all(u >= 0) && all(u < 1);
}
inline float3 cosine(float2 u) {
    float r = sqrt(u.x), phi = 2 * M_PI_F * u.y;
    return float3(r * cos(phi), r * sin(phi), sqrt(1 - u.x));
}
inline Evaluation thin_evaluate(float eta, float3 wo, float3 wi) {
    return {0, 0, isfinite(eta) && eta > 0 && all(isfinite(wo)) && all(isfinite(wi))};
}
inline Sample thin_sample(float eta, float3 wo, float uc, float2 u) {
    Sample s = {float3(0), 0, 0, true, false, true};
    if (!(eta > 0) || !isfinite(eta) || !all(isfinite(wo)) || !randoms(uc, u)) {
        s.valid = false;
        return s;
    }
    if (wo.z == 0)
        return s;
    float R;
    if (!rypath_dielectric::fresnel(abs(wo.z), eta, R)) {
        s.valid = false;
        return s;
    }
    // R + (1-R)^2 R / (1-R^2) = 2R/(1+R), including R=1.
    R = 2 * R / (1 + R);
    bool reflection = uc < R;
    s.wi = reflection ? float3(-wo.x, -wo.y, wo.z) : -wo;
    s.pdf = reflection ? R : 1 - R;
    s.weight = 1;
    s.accepted = s.pdf > 0;
    return s;
}
inline Evaluation diffuse_evaluate(float R, float T, float3 wo, float3 wi) {
    if (!isfinite(R) || !isfinite(T) || R < 0 || T < 0 || !all(isfinite(wo)) || !all(isfinite(wi)))
        return {0, 0, false};
    float sum = R + T;
    if (!isfinite(sum))
        return {0, 0, false};
    if (wo.z == 0 || wi.z == 0 || sum == 0)
        return {0, 0, true};
    float value = (wo.z > 0) == (wi.z > 0) ? R : T;
    return {value / M_PI_F, value / sum * abs(wi.z) / M_PI_F, true};
}
inline Sample diffuse_sample(float R, float T, float3 wo, float uc, float2 u) {
    Sample s = {float3(0), 0, 0, false, false, true};
    float sum = R + T;
    if (!isfinite(R) || !isfinite(T) || R < 0 || T < 0 || !isfinite(sum) || !all(isfinite(wo)) ||
        !randoms(uc, u)) {
        s.valid = false;
        return s;
    }
    if (sum == 0 || wo.z == 0)
        return s;
    bool reflection = uc < R / sum;
    s.wi = cosine(u);
    if ((wo.z < 0) == reflection)
        s.wi.z = -s.wi.z;
    s.pdf = (reflection ? R : T) / sum * abs(s.wi.z) / M_PI_F;
    s.weight = sum;
    s.accepted = s.pdf > 0;
    return s;
}
} // namespace rypath_transmission
