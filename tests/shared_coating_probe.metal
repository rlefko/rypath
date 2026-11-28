// SPDX-License-Identifier: GPL-3.0-only
#include "../shaders/packet_layered.h"
using namespace metal;
namespace shared_probe {
inline uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
inline float uniform(uint index, uint seed, uint domain) {
    return float(hash(index ^ hash(seed + domain * 0x9e3779b9u)) >> 8) * 0x1p-24f;
}
} // namespace shared_probe
kernel void rypath_shared_coating_probe(device float4 *output [[buffer(0)]],
                                        device uint *status [[buffer(1)]],
                                        constant uint4 &config [[buffer(2)]],
                                        uint id [[thread_position_in_grid]]) {
    if (id >= config.x)
        return;
    const uint sample = id % 32768u, seed = id / 32768u + 89001u;
    float uc = shared_probe::uniform(sample, seed, 0);
    float2 u(shared_probe::uniform(sample, seed, 1), shared_probe::uniform(sample, seed, 2));
    float3 wo = normalize(float3(-.7238673567771912f, -.6854109168052673f, .07891789078712463f));
    if (config.y == 1u)
        wo = float3(0, 0, 1);
    const float4 reflectance = config.z == 1u ? float4(0, .5f, .2f, .8f)
                                              : float4(.5241463780403137f, .5615267157554626f,
                                                       .4125249981880188f, .4808855354785919f);
    rypath_packet_layered::Parameters packet{};
    packet.active_mask = 15;
    for (uint k = 0; k < 4; ++k) {
        thread auto &p = packet.lane[k];
        p.interface = {1.5f, .3162277638912201f, .3162277638912201f, false};
        p.conductor_base = false;
        p.reflectance = reflectance[k];
        p.thickness = .01f;
        p.albedo = 0;
        p.g = 0;
        p.max_depth = 10;
        p.samples = 1;
        p.material_seed = 0;
    }
    if (config.w == 1u) {
        const float radius = sqrt(shared_probe::uniform(sample, seed, 3));
        const float phi = 2 * M_PI_F * shared_probe::uniform(sample, seed, 4);
        const float3 wi(radius * cos(phi), radius * sin(phi), sqrt(max(0.f, 1 - radius * radius)));
        const auto shared = rypath_packet_layered::evaluate(packet, wo, wi, 0);
        const auto density = rypath_packet_layered::directional_pdf(packet, wo, wi);
        const float light_pdf = wi.z / M_PI_F;
        const float factor = wi.z / (light_pdf + density.pdf);
        float4 scalar = 0;
        uint flags = shared.valid && density.valid ? 0u : 1u;
        for (uint k = 0; k < 4; ++k) {
            const auto one = rypath_layered::evaluate(packet.lane[k], wo, wi, 0);
            if (!one.valid)
                flags |= 2u << k;
            scalar[k] = one.f * factor;
        }
        output[2u * id] = shared.f * factor;
        output[2u * id + 1u] = scalar;
        status[id] = flags;
        return;
    }
    const auto shared = rypath_packet_layered::sample(packet, wo, uc, u, 0);
    float4 scalar = 0;
    uint flags = shared.valid ? 0u : 1u;
    for (uint k = 0; k < 4; ++k) {
        const auto one = rypath_layered::sample(packet.lane[k], wo, uc, u, 0);
        if (!one.valid)
            flags |= 2u << k;
        if (one.accepted)
            scalar[k] = one.weight;
    }
    output[2u * id] = shared.accepted ? shared.weight : float4(0);
    output[2u * id + 1u] = scalar;
    status[id] = flags;
}
