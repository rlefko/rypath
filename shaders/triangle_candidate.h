// SPDX-License-Identifier: Apache-2.0
// Compensated projection adapted from PBRT v4 and rypath bilinear arithmetic.
// PBRT is Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, Greg Humphreys.
// See third_party/pbrt/LICENSE.txt. Finite pairs are not exact predicates.
// PBRT ray-aligned projected edges; existing BilinearNumber representation/formulas.
#pragma once
#include "../src/geometry_wire.h"
#include "../src/triangle_extended.h"
#include "../src/triangle_product_guard.h"
#include "bilinear.h"
using namespace metal;
struct PreciseTriangleCandidate {
    bool valid, hit;
    BilinearNumber t;
    float2 uv;
    triangle_extended::Pair distance{};
};
inline int precise_triangle_sign(BilinearNumber x) {
    return x.high < 0 ? -1 : x.high > 0 ? 1 : x.low < 0 ? -1 : x.low > 0 ? 1 : 0;
}
inline bool pc_normal(float x) {
    uint u = as_type<uint>(x) & 0x7fffffff;
    return u == 0 || ((u >> 23) > 0 && (u >> 23) < 255);
}
inline float pc_check(float x, thread bool &ok) {
    ok = ok && pc_normal(x);
    return x;
}
inline float pc_add(float a, float b, thread bool &ok) {
    float s = a + b;
    if (s == 0 && a != -b)
        ok = false;
    return pc_check(s, ok);
}
// Cross terms retain only their rounded result, not its product residual.
inline float pc_mul(float a, float b, thread bool &ok) {
    if (!pc_normal(a) || !pc_normal(b))
        ok = false;
    const float product = a * b;
    if (a != 0 && b != 0 && product == 0 &&
        !rypath::triangle_numeric::rounded_product_is_zero(as_type<uint>(a), as_type<uint>(b)))
        ok = false;
    return pc_check(product, ok);
}
// Specifically the retained error of the already-rounded high*high product,
// not a general FMA validator. Normal float residuals may have very low bits.
inline float pc_product_error(float a, float b, float primary, thread bool &ok) {
    using namespace rypath::triangle_numeric;
    const auto domain =
        normal_product_residual_domain(as_type<uint>(a), as_type<uint>(b), as_type<uint>(primary));
    if (domain.supported) {
        const float error = fma(a, b, -primary);
        ok = ok && abs(error) <= as_type<float>(domain.half_ulp_bits);
        return pc_check(error, ok);
    }
    const auto range =
        product_residual_range(as_type<uint>(a), as_type<uint>(b), as_type<uint>(primary));
    if (range == unsupported)
        ok = false;
    const float error = fma(a, b, -primary);
    if ((range == normal_residual && error == 0) || (range == exact_product && error != 0))
        ok = false;
    return pc_check(error, ok);
}
inline float pc_div(float a, float b, thread bool &ok) {
    float q = a / b;
    if (a != 0 && q == 0)
        ok = false;
    return pc_check(q, ok);
}
// Compare the requested scale before adding it: e may be any signed int.
inline bool pc_normal_scale_exponent(int exponent, int e) {
    return exponent >= 1 && exponent <= 254 && e >= 1 - exponent && e <= 254 - exponent;
}
// All nonnormal inputs retain the original builtin's special-case semantics.
inline int pc_ilogb_abs(float x) {
    const uint exponent = (as_type<uint>(x) >> 23) & 255u;
    return exponent > 0 && exponent < 255 ? int(exponent) - 127 : ilogb(abs(x));
}
inline float pc_scale(float x, int e, thread bool &ok) {
    if ((as_type<uint>(x) & 0x7fffffff) == 0)
        return x;
    const uint bits = as_type<uint>(x);
    const int exponent = int((bits >> 23) & 255u);
    if (pc_normal_scale_exponent(exponent, e))
        return as_type<float>((bits & 0x807fffffu) | (uint(exponent + e) << 23));
    float y = ldexp(x, e);
    if (!pc_normal(y) || as_type<uint>(ldexp(y, -e)) != as_type<uint>(x))
        ok = false;
    return y;
}
inline BilinearNumber pc_add(BilinearNumber a, BilinearNumber b, thread bool &ok) {
    float sum = pc_add(a.high, b.high, ok), part = pc_add(sum, -a.high, ok);
    float error =
        pc_add(pc_add(pc_add(a.high, -pc_add(sum, -part, ok), ok), pc_add(b.high, -part, ok), ok),
               pc_add(a.low, b.low, ok), ok);
    float rounded = pc_add(sum, error, ok);
    return {rounded, pc_add(error, -pc_add(rounded, -sum, ok), ok)};
}
inline BilinearNumber pc_sub(BilinearNumber a, BilinearNumber b, thread bool &ok) {
    return pc_add(a, -b, ok);
}
inline BilinearNumber pc_mul_ordered(BilinearNumber a, BilinearNumber b, thread bool &ok) {
    float product = pc_mul(a.high, b.high, ok);
    float error =
        pc_add(pc_add(pc_product_error(a.high, b.high, product, ok), pc_mul(a.high, b.low, ok), ok),
               pc_mul(a.low, b.high, ok), ok);
    float rounded = pc_add(product, error, ok);
    return {rounded, pc_add(error, -pc_add(rounded, -product, ok), ok)};
}
// Canonical keys are invariant under negating a whole operand. If one
// historical ordering loses numeric range, retain the opposite supported graph.
inline bool pc_pair_before(BilinearNumber a, BilinearNumber b) {
    float ah = abs(a.high), bh = abs(b.high), al = abs(a.low), bl = abs(b.low);
    if (ah != bh)
        return ah < bh;
    if (al != bl)
        return al < bl;
    bool ar = al != 0 && ((as_type<uint>(a.high) ^ as_type<uint>(a.low)) >> 31);
    bool br = bl != 0 && ((as_type<uint>(b.high) ^ as_type<uint>(b.low)) >> 31);
    return ar < br;
}
inline BilinearNumber pc_mul(BilinearNumber a, BilinearNumber b, thread bool &ok) {
    if (pc_pair_before(b, a)) {
        auto t = a;
        a = b;
        b = t;
    }
    bool local = true;
    auto result = pc_mul_ordered(a, b, local);
    if (!local) {
        local = true;
        result = pc_mul_ordered(b, a, local);
    }
    ok = ok && local;
    return result;
}
inline BilinearNumber pc_div(BilinearNumber a, BilinearNumber b, thread bool &ok) {
    float q = pc_div(a.high, b.high, ok);
    if (!isfinite(q))
        return {q, 0};
    float correction = pc_div(pc_sub(a, pc_mul(b, BilinearNumber(q), ok), ok).high, b.high, ok);
    return pc_add(BilinearNumber(q), BilinearNumber(correction), ok);
}
inline BilinearNumber pc_scale(BilinearNumber x, int e, thread bool &ok) {
    return {pc_scale(x.high, e, ok), pc_scale(x.low, e, ok)};
}
inline BilinearProjection pc_project(float3 p, float3 o, float3 d, uint dominant, thread bool &ok) {
    uint i = (dominant + 1) % 3, j = (dominant + 2) % 3;
    auto x = pc_sub(BilinearNumber(p[i]), BilinearNumber(o[i]), ok),
         y = pc_sub(BilinearNumber(p[j]), BilinearNumber(o[j]), ok),
         z = pc_sub(BilinearNumber(p[dominant]), BilinearNumber(o[dominant]), ok);
    return {
        pc_sub(pc_mul(x, BilinearNumber(d[dominant]), ok), pc_mul(z, BilinearNumber(d[i]), ok), ok),
        pc_sub(pc_mul(y, BilinearNumber(d[dominant]), ok), pc_mul(z, BilinearNumber(d[j]), ok), ok),
        z};
}
inline BilinearNumber pc_det(BilinearProjection a, BilinearProjection b, thread bool &ok) {
    return pc_sub(pc_mul(a.x, b.y, ok), pc_mul(a.y, b.x, ok), ok);
}
inline float pc_abs(BilinearNumber x) { return max(abs(x.high), abs(x.low)); }
// Positions remain compensated through the forward instance transform. Each row
// is preconditioned before products, never rounded into a world-space float3.
struct TrianglePoint {
    BilinearNumber value[3];
};
inline BilinearProjection pc_project(TrianglePoint p, TrianglePoint o, float3 d, uint dominant,
                                     thread bool &ok) {
    uint i = (dominant + 1) % 3, j = (dominant + 2) % 3;
    auto x = pc_sub(p.value[i], o.value[i], ok), y = pc_sub(p.value[j], o.value[j], ok),
         z = pc_sub(p.value[dominant], o.value[dominant], ok);
    return {
        pc_sub(pc_mul(x, BilinearNumber(d[dominant]), ok), pc_mul(z, BilinearNumber(d[i]), ok), ok),
        pc_sub(pc_mul(y, BilinearNumber(d[dominant]), ok), pc_mul(z, BilinearNumber(d[j]), ok), ok),
        z};
}
inline BilinearNumber pc_transform_product(float coefficient, float coordinate, int exponent,
                                           thread bool &ok) {
    if ((as_type<uint>(coefficient) & 0x7fffffff) == 0 ||
        (as_type<uint>(coordinate) & 0x7fffffff) == 0)
        return BilinearNumber(0);
    int ce = pc_ilogb_abs(coefficient), pe = pc_ilogb_abs(coordinate);
    auto product = pc_mul(BilinearNumber(pc_scale(coefficient, -ce, ok)),
                          BilinearNumber(pc_scale(coordinate, -pe, ok)), ok);
    return pc_scale(product, ce + pe - exponent, ok);
}
inline PreciseTriangleCandidate
precise_triangle_candidate_fast(const device rypath::geometry_wire::Triangle &triangle,
                                float4x3 transform, float3 origin, float3 direction, float t_min,
                                float t_max) {
    PreciseTriangleCandidate miss{true, false, BilinearNumber(INFINITY), float2(0)};
    bool ok = true;
    if (!all(isfinite(origin)) || !all(isfinite(direction)) || !isfinite(t_min) || isnan(t_max) ||
        t_min < 0 || t_max < t_min || all(direction == 0) || !pc_normal(t_min) ||
        (isfinite(t_max) && !pc_normal(t_max))) {
        miss.valid = false;
        return miss;
    }
    // Reject raw subnormals by bits before exponent selection; FTZ arithmetic
    // must not reinterpret a nonzero source coordinate or matrix term as zero.
    for (uint axis = 0; axis < 3; ++axis) {
        if (!pc_normal(origin[axis]) || !pc_normal(direction[axis])) {
            miss.valid = false;
            return miss;
        }
        for (uint col = 0; col < 4; ++col)
            if (!pc_normal(transform[col][axis])) {
                miss.valid = false;
                return miss;
            }
    }
    TrianglePoint points[3], o;
    int3 exponents;
    int de = -10000;
    for (uint axis = 0; axis < 3; ++axis) {
        int exponent = -10000;
        if (origin[axis] != 0)
            exponent = max(exponent, pc_ilogb_abs(origin[axis]));
        if (transform[3][axis] != 0)
            exponent = max(exponent, pc_ilogb_abs(transform[3][axis]));
        for (uint corner = 0; corner < 3; ++corner)
            for (uint j = 0; j < 3; ++j) {
                float p = triangle.corners[corner][j], m = transform[j][axis];
                if (!pc_normal(p)) {
                    miss.valid = false;
                    return miss;
                }
                if (p != 0 && m != 0)
                    exponent = max(exponent, pc_ilogb_abs(p) + pc_ilogb_abs(m) + 1);
            }
        exponents[axis] = exponent == -10000 ? 0 : exponent + 2;
        o.value[axis] = BilinearNumber(pc_scale(origin[axis], -exponents[axis], ok));
        for (uint corner = 0; corner < 3; ++corner) {
            auto sum = BilinearNumber(pc_scale(transform[3][axis], -exponents[axis], ok));
            for (uint j = 0; j < 3; ++j)
                sum = pc_add(sum,
                             pc_transform_product(transform[j][axis], triangle.corners[corner][j],
                                                  exponents[axis], ok),
                             ok);
            points[corner].value[axis] = sum;
        }
        if (direction[axis] != 0)
            de = max(de, pc_ilogb_abs(direction[axis]) - exponents[axis]);
    }
    float3 d;
    for (uint axis = 0; axis < 3; ++axis)
        d[axis] = pc_scale(direction[axis], -exponents[axis] - de, ok);
    if (!ok) {
        miss.valid = false;
        return miss;
    }
    float3 ad = abs(d);
    uint dominant = ad.x >= ad.y && ad.x >= ad.z ? 0 : ad.y >= ad.z ? 1 : 2;
    auto a = pc_project(points[0], o, d, dominant, ok),
         b = pc_project(points[1], o, d, dominant, ok),
         c = pc_project(points[2], o, d, dominant, ok);
    float sx = max(pc_abs(a.x), max(pc_abs(b.x), pc_abs(c.x))),
          sy = max(pc_abs(a.y), max(pc_abs(b.y), pc_abs(c.y)));
    int xe = sx > 0 ? -pc_ilogb_abs(sx) : 0, ye = sy > 0 ? -pc_ilogb_abs(sy) : 0;
    a.x = pc_scale(a.x, xe, ok);
    b.x = pc_scale(b.x, xe, ok);
    c.x = pc_scale(c.x, xe, ok);
    a.y = pc_scale(a.y, ye, ok);
    b.y = pc_scale(b.y, ye, ok);
    c.y = pc_scale(c.y, ye, ok);
    auto e0 = pc_det(b, c, ok), e1 = pc_det(c, a, ok), e2 = pc_det(a, b, ok);
    if (!ok) {
        miss.valid = false;
        return miss;
    }
    int s0 = precise_triangle_sign(e0), s1 = precise_triangle_sign(e1),
        s2 = precise_triangle_sign(e2);
    // Computed zero edges are closed; finite pair predicates are approximate.
    if ((s0 < 0 || s1 < 0 || s2 < 0) && (s0 > 0 || s1 > 0 || s2 > 0))
        return miss;
    auto det = pc_add(pc_add(e0, e1, ok), e2, ok);
    if (!ok) {
        miss.valid = false;
        return miss;
    }
    if (precise_triangle_sign(det) == 0)
        return miss;
    auto numerator =
        pc_add(pc_add(pc_mul(e0, a.z, ok), pc_mul(e1, b.z, ok), ok), pc_mul(e2, c.z, ok), ok);
    auto t = pc_scale(pc_div(pc_div(numerator, det, ok), BilinearNumber(d[dominant]), ok), -de, ok),
         u = pc_div(e1, det, ok), v = pc_div(e2, det, ok);
    int min_sign = precise_triangle_sign(pc_sub(t, BilinearNumber(t_min), ok));
    int max_sign =
        isfinite(t_max) ? precise_triangle_sign(pc_sub(t, BilinearNumber(t_max), ok)) : -1;
    if (!ok) {
        miss.valid = false;
        return miss;
    }
    if (min_sign < 0 || max_sign > 0)
        return miss;
    return {true, true, t, float2(u.high, v.high)};
}

// Source-domain policy stays separate from recoverable intermediate range.
inline bool pc_source_valid(const device rypath::geometry_wire::Triangle &triangle,
                            float4x3 transform, float3 origin, float3 direction, float t_min,
                            float t_max) {
    if (!all(isfinite(origin)) || !all(isfinite(direction)) || !isfinite(t_min) || isnan(t_max) ||
        t_min < 0 || t_max < t_min || all(direction == 0) || !pc_normal(t_min) ||
        (isfinite(t_max) && !pc_normal(t_max)))
        return false;
    for (uint axis = 0; axis < 3; ++axis) {
        if (!pc_normal(origin[axis]) || !pc_normal(direction[axis]))
            return false;
        for (uint col = 0; col < 4; ++col)
            if (!pc_normal(transform[col][axis]))
                return false;
        for (uint corner = 0; corner < 3; ++corner)
            if (!pc_normal(triangle.corners[corner][axis]))
                return false;
    }
    return true;
}
struct ExtendedTriangleProjection {
    triangle_extended::Pair x, y, z;
};
inline triangle_extended::Pair pc_extended(float x) {
    return triangle_extended::pair_from_bits(as_type<uint>(x));
}
inline ExtendedTriangleProjection pc_extended_project(thread const triangle_extended::Pair *p,
                                                      thread const triangle_extended::Pair *o,
                                                      thread const triangle_extended::Pair *d,
                                                      uint dominant) {
    using namespace triangle_extended;
    uint i = (dominant + 1) % 3, j = (dominant + 2) % 3;
    auto x = sub(p[i], o[i]), y = sub(p[j], o[j]), z = sub(p[dominant], o[dominant]);
    return {sub(mul(x, d[dominant]), mul(z, d[i])), sub(mul(y, d[dominant]), mul(z, d[j])), z};
}
inline triangle_extended::Pair pc_extended_det(ExtendedTriangleProjection a,
                                               ExtendedTriangleProjection b) {
    return triangle_extended::sub(triangle_extended::mul(a.x, b.y),
                                  triangle_extended::mul(a.y, b.x));
}
inline triangle_extended::Float pc_extended_abs(triangle_extended::Pair x) {
    return triangle_extended::magnitude(x.high, x.low) >= 0 ? x.high : x.low;
}
// Recompute from original float32 geometry, affine coefficients, and world ray.
// Every component has its own exponent; no damaged fast intermediate is reused.
[[clang::noinline]] inline PreciseTriangleCandidate
precise_triangle_candidate_extended(const device rypath::geometry_wire::Triangle &triangle,
                                    float4x3 transform, float3 origin, float3 direction,
                                    float t_min, float t_max) {
    using namespace triangle_extended;
    PreciseTriangleCandidate miss{true, false, BilinearNumber(INFINITY), float2(0)};
    if (!pc_source_valid(triangle, transform, origin, direction, t_min, t_max)) {
        miss.valid = false;
        return miss;
    }
    Pair points[3][3], o[3], d[3];
    int exponents[3];
    int de = -10000;
    for (uint axis = 0; axis < 3; ++axis) {
        int exponent = -10000;
        if (origin[axis] != 0)
            exponent = max(exponent, ilogb(abs(origin[axis])));
        if (transform[3][axis] != 0)
            exponent = max(exponent, ilogb(abs(transform[3][axis])));
        for (uint corner = 0; corner < 3; ++corner)
            for (uint j = 0; j < 3; ++j)
                if (triangle.corners[corner][j] != 0 && transform[j][axis] != 0)
                    exponent = max(exponent, ilogb(abs(triangle.corners[corner][j])) +
                                                 ilogb(abs(transform[j][axis])) + 1);
        exponents[axis] = exponent == -10000 ? 0 : exponent + 2;
        o[axis] = scale(pc_extended(origin[axis]), -exponents[axis]);
        for (uint corner = 0; corner < 3; ++corner) {
            auto sum = scale(pc_extended(transform[3][axis]), -exponents[axis]);
            for (uint j = 0; j < 3; ++j)
                sum = add(sum, scale(mul(pc_extended(transform[j][axis]),
                                         pc_extended(triangle.corners[corner][j])),
                                     -exponents[axis]));
            points[corner][axis] = sum;
        }
        if (direction[axis] != 0)
            de = max(de, ilogb(abs(direction[axis])) - exponents[axis]);
    }
    uint dominant = 0;
    for (uint axis = 0; axis < 3; ++axis) {
        d[axis] = scale(pc_extended(direction[axis]), -exponents[axis] - de);
        if (magnitude(d[axis].high, d[dominant].high) > 0)
            dominant = axis;
    }
    auto a = pc_extended_project(points[0], o, d, dominant),
         b = pc_extended_project(points[1], o, d, dominant),
         c = pc_extended_project(points[2], o, d, dominant);
    auto sx = pc_extended_abs(a.x), sy = pc_extended_abs(a.y);
    Float xs[2] = {pc_extended_abs(b.x), pc_extended_abs(c.x)};
    Float ys[2] = {pc_extended_abs(b.y), pc_extended_abs(c.y)};
    for (uint i = 0; i < 2; ++i) {
        if (magnitude(xs[i], sx) > 0)
            sx = xs[i];
        if (magnitude(ys[i], sy) > 0)
            sy = ys[i];
    }
    int xe = sx.mantissa ? -sx.exponent - 23 : 0, ye = sy.mantissa ? -sy.exponent - 23 : 0;
    a.x = scale(a.x, xe);
    b.x = scale(b.x, xe);
    c.x = scale(c.x, xe);
    a.y = scale(a.y, ye);
    b.y = scale(b.y, ye);
    c.y = scale(c.y, ye);
    auto e0 = pc_extended_det(b, c), e1 = pc_extended_det(c, a), e2 = pc_extended_det(a, b);
    if (!valid(e0) || !valid(e1) || !valid(e2)) {
        miss.valid = false;
        return miss;
    }
    int s0 = compare(e0, {}).order, s1 = compare(e1, {}).order, s2 = compare(e2, {}).order;
    if ((s0 < 0 || s1 < 0 || s2 < 0) && (s0 > 0 || s1 > 0 || s2 > 0))
        return miss;
    auto determinant = add(add(e0, e1), e2);
    if (!valid(determinant)) {
        miss.valid = false;
        return miss;
    }
    if (compare(determinant, {}).order == 0)
        return miss;
    auto numerator = add(add(mul(e0, a.z), mul(e1, b.z)), mul(e2, c.z));
    auto t = scale(div(div(numerator, determinant), d[dominant]), -de), u = div(e1, determinant),
         v = div(e2, determinant);
    auto minimum = compare(t, pc_extended(t_min));
    auto maximum = isfinite(t_max) ? compare(t, pc_extended(t_max)) : Comparison{-1, true};
    if (!valid(t) || !valid(u) || !valid(v) || !minimum.valid || !maximum.valid) {
        miss.valid = false;
        return miss;
    }
    if (minimum.order < 0 || maximum.order > 0)
        return miss;
    auto th = to_bits(t.high), tl = to_bits(t.low), uh = to_bits(u.high), vh = to_bits(v.high);
    // UV/public low output may round to zero; complete distance remains intact.
    // A nonfinite public t or UV cannot be consumed by existing surface APIs.
    if (!th.finite || !tl.finite || !uh.finite || !vh.finite ||
        !rypath::triangle_numeric::normal_or_zero(th.bits) ||
        ((th.bits & 0x7fffffffu) == 0 && compare(t, {}).order != 0)) {
        miss.valid = false;
        return miss;
    }
    return {true,
            true,
            {as_type<float>(th.bits), as_type<float>(tl.bits)},
            float2(as_type<float>(uh.bits), as_type<float>(vh.bits)),
            t};
}
inline PreciseTriangleCandidate
precise_triangle_candidate(const device rypath::geometry_wire::Triangle &triangle,
                           float4x3 transform, float3 origin, float3 direction, float t_min,
                           float t_max) {
    if (!pc_source_valid(triangle, transform, origin, direction, t_min, t_max))
        return {false, false, BilinearNumber(INFINITY), float2(0)};
    auto hit =
        precise_triangle_candidate_fast(triangle, transform, origin, direction, t_min, t_max);
    if (!hit.valid)
        return precise_triangle_candidate_extended(triangle, transform, origin, direction, t_min,
                                                   t_max);
    if (hit.hit)
        hit.distance =
            triangle_extended::pair_from_bits(as_type<uint>(hit.t.high), as_type<uint>(hit.t.low));
    return hit;
}
