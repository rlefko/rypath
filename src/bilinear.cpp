// SPDX-License-Identifier: GPL-3.0-only

#include "bilinear.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rypath {
namespace {

struct Vector {
    double x, y, z;

    Vector operator+(Vector rhs) const { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
    Vector operator-(Vector rhs) const { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
    Vector operator*(double s) const { return {x * s, y * s, z * s}; }
    Vector operator/(double s) const { return {x / s, y / s, z / s}; }
};

Vector edge(GeometryPoint from, GeometryPoint to) {
    return {double{to.x} - from.x, double{to.y} - from.y, double{to.z} - from.z};
}

Vector cross(Vector a, Vector b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double dot(Vector a, Vector b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

double norm(Vector a) { return std::hypot(a.x, a.y, a.z); }

Vector max_abs(Vector a, Vector b) {
    return {std::max(std::abs(a.x), std::abs(b.x)), std::max(std::abs(a.y), std::abs(b.y)),
            std::max(std::abs(a.z), std::abs(b.z))};
}

Vector closest_on_segment(Vector a, Vector b) {
    const Vector direction = b - a;
    const double length = norm(direction);
    if (length == 0) {
        return a;
    }
    const double t = std::clamp(-dot(a, direction / length) / length, 0.0, 1.0);
    return a + direction * t;
}

Vector closest_to_origin(const std::array<Vector, 4> &corners) {
    const auto &[a, b, c, d] = corners;
    Vector closest = closest_on_segment(a, b);
    const auto consider = [&](Vector candidate) {
        if (norm(candidate) < norm(closest)) {
            closest = candidate;
        }
    };
    consider(closest_on_segment(a, c));
    consider(closest_on_segment(b, d));
    consider(closest_on_segment(c, d));

    // Also test the projection onto the interior of the parallelogram. A
    // rank-one parallelogram is covered by its boundary segments.
    const Vector span_u = b - a;
    const Vector span_v = c - a;
    const double length_u = norm(span_u);
    const double length_v = norm(span_v);
    if (length_u > 0 && length_v > 0) {
        const Vector direction_u = span_u / length_u;
        const Vector direction_v = span_v / length_v;
        const Vector normal = cross(direction_u, direction_v);
        const double area = norm(normal);
        if (area > 0) {
            const Vector unit = normal / area;
            const double u = dot(cross(direction_v, a), unit) / area / length_u;
            const double v = dot(cross(a, direction_u), unit) / area / length_v;
            if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                consider(a + span_u * u + span_v * v);
            }
        }
    }
    return closest;
}

} // namespace

std::optional<std::array<double, 2>>
bilinear_jacobian_bounds(const std::array<GeometryPoint, 4> &corners) {
    if (!std::ranges::all_of(corners, [](GeometryPoint p) {
            return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        })) {
        return std::nullopt;
    }
    const auto &[p00, p10, p01, p11] = corners;
    const Vector u0 = edge(p00, p10);
    const Vector u1 = edge(p01, p11);
    const Vector v0 = edge(p00, p01);
    const Vector v1 = edge(p10, p11);

    // dP/du depends only on v and dP/dv only on u. Their increments are the
    // same vector, so the cross product has no uv term and is affine.
    const std::array jacobians{cross(u0, v0), cross(u0, v1), cross(u1, v0), cross(u1, v1)};
    const Vector closest = closest_to_origin(jacobians);
    const double minimum = norm(closest);
    if (!(minimum > 0) || !std::isfinite(minimum)) {
        return std::nullopt;
    }

    // Project onto a separating plane normal. Every corner projection is a
    // lower bound candidate; taking their minimum certifies the whole square
    // even when the numerically computed minimizer is imperfect.
    const Vector normal = closest / minimum;
    double lower = minimum;
    double upper = 0;
    for (const Vector j : jacobians) {
        lower = std::min(lower, dot(normal, j));
        upper = std::max(upper, norm(j));
    }

    // Bound cancellation componentwise by absolute product sums. The margin
    // covers edge subtraction, interpolation of the two edges per tangent,
    // cross products, and the double-precision norm and support-plane
    // calculations. It is relative to the patch derivatives, independent of
    // translation, and introduces no absolute minimum area.
    const Vector u = max_abs(u0, u1);
    const Vector v = max_abs(v0, v1);
    const Vector product_sums{u.y * v.z + u.z * v.y, u.z * v.x + u.x * v.z, u.x * v.y + u.y * v.x};
    const double uncertainty = 32 * std::numeric_limits<float>::epsilon() * norm(product_sums);
    if (!(lower > uncertainty) || !std::isfinite(upper)) {
        return std::nullopt;
    }
    return std::array{lower - uncertainty, upper + uncertainty};
}

} // namespace rypath
