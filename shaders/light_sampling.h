// SPDX-License-Identifier: GPL-3.0-only AND Apache-2.0
// Bounds importance adapted from PBRT v4 b0bdf461, lightsamplers.h.
// Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt and README.md.
#pragma once
namespace rypath_light_sampling {
using namespace metal;
namespace wire = rypath::render_wire;
struct Context {
    float3 position, normal;
};
struct Selected {
    uint light;
    float pmf;
};
inline const device wire::LightBounds *nodes(constant wire::Parameters &p, const device uint *d) {
    return reinterpret_cast<const device wire::LightBounds *>(d + p.light_nodes_begin);
}
inline float importance(const device wire::LightBounds &b, Context ctx) {
    float3 lo = unpack(b.low), hi = unpack(b.high), center = (lo + hi) * .5f;
    float3 delta = ctx.position - center;
    float r2 = dot(hi - lo, hi - lo) * .25f, d2 = dot(delta, delta);
    float radius = sqrt(r2), distance = sqrt(d2);
    // At or inside the bounding sphere every direction is possible.
    float cb = d2 <= r2 ? -1.f : sqrt(max(0.f, 1 - r2 / d2)), sb = sqrt(max(0.f, 1 - cb * cb));
    float3 wi = distance > 0 ? delta / distance : float3(0, 0, 1);
    float cw = clamp(dot(unpack(b.direction), wi), -1.f, 1.f);
    if (b.flags & 1)
        cw = abs(cw);
    float sw = sqrt(max(0.f, 1 - cw * cw)), co = b.cos_outer, so = sqrt(max(0.f, 1 - co * co));
    float cx = cw > co ? 1.f : cw * co + sw * so, sx = cw > co ? 0.f : sw * co - cw * so;
    float cp = cx > cb ? 1.f : cx * cb + sx * sb;
    if (cp <= b.cos_emission)
        return 0;
    float value = b.power * cp / max(d2, max(radius, FLT_MIN));
    if (any(ctx.normal != 0)) {
        float ci = clamp(abs(dot(wi, ctx.normal)), 0.f, 1.f);
        float si = sqrt(max(0.f, 1 - ci * ci));
        value *= ci > cb ? 1.f : ci * cb + si * sb;
    }
    return max(0.f, value);
}
inline Selected sample(constant wire::Parameters &p, const device uint *data, Context ctx,
                       float u) {
    if (!p.light_count)
        return {0xffffffffu, 0};
    u = clamp(u, 0.f, 0x1.fffffep-1f);
    if (p.light_sampler == 2) {
        uint id = min(uint(u * p.light_count), p.light_count - 1);
        return {id, 1.f / p.light_count};
    }
    if (p.light_sampler == 1) {
        auto a = reinterpret_cast<const device wire::LightAlias *>(data + p.light_alias_begin);
        float v = u * p.light_count;
        uint i = min(uint(v), p.light_count - 1);
        uint id = v - i < a[i].threshold ? i : a[i].alias;
        return {id, a[id].pmf};
    }
    float infinite = float(p.infinite_light_count) /
                     float(p.infinite_light_count + (p.light_node_count ? 1 : 0));
    if (u < infinite) {
        uint i = min(uint((u / infinite) * p.infinite_light_count), p.infinite_light_count - 1);
        return {data[p.infinite_lights_begin + i],
                1.f / float(p.infinite_light_count + (p.light_node_count ? 1 : 0))};
    }
    if (!p.light_node_count)
        return {0xffffffffu, 0};
    u = min((u - infinite) / (1 - infinite), 0x1.fffffep-1f);
    float pmf = 1 - infinite;
    uint i = 0;
    auto tree = nodes(p, data);
    for (uint steps = 0; steps < p.light_node_count; ++steps) {
        const device auto &node = tree[i];
        if (node.light != 0xffffffffu) {
            if (importance(node, ctx) <= 0)
                return {0xffffffffu, 0};
            return {node.light, pmf};
        }
        float a = importance(tree[i + 1], ctx), b = importance(tree[node.child], ctx), sum = a + b;
        if (!(sum > 0))
            return {0xffffffffu, 0};
        float q = a / sum;
        if (u < q) {
            pmf *= q;
            u = min(u / q, 0x1.fffffep-1f);
            i++;
        } else {
            pmf *= 1 - q;
            u = min((u - q) / (1 - q), 0x1.fffffep-1f);
            i = node.child;
        }
    }
    return {0xffffffffu, 0};
}
inline float pmf(constant wire::Parameters &p, const device uint *data, Context ctx, uint id) {
    if (id >= p.light_count)
        return 0;
    if (p.light_sampler == 2)
        return 1.f / p.light_count;
    if (p.light_sampler == 1)
        return reinterpret_cast<const device wire::LightAlias *>(data + p.light_alias_begin)[id]
            .pmf;
    auto records = reinterpret_cast<const device wire::LightRecord *>(data + p.light_records_begin);
    if (records[id].flags & 1)
        return 1.f / float(p.infinite_light_count + (p.light_node_count ? 1 : 0));
    uint i = records[id].leaf;
    if (i >= p.light_node_count)
        return 0;
    auto tree = nodes(p, data);
    if (importance(tree[i], ctx) <= 0)
        return 0;
    float result = 1.f - float(p.infinite_light_count) /
                             float(p.infinite_light_count + (p.light_node_count ? 1 : 0));
    for (uint steps = 0; steps < p.light_node_count; ++steps) {
        uint parent = tree[i].parent;
        if (parent == 0xffffffffu)
            return result;
        float a = importance(tree[parent + 1], ctx), b = importance(tree[tree[parent].child], ctx),
              sum = a + b;
        if (!(sum > 0))
            return 0;
        result *= i == parent + 1 ? a / sum : b / sum;
        i = parent;
    }
    return 0;
}
} // namespace rypath_light_sampling
