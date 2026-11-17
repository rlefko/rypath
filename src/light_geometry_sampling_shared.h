// SPDX-License-Identifier: Apache-2.0
// Sampling adapted from PBRT v4 b0bdf461 shapes.{h,cpp}, util/sampling.{h,cpp}.
// Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt and README.md.
#pragma once

#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#define RYPATH_LG_THREAD thread
#else
#include <algorithm>
#include <cmath>
#define RYPATH_LG_THREAD
#endif

namespace rypath_light_geometry_math {
#if defined(__METAL_VERSION__)
using namespace metal;
using V2 = float2;
using V3 = float3;
#else
using std::abs;
using std::acos;
using std::asin;
using std::atan2;
using std::copysign;
using std::cos;
using std::isfinite;
using std::max;
using std::min;
using std::sin;
using std::sqrt;
struct V2 {
    float x = 0, y = 0;
    V2() = default;
    V2(float x, float y) : x(x), y(y) {}
};
struct V3 {
    float x = 0, y = 0, z = 0;
    V3() = default;
    V3(float x, float y, float z) : x(x), y(y), z(z) {}
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
inline V3 operator*(float a, V3 b) { return {a * b.x, a * b.y, a * b.z}; }
inline V3 operator*(V3 a, float b) { return b * a; }
inline V3 operator/(V3 a, float b) { return (1 / b) * a; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
#endif
#if defined(__METAL_VERSION__)
constant constexpr float pi = 3.14159265358979323846f;
constant constexpr float one_minus_epsilon = 0x1.fffffep-1f;
#else
inline constexpr float pi = 3.14159265358979323846f;
inline constexpr float one_minus_epsilon = 0x1.fffffep-1f;
#endif
inline float clamp01(float v) { return min(1.f, max(0.f, v)); }
inline float safe_sqrt(float v) { return sqrt(max(0.f, v)); }
inline float length3(V3 v) { return sqrt(dot(v, v)); }
inline V3 unit(V3 v) { return v / length3(v); }
inline bool finite(V3 v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }
inline bool nonzero(V3 v) { return dot(v, v) > 0; }
inline float angle(V3 a, V3 b) {
    return dot(a, b) < 0 ? pi - 2 * asin(min(1.f, length3(a + b) / 2))
                         : 2 * asin(min(1.f, length3(a - b) / 2));
}
inline float triangle_area(V3 a, V3 b, V3 c) {
    return abs(2 * atan2(dot(a, cross(b, c)), 1 + dot(a, b) + dot(a, c) + dot(b, c)));
}
inline float triangle_solid_angle(V3 p0, V3 p1, V3 p2, V3 reference) {
    if (!nonzero(p0 - reference) || !nonzero(p1 - reference) || !nonzero(p2 - reference))
        return 0;
    return triangle_area(unit(p0 - reference), unit(p1 - reference), unit(p2 - reference));
}
inline bool spherical_triangle(float area) { return area >= 3e-4f && area <= 6.22f; }

struct Weights {
    float a, b, c, d;
};
inline Weights normalized_weights(Weights w) {
    const float scale = max(max(w.a, w.b), max(w.c, w.d));
    return scale > 0 ? Weights{w.a / scale, w.b / scale, w.c / scale, w.d / scale}
                     : Weights{1, 1, 1, 1};
}
inline float linear_sample(float u, float a, float b) {
    if (u == 0 && a == 0)
        return 0;
    return min(one_minus_epsilon, u * (a + b) / (a + sqrt((1 - u) * a * a + u * b * b)));
}
inline V2 bilinear_sample(V2 u, Weights weights) {
    const auto w = normalized_weights(weights);
    const float y = linear_sample(u.y, w.a + w.b, w.c + w.d);
    const float x = linear_sample(u.x, (1 - y) * w.a + y * w.c, (1 - y) * w.b + y * w.d);
    return {x, y};
}
inline float bilinear_pdf(V2 u, Weights weights) {
    if (u.x < 0 || u.x > 1 || u.y < 0 || u.y > 1)
        return 0;
    const auto w = normalized_weights(weights);
    return 4 *
           ((1 - u.x) * (1 - u.y) * w.a + u.x * (1 - u.y) * w.b + (1 - u.x) * u.y * w.c +
            u.x * u.y * w.d) /
           (w.a + w.b + w.c + w.d);
}
inline Weights triangle_cosines(V3 a, V3 b, V3 c, V3 reference, V3 normal) {
    const float wa = max(.01f, abs(dot(normal, unit(a - reference))));
    const float wb = max(.01f, abs(dot(normal, unit(b - reference))));
    const float wc = max(.01f, abs(dot(normal, unit(c - reference))));
    return {wb, wb, wa, wc};
}
inline V2 triangle_uniform_uv(V2 u) {
    const float root = sqrt(u.x);
    return {1 - root, u.y * root};
}

// Output is geometric barycentrics (b1,b2), independent of texture UVs.
inline bool triangle_sample(V3 p0, V3 p1, V3 p2, V3 reference, V2 u, RYPATH_LG_THREAD V2 &uv) {
    const V3 a = unit(p0 - reference), b = unit(p1 - reference), c = unit(p2 - reference);
    const V3 nab = cross(a, b), nca = cross(c, a);
    if (!nonzero(nab) || !nonzero(nca))
        return false;
    const float alpha = angle(unit(nab), -unit(nca));
    const float area = triangle_area(a, b, c);
    const float ca = cos(alpha), sa = sin(alpha);
    const float subarea = u.x * area;
    const float sh = sin(subarea / 2), st = sin(subarea), ct_area = cos(subarea);
    const float one_minus_ct = 2 * sh * sh;
    const float sp = sa * ct_area - ca * st;
    const float cp = -ca * ct_area - sa * st;
    const float k1 = -2 * sin(alpha - subarea / 2) * sh;
    const float k2 = sa * (.5f * dot(a - b, a - b) - one_minus_ct) - ca * st;
    const float divisor = (k2 * sp + k1 * cp) * sa;
    if (divisor == 0)
        return false;
    // Rationalize 1-cos(beta') before dividing. Computing cos(beta') then
    // subtracting from one loses entire strips near the far-area threshold.
    const float one_minus_cosbp = min(2.f, max(0.f, (-k2 * one_minus_ct - k1 * st) / divisor));
    const float cosbp = 1 - one_minus_cosbp;
    const V3 arc = c - dot(c, a) * a;
    if (!nonzero(arc))
        return false;
    const V3 cprime = cosbp * a + safe_sqrt(one_minus_cosbp * (2 - one_minus_cosbp)) * unit(arc);
    const float one_minus_cos_theta = u.y * .5f * dot(cprime - b, cprime - b);
    const float ct = 1 - one_minus_cos_theta;
    const V3 tangent = cprime - dot(cprime, b) * b;
    if (!nonzero(tangent))
        return false;
    const V3 direction =
        ct * b + safe_sqrt(one_minus_cos_theta * (2 - one_minus_cos_theta)) * unit(tangent);
    const V3 e1 = p1 - p0, e2 = p2 - p0, s1 = cross(direction, e2);
    const float denom = dot(s1, e1);
    if (denom == 0)
        return false;
    const V3 s = reference - p0;
    float b1 = clamp01(dot(s, s1) / denom), b2 = clamp01(dot(direction, cross(s, e1)) / denom);
    if (b1 + b2 > 1) {
        const float sum = b1 + b2;
        b1 /= sum;
        b2 /= sum;
    }
    uv = {b1, b2};
    return isfinite(b1) && isfinite(b2);
}

inline V2 triangle_inverse(V3 p0, V3 p1, V3 p2, V3 reference, V3 direction) {
    const V3 a = unit(p0 - reference), b = unit(p1 - reference), c = unit(p2 - reference);
    V3 cp = unit(cross(cross(b, direction), cross(c, a)));
    if (dot(cp, a + c) < 0)
        cp = -cp;
    const float u0 = triangle_area(a, b, cp) / triangle_area(a, b, c);
    const float u1 = dot(direction - b, direction - b) / dot(cp - b, cp - b);
    return {clamp01(u0), clamp01(u1)};
}

inline Weights patch_jacobians(V3 p00, V3 p10, V3 p01, V3 p11) {
    return {length3(cross(p10 - p00, p01 - p00)), length3(cross(p10 - p00, p11 - p10)),
            length3(cross(p01 - p00, p11 - p01)), length3(cross(p11 - p10, p11 - p01))};
}
inline bool rectangle(V3 p00, V3 p10, V3 p01, V3 p11) {
    const V3 ex = p10 - p00, ey = p01 - p00;
    const float x2 = dot(ex, ex), y2 = dot(ey, ey);
    if (!(x2 > 0 && y2 > 0))
        return false;
    // Only accept an affine rectangle: a near-parallelogram or shear must keep
    // its actual bilinear Jacobian rather than acquire a rectangular density.
    const V3 diagonal_error = p11 - (p00 + ex + ey);
    return dot(diagonal_error, diagonal_error) <= 0x1p-40f * max(x2, y2) &&
           abs(dot(ex, ey)) <= 0x1p-20f * sqrt(x2 * y2);
}
inline float rectangle_solid_angle(V3 p00, V3 p10, V3 p01, V3 p11, V3 reference) {
    return triangle_solid_angle(p00, p10, p11, reference) +
           triangle_solid_angle(p00, p11, p01, reference);
}
inline Weights rectangle_cosines(V3 a, V3 b, V3 c, V3 d, V3 reference, V3 normal) {
    return {max(.01f, abs(dot(unit(a - reference), normal))),
            max(.01f, abs(dot(unit(b - reference), normal))),
            max(.01f, abs(dot(unit(c - reference), normal))),
            max(.01f, abs(dot(unit(d - reference), normal)))};
}

struct RectangleFrame {
    V3 x, y, z;
    float x0, x1, y0, y1, z0, b0, b1, g2, g3, area;
};
inline RectangleFrame rectangle_frame(V3 reference, V3 p00, V3 ex, V3 ey) {
    RectangleFrame r;
    r.x = unit(ex);
    r.y = unit(ey);
    r.z = cross(r.x, r.y);
    const V3 delta = p00 - reference;
    r.x0 = dot(delta, r.x);
    r.x1 = r.x0 + length3(ex);
    r.y0 = dot(delta, r.y);
    r.y1 = r.y0 + length3(ey);
    r.z0 = dot(delta, r.z);
    if (r.z0 > 0) {
        r.z = -r.z;
        r.z0 = -r.z0;
    }
    const V3 a = {r.x0, r.y0, r.z0}, b = {r.x1, r.y0, r.z0};
    const V3 c = {r.x1, r.y1, r.z0}, d = {r.x0, r.y1, r.z0};
    const V3 n0 = unit(cross(a, b));
    const V3 n2 = unit(cross(c, d)), n3 = unit(cross(d, a));
    r.g2 = angle(-n2, n3);
    r.g3 = angle(-n3, n0);
    r.b0 = n0.z;
    r.b1 = n2.z;
    r.area = rectangle_solid_angle(p00, p00 + ex, p00 + ey, p00 + ex + ey, reference);
    return r;
}
inline V2 rectangle_sample(RectangleFrame r, V2 u) {
    // Equivalent to PBRT's angle interpolation, using a stable solid angle.
    // Above the caller's 1e-4 threshold always use the spherical transform;
    // PBRT's internal <1e-3 uniform-point shortcut has no matching Jacobian.
    const float au = u.x * r.area - r.g2 - r.g3;
    const float fu = (cos(au) * r.b0 - r.b1) / sin(au);
    const float cu = min(one_minus_epsilon,
                         max(-one_minus_epsilon, copysign(1 / sqrt(fu * fu + r.b0 * r.b0), fu)));
    const float xu = min(r.x1, max(r.x0, -cu * r.z0 / safe_sqrt(1 - cu * cu)));
    const float dd = sqrt(xu * xu + r.z0 * r.z0);
    const float h0 = r.y0 / sqrt(dd * dd + r.y0 * r.y0);
    const float h1 = r.y1 / sqrt(dd * dd + r.y1 * r.y1);
    const float hv = h0 + u.y * (h1 - h0);
    const float yv = min(r.y1, max(r.y0, hv * dd / safe_sqrt(1 - hv * hv)));
    return {clamp01((xu - r.x0) / (r.x1 - r.x0)), clamp01((yv - r.y0) / (r.y1 - r.y0))};
}
inline V2 rectangle_inverse(RectangleFrame r, V2 uv) {
    float xu = r.x0 + uv.x * (r.x1 - r.x0);
    const float yv = r.y0 + uv.y * (r.y1 - r.y0);
    if (xu == 0)
        xu = 1e-10f;
    const float fusq = max(0.f, 1 + r.z0 * r.z0 / (xu * xu) - r.b0 * r.b0);
    const float fu = copysign(sqrt(fusq), xu);
    const float root = safe_sqrt(r.b0 * r.b0 - r.b1 * r.b1 + fusq);
    float au = atan2(-r.b1 * fu - copysign(r.b0 * root, fu * r.b0), r.b0 * r.b1 - root * abs(fu));
    if (au > 0)
        au -= 2 * pi;
    const float u0 = (au + r.g2 + r.g3) / r.area;
    const float dd2 = xu * xu + r.z0 * r.z0;
    const float h0 = r.y0 / sqrt(dd2 + r.y0 * r.y0);
    const float h1 = r.y1 / sqrt(dd2 + r.y1 * r.y1);
    const float hv = yv / sqrt(dd2 + yv * yv);
    return {clamp01(u0), clamp01((hv - h0) / (h1 - h0))};
}

inline float sphere_cone_pdf(float radius, float distance2) {
    const float sin2 = radius * radius / distance2;
    const float one_minus_cos = sin2 < .00068523f ? sin2 / 2 : 1 - safe_sqrt(1 - sin2);
    return 1 / (2 * pi * one_minus_cos);
}
inline V3 sphere_cone_point(V3 center, float radius, V3 reference, V2 u) {
    const V3 axis = unit(center - reference);
    const float sin2max = radius * radius / dot(center - reference, center - reference);
    float ct = 1 + (safe_sqrt(1 - sin2max) - 1) * u.x;
    float sin2 = 1 - ct * ct;
    if (sin2max < .00068523f) {
        sin2 = sin2max * u.x;
        ct = safe_sqrt(1 - sin2);
    }
    const float ca = sin2 / sqrt(sin2max) + ct * safe_sqrt(1 - sin2 / sin2max);
    const float sa = safe_sqrt(1 - ca * ca), phi = 2 * pi * u.y;
    const V3 helper = abs(axis.x) > .9f ? V3{0, 1, 0} : V3{1, 0, 0};
    const V3 x = unit(cross(helper, axis)), y = cross(axis, x);
    return center - radius * (sa * cos(phi) * x + sa * sin(phi) * y + ca * axis);
}
} // namespace rypath_light_geometry_math
#undef RYPATH_LG_THREAD
