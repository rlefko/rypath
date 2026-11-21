// SPDX-License-Identifier: Apache-2.0
// Layered random walks and MIS adapted from PBRT v4 b0bdf461.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt. Scalar spectral lane; radiance transport.
#pragma once
#include "../src/layered_rng_shared.h"
#include "transmission.h"
#ifndef RYPATH_COATING_DIAGNOSTIC_PDF_FIXED
#define RYPATH_COATING_DIAGNOSTIC_PDF_FIXED 0
#endif
#ifndef RYPATH_COATING_DIAGNOSTIC_EVAL_EVENT
#define RYPATH_COATING_DIAGNOSTIC_EVAL_EVENT 0
#endif
namespace rypath_layered {
using Evaluation = rypath_conductor::Evaluation;
using Sample = rypath_conductor::Sample;
struct Parameters {
    rypath_dielectric::Parameters interface;
    rypath_conductor::Parameters conductor;
    bool conductor_base;
    float reflectance, thickness, albedo, g;
    uint max_depth, samples;
    int material_seed;
};
// No shared state: each PBRT operation hashes its exact ordered float inputs.
struct RNG {
    rypath_layered_rng::PCG pcg;
    RNG(rypath_layered_rng::Wide sequence, rypath_layered_rng::Wide offset)
        : pcg(sequence, offset) {}
    float next() { return pcg.next(); }
    float2 pair() {
        float a = next();
        float b = next();
        return float2(a, b);
    }
};
// Diagnostic builds can isolate the former fixed-PDF and event-local value
// policies. Production defaults to PBRT domains; the sampled joint walk always
// uses PBRT PCG, so the two-by-two experiment changes only f/PDF estimates.
#if RYPATH_COATING_DIAGNOSTIC_PDF_FIXED || RYPATH_COATING_DIAGNOSTIC_EVAL_EVENT
struct DiagnosticLegacyRNG {
    uint state;
    float next() {
        state += 0x9e3779b9u;
        uint v = state;
        v = (v ^ (v >> 16)) * 0x21f0aaadu;
        v = (v ^ (v >> 15)) * 0x735a2d97u;
        v ^= v >> 15;
        return float(v >> 8) * 0x1p-24f;
    }
    float2 pair() {
        float a = next(), b = next();
        return float2(a, b);
    }
};
#endif
inline rypath_layered_rng::Wide direction_hash(float3 w) {
    return rypath_layered_rng::direction(w.x, w.y, w.z);
}
inline rypath_layered_rng::Wide seeded_direction_hash(int seed, float3 w) {
    return rypath_layered_rng::seeded_direction(seed, w.x, w.y, w.z);
}
inline bool valid_parameters(const thread Parameters &p) {
    return isfinite(p.reflectance) && p.reflectance >= 0 && p.reflectance <= 1 &&
           isfinite(p.thickness) && isfinite(p.albedo) && p.albedo >= 0 && p.albedo <= 1 &&
           isfinite(p.g) && abs(p.g) <= 1 && p.samples > 0 && isfinite(p.interface.eta) &&
           p.interface.eta > 0;
}
inline float thickness(const thread Parameters &p) { return max(p.thickness, 0x1p-126f); }
inline float tr(float dz, float3 w) { return abs(dz) <= 0x1p-126f ? 1 : exp(-abs(dz / w.z)); }
inline float power(float a, float b) {
    float m = max(a, b);
    if (m == 0)
        return 0;
    a /= m;
    b /= m;
    return a * a / (a * a + b * b);
}
// HG arguments use the outward direction convention, as in PBRT.
inline float phase(float3 wo, float3 wi, float g) {
    g = clamp(g, -.99f, .99f);
    float c = clamp(dot(wo, wi), -1.0f, 1.0f);
    float d = g < 0 ? (1 + g) * (1 + g) - 2 * g * (1 - c) : (1 - g) * (1 - g) + 2 * g * (1 + c);
    return ((1 - g) * (1 + g)) / (4 * M_PI_F * d * sqrt(d));
}
inline float3 phase_sample(float3 wo, float g, float2 u) {
    g = clamp(g, -.99f, .99f);
    float c;
    if (abs(g) < 1e-3f)
        c = 1 - 2 * u.x;
    else {
        float s = (1 - g * g) / (1 + g - 2 * g * u.x);
        c = -(1 + g * g - s * s) / (2 * g);
    }
    c = clamp(c, -1.0f, 1.0f);
    float3 x;
    rypath_conductor::unit(
        abs(wo.z) < .99999f ? cross(float3(0, 0, 1), wo) : cross(float3(0, 1, 0), wo), x);
    float3 y = cross(wo, x);
    float st = sqrt(max(0.0f, 1 - c * c)), phi = 2 * M_PI_F * u.y;
    return st * cos(phi) * x + st * sin(phi) * y + c * wo;
}
// branch: 0=all, 1=reflection only, 2=transmission only. Conditional
// PDFs are required by the layered NEE estimator; the underlying GGX uses all.
inline bool branch_probability(const thread rypath_dielectric::Parameters &p, float3 wo, float3 wi,
                               thread float &F) {
    float3 m(0, 0, 1);
    if (!p.smooth) {
        bool reflection = (wo.z > 0) == (wi.z > 0);
        float3 h = reflection
                       ? wo + wi
                       : (wo.z > 0 ? fma(float3(p.eta), wi, wo) : fma(float3(p.eta), wo, wi));
        if (!rypath_conductor::unit(h, m))
            return false;
        if (m.z < 0)
            m = -m;
    }
    return rypath_dielectric::fresnel(dot(wo, m), p.eta, F);
}
inline Sample top_sample(const thread Parameters &p, float3 wo, float uc, float2 u, uint branch = 0,
                         bool importance = false) {
    auto s = rypath_dielectric::sample(p.interface, wo,
                                       branch == 1 ? 0.0f : (branch == 2 ? 0x1.fffffep-1f : uc), u);
    if (!s.valid || !s.accepted)
        return s;
    bool reflection = (wo.z > 0) == (s.wi.z > 0);
    if ((branch == 1 && !reflection) || (branch == 2 && reflection))
        return {float3(0), 0, 0, s.delta, false, true};
    if (branch) {
        float F;
        if (!branch_probability(p.interface, wo, s.wi, F))
            return {float3(0), 0, 0, s.delta, false, false};
        float probability = reflection ? F : 1 - F;
        if (!(probability > 0))
            return {float3(0), 0, 0, s.delta, false, true};
        s.weight *= probability;
        s.pdf /= probability;
    }
    if (importance && !reflection) {
        float scale =
            wo.z > 0 ? p.interface.eta * p.interface.eta : 1 / (p.interface.eta * p.interface.eta);
        s.weight *= scale;
    }
    s.valid = isfinite(s.weight) && isfinite(s.pdf) && s.pdf > 0;
    return s;
}
inline Evaluation top_evaluate(const thread Parameters &p, float3 wo, float3 wi, uint branch = 0) {
    auto e = rypath_dielectric::evaluate(p.interface, wo, wi);
    if (!e.valid || e.pdf == 0 || branch == 0)
        return e;
    bool reflection = (wo.z > 0) == (wi.z > 0);
    if ((branch == 1 && !reflection) || (branch == 2 && reflection))
        return {0, 0, true};
    float F;
    if (!branch_probability(p.interface, wo, wi, F))
        return {0, 0, false};
    float probability = reflection ? F : 1 - F;
    if (!(probability > 0))
        return {0, 0, true};
    e.pdf /= probability;
    e.valid = isfinite(e.pdf);
    return e;
}
inline Evaluation base_evaluate(const thread Parameters &p, float3 wo, float3 wi) {
    if (p.conductor_base)
        return rypath_conductor::evaluate(p.conductor, wo, wi);
    if (!all(isfinite(wo)) || !all(isfinite(wi)))
        return {0, 0, false};
    if (wo.z * wi.z <= 0)
        return {0, 0, true};
    // DiffuseBxDF::PDF is cosine density even for a black substrate; unlike
    // DiffuseTransmissionBxDF it does not select lobes by reflectance.
    return {p.reflectance / M_PI_F, abs(wi.z) / M_PI_F, true};
}
inline Sample base_sample(const thread Parameters &p, float3 wo, float2 u) {
#pragma clang fp contract(off)
    if (p.conductor_base)
        return rypath_conductor::sample(p.conductor, wo, u);
    Sample fail{float3(0), 0, 0, false, false, true};
    if (!isfinite(p.reflectance) || p.reflectance < 0 || !all(isfinite(wo)) ||
        !rypath_transmission::randoms(0, u)) {
        fail.valid = false;
        return fail;
    }
    if (p.reflectance == 0 || wo.z == 0)
        return fail;
    // PBRT DiffuseBxDF uses SampleCosineHemisphere's concentric disk map.
    // Retain that map here: equal distributions alone do not preserve the
    // direction-seeded finite f/PDF estimates or the sampled hidden path.
    float2 offset = 2 * u - 1;
    float2 disk(0);
    if (offset.x != 0 || offset.y != 0) {
        float radius, theta;
        if (abs(offset.x) > abs(offset.y)) {
            radius = offset.x;
            theta = (M_PI_F / 4) * (offset.y / offset.x);
        } else {
            radius = offset.y;
            theta = (M_PI_F / 2) - (M_PI_F / 4) * (offset.x / offset.y);
        }
        disk = radius * float2(cos(theta), sin(theta));
    }
    float3 wi(disk, sqrt(max(0.0f, 1 - disk.x * disk.x - disk.y * disk.y)));
    if (wo.z < 0)
        wi.z = -wi.z;
    float pdf = abs(wi.z) / M_PI_F;
    return {wi, p.reflectance, pdf, false, p.reflectance > 0 && pdf > 0, true};
}
inline bool base_delta(const thread Parameters &p) {
    return p.conductor_base && p.conductor.smooth;
}
// PBRT DielectricBxDF::Flags depends on microfacet roughness even when eta=1
// makes Sample_f return a delta transmission. Keep its layered MIS decisions
// separate from the sampled event's delta flag.
inline bool top_specular_flags(const thread Parameters &p) {
    return max(p.interface.alpha_x, p.interface.alpha_y) < 1e-3f;
}
// PBRT's directional PDF is an approximate TRT estimator, mixed with 10%
// uniform sphere density. It is NOT the sampled internal path's joint PDF.
inline Evaluation directional_pdf(const thread Parameters &p, float3 wo, float3 wi) {
    if (!valid_parameters(p) || !all(isfinite(wo)) || !all(isfinite(wi)))
        return {0, 0, false};
    if (wo.z < 0) {
        wo = -wo;
        wi = -wi;
    }
    float sum = 0;
#if RYPATH_COATING_DIAGNOSTIC_PDF_FIXED
    DiagnosticLegacyRNG rng{0xb5297a4du};
#else
    RNG rng{seeded_direction_hash(p.material_seed, wi), direction_hash(wo)};
#endif
    if (wo.z * wi.z > 0) {
        auto direct = top_evaluate(p, wo, wi, 1);
        if (!direct.valid)
            return {0, 0, false};
        sum = float(p.samples) * direct.pdf;
        for (uint n = 0; n < p.samples; ++n) {
            float uc = rng.next();
            auto a = top_sample(p, wo, uc, rng.pair(), 2);
            uc = rng.next();
            auto b = top_sample(p, wi, uc, rng.pair(), 2, true);
            if (!a.valid || !b.valid)
                return {0, 0, false};
            if (!a.accepted || !b.accepted)
                continue;
            if (top_specular_flags(p)) {
                auto e = base_evaluate(p, -a.wi, -b.wi);
                if (!e.valid)
                    return {0, 0, false};
                sum += e.pdf;
            } else {
                // PBRT consumes the interface choice even for a single-lobe base.
                (void)rng.next();
                auto c = base_sample(p, -a.wi, rng.pair());
                if (!c.valid)
                    return {0, 0, false};
                if (!c.accepted)
                    continue;
                auto t = top_evaluate(p, -c.wi, wi);
                if (!t.valid)
                    return {0, 0, false};
                if (base_delta(p))
                    sum += t.pdf;
                else {
                    auto r = base_evaluate(p, -a.wi, -b.wi);
                    if (!r.valid)
                        return {0, 0, false};
                    sum += power(b.pdf, r.pdf) * r.pdf + power(c.pdf, t.pdf) * t.pdf;
                }
            }
        }
    }
    float pdf = .1f / (4 * M_PI_F) + .9f * sum / float(p.samples);
    return {0, pdf, isfinite(pdf) && pdf > 0};
}
inline Sample sample(const thread Parameters &p, float3 wo, float uc, float2 u, uint seed) {
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
    auto s = top_sample(p, wo, uc, u);
    if (!s.valid || !s.accepted)
        return s;
    float3 w = s.wi;
    float weight = s.weight, z = thickness(p);
    bool delta = s.delta;
    (void)seed;
    RNG rng{seeded_direction_hash(p.material_seed, wo), rypath_layered_rng::sample(uc, u.x, u.y)};
    bool exited = w.z > 0;
    for (uint depth = 0; !exited && depth < p.max_depth; ++depth) {
        if (depth > 3 && weight < .25f) {
            float q = max(0.0f, 1 - weight);
            float survival = 1 - q;
            if (rng.next() < q)
                return fail;
            weight /= survival;
        }
        if (w.z == 0)
            return fail;
        if (p.albedo > 0) {
            float dz = -log(1 - rng.next()) * abs(w.z);
            float zp = w.z > 0 ? z + dz : z - dz;
            if (zp == z)
                return fail;
            if (zp > 0 && zp < thickness(p)) {
                w = phase_sample(-w, p.g, rng.pair());
                weight *= p.albedo;
                z = zp;
                delta = false;
                if (!all(isfinite(w)) || !isfinite(weight)) {
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
        auto b = z == 0 ? base_sample(p, -w, uv) : top_sample(p, -w, choice, uv);
        if (!b.valid || !b.accepted)
            return b;
        weight *= b.weight;
        delta = delta && b.delta;
        w = b.wi;
        exited = z != 0 && w.z > 0;
        if (!isfinite(weight) || weight < 0) {
            fail.valid = false;
            return fail;
        }
        if (weight == 0)
            return fail;
    }
    if (!exited)
        return fail;
    auto pdf = directional_pdf(p, wo, w);
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
inline Evaluation evaluate(const thread Parameters &p, float3 wo, float3 wi, uint seed) {
    if (!valid_parameters(p) || !all(isfinite(wo)) || !all(isfinite(wi)))
        return {0, 0, false};
    if (wo.z < 0) {
        wo = -wo;
        wi = -wi;
    }
    auto density = directional_pdf(p, wo, wi);
    if (!density.valid)
        return density;
    // Both supported bases are opaque: only top-to-top reflection can exit.
    if (wo.z == 0 || wi.z <= 0)
        return {0, density.pdf, true};
    auto direct = top_evaluate(p, wo, wi);
    if (!direct.valid)
        return {0, 0, false};
    float total = float(p.samples) * direct.f;
    (void)seed;
#if RYPATH_COATING_DIAGNOSTIC_EVAL_EVENT
    DiagnosticLegacyRNG rng{seed ^ 0x1b56c4e9u};
#else
    RNG rng{seeded_direction_hash(p.material_seed, wo), direction_hash(wi)};
#endif
    for (uint n = 0; n < p.samples; ++n) {
        float uc = rng.next();
        auto a = top_sample(p, wo, uc, rng.pair(), 2);
        if (!a.valid)
            return {0, 0, false};
        if (!a.accepted)
            continue;
        uc = rng.next();
        auto b = top_sample(p, wi, uc, rng.pair(), 2, true);
        if (!b.valid)
            return {0, 0, false};
        if (!b.accepted)
            continue;
        float beta = a.weight, z = thickness(p);
        float3 w = a.wi;
        // b.weight=b.f*abs(b.wi.z)/b.pdf; NEE uses b.f/b.pdf.
        float light = b.weight / abs(b.wi.z);
        for (uint depth = 0; depth < p.max_depth; ++depth) {
            if (depth > 3 && beta < .25f) {
                float q = max(0.0f, 1 - beta);
                float survival = 1 - q;
                if (rng.next() < q)
                    break;
                beta /= survival;
            }
            if (w.z == 0)
                break;
            if (p.albedo == 0) {
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
                    total += beta * p.albedo * ph * wt * tr(zp - thickness(p), b.wi) * light;
                    float3 next = phase_sample(-w, p.g, rng.pair());
                    float phase_pdf = phase(-w, next, p.g);
                    // PBRT rejects a tangent phase proposal before changing path state.
                    if (next.z == 0 || phase_pdf == 0)
                        continue;
                    beta *= p.albedo;
                    w = next;
                    z = zp;
                    if (w.z > 0 && !top_specular_flags(p)) {
                        auto e = top_evaluate(p, -w, wi, 2);
                        auto exit_proposal = top_evaluate(p, wi, -w, 2);
                        if (!e.valid || !exit_proposal.valid)
                            return {0, 0, false};
                        // The virtual-light proposal starts at wi and samples
                        // -w, in the same internal solid angle as the phase proposal.
                        total += beta * tr(z - thickness(p), w) * e.f *
                                 power(phase_pdf, exit_proposal.pdf);
                    }
                    if (!all(isfinite(w)) || !isfinite(beta) || !isfinite(total))
                        return {0, 0, false};
                    continue;
                }
                z = clamp(zp, 0.0f, thickness(p));
            }
            if (z == thickness(p)) {
                uc = rng.next();
                auto s = top_sample(p, -w, uc, rng.pair(), 1);
                if (!s.valid)
                    return {0, 0, false};
                if (!s.accepted)
                    break;
                beta *= s.weight;
                w = s.wi;
            } else {
                if (!base_delta(p)) {
                    auto e = base_evaluate(p, -w, -b.wi);
                    if (!e.valid)
                        return {0, 0, false};
                    float wt = top_specular_flags(p) ? 1 : power(b.pdf, e.pdf);
                    total += beta * e.f * abs(b.wi.z) * wt * tr(thickness(p), b.wi) * light;
                }
                // Keep the unused single-lobe choice in the PBRT RNG schedule.
                (void)rng.next();
                auto s = base_sample(p, -w, rng.pair());
                if (!s.valid)
                    return {0, 0, false};
                if (!s.accepted)
                    break;
                beta *= s.weight;
                w = s.wi;
                if (!top_specular_flags(p)) {
                    auto e = top_evaluate(p, -w, wi, 2);
                    if (!e.valid)
                        return {0, 0, false};
                    float wt = 1;
                    if (!base_delta(p)) {
                        auto exit_proposal = top_evaluate(p, wi, -w, 2);
                        if (!exit_proposal.valid)
                            return {0, 0, false};
                        wt = power(s.pdf, exit_proposal.pdf);
                    }
                    total += beta * tr(thickness(p), w) * e.f * wt;
                }
            }
            if (!isfinite(beta) || !isfinite(total) || beta < 0 || total < 0)
                return {0, 0, false};
        }
    }
    float f = total / float(p.samples);
    return {f, density.pdf, isfinite(f) && f >= 0};
}
} // namespace rypath_layered
