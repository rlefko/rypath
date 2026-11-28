// SPDX-License-Identifier: GPL-3.0-only
#define RYPATH_HERO_SHARED_COATED_DIFFUSE 1
#include "../shaders/render.metal"
using namespace rypath_bdpt;
kernel void rypath_shared_coating_current_contract(device uint *out [[buffer(0)]],
                                                   uint id [[thread_position_in_grid]]) {
    if (id >= 64u)
        return;
    rypath_packet::Prepared full{};
    full.active_mask = 15;
    HeroFrozenBSDF compact{};
    compact.kind = 5;
    compact.active_mask = 15;
    uint variant = id % 16u;
    for (uint k = 0; k < 4; ++k) {
        thread auto &lane = full.lane[k];
        lane.kind = 5;
        lane.layered = {{1.5f, .3162277638912201f, .3162277638912201f, false},
                        {1, 1, .2f, .3f, false},
                        false,
                        .2f * float(k),
                        .01f,
                        0,
                        0,
                        10,
                        1,
                        0};
        if (variant == 1u && k == 1u)
            lane.layered.interface.eta = 1.51f;
        if (variant == 2u && k == 1u)
            lane.layered.albedo = .1f;
        if (variant == 3u && k == 1u)
            lane.layered.thickness = .02f;
        if (variant == 4u && k == 1u)
            lane.layered.interface.alpha_x = .2f;
        if (variant == 5u && k == 1u)
            lane.layered.g = .2f;
        if (variant == 6u && k == 1u)
            lane.layered.max_depth = 9;
        if (variant == 7u && k == 1u)
            lane.layered.samples = 2;
        if (variant == 8u && k == 1u)
            lane.layered.material_seed = 1;
        if (variant == 9u) {
            lane.kind = 6;
            lane.layered.conductor_base = true;
            compact.kind = 6;
        }
        if (variant == 10u)
            lane.layered.albedo = .1f;
        if (variant == 11u && k == 1u)
            lane.layered.interface.smooth = true;
        if (variant == 12u && k == 1u)
            lane.layered.interface.alpha_y = .2f;
        if (variant == 13u)
            full.active_mask = compact.active_mask = 1;
        if (variant == 14u)
            full.active_mask = compact.active_mask = 3;
        if (variant == 15u)
            lane.layered.interface.smooth = true;
        if (full.active_mask & (1u << k))
            hero_current_store_lane(compact, k, lane);
    }
    uint fail = 0;
    const bool expect = variant == 0u || variant == 13u || variant == 14u || variant == 15u;
    if (hero_frozen_shared_coating(compact) != expect)
        fail |= 1u;
    if (rypath_hero_scattering::requires_single_hero(full) != hero_current_requires_single(compact))
        fail |= 2u;
    const auto frozen = hero_freeze_bsdf(full);
    if (expect) {
        if (frozen.kind != compact.kind || frozen.active_mask != compact.active_mask)
            fail |= 4u;
        for (uint k = 0; k < 24; ++k)
            if (frozen.payload[k] != compact.payload[k])
                fail |= 8u;
    }
    HeroTrack a, b;
    for (uint k = 0; k < 4; ++k) {
        a.density.lane[k] = b.density.lane[k] = (full.active_mask & (1u << k))
                                                    ? rypath_hero::number(exp2(-float(k)))
                                                    : rypath_hero::Number{};
    }
    float4 ba(1, .25f, .5f, .75f), bb = ba;
    const bool pa = hero_prepare_future(a, ba, full), pb = hero_prepare_future(b, bb, compact);
    if (pa != pb || any(as_type<uint4>(ba) != as_type<uint4>(bb)))
        fail |= 16u;
    for (uint k = 0; k < 4; ++k)
        if (a.density.lane[k].fraction != b.density.lane[k].fraction ||
            a.density.lane[k].exponent != b.density.lane[k].exponent)
            fail |= 32u;
    if (expect && full.active_mask != 1u &&
        any(as_type<uint4>(ba) != as_type<uint4>(float4(1, .25f, .5f, .75f))))
        fail |= 64u;
    const float3 wo = normalize(float3(.3f, .2f, 1)), wi = normalize(float3(-.2f, .1f, 1));
    const float uc = (id / 16u) * .3f + .01f;
    const auto sa = rypath_hero_scattering::sample(full, wo, uc, float2(.15f, .25f), 719);
    const auto sb = hero_current_sample(compact, wo, uc, float2(.15f, .25f), 719);
    if (sa.valid != sb.valid || sa.accepted != sb.accepted || sa.fallback != sb.fallback ||
        sa.delta != sb.delta || sa.transmission != sb.transmission ||
        any(as_type<uint3>(sa.wi) != as_type<uint3>(sb.wi)) ||
        any(as_type<uint4>(sa.fallback_weight) != as_type<uint4>(sb.fallback_weight)))
        fail |= 128u;
    if (expect && full.active_mask == 15u) {
        auto rotated = full;
        const uint shift = 1u + id % 3u;
        for (uint k = 0; k < 4; ++k)
            rotated.lane[k] = full.lane[(k + shift) % 4u];
        const auto rs = rypath_hero_scattering::sample(rotated, wo, uc, float2(.15f, .25f), 719);
        if (rs.valid != sa.valid || rs.accepted != sa.accepted ||
            any(as_type<uint3>(rs.wi) != as_type<uint3>(sa.wi)))
            fail |= 512u;
        for (uint k = 0; k < 4; ++k)
            if (as_type<uint>(rs.fallback_weight[k]) !=
                as_type<uint>(sa.fallback_weight[(k + shift) % 4u]))
                fail |= 1024u;
    }
    const auto ea = rypath_hero_scattering::evaluate(full, wo, wi, 719),
               eb = hero_current_evaluate(compact, wo, wi, 719);
    if (ea.valid != eb.valid || any(as_type<uint4>(ea.f) != as_type<uint4>(eb.f)) ||
        any(as_type<uint4>(ea.pdf) != as_type<uint4>(eb.pdf)))
        fail |= 256u;
    if (ea.valid && expect) {
        const float light_pdf = .123f;
        const auto weighted =
            rypath_hero::balance_contribution(a.density, ea.f, float4(light_pdf), ea.pdf);
        const float expected_weight = light_pdf / (light_pdf + ea.pdf[0]);
        if (!weighted.valid)
            fail |= 2048u;
        for (uint k = 0; k < 4; ++k)
            if (abs(weighted.value[k] - ea.f[k] * expected_weight) > 2e-6f * max(1.f, abs(ea.f[k])))
                fail |= 4096u;
    }
    out[id] = fail;
}
