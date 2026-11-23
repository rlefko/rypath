// SPDX-License-Identifier: GPL-3.0-only AND Apache-2.0
// Positional light proposals adapted from PBRT v4 b0bdf461 lights.cpp.
// Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt and README.md.
#pragma once
namespace rypath_light_emission {
using namespace metal;
namespace wire = rypath::render_wire;
constant constexpr float pi = 3.14159265358979323846f;
// Retain support when a positive native texel's float CDF interval collapses.
constant constexpr float uniform_fraction = 0x1p-16f;
inline float3 local(const device wire::PositionalLight &l, float3 w) {
    return float3(
        dot(float3(l.light_from_world[0], l.light_from_world[1], l.light_from_world[2]), w),
        dot(float3(l.light_from_world[3], l.light_from_world[4], l.light_from_world[5]), w),
        dot(float3(l.light_from_world[6], l.light_from_world[7], l.light_from_world[8]), w));
}
inline float determinant(const device wire::PositionalLight &l) {
    float3 a(l.light_from_world[0], l.light_from_world[1], l.light_from_world[2]),
        b(l.light_from_world[3], l.light_from_world[4], l.light_from_world[5]),
        c(l.light_from_world[6], l.light_from_world[7], l.light_from_world[8]);
    return dot(a, cross(b, c));
}
inline float3 world(const device wire::PositionalLight &l, float3 w) {
    float3 a(l.light_from_world[0], l.light_from_world[1], l.light_from_world[2]),
        b(l.light_from_world[3], l.light_from_world[4], l.light_from_world[5]),
        c(l.light_from_world[6], l.light_from_world[7], l.light_from_world[8]);
    return normalize((w.x * cross(b, c) + w.y * cross(c, a) + w.z * cross(a, b)) /
                     dot(a, cross(b, c)));
}
inline float transform_pdf(const device wire::PositionalLight &l, float3 w, float local_pdf) {
    float3 q = local(l, w);
    float length2 = dot(q, q);
    return local_pdf * abs(determinant(l)) / (length2 * sqrt(length2));
}
inline float invert(const device float *cdf, uint count, float u, thread uint &cell) {
    uint lo = 0, hi = count;
    while (lo + 1 < hi) {
        uint mid = (lo + hi) / 2;
        if (cdf[mid] <= u)
            lo = mid;
        else
            hi = mid;
    }
    cell = min(lo, count - 1);
    float mass = cdf[cell + 1] - cdf[cell];
    return mass > 0 ? clamp((u - cdf[cell]) / mass, 0.f, 0x1.fffffep-1f) : .5f;
}
inline float map_pdf(const device wire::PositionalLight &l, const device uint *data, float2 uv) {
    if (any(uv < 0) || any(uv > 1) || !l.sampling_width || !l.sampling_height)
        return 0;
    uint x = min(uint(uv.x * l.sampling_width), l.sampling_width - 1),
         y = min(uint(uv.y * l.sampling_height), l.sampling_height - 1);
    auto cdf = reinterpret_cast<const device float *>(data + l.sampling_begin);
    auto row = cdf + l.sampling_height + 1 + y * (l.sampling_width + 1);
    float density =
        (cdf[y + 1] - cdf[y]) * l.sampling_height * (row[x + 1] - row[x]) * l.sampling_width;
    return uniform_fraction + (1 - uniform_fraction) * density;
}
inline float2 map_sample(const device wire::PositionalLight &l, const device uint *data, float2 u) {
    if (u.x < uniform_fraction)
        return float2(u.x / uniform_fraction, u.y);
    u.x = min((u.x - uniform_fraction) / (1 - uniform_fraction), 0x1.fffffep-1f);
    auto cdf = reinterpret_cast<const device float *>(data + l.sampling_begin);
    uint x, y;
    float v = invert(cdf, l.sampling_height, u.y, y);
    auto row = cdf + l.sampling_height + 1 + y * (l.sampling_width + 1);
    float t = invert(row, l.sampling_width, u.x, x);
    return float2((x + t) / l.sampling_width, (y + v) / l.sampling_height);
}
inline float smooth(float t) { return t * t * (3 - 2 * t); }
inline float spot_pdf(const device wire::PositionalLight &l, float cosine) {
    float a = l.cos_falloff_start, b = l.cos_falloff_end;
    float integral = (1 - a) + (a - b) * .5f;
    if (!(integral > 0) || cosine < b)
        return 0;
    float f = cosine >= a ? 1.f : smooth(clamp((cosine - b) / (a - b), 0.f, 1.f));
    return f / (2 * pi * integral);
}
inline float pdf(const device wire::PositionalLight &l, const device uint *data, float3 direction) {
    if (l.kind == wire::point_light)
        return 1 / (4 * pi);
    float3 w = normalize(local(l, direction));
    float density = 0;
    if (l.kind == wire::spot_light)
        density = spot_pdf(l, w.z);
    else if (l.kind == wire::goniometric_light)
        density = map_pdf(l, data, rypath_image_light::sphere_to_square(w, false)) / (4 * pi);
    else if (l.kind == wire::projection_light && w.z > 0) {
        float2 screen = w.xy / w.z * l.inv_tan_half_fov;
        float2 uv = (screen / float2(l.screen_half_width, l.screen_half_height) + 1) * .5f;
        float area = 4 * l.screen_half_width * l.screen_half_height /
                     (l.inv_tan_half_fov * l.inv_tan_half_fov);
        density = map_pdf(l, data, uv) / (area * w.z * w.z * w.z);
    }
    return transform_pdf(l, direction, density);
}
inline bool sample(const device wire::PositionalLight &l, const device uint *data, float2 u,
                   thread float3 &direction, thread float &density) {
    if (l.kind == wire::point_light) {
        float z = 1 - 2 * u.x, phi = 2 * pi * u.y, r = sqrt(max(0.f, 1 - z * z));
        direction = float3(r * cos(phi), r * sin(phi), z);
        density = 1 / (4 * pi);
        return true;
    }
    float3 w;
    if (l.kind == wire::spot_light) {
        float a = l.cos_falloff_start, b = l.cos_falloff_end, center = 1 - a,
              falloff = (a - b) * .5f, total = center + falloff;
        if (!(total > 0))
            return false;
        float cosine;
        if (u.x < center / total)
            cosine = 1 - (u.x * total);
        else {
            float target = clamp((u.x * total - center) / falloff, 0.f, 1.f), lo = 0, hi = 1;
            for (uint k = 0; k < 24; ++k) {
                float t = (lo + hi) * .5f, cdf = t * t * t * (2 - t);
                if (cdf < target)
                    lo = t;
                else
                    hi = t;
            }
            cosine = mix(b, a, (lo + hi) * .5f);
        }
        float phi = 2 * pi * u.y, r = sqrt(max(0.f, 1 - cosine * cosine));
        w = float3(r * cos(phi), r * sin(phi), cosine);
    } else {
        if (!l.sampling_width || !l.sampling_height)
            return false;
        float2 uv = map_sample(l, data, u);
        if (l.kind == wire::goniometric_light)
            w = rypath_image_light::square_to_sphere(uv);
        else
            w = normalize(float3((2 * uv - 1) * float2(l.screen_half_width, l.screen_half_height) /
                                     l.inv_tan_half_fov,
                                 1));
    }
    direction = world(l, w);
    density = pdf(l, data, direction);
    return all(isfinite(direction)) && isfinite(density) && density > 0;
}
} // namespace rypath_light_emission
