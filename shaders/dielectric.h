// SPDX-License-Identifier: Apache-2.0
// PBRT v4 b0bdf461 dielectric equations, adapted for checked Metal arithmetic.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt. Visible GGX: Heitz, JCGT 7(4), 2018.
#pragma once
#include "conductor.h"
namespace rypath_dielectric {
using Evaluation = rypath_conductor::Evaluation;
using Sample = rypath_conductor::Sample;
struct Parameters {
    float eta, alpha_x, alpha_y;
    bool smooth;
};
inline bool prepare(float eta, float u, float v, bool remap, thread Parameters &p) {
    if (!isfinite(eta) || eta < 0 || !isfinite(u) || !isfinite(v) || u < 0 || v < 0)
        return false;
    p = {eta == 0 ? 1 : eta, remap ? sqrt(u) : u, remap ? sqrt(v) : v, false};
    p.smooth = p.eta == 1 || max(p.alpha_x, p.alpha_y) < 1e-3f;
    if (!p.smooth) {
        p.alpha_x = max(p.alpha_x, 1e-4f);
        p.alpha_y = max(p.alpha_y, 1e-4f);
    }
    return true;
}
inline rypath_conductor::Parameters ggx(const thread Parameters &p) {
    return {p.eta, 0, p.alpha_x, p.alpha_y, p.smooth};
}
// Compensated Snell discriminant preserves both critical-angle and eta~1 limits.
inline bool optics(float cosine, float eta, thread float &F, thread float &etap, thread float &ct,
                   thread bool &tir) {
    if (!isfinite(cosine))
        return false;
    etap = cosine < 0 ? 1 / eta : eta;
    float c = min(abs(cosine), 1.0f);
    if (!(etap > 0) || !isfinite(etap))
        return false;
    if (etap == 1) {
        F = 0;
        ct = c;
        tir = false;
        return true;
    }
    float c2 = c * c, discriminant;
    if (cosine < 0) {
        // Keep the authored index instead of rounding its reciprocal before
        // testing the critical angle on the exiting side.
        float e2 = eta * eta;
        float2 a = rypath_conductor::two_sum(1, -e2);
        discriminant =
            fma(e2, c2, a.x) + (a.y + fma(eta, eta, -e2) * (c2 - 1) + e2 * fma(c, c, -c2));
    } else {
        float e2 = eta * eta;
        float2 a = rypath_conductor::two_sum(e2, -1), b = rypath_conductor::two_sum(a.x, c2);
        discriminant = b.x + (a.y + b.y + fma(eta, eta, -e2) + fma(c, c, -c2));
    }
    if (!isfinite(discriminant))
        return false;
    tir = discriminant <= 0;
    if (tir) {
        F = 1;
        ct = 0;
        return true;
    }
    ct = sqrt(discriminant) / (cosine < 0 ? 1 : eta);
    float rp = (etap * c - ct) / (etap * c + ct);
    float rs = (c - etap * ct) / (c + etap * ct);
    F = .5f * (rp * rp + rs * rs);
    return isfinite(ct) && isfinite(F) && F >= 0 && F <= 1;
}
inline bool fresnel(float cosine, float eta, thread float &F) {
    float ep, ct;
    bool tir;
    return optics(cosine, eta, F, ep, ct, tir);
}
inline Evaluation evaluate(const thread Parameters &p, float3 wo, float3 wi) {
    if (!all(isfinite(wo)) || !all(isfinite(wi)))
        return {0, 0, false};
    if (p.smooth || wo.z == 0 || wi.z == 0)
        return {0, 0, true};
    bool reflection = (wo.z > 0) == (wi.z > 0);
    bool entering = wo.z > 0;
    // Rescale the exiting half-vector by eta to retain the authored index
    // instead of first rounding 1/eta at the nearly matched limit.
    float3 h =
        reflection ? wo + wi : (entering ? fma(float3(p.eta), wi, wo) : fma(float3(p.eta), wo, wi));
    float3 m;
    if (all(h == 0))
        return {0, 0, true};
    if (!rypath_conductor::unit(h, m))
        return {0, 0, false};
    if (m.z < 0)
        m = -m;
    float co = dot(wo, m), ci = dot(wi, m);
    if (co * wo.z < 0 || ci * wi.z < 0)
        return {0, 0, true};
    float F, ct, optical_ep;
    bool tir;
    if (!optics(co, p.eta, F, optical_ep, ct, tir))
        return {0, 0, false};
    auto g = ggx(p);
    float lo = rypath_conductor::lambda(g, wo), li = rypath_conductor::lambda(g, wi);
    float d = rypath_conductor::distribution(g, m), value, pdf;
    if (!isfinite(lo) || !isfinite(li) || !isfinite(d))
        return {0, 0, false};
    if (d == 0)
        return {0, 0, true};
    if (reflection) {
        value = d * F / (4 * abs(wo.z) * abs(wi.z) * (1 + lo + li));
        pdf = d * F / (4 * abs(wo.z) * (1 + lo));
    } else {
        // h is parallel to m; this norm form avoids subtracting the two
        // nearly equal Snell cosines when eta approaches one.
        float den = dot(h, m) / (entering ? p.eta : 1);
        float jacobian = abs(ci) / (den * den);
        pdf = d * abs(co) / (abs(wo.z) * (1 + lo)) * jacobian * (1 - F);
        float scale = entering ? 1 / (p.eta * p.eta) : p.eta * p.eta;
        value = d * (1 - F) * abs(ci * co) * scale /
                (abs(wo.z) * abs(wi.z) * den * den * (1 + lo + li));
    }
    return {value, pdf,
            isfinite(lo) && isfinite(li) && isfinite(d) && isfinite(value) && isfinite(pdf) &&
                value >= 0 && pdf >= 0};
}
// c/etap-ct, expressed with the authored index on both sides. Computing
// 1/eta first can double eta-1 at the adjacent binary32 value below unity.
inline float refraction_coefficient(float eta, float co, float ct) {
    float c = abs(co);
    return co < 0 ? ((eta - 1) * (eta + 1)) / (eta * c + ct)
                  : (((1 - eta) * (1 + eta)) / eta) / (c + eta * ct);
}
inline Sample sample(const thread Parameters &p, float3 wo, float uc, float2 u,
                     thread float &sampled_eta) {
    sampled_eta = 1;
    Sample s = {float3(0), 0, 0, p.smooth, false, true};
    if (!all(isfinite(wo)) || !isfinite(uc) || uc < 0 || uc >= 1 || !all(isfinite(u)) ||
        any(u < 0) || any(u >= 1)) {
        s.valid = false;
        return s;
    }
    if (wo.z == 0)
        return s;
    float3 m(0, 0, 1);
    if (!p.smooth) {
        float3 wh;
        if (!rypath_conductor::unit(float3(p.alpha_x * wo.x, p.alpha_y * wo.y, wo.z), wh)) {
            s.valid = false;
            return s;
        }
        if (wh.z < 0)
            wh = -wh;
        float3 t1;
        if (wh.z < .99999f) {
            if (!rypath_conductor::unit(float3(-wh.y, wh.x, 0), t1)) {
                s.valid = false;
                return s;
            }
        } else
            t1 = float3(1, 0, 0);
        float3 t2 = cross(wh, t1);
        float r = sqrt(u.x), phi = 2 * M_PI_F * u.y;
        float2 disk = r * float2(cos(phi), sin(phi));
        float h = sqrt(max(0.0f, 1 - disk.x * disk.x)), a = (1 + wh.z) * .5f;
        float old_y = disk.y;
        disk.y = (1 - a) * h + a * disk.y;
        float old_minus_h = old_y > 0 ? -(1 - u.x) / (h + old_y) : old_y - h;
        float z = sqrt(max(0.0f, (1 - u.x) + (1 - a) * old_minus_h * (old_y + disk.y)));
        float3 nh = disk.x * t1 + disk.y * t2 + z * wh;
        if (!rypath_conductor::unit(float3(p.alpha_x * nh.x, p.alpha_y * nh.y, max(1e-6f, nh.z)),
                                    m)) {
            s.valid = false;
            return s;
        }
    }
    float co = dot(wo, m), F, ep, ct;
    bool tir;
    if (!optics(co, p.eta, F, ep, ct, tir)) {
        s.valid = false;
        return s;
    }
    bool reflection = uc < F;
    if (reflection)
        s.wi = -wo + 2 * co * m;
    else {
        if (tir) {
            s.valid = false;
            return s;
        }
        // Rationalized refraction coefficient avoids cancellation at eta~1.
        float coefficient = p.eta == 1 ? 0 : refraction_coefficient(p.eta, co, ct);
        s.wi = (co < 0 ? -p.eta * wo : -wo / p.eta) + coefficient * (co < 0 ? -m : m);
    }
    if (!all(isfinite(s.wi))) {
        s.valid = false;
        return s;
    }
    if (s.wi.z == 0 || (((s.wi.z > 0) == (wo.z > 0)) != reflection))
        return s;
    float scale = reflection ? 1 : (co < 0 ? p.eta * p.eta : 1 / (p.eta * p.eta));
    if (p.smooth) {
        s.pdf = reflection ? F : 1 - F;
        s.weight = scale;
    } else {
        auto g = ggx(p);
        float lo = rypath_conductor::lambda(g, wo), li = rypath_conductor::lambda(g, s.wi);
        float d = rypath_conductor::distribution(g, m);
        if (!isfinite(lo) || !isfinite(li) || !isfinite(d)) {
            s.valid = false;
            return s;
        }
        if (d == 0)
            return s;
        if (reflection)
            s.pdf = d * F / (4 * abs(wo.z) * (1 + lo));
        else {
            // |ci+co/ep| = |1-ep²| / (ep² (ct+c/ep)).
            float den = refraction_coefficient(p.eta, co, ct);
            s.pdf = d * abs(co) * ct * (1 - F) / (abs(wo.z) * (1 + lo) * den * den);
        }
        s.weight = scale * (1 + lo) / (1 + lo + li);
        s.valid = isfinite(lo) && isfinite(li) && isfinite(d);
    }
    s.valid = s.valid && isfinite(s.pdf) && s.pdf > 0 && isfinite(s.weight) && s.weight > 0;
    s.accepted = s.valid;
    if (s.accepted && !reflection)
        sampled_eta = ep;
    return s;
}
// Preserve the scalar return ABI and one shared sampling-math implementation.
inline Sample sample(const thread Parameters &p, float3 wo, float uc, float2 u) {
    float sampled_eta;
    return sample(p, wo, uc, u, sampled_eta);
}
} // namespace rypath_dielectric
