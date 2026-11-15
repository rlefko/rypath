// SPDX-License-Identifier: Apache-2.0
// Adapted and modified for Metal and checked arithmetic from PBRT v4 b0bdf461.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt. VNDF: Heitz, JCGT 7(4), 2018.
#pragma once
#include <metal_stdlib>
using namespace metal;
namespace rypath_conductor {
struct Parameters {
    float eta, k, alpha_x, alpha_y;
    bool smooth;
};
struct Evaluation {
    float f, pdf;
    bool valid;
};
struct Sample {
    float3 wi;
    float weight, pdf;
    bool delta, accepted, valid;
};
inline bool prepare(float eta, float k, float u, float v, bool remap, thread Parameters &p) {
    if (!isfinite(eta) || !isfinite(k) || !isfinite(u) || !isfinite(v) || eta < 0 || k < 0 ||
        u < 0 || v < 0 || (eta == 0 && k == 0))
        return false;
    p = {eta, k, remap ? sqrt(u) : u, remap ? sqrt(v) : v, false};
    p.smooth = max(p.alpha_x, p.alpha_y) < 1e-3f;
    if (!p.smooth) {
        p.alpha_x = max(p.alpha_x, 1e-4f);
        p.alpha_y = max(p.alpha_y, 1e-4f);
    }
    return true;
}
inline bool unit(float3 v, thread float3 &out) {
    float s = max(max(abs(v.x), abs(v.y)), abs(v.z));
    if (!(s > 0) || !isfinite(s))
        return false;
    v /= s;
    out = v / sqrt(dot(v, v));
    return all(isfinite(out));
}
inline bool complex_ratio_norm(float2 a, float2 b, thread float &out) {
    float s = max(abs(b.x), abs(b.y));
    if (!(s > 0) || !isfinite(s) || !all(isfinite(a)))
        return false;
    a /= s;
    b /= s;
    out = dot(a, a) / dot(b, b);
    return isfinite(out);
}
// Error-free addition of finite operands (shader compiled without fast-math).
inline float2 two_sum(float a, float b) {
    float sum = a + b;
    float b_virtual = sum - a;
    return float2(sum, (a - (sum - b_virtual)) + (b - b_virtual));
}
inline bool fresnel(float cosine, float eta, float k, thread float &f) {
    if (!isfinite(cosine) || !isfinite(eta) || !isfinite(k) || eta < 0 || k < 0 ||
        (eta == 0 && k == 0))
        return false;
    float c = clamp(cosine, 0.0f, 1.0f);
    if (eta == 1 && k == 0) {
        f = 0;
        return true;
    }
    if (c == 0) {
        f = 1;
        return true;
    }
    // sqrt(eta_complex^2 - sin(theta)^2); equivalent complex Snell equations.
    float2 n2(eta * eta - k * k, 2 * eta * k);
    if (!all(isfinite(n2)))
        return false;
    // Compensate all three squares and their sum. Factoring eta^2-1 alone
    // loses small eta at normal incidence; subtracting sin^2 alone loses c^2
    // near the vacuum grazing limit. Keep both residuals without picking a regime.
    float eta2 = eta * eta, k2 = k * k, c2 = c * c;
    float2 ab = two_sum(eta2, -k2), cd = two_sum(c2, -1);
    float2 total = two_sum(ab.x, cd.x);
    float correction =
        ab.y + cd.y + total.y + fma(eta, eta, -eta2) - fma(k, k, -k2) + fma(c, c, -c2);
    float2 z(total.x + correction, n2.y);
    float scale = max(abs(z.x), abs(z.y));
    float radius = scale == 0 ? 0 : scale * length(z / scale);
    if (!isfinite(radius))
        return false;
    float qr, qi;
    if (z.x >= 0) {
        qr = sqrt(max(0.0f, 0.5f * radius + 0.5f * z.x));
        qi = qr > 0 ? z.y / (2 * qr) : 0;
    } else {
        qi = sqrt(max(0.0f, 0.5f * radius - 0.5f * z.x));
        qr = qi > 0 ? z.y / (2 * qi) : 0;
    }
    float2 q(qr, qi);
    float rs, rp;
    if (!complex_ratio_norm(float2(c, 0) - q, float2(c, 0) + q, rs) ||
        !complex_ratio_norm(n2 * c - q, n2 * c + q, rp))
        return false;
    f = (rs + rp) * 0.5f;
    return isfinite(f) && f >= 0;
}
inline float lambda(const thread Parameters &p, float3 w) {
    float3 v(1, p.alpha_x * w.x / w.z, p.alpha_y * w.y / w.z);
    float s = max(max(abs(v.x), abs(v.y)), abs(v.z));
    return 0.5f * (s * length(v / s) - 1);
}
inline float distribution(const thread Parameters &p, float3 m) {
    // PBRT's explicit grazing cutoff is part of the pinned model.
    if (m.z * m.z * m.z * m.z < 1e-16f)
        return 0;
    float3 v(m.x / p.alpha_x, m.y / p.alpha_y, m.z);
    float t = dot(v, v);
    float denominator = M_PI_F * p.alpha_x * p.alpha_y * t * t;
    if (!isfinite(denominator) || !(denominator > 0))
        return NAN;
    float d = 1 / denominator;
    return d > 0 ? d : NAN;
}
inline Evaluation evaluate(const thread Parameters &p, float3 wo, float3 wi) {
    if (!all(isfinite(wo)) || !all(isfinite(wi)))
        return {0, 0, false};
    if (p.smooth || wo.z == 0 || wi.z == 0 || ((wo.z > 0) != (wi.z > 0)))
        return {0, 0, true};
    float3 m;
    if (!unit(wo + wi, m))
        return {0, 0, false};
    float f;
    if (!fresnel(abs(dot(wo, m)), p.eta, p.k, f))
        return {0, 0, false};
    float lo = lambda(p, wo), li = lambda(p, wi), d = distribution(p, m);
    float value = d * f / (4 * abs(wo.z) * abs(wi.z) * (1 + lo + li));
    float pdf = d / (4 * abs(wo.z) * (1 + lo));
    bool valid = isfinite(lo) && isfinite(li) && isfinite(d) && isfinite(value) && isfinite(pdf) &&
                 value >= 0 && pdf >= 0;
    return {value, pdf, valid};
}
inline Sample sample(const thread Parameters &p, float3 wo, float2 u) {
    Sample s = {float3(0), 0, 0, p.smooth, false, true};
    if (!all(isfinite(wo)) || !all(isfinite(u)) || any(u < 0) || any(u >= 1)) {
        s.valid = false;
        return s;
    }
    if (wo.z == 0)
        return s;
    if (p.smooth) {
        s.wi = float3(-wo.x, -wo.y, wo.z);
        s.pdf = 1;
        s.valid = fresnel(abs(wo.z), p.eta, p.k, s.weight);
        s.accepted = s.valid && s.weight > 0;
        return s;
    }
    float3 wh;
    if (!unit(float3(p.alpha_x * wo.x, p.alpha_y * wo.y, wo.z), wh)) {
        s.valid = false;
        return s;
    }
    if (wh.z < 0)
        wh = -wh;
    float3 t1;
    if (wh.z < 0.99999f) {
        if (!unit(float3(-wh.y, wh.x, 0), t1)) {
            s.valid = false;
            return s;
        }
    } else
        t1 = float3(1, 0, 0);
    float3 t2 = cross(wh, t1);
    float r = sqrt(u.x), phi = 2 * M_PI_F * u.y;
    float2 disk = r * float2(cos(phi), sin(phi));
    float h = sqrt(max(0.0f, 1 - disk.x * disk.x));
    float a = (1 + wh.z) * 0.5f;
    float old_y = disk.y;
    disk.y = (1 - a) * h + a * disk.y;
    // Retain 1-u at the outer disk endpoint even when sqrt(u) rounds to one.
    float old_minus_h = old_y > 0 ? -(1 - u.x) / (h + old_y) : old_y - h;
    float z = sqrt(max(0.0f, (1 - u.x) + (1 - a) * old_minus_h * (old_y + disk.y)));
    float3 nh = disk.x * t1 + disk.y * t2 + z * wh, m;
    if (!unit(float3(p.alpha_x * nh.x, p.alpha_y * nh.y, max(1e-6f, nh.z)), m)) {
        s.valid = false;
        return s;
    }
    s.wi = -wo + 2 * dot(wo, m) * m;
    if (!all(isfinite(s.wi))) {
        s.valid = false;
        return s;
    }
    if (s.wi.z == 0 || ((wo.z > 0) != (s.wi.z > 0)))
        return s;
    float f;
    s.valid = fresnel(abs(dot(wo, m)), p.eta, p.k, f);
    if (!s.valid)
        return s;
    float lo = lambda(p, wo), li = lambda(p, s.wi);
    float d = distribution(p, m);
    // PBRT deliberately truncates the grazing D lobe; this is a model null,
    // distinct from a nonfinite density or a positive density lost to arithmetic.
    if (d == 0)
        return s;
    s.pdf = d / (4 * abs(wo.z) * (1 + lo));
    // Algebraic f*cos/pdf ratio avoids cancellation and 0/0 at a tiny density.
    s.weight = f * (1 + lo) / (1 + lo + li);
    s.valid = isfinite(lo) && isfinite(li) && isfinite(s.pdf) && isfinite(s.weight) &&
              s.weight >= 0 && s.pdf > 0;
    s.accepted = s.valid && s.weight > 0;
    return s;
}
} // namespace rypath_conductor
