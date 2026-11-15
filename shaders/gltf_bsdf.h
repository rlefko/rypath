// SPDX-License-Identifier: GPL-3.0-only
// glTF metallic-roughness spectral adaptation. See docs/gltf-import.md.
// Visible-normal disk sampling follows Heitz (2018); stable endpoint arithmetic
// follows the Apache-2.0 PBRT adaptation in conductor.h.
#pragma once
#include "conductor.h"
namespace rypath_gltf {
using Evaluation = rypath_conductor::Evaluation;
using Sample = rypath_conductor::Sample;
struct Parameters {
    float color, metallic, alpha, specular_probability;
};
inline bool prepare(float color, float metallic, float roughness, thread Parameters &p) {
    if (!isfinite(color) || !isfinite(metallic) || !isfinite(roughness) || color < 0 || color > 1 ||
        metallic < 0 || metallic > 1 || roughness < 0 || roughness > 1)
        return false;
    p = {color, metallic, roughness * roughness, metallic == 1 || color == 0 ? 1.0f : .5f};
    // Metal comparisons may flush subnormal inputs; inspect bits before choosing delta.
    return (as_type<uint>(roughness) & 0x7fffffffu) == 0 || p.alpha > 0;
}
inline float fifth(float v) {
    float s = v * v;
    return s * s * v;
}
inline float fresnel(const thread Parameters &p, float cosine) {
    const float t = fifth(1 - clamp(cosine, 0.0f, 1.0f));
    return (1 - p.metallic) * (.04f + .96f * t) + p.metallic * (p.color + (1 - p.color) * t);
}
inline float lambda(float alpha, float3 w) {
    const float a = alpha * length(w.xy) / abs(w.z);
    if (!isfinite(a))
        return NAN;
    const float scale = max(1.0f, a);
    const float radius = scale * length(float2(1, a) / scale);
    // Rationalize at small slopes without squaring an unbounded slope.
    return .5f * a * (a / (radius + 1));
}
inline float distribution(float alpha, float3 h) {
    const float a2 = alpha * alpha;
    const float denominator = dot(h.xy, h.xy) + a2 * h.z * h.z;
    if (!(a2 > 0) || !(denominator > 0))
        return NAN;
    const float ratio = alpha / denominator;
    const float d = ratio * ratio / M_PI_F;
    return isfinite(d) && d > 0 ? d : NAN;
}
inline Evaluation evaluate(const thread Parameters &p, float3 wo, float3 wi) {
    if (!all(isfinite(wo)) || !all(isfinite(wi)))
        return {0, 0, false};
    if (wo.z <= 0 || wi.z <= 0)
        return {0, 0, true};
    float3 h;
    if (!rypath_conductor::unit(wo + wi, h))
        return {0, 0, false};
    const float t = fifth(1 - clamp(abs(dot(wo, h)), 0.0f, 1.0f));
    const float diffuse = (1 - p.metallic) * (.96f * (1 - t)) * p.color / M_PI_F;
    float f = diffuse;
    float pdf = (1 - p.specular_probability) * wi.z / M_PI_F;
    if (p.alpha > 0) {
        const float lo = lambda(p.alpha, wo), li = lambda(p.alpha, wi);
        const float d = distribution(p.alpha, h);
        f += fresnel(p, abs(dot(wo, h))) * d / (4 * wo.z * wi.z * (1 + lo + li));
        pdf += p.specular_probability * d / (4 * wo.z * (1 + lo));
    }
    return {f, pdf, isfinite(f) && f >= 0 && isfinite(pdf) && pdf >= 0};
}
inline Sample sample(const thread Parameters &p, float3 wo, float uc, float2 u) {
    Sample s{float3(0), 0, 0, false, false, true};
    if (!all(isfinite(wo)) || !all(isfinite(u)) || !isfinite(uc) || uc < 0 || uc >= 1 ||
        any(u < 0) || any(u >= 1)) {
        s.valid = false;
        return s;
    }
    if (wo.z <= 0)
        return s;
    if (uc < p.specular_probability) {
        if (p.alpha == 0) {
            s.wi = float3(-wo.x, -wo.y, wo.z);
            s.pdf = p.specular_probability;
            s.weight = fresnel(p, wo.z) / p.specular_probability;
            s.delta = true;
            s.accepted = s.weight > 0;
            return s;
        }
        float3 wh;
        if (!rypath_conductor::unit(float3(p.alpha * wo.xy, wo.z), wh)) {
            s.valid = false;
            return s;
        }
        float3 t1(1, 0, 0);
        if (wh.z < .99999f && !rypath_conductor::unit(float3(-wh.y, wh.x, 0), t1)) {
            s.valid = false;
            return s;
        }
        const float3 t2 = cross(wh, t1);
        const float r = sqrt(u.x), phi = 2 * M_PI_F * u.y;
        float2 disk = r * float2(cos(phi), sin(phi));
        const float height = sqrt(max(0.0f, 1 - disk.x * disk.x));
        const float a = (1 + wh.z) * .5f;
        const float old_y = disk.y;
        disk.y = (1 - a) * height + a * old_y;
        const float difference = old_y > 0 ? -(1 - u.x) / (height + old_y) : old_y - height;
        const float z = sqrt(max(0.0f, (1 - u.x) + (1 - a) * difference * (old_y + disk.y)));
        const float3 nh = disk.x * t1 + disk.y * t2 + z * wh;
        float3 h;
        if (!rypath_conductor::unit(float3(p.alpha * nh.xy, max(0.0f, nh.z)), h)) {
            s.valid = false;
            return s;
        }
        s.wi = -wo + 2 * dot(wo, h) * h;
        if (!all(isfinite(s.wi))) {
            s.valid = false;
            return s;
        }
        if (s.wi.z <= 0)
            return s;
    } else {
        const float r = sqrt(u.x), phi = 2 * M_PI_F * u.y;
        s.wi = float3(r * cos(phi), r * sin(phi), sqrt(1 - u.x));
    }
    const auto e = evaluate(p, wo, s.wi);
    s.pdf = e.pdf;
    s.weight = e.pdf > 0 ? e.f * s.wi.z / e.pdf : 0;
    s.valid = e.valid && e.pdf > 0 && isfinite(s.weight) && s.weight >= 0;
    s.accepted = s.valid && s.weight > 0;
    return s;
}
} // namespace rypath_gltf
