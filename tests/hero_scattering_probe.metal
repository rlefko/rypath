// SPDX-License-Identifier: GPL-3.0-only
#include "../shaders/hero_scattering.h"
#include "hero_scattering_probe_wire.h"
namespace p = hero_scattering_probe;
namespace h = rypath_hero_scattering;
kernel void rypath_hero_scattering_probe(device const p::Query *queries [[buffer(0)]],
                                         device p::Result *results [[buffer(1)]],
                                         constant uint &count [[buffer(2)]],
                                         uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    const auto q = queries[index];
    rypath_packet::Prepared prepared{};
    prepared.active_mask = q.active_mask;
    for (uint i = 0; i < 4; ++i) {
        thread auto &lane = prepared.lane[i];
        lane.kind = q.kind;
        lane.reflectance = q.reflectance[i];
        lane.transmittance = q.transmittance[i];
        rypath_dielectric::prepare(q.eta[i], q.alpha, q.alpha, false, lane.dielectric);
        if (q.force_rough)
            lane.dielectric.smooth = false;
        rypath_conductor::prepare(q.eta[i], q.transmittance[i], q.alpha, q.alpha, false,
                                  lane.conductor);
        lane.gltf = {q.reflectance[i], 1, q.alpha, 1};
        lane.layered.interface = lane.dielectric;
        lane.layered.reflectance = q.reflectance[i];
        lane.layered.thickness = 0;
        lane.layered.max_depth = 8;
        lane.layered.samples = 1;
        lane.layered.material_seed = 17;
    }
    const float3 wo(q.wo[0], q.wo[1], q.wo[2]), wi(q.wi[0], q.wi[1], q.wi[2]);
    const auto mode = q.importance ? h::TransportMode::Importance : h::TransportMode::Radiance;
    const auto sampled = h::sample(prepared, wo, q.uc, float2(q.u[0], q.u[1]), 54321u, mode);
    const auto evaluated = h::evaluate(prepared, wo, wi, 54321u, mode);
    p::Result out{};
    for (uint i = 0; i < 3; ++i)
        out.wi[i] = sampled.wi[i];
    for (uint i = 0; i < 4; ++i) {
        out.numerator[i] = sampled.numerator[i];
        out.pdf[i] = sampled.pdf[i];
        out.eta[i] = sampled.eta[i];
        out.f[i] = evaluated.f[i];
        out.evaluation_pdf[i] = evaluated.pdf[i];
        out.fallback_weight[i] = sampled.fallback_weight[i];
    }
    out.fallback_pdf = sampled.fallback_pdf;
    out.valid = sampled.valid;
    out.accepted = sampled.accepted;
    out.delta = sampled.delta;
    out.transmission = sampled.transmission;
    out.fallback = sampled.fallback;
    out.evaluation_valid = evaluated.valid;
    out.requires_single = h::requires_single_hero(prepared);
    if (q.kind == 5 && q.active_mask == 1) {
        const auto legacy =
            rypath_packet::sample(prepared, wo, q.uc, float2(q.u[0], q.u[1]), 54321u, mode);
        const auto eval = rypath_packet::evaluate(prepared, wo, wi, 54321u, mode);
        for (uint i = 0; i < 4; ++i) {
            out.legacy_weight[i] = legacy.weight[i];
            out.legacy_f[i] = eval.f[i];
        }
        out.legacy_pdf = legacy.pdf;
        out.legacy_evaluation_pdf = eval.pdf;
    }
    results[index] = out;
}
