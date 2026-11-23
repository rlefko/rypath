// SPDX-License-Identifier: GPL-3.0-only
#include "../src/packet_spectrum_shared.h"
#include "layered_rng_oracles.h"
#include "packet_scattering.h"
// The same shared RR helper used by random_walk, plus actual sampled-eta producers.
kernel void packet_outer_roulette_probe(device float4 *output [[buffer(0)]],
                                        const device uint *table [[buffer(1)]],
                                        uint tid [[thread_position_in_grid]]) {
    if (tid >= 48)
        return;
    if (tid < 16) {
        rypath_packet_roulette::Vector beta(.25f, .125f, 0, 0);
        rypath_packet_roulette::EtaScale state;
        uint depth = 2;
        float u = .75f;
        if (tid == 0)
            u = 0;
        if (tid == 2) {
            depth = 1;
            u = 0;
        }
        if (tid == 3)
            beta = float4(0x1p-26f, 0, 0, 0);
        if (tid == 4)
            rypath_packet_roulette::account_transmission(state, INFINITY, true);
        if (tid == 5) {
            rypath_packet_roulette::account_transmission(state, 2, true);
            beta = .25f;
        }
        if (tid == 6) {
            rypath_packet_roulette::account_transmission(state, 2, true);
            rypath_packet_roulette::account_transmission(state, .5f, true);
        }
        if (tid == 7)
            rypath_packet_roulette::account_transmission(state, 0x1p65f, true);
        if (tid == 8)
            u = 0x1.fffffep-1f;
        if (tid == 9)
            beta = float4(0, .25f, 0, .125f);
        if (tid == 10)
            beta = float4(1, .5f, 0, 0);
        if (tid == 11)
            beta = 0;
        if (tid == 12)
            rypath_packet_roulette::account_transmission(state, NAN, false);
        if (tid == 13) {
            beta = float4(2, 1, 0, 0);
            state.value = FLT_MAX;
        }
        if (tid == 14) {
            beta = float4(0x1p-126f, 0, 0, 0);
            state.value = 0x1p-10f;
        }
        if (tid == 15) {
            state.value = 0x1p120f;
            rypath_packet_roulette::account_transmission(state, 16, true);
        }
        const auto decision = rypath_packet_roulette::prepare(beta, state, depth);
        const bool alive = rypath_packet_roulette::continue_path(beta, state, depth, u);
        output[tid * 4] = beta;
        output[tid * 4 + 1] =
            float4(decision.termination, decision.eligible, state.value, state.enabled);
        output[tid * 4 + 2] = float4(alive, depth, 0, 0);
        output[tid * 4 + 3] = float4(rypath_packet_roulette::opposite_hemispheres(1e-20f, -1e-20f),
                                     rypath_packet_roulette::opposite_hemispheres(-1e-20f, 1e-20f),
                                     rypath_packet_roulette::opposite_hemispheres(1e-20f, 1e-20f),
                                     rypath_packet_roulette::opposite_hemispheres(-0.f, 1.f));
        return;
    }
    const uint which = (tid - 16) % 16;
    const auto mode = tid >= 32 ? rypath_packet::TransportMode::Importance
                                : rypath_packet::TransportMode::Radiance;
    rypath_packet::Prepared p{};
    p.active_mask = which == 13 ? 1u : 15u;
    float3 wo(0, 0, 1);
    float uc = .99f;
    float2 u(.125f, .375f);
    if (which == 1 || which == 5 || which == 15)
        wo.z = -1;
    if (which == 2)
        uc = 0;
    if (which == 3)
        wo = float3(sqrt(.75f), 0, -.5f);
    if (which == 6) {
        wo = float3(1, 0, -1e-37f);
        u = float2(.9f, .25f);
    }
    if (which == 10)
        uc = 0;
    if (which == 12)
        u.y = .25f;
    if (which == 14)
        u.y = .75f;
    if (which == 15)
        u.y = .25f;
    for (uint lane = 0; lane < 4; ++lane) {
        thread auto &a = p.lane[lane];
        a.kind = 2;
        rypath_dielectric::prepare(
            which == 6 ? .4f : 1.5f, which == 6 ? 10.f : (which == 4 || which == 5 ? .2f : 0.f),
            which == 6 ? 10.f : (which == 4 || which == 5 ? .3f : 0.f), false, a.dielectric);
        if (which == 7)
            a.kind = 3;
        if (which == 8) {
            a.kind = 4;
            a.reflectance = 0;
            a.transmittance = .5f;
        }
        if (which == 9) {
            a.kind = 11;
            rypath_gltf::prepare(.5f, 0, 0, a.gltf);
        }
        if (which == 10) {
            a.kind = 5;
            thread auto &l = a.layered;
            l.interface = a.dielectric;
            l.reflectance = .5f;
            l.thickness = .1f;
            l.albedo = 0;
            l.g = 0;
            l.max_depth = 10;
            l.samples = 1;
        }
        if (which == 11) {
            a.kind = 1;
            rypath_conductor::prepare(1.5f, 2, 0, 0, false, a.conductor);
        }
        if (which == 12 || which == 14 || which == 15) {
            a.kind = 8;
            a.table = table;
            a.wavelength = 400 + 100 * lane;
        }
        if (which == 13 && lane > 0)
            a.dielectric.eta = 2;
    }
    float exact_eta = 1;
    bool unchanged_scalar = true;
    if (which <= 6 || which == 13) {
        const auto a = rypath_dielectric::sample(p.lane[0].dielectric, wo, uc, u, exact_eta);
        const auto b = rypath_dielectric::sample(p.lane[0].dielectric, wo, uc, u);
        unchanged_scalar = all(as_type<uint3>(a.wi) == as_type<uint3>(b.wi)) &&
                           as_type<uint>(a.pdf) == as_type<uint>(b.pdf) &&
                           as_type<uint>(a.weight) == as_type<uint>(b.weight) &&
                           a.delta == b.delta && a.accepted == b.accepted && a.valid == b.valid;
    }
    const auto sampled = rypath_packet::sample(p, wo, uc, u, 17, mode);
    output[tid * 4] = float4(sampled.eta, exact_eta, sampled.valid, sampled.accepted);
    output[tid * 4 + 1] =
        float4(sampled.transmission, sampled.delta, sampled.weight.x, unchanged_scalar);
    output[tid * 4 + 2] = float4(wo.z, sampled.wi.z, p.active_mask,
                                 sampled.weight.y + sampled.weight.z + sampled.weight.w);
    output[tid * 4 + 3] = 0;
}
// Fixed analytic boundary queries through the actual packet kind0 sampler.
kernel void packet_diffuse_map_probe(const device float4 *queries [[buffer(0)]],
                                     device float4 *output [[buffer(1)]],
                                     constant uint &count [[buffer(2)]],
                                     uint index [[thread_position_in_grid]]) {
    if (index >= count * 8)
        return;
    const float4 query = queries[index % count];
    const uint variant = index / count;
    const float3 wo(0, 0, (variant & 1u) ? -1.0f : 1.0f);
    const auto mode = (variant & 2u) ? rypath_packet::TransportMode::Importance
                                     : rypath_packet::TransportMode::Radiance;
    rypath_packet::Prepared p{};
    p.active_mask = (variant & 4u) ? 10u : 15u;
    for (uint lane = 0; lane < 4; ++lane) {
        p.lane[lane].kind = 0;
        p.lane[lane].reflectance = float(lane + 1) * .125f;
    }
    const auto s = rypath_packet::sample(p, wo, query.z, query.xy, 17, mode);
    rypath_packet::Evaluation e{};
    if (s.valid && s.accepted)
        e = rypath_packet::evaluate(p, wo, s.wi, 17, mode);
    output[index * 6] = float4(s.wi, s.pdf);
    output[index * 6 + 1] = s.weight;
    output[index * 6 + 2] = float4(s.reverse_pdf, s.valid, s.accepted, s.transmission);
    output[index * 6 + 3] = float4(e.pdf, e.reverse_pdf, e.valid, s.delta);
    // Center variates still use the old polar map on the untouched routes.
    rypath_scattering::Prepared scalar{};
    scalar.kind = 0;
    scalar.reflectance = 1;
    const auto old_scalar =
        rypath_scattering::sample(scalar, float3(0, 0, 1), .25f, float2(.5f), 17);
    scalar.kind = 4;
    const auto old_transmission =
        rypath_packet::scalar_sample(scalar, float3(0, 0, 1), .25f, float2(.5f));
    output[index * 6 + 4] = float4(old_scalar.wi, old_scalar.valid && old_scalar.accepted);
    output[index * 6 + 5] =
        float4(old_transmission.wi, old_transmission.valid && old_transmission.accepted);
}
kernel void packet_scattering_probe(device float4 *out [[buffer(0)]],
                                    const device uint *table [[buffer(1)]],
                                    uint tid [[thread_position_in_grid]]) {
    if (tid >= 1024)
        return;
    rypath_packet::Prepared p{};
    p.active_mask = 15;
    float3 wo = normalize(float3(.2, .1, 1));
    float uc = (float(tid) + .5f) / 1024;
    float2 u(fract(uc * 17), fract(uc * 37));
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 4;
        p.lane[i].reflectance = float(i) * .1f;
        p.lane[i].transmittance = float(i) * .2f;
    }
    auto a = rypath_packet::sample(p, wo, uc, u, tid);
    float4 expected;
    bool reflect = wo.z * a.wi.z > 0;
    for (uint i = 0; i < 4; ++i)
        expected[i] = float(i) * (reflect ? .3f : .3f);
    float error = length(a.weight - expected);
    auto e = rypath_packet::evaluate(p, wo, a.wi, tid);
    error += abs(e.pdf - a.pdf) +
             abs(e.reverse_pdf -
                 rypath_packet::pdf(p, a.wi, wo, rypath_packet::TransportMode::Importance));
    out[tid * 14] = float4(error, a.valid, a.accepted, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 2;
        rypath_dielectric::prepare(1.5, .2, .3, false, p.lane[i].dielectric);
    }
    auto r = rypath_packet::sample(p, wo, .9f, u, tid);
    auto b = rypath_packet::sample(p, wo, .9f, u, tid, rypath_packet::TransportMode::Importance);
    out[tid * 14 + 1] =
        float4(length(b.weight - r.weight * (r.transmission ? 2.25f : 1)), r.valid, b.valid, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 5;
        thread auto &l = p.lane[i].layered;
        rypath_dielectric::prepare(1, 0, 0, false, l.interface);
        l.conductor_base = false;
        l.reflectance = float(i) * .2f;
        l.thickness = .1f;
        l.albedo = 0;
        l.g = 0;
        l.max_depth = 8;
        l.samples = 1;
    }
    auto l = rypath_packet::sample(p, wo, .9f, u, tid);
    float4 analytic = float4(0, .2, .4, .6) * exp(-.1f * (1 / abs(wo.z) + 1 / abs(l.wi.z)));
    out[tid * 14 + 2] =
        float4(length(l.weight - analytic), l.valid, l.accepted, l.pdf_is_proportional);
    p.active_mask = 2;
    auto m = rypath_packet::sample(p, wo, .9f, u, tid);
    out[tid * 14 + 3] =
        float4(abs(m.weight.x) + abs(m.weight.z) + abs(m.weight.w), m.valid, m.accepted, 0);
    p.active_mask = 15;
    float3 wi = normalize(float3(-.3, .2, 1));
    auto eval = rypath_packet::evaluate(p, wo, wi, tid);
    float4 ef = float4(0, .2, .4, .6) / M_PI_F * exp(-.1f * (1 / wo.z + 1 / wi.z));
    out[tid * 14 + 4] = float4(length(eval.f - ef), eval.valid, 1, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 11;
        rypath_gltf::prepare(i == 0 ? 0 : .5f, i == 0 ? 1 : 0, 0, p.lane[i].gltf);
    }
    auto gltf = rypath_packet::sample(p, wo, .8f, u, tid);
    auto ge = rypath_packet::evaluate(p, wo, gltf.wi, tid);
    out[tid * 14 + 5] = float4(length(gltf.weight - ge.f * (abs(gltf.wi.z) / gltf.pdf)), gltf.valid,
                               gltf.accepted, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 3;
        p.lane[i].dielectric.eta = i == 0 ? 1 : 1.5f;
    }
    auto thin = rypath_packet::sample(p, wo, .01f, u, tid);
    out[tid * 14 + 6] = float4(thin.weight.x, thin.valid, thin.accepted, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 5;
        thread auto &l = p.lane[i].layered;
        rypath_dielectric::prepare(1.5, .2, .2, false, l.interface);
        l.reflectance = .8f;
        l.thickness = .2f;
        l.albedo = .7f;
        l.g = .1f;
        l.max_depth = 16;
    }
    auto vector_walk = rypath_packet::sample(p, wo, uc, u, tid);
    auto scalar_walk = rypath_layered::sample(p.lane[0].layered, wo, uc, u, tid);
    float walk_error = vector_walk.accepted == scalar_walk.accepted ? 0 : 1;
    if (vector_walk.accepted && scalar_walk.accepted)
        walk_error += length(vector_walk.weight - float4(scalar_walk.weight));
    out[tid * 14 + 7] = float4(walk_error, vector_walk.valid, scalar_walk.valid, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 8;
        p.lane[i].table = table;
        p.lane[i].wavelength = 400 + 100 * i;
    }
    auto fourier = rypath_packet::sample(p, wo, uc, u, tid);
    auto fourier_importance =
        rypath_packet::sample(p, wo, uc, u, tid, rypath_packet::TransportMode::Importance);
    float fs = fourier.transmission ? 2.25f : 1;
    float ferr = length(fourier_importance.weight - fourier.weight * fs);
    ferr += abs(fourier.pdf - 1 / (4 * M_PI_F)) + abs(fourier.reverse_pdf - 1 / (4 * M_PI_F));
    if (fourier.transmission)
        ferr += abs(fourier.eta - 1.5f);
    out[tid * 14 + 8] = float4(ferr, fourier.valid, fourier.accepted, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 1;
        rypath_conductor::prepare(i == 0 ? 1 : .3f, i == 0 ? 0 : 3, 0, 0, false,
                                  p.lane[i].conductor);
    }
    auto metal = rypath_packet::sample(p, wo, uc, u, tid);
    float F = 0;
    rypath_conductor::fresnel(abs(wo.z), .3f, 3, F);
    float merr = abs(metal.weight.x) + abs(metal.weight.y - F) + abs(metal.weight.z - F) +
                 abs(metal.weight.w - F);
    out[tid * 14 + 9] = float4(merr, metal.valid, metal.accepted, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 5;
        thread auto &layer = p.lane[i].layered;
        layer.reflectance = .8f * float(i) / 3;
        layer.albedo = .7f * float(i) / 3;
    }
    auto spectral_walk = rypath_packet::sample(p, wo, uc, u, tid);
    auto bright_walk = rypath_layered::sample(p.lane[3].layered, wo, uc, u, tid);
    float spectral_error = spectral_walk.accepted == bright_walk.accepted ? 0 : 1;
    if (spectral_walk.accepted && bright_walk.accepted)
        spectral_error += abs(spectral_walk.weight.w - bright_walk.weight) +
                          length(spectral_walk.wi - bright_walk.wi);
    out[tid * 14 + 10] = float4(spectral_error, spectral_walk.valid, bright_walk.valid, 0);
    auto shuffled = p;
    for (uint i = 0; i < 4; ++i)
        shuffled.lane[i] = p.lane[3 - i];
    const float3 probe_wi = normalize(float3(.1f, .2f, 1));
    const float a_pdf = rypath_packet::pdf(p, wo, probe_wi);
    const float b_pdf = rypath_packet::pdf(shuffled, wo, probe_wi);
    const float old_gap =
        abs(rypath_layered::directional_pdf(p.lane[0].layered, wo, probe_wi).pdf -
            rypath_layered::directional_pdf(shuffled.lane[0].layered, wo, probe_wi).pdf);
    out[tid * 14 + 11] = float4(abs(a_pdf - b_pdf), isfinite(a_pdf), isfinite(b_pdf), old_gap);
    const float endpoints[12] = {0,
                                 .25f,
                                 .5f,
                                 .75f,
                                 0x1.fffffep-1f,
                                 .25f - 0x1p-26f,
                                 .25f + 0x1p-25f,
                                 .5f - 0x1p-25f,
                                 .5f + 0x1p-24f,
                                 .75f - 0x1p-24f,
                                 .75f + 0x1p-24f,
                                 0x1.fffffcp-1f};
    auto wavelengths = rypath_packet_spectrum::sample_visible(endpoints[tid % 12]);
    bool bounded = true;
    for (uint i = 0; i < 4; ++i)
        bounded = bounded && wavelengths.lambda[i] >= 360 && wavelengths.lambda[i] <= 830 &&
                  isfinite(wavelengths.pdf[i]) && wavelengths.pdf[i] > 0;
    auto invalid = rypath_packet_spectrum::sample_visible(1.f);
    bool invalid_rejected = !isfinite(invalid.lambda[0]) && !isfinite(invalid.pdf[0]);
    out[tid * 14 + 12] = float4(0, bounded, invalid_rejected, 0);
    // Exact host/MSL agreement with independently obtained PBRT typed hashes.
    const constant auto &oracle = layered_rng_oracles[tid % 4];
    using namespace rypath_layered_rng;
    Wide h[5] = {seeded_direction(oracle.seed, .25f, -.5f, .75f), direction(-.125f, .625f, -0.f),
                 seeded_direction(oracle.seed, -.125f, .625f, -0.f), direction(.25f, -.5f, .75f),
                 sample(.375f, .125f, .875f)};
    bool rng_match = true;
    for (uint i = 0; i < 5; ++i)
        rng_match = rng_match && h[i] == oracle.hashes[i];
    for (uint domain = 0; domain < 3; ++domain) {
        PCG rng(domain == 1 ? h[2] : h[0], domain == 0 ? h[1] : domain == 1 ? h[3] : h[4]);
        for (uint i = 0; i < 4; ++i)
            rng_match = rng_match && bits(rng.next()) == oracle.draws[4 * domain + i];
    }
    out[tid * 14 + 13] = float4(rng_match ? 0 : 1, 1, 1, 0);
}

// Route-specific weighted moments: sampled joint weight, stochastic f, and
// approximate directional PDF are each compared with their own PBRT oracle.
kernel void coating_moments_probe(device float4 *out [[buffer(0)]],
                                  uint tid [[thread_position_in_grid]]) {
    constexpr uint count = 131072;
    if (tid >= 12 * count)
        return;
    const uint route = tid / count, n = tid % count;
    const uint configuration = route / 4;
    const bool conductor = (route % 4) >= 2, importance = (route & 1) != 0;
    rypath_packet::Prepared p{};
    p.active_mask = 15;
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = conductor ? 6 : 5;
        thread auto &l = p.lane[i].layered;
        rypath_dielectric::prepare(1.5f, .2f, .3f, false, l.interface);
        rypath_conductor::prepare(.3f, 3.f, .25f, .15f, false, l.conductor);
        l.conductor_base = conductor;
        l.reflectance = .7f;
        l.thickness = .2f;
        l.albedo = .6f;
        l.g = .1f;
        l.max_depth = configuration == 1 ? 4 : configuration == 2 ? 24 : 12;
        l.samples = configuration == 1 ? 1 : configuration == 2 ? 16 : 4;
        l.material_seed = 0;
    }
    rypath_layered_rng::PCG outer(17, 29);
    outer.advance(5ul * n);
    const float uc = outer.next(), x = outer.next(), y = outer.next();
    const float z = outer.next(), phi = 2 * M_PI_F * outer.next();
    float3 wo =
        configuration == 2 ? normalize(float3(.8f, .3f, .05f)) : normalize(float3(.2f, .1f, 1.f));
    if (configuration == 1)
        wo = -wo;
    const float st = sqrt(max(0.f, 1 - z * z));
    float3 wi(st * cos(phi), st * sin(phi), z);
    if (configuration == 1)
        wi = -wi;
    const auto mode = importance ? rypath_packet::TransportMode::Importance
                                 : rypath_packet::TransportMode::Radiance;
    const auto s = rypath_packet::sample(p, wo, uc, float2(x, y), n, mode);
    const auto e = rypath_packet::evaluate(p, wo, wi, n, mode);
    const float weight = s.accepted ? s.weight.x : 0;
    const float f = e.f.x * abs(wi.z) * (2 * M_PI_F);
    const float env_s = 1 + .25f * s.wi.x + .5f * s.wi.z * s.wi.z;
    const float env_e = 1 + .25f * wi.x + .5f * wi.z * wi.z;
    out[2 * tid] = float4(weight, weight * env_s, f, f * env_e);
    out[2 * tid + 1] = float4(e.pdf * (2 * M_PI_F), s.valid && e.valid, 0, 0);
}

// Raw local inputs bypass frame reconstruction so the hashed PBRT domains and
// per-interface random-number schedule can be tested against fixed queries.
#include "../src/layered_probe_wire.h"
kernel void coating_schedule_probe(const device rypath::layered_probe::Query *queries [[buffer(0)]],
                                   device rypath::layered_probe::Result *results [[buffer(1)]],
                                   constant uint &count [[buffer(2)]],
                                   uint id [[thread_position_in_grid]]) {
    if (id >= count)
        return;
    const auto q = queries[id];
    rypath_packet_layered::Parameters p{};
    p.active_mask = 15;
    for (uint lane = 0; lane < 4; ++lane) {
        thread auto &l = p.lane[lane];
        rypath_dielectric::prepare(q.eta, q.ax, q.ay, false, l.interface);
        rypath_conductor::prepare(q.base_eta, q.base_k, q.base_ax, q.base_ay, false, l.conductor);
        l.conductor_base = q.conductor_base;
        l.reflectance = q.reflectance;
        l.thickness = q.thickness;
        l.albedo = q.albedo;
        l.g = q.g;
        l.max_depth = q.max_depth;
        l.samples = q.samples;
        l.material_seed = q.material_seed;
    }
    const float3 wo(q.ox, q.oy, q.oz), wi(q.ix, q.iy, q.iz);
    const bool importance = q.operation == 3;
    const auto e = q.operation == 4
                       ? rypath_packet_layered::base_evaluate(p, wo, wi)
                       : rypath_packet_layered::evaluate(p, wo, wi, q.seed, importance);
    const auto s = q.operation == 4 ? rypath_packet_layered::base_sample(p, wo, float2(q.u, q.v))
                                    : rypath_packet_layered::sample(p, wo, q.uc, float2(q.u, q.v),
                                                                    q.seed, importance);
    rypath::layered_probe::Result r{};
    r.f = e.f.x;
    r.pdf = e.pdf;
    r.evaluation_valid = e.valid;
    r.weight = s.weight.x;
    r.sample_pdf = s.pdf;
    r.ix = s.wi.x;
    r.iy = s.wi.y;
    r.iz = s.wi.z;
    r.valid = s.valid;
    r.accepted = s.accepted;
    results[id] = r;
}

// Fixed-input probe of the production helper, independent of BSDF sampling.
#include "../src/shading_normal_probe_wire.h"
#include "../src/shading_normal_shared.h"
kernel void shading_normal_probe(const device rypath::shading_normal_probe::Query *queries
                                 [[buffer(0)]],
                                 device rypath::shading_normal_probe::Result *results [[buffer(1)]],
                                 constant uint &count [[buffer(2)]],
                                 uint tid [[thread_position_in_grid]]) {
    if (tid >= count)
        return;
    const auto q = queries[tid];
    const auto c = rypath_shading_normal::correction(q.wo_ns, q.wi_ns, q.wo_ng, q.wi_ng);
    const auto endpoint =
        rypath_shading_normal::cosine_correction(q.wo_ns, q.wi_ns, q.wo_ng, q.wi_ng);
    results[tid] = {rypath_shading_normal::apply(q.a, c),
                    rypath_shading_normal::multiply(q.a, q.b, c),
                    c.scale,
                    c.exponent,
                    uint(c.valid),
                    rypath_shading_normal::apply(q.a, endpoint),
                    rypath_shading_normal::multiply(q.a, q.b, endpoint),
                    0};
}

// A finite Prepared coating can still exceed binary32 optical range. Keep this
// separate from the fixed schedule/moment routes: invalid arithmetic is not a
// legitimate zero-support density. No injected failures or seed search.
kernel void packet_pdf_validity_probe(device uint *output [[buffer(0)]],
                                      uint tid [[thread_position_in_grid]]) {
    constexpr uint cases = 7, words = 32;
    if (tid >= cases)
        return;
    device uint *out = output + tid * words;
    for (uint i = 0; i < words; ++i)
        out[i] = 0;
    out[0] = 0x50444631u;
    out[1] = tid;
    rypath_packet::Prepared p{};
    p.active_mask = 15;
    bool prepared = true;
    for (uint i = 0; i < 4; ++i) {
        thread auto &a = p.lane[i];
        a.kind = tid == 4 ? 0 : tid == 6 ? 2 : 5;
        a.reflectance = .5f;
        prepared = rypath_dielectric::prepare(tid == 1 ? 0x1p65f : 1.5f, tid == 6 ? 0.f : .1f,
                                              tid == 6 ? 0.f : .1f, false, a.dielectric) &&
                   prepared;
        thread auto &l = a.layered;
        l.interface = a.dielectric;
        l.conductor_base = false;
        l.reflectance = .5f;
        l.thickness = .01f;
        l.albedo = l.g = 0;
        l.max_depth = 10;
        l.samples = 1;
        l.material_seed = 24;
    }
    const float3 wo = tid == 5 ? float3(1, 0, 0) : float3(0, 0, 1);
    const float3 wi = tid == 2 || tid == 4 ? float3(0, 0, -1)
                      : tid == 3           ? float3(1, 0, 0)
                                           : float3(0, 0, 1);
    const bool coated = p.lane[0].kind == 5;
    const auto leaf =
        coated ? rypath_packet_layered::directional_pdf(rypath_packet::layered(p), wo, wi)
               : rypath_packet::scalar_evaluate(rypath_packet::proposal(p), wo, wi);
    const auto reverse =
        coated ? rypath_packet_layered::directional_pdf(rypath_packet::layered(p), wi, wo)
               : rypath_packet::scalar_evaluate(rypath_packet::proposal(p), wi, wo);
    const auto e = rypath_packet::evaluate(p, wo, wi, 0);
    const float uc = tid == 0 ? 0.f : .8f; // Case0 takes a supported direct reflection.
    const auto s = rypath_packet::sample(p, wo, uc, float2(.3f, .7f), 0);
    bool joint_preserved = true;
    if (coated) {
        const auto joint =
            rypath_packet_layered::sample(rypath_packet::layered(p), wo, uc, float2(.3f, .7f), 0);
        joint_preserved = all(as_type<uint4>(joint.weight) == as_type<uint4>(s.weight)) &&
                          all(as_type<uint3>(joint.wi) == as_type<uint3>(s.wi)) &&
                          as_type<uint>(joint.pdf) == as_type<uint>(s.pdf) &&
                          joint.delta == s.delta;
    }
    out[2] = prepared;
    out[3] = rypath_packet::compatible(p);
    out[4] = leaf.valid;
    out[5] = as_type<uint>(leaf.pdf);
    out[6] = as_type<uint>(rypath_packet::pdf(p, wo, wi));
    out[7] = as_type<uint>(rypath_packet::pdf(p, wi, wo));
    out[8] = e.valid;
    out[9] = s.valid;
    out[10] = s.accepted;
    out[11] = s.delta;
    out[12] = all(isfinite(e.f));
    out[13] = joint_preserved;
    out[14] = any(s.weight > 0);
    out[24] = as_type<uint>(e.pdf);
    out[25] = as_type<uint>(e.reverse_pdf);
    out[26] = reverse.valid;
    out[27] = as_type<uint>(reverse.pdf);
    out[28] = all(isfinite(s.weight)) && all(s.weight >= 0);
    out[29] = isfinite(s.pdf) && s.pdf >= 0 && isfinite(s.reverse_pdf) && s.reverse_pdf >= 0;
}
