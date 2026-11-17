// SPDX-License-Identifier: GPL-3.0-only
#include "../src/packet_spectrum_shared.h"
#include "packet_scattering.h"
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
    out[tid * 13] = float4(error, a.valid, a.accepted, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 2;
        rypath_dielectric::prepare(1.5, .2, .3, false, p.lane[i].dielectric);
    }
    auto r = rypath_packet::sample(p, wo, .9f, u, tid);
    auto b = rypath_packet::sample(p, wo, .9f, u, tid, rypath_packet::TransportMode::Importance);
    out[tid * 13 + 1] =
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
    out[tid * 13 + 2] =
        float4(length(l.weight - analytic), l.valid, l.accepted, l.pdf_is_proportional);
    p.active_mask = 2;
    auto m = rypath_packet::sample(p, wo, .9f, u, tid);
    out[tid * 13 + 3] =
        float4(abs(m.weight.x) + abs(m.weight.z) + abs(m.weight.w), m.valid, m.accepted, 0);
    p.active_mask = 15;
    float3 wi = normalize(float3(-.3, .2, 1));
    auto eval = rypath_packet::evaluate(p, wo, wi, tid);
    float4 ef = float4(0, .2, .4, .6) / M_PI_F * exp(-.1f * (1 / wo.z + 1 / wi.z));
    out[tid * 13 + 4] = float4(length(eval.f - ef), eval.valid, 1, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 11;
        rypath_gltf::prepare(i == 0 ? 0 : .5f, i == 0 ? 1 : 0, 0, p.lane[i].gltf);
    }
    auto gltf = rypath_packet::sample(p, wo, .8f, u, tid);
    auto ge = rypath_packet::evaluate(p, wo, gltf.wi, tid);
    out[tid * 13 + 5] = float4(length(gltf.weight - ge.f * (abs(gltf.wi.z) / gltf.pdf)), gltf.valid,
                               gltf.accepted, 0);
    for (uint i = 0; i < 4; ++i) {
        p.lane[i].kind = 3;
        p.lane[i].dielectric.eta = i == 0 ? 1 : 1.5f;
    }
    auto thin = rypath_packet::sample(p, wo, .01f, u, tid);
    out[tid * 13 + 6] = float4(thin.weight.x, thin.valid, thin.accepted, 0);
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
    out[tid * 13 + 7] = float4(walk_error, vector_walk.valid, scalar_walk.valid, 0);
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
    out[tid * 13 + 8] = float4(ferr, fourier.valid, fourier.accepted, 0);
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
    out[tid * 13 + 9] = float4(merr, metal.valid, metal.accepted, 0);
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
    out[tid * 13 + 10] = float4(spectral_error, spectral_walk.valid, bright_walk.valid, 0);
    auto shuffled = p;
    for (uint i = 0; i < 4; ++i)
        shuffled.lane[i] = p.lane[3 - i];
    const float3 probe_wi = normalize(float3(.1f, .2f, 1));
    const float a_pdf = rypath_packet::pdf(p, wo, probe_wi);
    const float b_pdf = rypath_packet::pdf(shuffled, wo, probe_wi);
    const float old_gap =
        abs(rypath_layered::directional_pdf(p.lane[0].layered, wo, probe_wi).pdf -
            rypath_layered::directional_pdf(shuffled.lane[0].layered, wo, probe_wi).pdf);
    out[tid * 13 + 11] = float4(abs(a_pdf - b_pdf), isfinite(a_pdf), isfinite(b_pdf), old_gap);
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
    out[tid * 13 + 12] = float4(0, bounded, invalid_rejected, 0);
}
