// SPDX-License-Identifier: Apache-2.0
// Shared packet walk adapted from layered.h / PBRT v4 b0bdf461.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt.
#pragma once
#include "layered.h"
// Renderer specialization and all-family probes require distinct symbols.
// Separate namespaces prevent weak-ODR coalescing when their AIR shares a metallib.
#if defined(RYPATH_PACKET_RENDER)
namespace rypath_packet_layered_renderer {
#else
namespace rypath_packet_layered {
#endif
using namespace rypath_layered;
struct Parameters {
    rypath_layered::Parameters lane[4];
    uint active_mask;
};
struct Evaluation {
    float4 f;
    float pdf;
    bool valid;
};
struct Sample {
    float3 wi;
    float4 weight;
    float pdf;
    bool delta, accepted, valid;
};
inline float maximum(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }
// This is the directional MIS proxy, never the sampled hidden-path PDF.
// Base geometric proposals do not depend on spectral Fresnel/reflectance;
// nonzero proxy coefficients avoid a hero-black acceptance branch and make
// the density invariant under wavelength permutation.
inline rypath_conductor::Evaluation directional_pdf(const thread Parameters &packet, float3 wo,
                                                    float3 wi) {
    auto proxy = packet.lane[0];
    proxy.reflectance = 1;
    proxy.conductor.eta = 1;
    proxy.conductor.k = 1;
    return rypath_layered::directional_pdf(proxy, wo, wi);
}
inline Sample wrap(rypath_conductor::Sample s) {
    return {s.wi, float4(s.weight), s.pdf, s.delta, s.accepted, s.valid};
}
inline float4 mask(const thread Parameters &p) {
    return float4((p.active_mask & 1) ? 1 : 0, (p.active_mask & 2) ? 1 : 0,
                  (p.active_mask & 4) ? 1 : 0, (p.active_mask & 8) ? 1 : 0);
}
inline float4 albedo(const thread Parameters &p) {
    float4 a(0);
    for (uint i = 0; i < 4; ++i)
        if (p.active_mask & (1u << i))
            a[i] = p.lane[i].albedo;
    return a;
}
inline rypath_conductor::Evaluation top_evaluate_mode(const thread rypath_layered::Parameters &p,
                                                      float3 wo, float3 wi, uint branch = 0,
                                                      bool importance = false) {
    auto e = top_evaluate(p, wo, wi, branch);
    if (importance && wo.z * wi.z < 0)
        e.f *=
            wo.z > 0 ? p.interface.eta * p.interface.eta : 1 / (p.interface.eta * p.interface.eta);
    return e;
}
inline Evaluation base_evaluate(const thread Parameters &p, float3 wo, float3 wi) {
    Evaluation e{float4(0), 0, true};
    for (uint i = 0; i < 4; ++i) {
        auto a = rypath_layered::base_evaluate(p.lane[i], wo, wi);
        if (i == 0)
            e.pdf = a.pdf;
        if (p.active_mask & (1u << i)) {
            e.f[i] = a.f;
            e.valid = e.valid && a.valid;
        }
    }
    return e;
}
inline Sample base_sample(const thread Parameters &p, float3 wo, float2 u) {
    auto proxy = p.lane[0];
    proxy.reflectance = 1;
    proxy.conductor.eta = 1;
    proxy.conductor.k = 1;
    auto s = rypath_layered::base_sample(proxy, wo, u);
    Sample out = wrap(s);
    out.weight = float4(0);
    if (!s.valid || !s.accepted)
        return out;
    for (uint i = 0; i < 4; ++i)
        if (p.active_mask & (1u << i)) {
            if (!proxy.conductor_base)
                out.weight[i] = p.lane[i].reflectance;
            else if (s.delta) {
                float f = 0;
                out.valid =
                    out.valid && rypath_conductor::fresnel(abs(wo.z), p.lane[i].conductor.eta,
                                                           p.lane[i].conductor.k, f);
                out.weight[i] = f;
            } else {
                auto e = rypath_layered::base_evaluate(p.lane[i], wo, s.wi);
                out.valid = out.valid && e.valid;
                out.weight[i] = e.f * abs(s.wi.z) / s.pdf;
            }
        }
    out.accepted = out.valid && maximum(out.weight) > 0;
    return out;
}
[[clang::noinline]] inline Sample sample(const thread Parameters &packet, float3 wo, float uc,
                                         float2 u, uint seed, bool importance = false) {
    const thread auto &p = packet.lane[0];
    Sample fail = {float3(0), 0, 0, false, false, true};
    if (!valid_parameters(p) || !all(isfinite(wo)) || !rypath_transmission::randoms(uc, u)) {
        fail.valid = false;
        return fail;
    }
    if (wo.z == 0)
        return fail;
    bool flip = wo.z < 0;
    if (flip)
        wo = -wo;
    auto s = top_sample(p, wo, uc, u, 0, importance);
    if (!s.valid || !s.accepted)
        return wrap(s);
    float3 w = s.wi;
    float4 weight = s.weight * mask(packet);
    float z = thickness(p);
    bool delta = s.delta;
    (void)seed;
    RNG rng{seeded_direction_hash(p.material_seed, wo), rypath_layered_rng::sample(uc, u.x, u.y)};
    bool exited = w.z > 0;
    for (uint depth = 0; !exited && depth < p.max_depth; ++depth) {
        if (depth > 3 && maximum(weight) < .25f) {
            float survival = maximum(weight);
            if (rng.next() >= survival)
                return fail;
            weight /= survival;
        }
        if (w.z == 0)
            return fail;
        if (maximum(albedo(packet)) > 0) {
            float dz = -log(1 - rng.next()) * abs(w.z);
            float zp = w.z > 0 ? z + dz : z - dz;
            if (zp == z)
                return fail;
            if (zp > 0 && zp < thickness(p)) {
                w = phase_sample(-w, p.g, rng.pair());
                weight *= albedo(packet);
                z = zp;
                delta = false;
                if (!all(isfinite(w)) || !all(isfinite(weight))) {
                    fail.valid = false;
                    return fail;
                }
                continue;
            }
            z = clamp(zp, 0.0f, thickness(p));
        } else {
            z = z == thickness(p) ? 0 : thickness(p);
            weight *= tr(thickness(p), w);
        }
        float choice = rng.next();
        float2 uv = rng.pair();
        auto b = z == 0 ? base_sample(packet, -w, uv)
                        : wrap(top_sample(p, -w, choice, uv, 0, importance));
        if (!b.valid || !b.accepted)
            return b;
        weight *= b.weight;
        delta = delta && b.delta;
        w = b.wi;
        exited = z != 0 && w.z > 0;
        if (!all(isfinite(weight)) || any(weight < 0)) {
            fail.valid = false;
            return fail;
        }
        if (maximum(weight) == 0)
            return fail;
    }
    if (!exited)
        return fail;
    auto pdf = directional_pdf(packet, wo, w);
    if (!pdf.valid) {
        fail.valid = false;
        return fail;
    }
    // Throughput is the product of f*cos/pdf for the sampled JOINT path;
    // replacing this by evaluate().f*cos/directional_pdf would bias transport.
    return {flip ? -w : w, weight, pdf.pdf, delta, true, true};
}
// Finite-depth PBRT layered f estimator. The medium has unit extinction;
// thickness therefore measures optical depth. albedo=0 uses exact Beer attenuation.
[[clang::noinline]] inline Evaluation evaluate(const thread Parameters &packet, float3 wo,
                                               float3 wi, uint seed, bool importance = false) {
    const thread auto &p = packet.lane[0];
    if (!valid_parameters(p) || !all(isfinite(wo)) || !all(isfinite(wi)))
        return {0, 0, false};
    if (wo.z < 0) {
        wo = -wo;
        wi = -wi;
    }
    auto density = directional_pdf(packet, wo, wi);
    if (!density.valid)
        return {float4(0), density.pdf, density.valid};
    // Both supported bases are opaque: only top-to-top reflection can exit.
    if (wo.z == 0 || wi.z <= 0)
        return {0, density.pdf, true};
    auto direct = top_evaluate_mode(p, wo, wi, 0, importance);
    if (!direct.valid)
        return {0, 0, false};
    float4 total = float(p.samples) * direct.f * mask(packet);
    (void)seed;
#if RYPATH_COATING_DIAGNOSTIC_EVAL_EVENT
    DiagnosticLegacyRNG rng{seed ^ 0x1b56c4e9u};
#else
    RNG rng{seeded_direction_hash(p.material_seed, wo), direction_hash(wi)};
#endif
    for (uint n = 0; n < p.samples; ++n) {
        float uc = rng.next();
        auto a = top_sample(p, wo, uc, rng.pair(), 2, importance);
        uc = rng.next();
        auto b = top_sample(p, wi, uc, rng.pair(), 2, !importance);
        if (!a.valid || !b.valid)
            return {0, 0, false};
        if (!a.accepted || !b.accepted)
            continue;
        float4 beta = a.weight * mask(packet);
        float z = thickness(p);
        float3 w = a.wi;
        // b.weight=b.f*abs(b.wi.z)/b.pdf; NEE uses b.f/b.pdf.
        float4 light = b.weight / abs(b.wi.z);
        for (uint depth = 0; depth < p.max_depth; ++depth) {
            if (depth > 3 && maximum(beta) < .25f) {
                float survival = maximum(beta);
                if (rng.next() >= survival)
                    break;
                beta /= survival;
            }
            if (w.z == 0)
                break;
            if (maximum(albedo(packet)) == 0) {
                z = z == thickness(p) ? 0 : thickness(p);
                beta *= tr(thickness(p), w);
            } else {
                float dz = -log(1 - rng.next()) * abs(w.z);
                float zp = w.z > 0 ? z + dz : z - dz;
                if (zp == z)
                    continue; // pinned f estimator's null-length step
                if (zp > 0 && zp < thickness(p)) {
                    float ph = phase(-w, -b.wi, p.g);
                    float wt = top_specular_flags(p) ? 1 : power(b.pdf, ph);
                    total += beta * albedo(packet) * ph * wt * tr(zp - thickness(p), b.wi) * light;
                    float3 next = phase_sample(-w, p.g, rng.pair());
                    float phase_pdf = phase(-w, next, p.g);
                    beta *= albedo(packet);
                    w = next;
                    z = zp;
                    if (w.z > 0 && !top_specular_flags(p)) {
                        auto e = top_evaluate_mode(p, -w, wi, 2, importance);
                        if (!e.valid)
                            return {0, 0, false};
                        total += beta * tr(z - thickness(p), w) * e.f * power(phase_pdf, e.pdf);
                    }
                    if (!all(isfinite(w)) || !all(isfinite(beta)) || !all(isfinite(total)))
                        return {0, 0, false};
                    continue;
                }
                z = clamp(zp, 0.0f, thickness(p));
            }
            if (z == thickness(p)) {
                uc = rng.next();
                auto s = top_sample(p, -w, uc, rng.pair(), 1, importance);
                if (!s.valid)
                    return {0, 0, false};
                if (!s.accepted)
                    break;
                beta *= s.weight;
                w = s.wi;
            } else {
                if (!base_delta(p)) {
                    auto e = base_evaluate(packet, -w, -b.wi);
                    if (!e.valid)
                        return {0, 0, false};
                    float wt = top_specular_flags(p) ? 1 : power(b.pdf, e.pdf);
                    total += beta * e.f * abs(b.wi.z) * wt * tr(thickness(p), b.wi) * light;
                }
                auto s = base_sample(packet, -w, rng.pair());
                if (!s.valid)
                    return {0, 0, false};
                if (!s.accepted)
                    break;
                beta *= s.weight;
                w = s.wi;
                if (!top_specular_flags(p)) {
                    auto e = top_evaluate_mode(p, -w, wi, 2, importance);
                    if (!e.valid)
                        return {0, 0, false};
                    float wt = base_delta(p) ? 1 : power(s.pdf, e.pdf);
                    total += beta * tr(thickness(p), w) * e.f * wt;
                }
            }
            if (!all(isfinite(beta)) || !all(isfinite(total)) || any(beta < 0) || any(total < 0))
                return {0, 0, false};
        }
    }
    float4 f = total / float(p.samples);
    return {f, density.pdf, all(isfinite(f)) && all(f >= 0)};
}
} // namespace packet implementation
#if defined(RYPATH_PACKET_RENDER)
namespace rypath_packet_layered = rypath_packet_layered_renderer;
#endif
