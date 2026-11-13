// SPDX-License-Identifier: Apache-2.0
// Ruling intersection and compensated products adapted from pbrt-v4.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// License text: third_party/pbrt/LICENSE.txt.

#pragma once

#include <metal_stdlib>

struct BilinearIntersection {
    bool hit;
    float t;
    metal::float2 uv;
};

inline float bilinear_max_abs(metal::float3 value) {
    return metal::max(metal::abs(value.x), metal::max(metal::abs(value.y), metal::abs(value.z)));
}

// Float pairs carry the rounding error through projection and the quadratic.
// Near tangency, the leading terms of the discriminant can nearly cancel.
struct BilinearNumber {
    float high;
    float low;

    BilinearNumber(float value = 0.0f) : high(value), low(0.0f) {}
    BilinearNumber(float h, float l) : high(h), low(l) {}

    BilinearNumber operator-() const { return {-high, -low}; }

    BilinearNumber operator+(BilinearNumber rhs) const {
        const float sum = high + rhs.high;
        const float rhs_part = sum - high;
        const float error = (high - (sum - rhs_part)) + (rhs.high - rhs_part) + (low + rhs.low);
        const float rounded = sum + error;
        return {rounded, error - (rounded - sum)};
    }

    BilinearNumber operator-(BilinearNumber rhs) const { return *this + -rhs; }

    BilinearNumber operator*(BilinearNumber rhs) const {
        const float product = high * rhs.high;
        const float error = metal::fma(high, rhs.high, -product) + high * rhs.low + low * rhs.high;
        const float rounded = product + error;
        return {rounded, error - (rounded - product)};
    }

    BilinearNumber operator/(BilinearNumber rhs) const {
        const float quotient = high / rhs.high;
        if (!metal::isfinite(quotient)) {
            return {quotient, 0.0f};
        }
        const float correction = (*this - rhs * quotient).high / rhs.high;
        return BilinearNumber(quotient) + correction;
    }

    BilinearNumber scale(int exponent) const {
        return {metal::ldexp(high, exponent), metal::ldexp(low, exponent)};
    }
};

struct BilinearProjection {
    BilinearNumber x, y, z;

    BilinearProjection operator+(BilinearProjection rhs) const {
        return {x + rhs.x, y + rhs.y, z + rhs.z};
    }

    BilinearProjection operator-(BilinearProjection rhs) const {
        return {x - rhs.x, y - rhs.y, z - rhs.z};
    }

    BilinearProjection operator*(BilinearNumber s) const { return {x * s, y * s, z * s}; }
};

inline BilinearNumber bilinear_determinant(BilinearProjection a, BilinearProjection b) {
    return a.x * b.y - a.y * b.x;
}

inline float projected_magnitude(BilinearProjection a) {
    return metal::max(metal::abs(a.x.high), metal::abs(a.y.high));
}

inline float minimum_projected_ruling(BilinearProjection edge, BilinearProjection warp) {
    const metal::float2 start(edge.x.high, edge.y.high);
    const metal::float2 step(warp.x.high, warp.y.high);
    const float scale = metal::max(metal::abs(step.x), metal::abs(step.y));
    if (scale == 0.0f) {
        return projected_magnitude(edge);
    }
    const auto unit_step = step / scale;
    const float t = metal::clamp(
        -metal::dot(start, unit_step) / (scale * metal::dot(unit_step, unit_step)), 0.0f, 1.0f);
    return projected_magnitude(edge + warp * t);
}

inline BilinearProjection project_bilinear_corner(metal::float3 p, metal::float3 origin,
                                                  metal::float3 direction, int position_exponent,
                                                  uint dominant) {
    const uint i = (dominant + 1) % 3;
    const uint j = (dominant + 2) % 3;
    const auto x = BilinearNumber(p[i]).scale(-position_exponent) -
                   BilinearNumber(origin[i]).scale(-position_exponent);
    const auto y = BilinearNumber(p[j]).scale(-position_exponent) -
                   BilinearNumber(origin[j]).scale(-position_exponent);
    const auto z = BilinearNumber(p[dominant]).scale(-position_exponent) -
                   BilinearNumber(origin[dominant]).scale(-position_exponent);
    // Project perpendicular to the ray without dividing. Store the dominant
    // coordinate too, to recover t directly without intersecting nearly parallel lines.
    return {x * direction[dominant] - z * direction[i], y * direction[dominant] - z * direction[j],
            z};
}

inline bool bilinear_unit_interval(BilinearNumber value) {
    return value.high >= 0.0f && (value - 1.0f).high <= 0.0f;
}

inline void consider_bilinear_edge(BilinearProjection start, BilinearProjection end,
                                   metal::float2 uv_start, metal::float2 uv_end, float direction,
                                   int ray_exponent, float t_min,
                                   thread BilinearIntersection &best) {
    const auto difference = start - end;
    const bool use_x = metal::abs(difference.x.high) >= metal::abs(difference.y.high);
    const auto from = use_x ? start.x : start.y;
    const auto to = use_x ? end.x : end.y;
    const auto denominator = use_x ? difference.x : difference.y;
    // A projected segment must bracket the ray in the better-conditioned axis.
    // If its projection collapses to a point, it has no isolated edge hit.
    if (denominator.high == 0.0f ||
        ((from.high > 0.0f && to.high > 0.0f) || (from.high < 0.0f && to.high < 0.0f))) {
        return;
    }

    // In the other axis require collinearity to the precision of the
    // compensated determinant. The scale follows the cancelling products.
    const auto left = start.x * end.y;
    const auto right = start.y * end.x;
    // Each float pair retains about 48 significant bits. Allow a small
    // accumulation margin for the projected corner and determinant products.
    constexpr float pair_roundoff = 0x1p-42f;
    const float budget = pair_roundoff * (metal::abs(left.high) + metal::abs(right.high));
    if (!(metal::abs((left - right).high) <= budget)) {
        return;
    }

    // This interpolation is symmetric in the two endpoints. Unlike an
    // unconstrained quadratic root, it cannot cross the patch boundary.
    const auto z = (end.z * from - start.z * to) / denominator;
    const float t = (z / direction).scale(ray_exponent).high;
    if (metal::isfinite(t) && t >= t_min && t <= best.t) {
        best = {true, t, metal::mix(uv_start, uv_end, (from / denominator).high)};
    }
}

inline void consider_bilinear_root(BilinearNumber u, BilinearProjection origin,
                                   BilinearProjection edge_u, BilinearProjection edge_v,
                                   BilinearProjection warp, float direction, int ray_exponent,
                                   float t_min, bool swapped, thread BilinearIntersection &best) {
    if (!bilinear_unit_interval(u)) {
        return;
    }
    const auto delta = origin + edge_u * u;
    const auto ruling = edge_v + warp * u;
    const bool use_x = metal::abs(ruling.x.high) >= metal::abs(ruling.y.high);
    const auto denominator = use_x ? ruling.x : ruling.y;
    if (denominator.high == 0.0f) {
        // The ruling is parallel to the ray and supplies no isolated hit.
        return;
    }
    const auto v = -(use_x ? delta.x : delta.y) / denominator;
    if (!bilinear_unit_interval(v)) {
        return;
    }

    const auto point = delta + ruling * v;
    // Check the remaining projected equation. The relative allowance is
    // for rounding in the compensated products and root, not for expanding
    // the patch domain or ray interval.
    constexpr float residual_factor = 0x1p-39f;
    const float budget =
        residual_factor * (projected_magnitude(origin) + projected_magnitude(edge_u) +
                           projected_magnitude(edge_v) + projected_magnitude(warp));
    if (!(projected_magnitude(point) <= budget)) {
        return;
    }

    const float t = (point.z / direction).scale(ray_exponent).high;
    if (metal::isfinite(t) && t >= t_min && t <= best.t) {
        best = {true, t, swapped ? metal::float2(v.high, u.high) : metal::float2(u.high, v.high)};
    }
}

inline BilinearIntersection intersect_bilinear(metal::float3 p00, metal::float3 p10,
                                               metal::float3 p01, metal::float3 p11,
                                               metal::float3 origin, metal::float3 direction,
                                               float t_min, float t_max) {
    BilinearIntersection best{false, t_max, {0.0f, 0.0f}};
    const float position_max =
        metal::max(metal::max(bilinear_max_abs(p00), bilinear_max_abs(p10)),
                   metal::max(metal::max(bilinear_max_abs(p01), bilinear_max_abs(p11)),
                              bilinear_max_abs(origin)));
    const float direction_max = bilinear_max_abs(direction);
    if (!(position_max > 0.0f && direction_max > 0.0f && metal::isfinite(position_max) &&
          metal::isfinite(direction_max))) {
        return best;
    }

    int position_exponent, direction_exponent;
    metal::frexp(position_max, position_exponent);
    metal::frexp(direction_max, direction_exponent);
    direction = metal::ldexp(direction, -direction_exponent);
    const auto magnitude = metal::abs(direction);
    const uint dominant = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z
                              ? 0
                              : (magnitude.y >= magnitude.z ? 1 : 2);

    const auto offset =
        project_bilinear_corner(p00, origin, direction, position_exponent, dominant);
    const auto corner_u =
        project_bilinear_corner(p10, origin, direction, position_exponent, dominant);
    const auto corner_v =
        project_bilinear_corner(p01, origin, direction, position_exponent, dominant);
    const auto opposite =
        project_bilinear_corner(p11, origin, direction, position_exponent, dominant);
    auto edge_u = corner_u - offset;
    auto edge_v = corner_v - offset;
    const auto warp = (opposite - offset) - edge_u - edge_v;
    const int ray_exponent = position_exponent - direction_exponent;

    // Prefer the family that stays farther from parallel to the ray across
    // its entire parameter interval.
    const bool swapped =
        minimum_projected_ruling(edge_u, warp) > minimum_projected_ruling(edge_v, warp);
    if (swapped) {
        const auto first = edge_u;
        edge_u = edge_v;
        edge_v = first;
    }

    // The projected offset to the ruling at u must be parallel to that ruling.
    const auto a = bilinear_determinant(edge_u, warp);
    const auto b = bilinear_determinant(offset, warp) + bilinear_determinant(edge_u, edge_v);
    const auto c = bilinear_determinant(offset, edge_v);
    if (a.high == 0.0f && b.high == 0.0f && c.high == 0.0f) {
        return best;
    }

    // Solve each straight boundary independently of quadratic root rounding.
    // Neighboring patches use the same endpoint coordinates, scaled by powers
    // of two, even when their ruling families differ.
    const float dominant_direction = direction[dominant];
    consider_bilinear_edge(offset, corner_u, {0, 0}, {1, 0}, dominant_direction, ray_exponent,
                           t_min, best);
    consider_bilinear_edge(offset, corner_v, {0, 0}, {0, 1}, dominant_direction, ray_exponent,
                           t_min, best);
    consider_bilinear_edge(corner_u, opposite, {1, 0}, {1, 1}, dominant_direction, ray_exponent,
                           t_min, best);
    consider_bilinear_edge(corner_v, opposite, {0, 1}, {1, 1}, dominant_direction, ray_exponent,
                           t_min, best);

    if (a.high == 0.0f) {
        if (b.high != 0.0f) {
            consider_bilinear_root(-c / b, offset, edge_u, edge_v, warp, dominant_direction,
                                   ray_exponent, t_min, swapped, best);
        }
        return best;
    }

    int coefficient_exponent;
    metal::frexp(bilinear_max_abs({a.high, b.high, c.high}), coefficient_exponent);
    const auto scaled_a = a.scale(-coefficient_exponent);
    const auto scaled_b = b.scale(-coefficient_exponent);
    const auto scaled_c = c.scale(-coefficient_exponent);
    const auto discriminant = scaled_b * scaled_b - scaled_a * scaled_c * 4.0f;
    if (discriminant.high < 0.0f) {
        return best;
    }
    if (discriminant.high == 0.0f) {
        consider_bilinear_root(scaled_b * -0.5f / scaled_a, offset, edge_u, edge_v, warp,
                               dominant_direction, ray_exponent, t_min, swapped, best);
        return best;
    }

    const float root = metal::sqrt(discriminant.high);
    const auto correction = (discriminant - BilinearNumber(root) * root) / (2.0f * root);
    const auto square_root = BilinearNumber(root) + correction;
    const auto q = (scaled_b + (scaled_b.high < 0.0f ? -square_root : square_root)) * -0.5f;
    consider_bilinear_root(q / scaled_a, offset, edge_u, edge_v, warp, dominant_direction,
                           ray_exponent, t_min, swapped, best);
    consider_bilinear_root(scaled_c / q, offset, edge_u, edge_v, warp, dominant_direction,
                           ray_exponent, t_min, swapped, best);
    return best;
}
