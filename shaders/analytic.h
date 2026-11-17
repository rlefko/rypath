// SPDX-License-Identifier: GPL-3.0-only AND Apache-2.0
// PBRT analytic parameterizations adapted from pbrt-v4 b0bdf461 shapes.{h,cpp}.
// Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt and README.md.
#pragma once
#include "../src/geometry_wire.h"
#include <metal_stdlib>

struct AnalyticSurface {
    metal::float3 position;
    metal::float3 normal;
    metal::float3 dpdu, dpdv, dndu, dndv;
    metal::float3 error;
    metal::float2 uv;
    bool valid;
};
struct AnalyticIntersection {
    bool hit;
    float t;
    metal::float2 uv;
};
inline float analytic_area(rypath::geometry_wire::Analytic s) {
    return s.subtype == 2u
               ? .5f * s.phi_max * (s.radius - s.inner_radius) * (s.radius + s.inner_radius)
               : s.phi_max * s.radius * (s.z_max - s.z_min);
}
inline AnalyticSurface evaluate_analytic(rypath::geometry_wire::Analytic s, metal::float2 uv) {
    using namespace metal;
    const float phi = uv.x * s.phi_max;
    const float c = cos(phi), sn = sin(phi), r = s.radius, pm = s.phi_max;
    AnalyticSurface out{};
    out.uv = uv;
    if (s.subtype == 2u) {
        const float radius = fma(uv.y, s.inner_radius - r, r);
        out.position = float3(radius * c, radius * sn, s.height);
        out.normal = float3(0, 0, 1);
        const float frame_radius = radius > 0 ? radius : r;
        out.dpdu = float3(-pm * frame_radius * sn, pm * frame_radius * c, 0);
        out.dpdv = float3((s.inner_radius - r) * c, (s.inner_radius - r) * sn, 0);
    } else if (s.subtype == 3u) {
        out.position = float3(r * c, r * sn, fma(uv.y, s.z_max - s.z_min, s.z_min));
        out.normal = float3(c, sn, 0);
        out.dpdu = float3(-pm * r * sn, pm * r * c, 0);
        out.dpdv = float3(0, 0, s.z_max - s.z_min);
        out.dndu = float3(-pm * sn, pm * c, 0);
    } else if (s.subtype == 1u) {
        const float t0 = acos(clamp(s.z_min / r, -1.0f, 1.0f));
        const float t1 = acos(clamp(s.z_max / r, -1.0f, 1.0f));
        const float dt = t1 - t0, theta = fma(uv.y, dt, t0);
        const float st = max(0.0f, sin(theta)), ct = cos(theta);
        out.position = float3(r * st * c, r * st * sn, r * ct);
        out.normal = float3(st * c, st * sn, ct);
        // Use a limiting frame at coordinate singularities, with exact normal.
        const float frame_sin = max(st, 1e-7f);
        out.dpdu = float3(-pm * r * frame_sin * sn, pm * r * frame_sin * c, 0);
        out.dpdv = float3(dt * r * ct * c, dt * r * ct * sn, -dt * r * st);
        out.dndu = float3(-pm * frame_sin * sn, pm * frame_sin * c, 0);
        out.dndv = float3(dt * ct * c, dt * ct * sn, -dt * st);
    }
    out.error = 0x1p-19f * (abs(out.position) + float3(s.radius));
    out.valid = s.subtype >= 1u && s.subtype <= 3u && all(isfinite(out.position)) &&
                all(isfinite(out.normal)) && all(isfinite(out.dpdu)) && all(isfinite(out.dpdv)) &&
                all(isfinite(out.dndu)) && all(isfinite(out.dndv)) && all(isfinite(out.error));
    return out;
}
inline AnalyticSurface sample_analytic_area(rypath::geometry_wire::Analytic s, metal::float2 u) {
    using namespace metal;
    float2 uv(u.y, u.x);
    if (s.subtype == 1u) {
        const float z = fma(u.x, s.z_max - s.z_min, s.z_min);
        const float t0 = acos(clamp(s.z_min / s.radius, -1.0f, 1.0f));
        const float t1 = acos(clamp(s.z_max / s.radius, -1.0f, 1.0f));
        uv.y = (acos(clamp(z / s.radius, -1.0f, 1.0f)) - t0) / (t1 - t0);
    } else if (s.subtype == 2u) {
        const float radius =
            sqrt(fma(u.x, (s.radius - s.inner_radius) * (s.radius + s.inner_radius),
                     s.inner_radius * s.inner_radius));
        uv.y = (s.radius - radius) / (s.radius - s.inner_radius);
    }
    return evaluate_analytic(s, uv);
}
inline float analytic_stable_length(metal::float3 v) {
    using namespace metal;
    const float scale = max(abs(v.x), max(abs(v.y), abs(v.z)));
    return scale > 0 ? scale * length(v / scale) : 0;
}
inline float analytic_area_jacobian(metal::float3 n, metal::float3 row0, metal::float3 row1,
                                    metal::float3 row2) {
    using namespace metal;
    // Rows of the cofactor matrix multiply the object-space unit normal.
    const float3 c0 = cross(row1, row2), c1 = cross(row2, row0), c2 = cross(row0, row1);
    return analytic_stable_length(float3(dot(c0, n), dot(c1, n), dot(c2, n)));
}
// Predicate receives normalized PBRT (u,v) and the unchanged affine ray t.
// A rejected root must not suppress the second root of the same primitive.
template <typename Accept>
inline AnalyticIntersection intersect_analytic_if(rypath::geometry_wire::Analytic s,
                                                  metal::float3 origin, metal::float3 direction,
                                                  float t_min, float t_max, thread Accept &accept) {
    using namespace metal;
    float roots[2];
    uint count = 0;
    if (s.subtype == 2u) {
        if (direction.z == 0)
            return {false, INFINITY, {0, 0}};
        roots[count++] = (s.height - origin.z) / direction.z;
    } else {
        const float3 o = s.subtype == 1u ? origin : float3(origin.xy, 0);
        const float3 d = s.subtype == 1u ? direction : float3(direction.xy, 0);
        const float a = dot(d, d);
        if (!(a > 0) || !isfinite(a))
            return {false, INFINITY, {0, 0}};
        const float b = 2 * dot(o, d);
        const float c = fma(o.x, o.x, fma(o.y, o.y, o.z * o.z)) - s.radius * s.radius;
        const float projection = -dot(o, d) / a;
        const float3 closest = fma(float3(projection), d, o);
        const float distance = length(closest);
        const float discr = 4 * a * (s.radius - distance) * (s.radius + distance);
        if (!(discr >= 0) || !isfinite(discr))
            return {false, INFINITY, {0, 0}};
        const float q = -.5f * (b + copysign(sqrt(discr), b));
        if (q == 0)
            roots[count++] = projection;
        else {
            roots[count++] = q / a;
            roots[count++] = c / q;
            if (roots[0] > roots[1]) {
                const float swap = roots[0];
                roots[0] = roots[1];
                roots[1] = swap;
            }
        }
    }
    for (uint i = 0; i < count; ++i) {
        const float t = roots[i];
        if (!isfinite(t) || t < t_min || t > t_max)
            continue;
        float3 p = fma(float3(t), direction, origin);
        if (s.subtype == 1u) {
            const float length_p = length(p);
            if (!(length_p > 0))
                continue;
            p *= s.radius / length_p;
        } else if (s.subtype == 3u) {
            const float length_p = length(p.xy);
            if (!(length_p > 0))
                continue;
            p.xy *= s.radius / length_p;
        }
        float phi = atan2(p.y, p.x);
        if (phi < 0)
            phi += 2 * M_PI_F;
        if (phi > s.phi_max)
            continue;
        float v;
        if (s.subtype == 2u) {
            const float radius = length(p.xy);
            if (radius < s.inner_radius || radius > s.radius)
                continue;
            v = (s.radius - radius) / (s.radius - s.inner_radius);
        } else {
            if (p.z < s.z_min || p.z > s.z_max)
                continue;
            if (s.subtype == 1u) {
                const float t0 = acos(clamp(s.z_min / s.radius, -1.0f, 1.0f));
                const float t1 = acos(clamp(s.z_max / s.radius, -1.0f, 1.0f));
                v = (acos(clamp(p.z / s.radius, -1.0f, 1.0f)) - t0) / (t1 - t0);
            } else
                v = (p.z - s.z_min) / (s.z_max - s.z_min);
        }
        const float2 uv(clamp(phi / s.phi_max, 0.0f, 1.0f), clamp(v, 0.0f, 1.0f));
        if (all(isfinite(uv)) && accept(uv, t))
            return {true, t, uv};
    }
    return {false, INFINITY, {0, 0}};
}
struct AcceptAnalytic {
    bool operator()(metal::float2, float) const { return true; }
};
inline AnalyticIntersection intersect_analytic(rypath::geometry_wire::Analytic s, metal::float3 o,
                                               metal::float3 d, float lo, float hi) {
    AcceptAnalytic accept;
    return intersect_analytic_if(s, o, d, lo, hi, accept);
}
