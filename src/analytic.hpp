// SPDX-License-Identifier: GPL-3.0-only AND Apache-2.0
// PBRT analytic parameterizations adapted from pbrt-v4 b0bdf461 shapes.{h,cpp}.
// Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt and README.md.
#pragma once
#include "geometry.hpp"
#include "geometry_wire.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace rypath {
inline std::string validate_analytic(const GeometryAnalytic &s) {
    if (s.kind != AnalyticKind::sphere && s.kind != AnalyticKind::disk &&
        s.kind != AnalyticKind::cylinder)
        return "Unknown analytic shape kind";
    for (const float x : {s.radius, s.z_min, s.z_max, s.phi_max, s.inner_radius, s.height})
        if (!std::isfinite(x))
            return "Analytic shape parameters must be finite";
    if (!(s.radius > 0 && std::isnormal(s.radius)) ||
        !(std::isnormal(s.phi_max) && s.phi_max > 0 && s.phi_max <= 2 * std::numbers::pi_v<float>))
        return "Analytic radius and angular extent must be positive and representable";
    if (s.kind == AnalyticKind::disk) {
        if (!(s.inner_radius >= 0 && s.inner_radius < s.radius))
            return "Analytic disk requires 0 <= inner radius < radius";
    } else {
        if (!(s.z_min < s.z_max))
            return "Analytic surface requires positive height";
        if (!std::isnormal(s.z_max - s.z_min))
            return "Analytic surface height overflows or collapses in binary32";
        if (s.kind == AnalyticKind::sphere && (s.z_min < -s.radius || s.z_max > s.radius))
            return "Analytic sphere clipping lies outside its radius";
        if (s.kind == AnalyticKind::sphere &&
            !(std::acos(s.z_min / s.radius) > std::acos(s.z_max / s.radius)))
            return "Analytic sphere angular parameterization collapses in binary32";
    }
    // Deliberately bounded shader domain: leave headroom for binary32
    // squared radii, quadratics, parameter derivatives and flush-to-zero.
    constexpr float minimum = 0x1p-16f, maximum = 0x1p16f;
    if (s.radius < minimum || s.radius > maximum || s.phi_max < minimum ||
        std::abs(s.z_min) > maximum || std::abs(s.z_max) > maximum ||
        std::abs(s.height) > maximum ||
        (s.kind == AnalyticKind::disk
             ? s.radius - s.inner_radius < minimum
             : (s.z_max - s.z_min < minimum || s.z_max - s.z_min > maximum)))
        return "Analytic parameters exceed the shader numerical domain (lengths 2^-16..2^16, "
               "angular extent >= 2^-16)";
    return {};
}
inline geometry_wire::Analytic pack_analytic(const GeometryAnalytic &s) {
    return {geometry_wire::analytic,
            static_cast<std::uint32_t>(s.kind),
            s.radius,
            s.z_min,
            s.z_max,
            s.phi_max,
            s.inner_radius,
            s.height};
}
inline std::array<GeometryPoint, 2> analytic_bounds(const GeometryAnalytic &s) {
    // Full azimuth bounds are deliberately conservative for clipped wedges.
    const float lo = s.kind == AnalyticKind::disk ? s.height : s.z_min;
    const float hi = s.kind == AnalyticKind::disk ? s.height : s.z_max;
    return {GeometryPoint{-s.radius, -s.radius, lo}, GeometryPoint{s.radius, s.radius, hi}};
}
inline double analytic_area(const GeometryAnalytic &s) {
    if (s.kind == AnalyticKind::disk)
        return .5 * s.phi_max *
               (double(s.radius) * s.radius - double(s.inner_radius) * s.inner_radius);
    return double(s.phi_max) * s.radius * (double(s.z_max) - s.z_min);
}
struct AnalyticSurface {
    GeometryPoint position{}, normal{}, dpdu{}, dpdv{}, dndu{}, dndv{};
    std::array<float, 2> uv{};
};
inline AnalyticSurface evaluate_analytic(const GeometryAnalytic &s, std::array<float, 2> uv) {
    const double phi = double(uv[0]) * s.phi_max;
    const double c = std::cos(phi), sn = std::sin(phi), r = s.radius, pm = s.phi_max;
    const auto p = [](double x, double y, double z) {
        return GeometryPoint{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    };
    AnalyticSurface out;
    out.uv = uv;
    if (s.kind == AnalyticKind::disk) {
        const double radius = r + double(uv[1]) * (double(s.inner_radius) - r);
        out.position = p(radius * c, radius * sn, s.height);
        out.normal = {0, 0, 1};
        // At the center retain the limiting azimuthal frame, rather than NaNs.
        const double frame_radius = radius > 0 ? radius : r;
        out.dpdu = p(-pm * frame_radius * sn, pm * frame_radius * c, 0);
        out.dpdv = p((s.inner_radius - r) * c, (s.inner_radius - r) * sn, 0);
    } else if (s.kind == AnalyticKind::cylinder) {
        out.position = p(r * c, r * sn, s.z_min + double(uv[1]) * (s.z_max - double(s.z_min)));
        out.normal = p(c, sn, 0);
        out.dpdu = p(-pm * r * sn, pm * r * c, 0);
        out.dpdv = p(0, 0, double(s.z_max) - s.z_min);
        out.dndu = p(-pm * sn, pm * c, 0);
    } else {
        const double t0 = std::acos(std::clamp(double(s.z_min) / r, -1., 1.));
        const double t1 = std::acos(std::clamp(double(s.z_max) / r, -1., 1.));
        const double dt = t1 - t0, theta = t0 + uv[1] * dt;
        const double st = std::sin(theta), ct = std::cos(theta);
        out.position = p(r * st * c, r * st * sn, r * ct);
        out.normal = p(st * c, st * sn, ct);
        const double frame_sin = std::abs(st) > 1e-7 ? st : 1e-7;
        out.dpdu = p(-pm * r * frame_sin * sn, pm * r * frame_sin * c, 0);
        out.dpdv = p(dt * r * ct * c, dt * r * ct * sn, -dt * r * st);
        out.dndu = p(-pm * frame_sin * sn, pm * frame_sin * c, 0);
        out.dndv = p(dt * ct * c, dt * ct * sn, -dt * st);
    }
    return out;
}
inline AnalyticSurface sample_analytic_area(const GeometryAnalytic &s, std::array<float, 2> u) {
    std::array<float, 2> uv{u[1], u[0]};
    if (s.kind == AnalyticKind::sphere) {
        const double r = s.radius;
        const double z = s.z_min + double(u[0]) * (double(s.z_max) - s.z_min);
        const double t0 = std::acos(std::clamp(double(s.z_min) / r, -1., 1.));
        const double t1 = std::acos(std::clamp(double(s.z_max) / r, -1., 1.));
        uv[1] = static_cast<float>((std::acos(std::clamp(z / r, -1., 1.)) - t0) / (t1 - t0));
    } else if (s.kind == AnalyticKind::disk) {
        const double rr = double(s.radius) * s.radius, ir = double(s.inner_radius) * s.inner_radius;
        const double radius = std::sqrt(ir + double(u[0]) * (rr - ir));
        uv[1] = static_cast<float>((s.radius - radius) / (s.radius - double(s.inner_radius)));
    }
    return evaluate_analytic(s, uv);
}
// dA_world / dA_object = |det M| |M^-T n|, evaluated via the cofactor matrix.
inline double analytic_area_jacobian(GeometryPoint n, const GeometryTransform &xf) {
    const auto &m = xf.values;
    const double x = (double(m[5]) * m[10] - double(m[6]) * m[9]) * n.x +
                     (double(m[6]) * m[8] - double(m[4]) * m[10]) * n.y +
                     (double(m[4]) * m[9] - double(m[5]) * m[8]) * n.z;
    const double y = (double(m[2]) * m[9] - double(m[1]) * m[10]) * n.x +
                     (double(m[0]) * m[10] - double(m[2]) * m[8]) * n.y +
                     (double(m[1]) * m[8] - double(m[0]) * m[9]) * n.z;
    const double z = (double(m[1]) * m[6] - double(m[2]) * m[5]) * n.x +
                     (double(m[2]) * m[4] - double(m[0]) * m[6]) * n.y +
                     (double(m[0]) * m[5] - double(m[1]) * m[4]) * n.z;
    return std::hypot(x, y, z);
}

// Classify a rounded affine map using its Gram matrix. The tolerance permits
// binary32 rotation roundoff, not materially nonuniform scale or shear.
inline std::optional<double> analytic_similarity_scale_squared(const GeometryTransform &xf) {
    const auto &m = xf.values;
    std::array<std::array<double, 3>, 3> c{
        {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}}};
    const auto dot = [](const auto &a, const auto &b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const double scale = (dot(c[0], c[0]) + dot(c[1], c[1]) + dot(c[2], c[2])) / 3;
    if (!(scale > 0 && std::isfinite(scale)))
        return {};
    const double tolerance = 64 * std::numeric_limits<float>::epsilon() * scale;
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::abs(dot(c[i], c[i]) - scale) > tolerance)
            return {};
        for (std::size_t j = i + 1; j < 3; ++j)
            if (std::abs(dot(c[i], c[j])) > tolerance)
                return {};
    }
    return scale;
}

// Exact area rule in the declared power-calibration profile. Curved affine
// nonsimilarities need a separate numerical integration contract.
inline std::optional<double> analytic_world_area(const GeometryAnalytic &s,
                                                 const GeometryTransform &xf) {
    if (s.kind == AnalyticKind::disk)
        return analytic_area(s) * analytic_area_jacobian({0, 0, 1}, xf);
    if (const auto scale = analytic_similarity_scale_squared(xf))
        return analytic_area(s) * *scale;
    return {};
}

inline std::string validate_analytic_geometry(const GeometryMesh &mesh,
                                              const GeometryTransform &xf) {
    if (mesh.analytics.empty())
        return {};
    const auto &m = xf.values;
    const std::array<double, 3> a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
    const std::array<double, 9> cof{
        b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0],
        c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0],
        a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double determinant = a[0] * cof[0] + a[1] * cof[1] + a[2] * cof[2];
    if (determinant == 0 || !std::isfinite(determinant))
        return "Analytic transform must be finite and nonsingular";
    constexpr double limit = std::numeric_limits<float>::max();
    for (const auto x : cof)
        if (!std::isfinite(x / determinant) || std::abs(x / determinant) > limit)
            return "Analytic inverse transform exceeds binary32 range";
    double linear_squared = 0, inverse_squared = 0;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column)
            linear_squared += double(m[row * 4 + column]) * m[row * 4 + column];
    for (const auto x : cof)
        inverse_squared += (x / determinant) * (x / determinant);
    const double linear_norm = std::sqrt(linear_squared);
    const double inverse_norm = std::sqrt(inverse_squared);
    if (linear_norm > 0x1p16 || inverse_norm > 0x1p16)
        return "Analytic affine linear/inverse norms exceed the shader numerical domain (2^16)";
    for (const auto &s : mesh.analytics) {
        if (const auto error = validate_analytic(s); !error.empty())
            return error;
        const double height = double(s.z_max) - s.z_min;
        const double gap = double(s.radius) - s.inner_radius;
        const double theta =
            s.kind == AnalyticKind::sphere
                ? std::acos(double(s.z_min) / s.radius) - std::acos(double(s.z_max) / s.radius)
                : 1;
        const double du = double(s.radius) * s.phi_max;
        const double dv = s.kind == AnalyticKind::disk     ? gap
                          : s.kind == AnalyticKind::sphere ? s.radius * theta
                                                           : height;
        const double minimum_frame = std::min(du * 1e-7, dv) / inverse_norm;
        const double maximum_frame = std::max(du, dv) * linear_norm;
        if (minimum_frame < 0x1p-50 || maximum_frame > 0x1p30)
            return "Analytic transformed differential frame exceeds the shader numerical domain";
        const auto bounds = analytic_bounds(s);
        for (unsigned row = 0; row < 3; ++row) {
            const std::array<double, 3> lo{bounds[0].x, bounds[0].y, bounds[0].z};
            const std::array<double, 3> hi{bounds[1].x, bounds[1].y, bounds[1].z};
            double lower = m[row * 4 + 3], upper = lower;
            for (unsigned axis = 0; axis < 3; ++axis) {
                const double x = double(m[row * 4 + axis]) * lo[axis],
                             y = double(m[row * 4 + axis]) * hi[axis];
                lower += std::min(x, y);
                upper += std::max(x, y);
            }
            if (!std::isfinite(lower) || !std::isfinite(upper) || std::abs(lower) > limit / 16 ||
                std::abs(upper) > limit / 16)
                return "Analytic world bounds exceed binary32 rendering range";
        }
        // Each point's area proposal requires a normal, finite reciprocal PDF.
        // Bound the cofactor action conservatively over all object normals.
        double upper = 0;
        for (const auto x : cof)
            upper += x * x;
        const double scale_upper = std::sqrt(upper);
        const double scale_lower = std::abs(determinant) / linear_norm;
        const double area = analytic_area(s);
        if (!(area * scale_lower >= std::numeric_limits<float>::min()) ||
            area * scale_upper > limit)
            return "Analytic transformed area density exceeds binary32 rendering range";
    }
    return {};
}
struct AnalyticIntersection {
    bool hit = false;
    double t = std::numeric_limits<double>::infinity();
    std::array<float, 2> uv{};
};
// Independent binary64 host implementation. Ray is already in object space.
inline AnalyticIntersection intersect_analytic(const GeometryAnalytic &s, const GeometryRay &ray) {
    const double ox = ray.origin.x, oy = ray.origin.y, oz = ray.origin.z;
    const double dx = ray.direction.x, dy = ray.direction.y, dz = ray.direction.z;
    double roots[2]{};
    int count = 0;
    if (s.kind == AnalyticKind::disk) {
        if (dz == 0)
            return {};
        roots[count++] = (s.height - oz) / dz;
    } else {
        const bool sphere = s.kind == AnalyticKind::sphere;
        const double a = dx * dx + dy * dy + (sphere ? dz * dz : 0);
        if (a == 0)
            return {};
        const double b = 2 * (ox * dx + oy * dy + (sphere ? oz * dz : 0));
        const double c = ox * ox + oy * oy + (sphere ? oz * oz : 0) - double(s.radius) * s.radius;
        const double projection = -b / (2 * a);
        const double vx = std::fma(projection, dx, ox), vy = std::fma(projection, dy, oy);
        const double vz = sphere ? std::fma(projection, dz, oz) : 0;
        const double length = std::hypot(vx, vy, vz);
        const double discr = 4 * a * (s.radius - length) * (s.radius + length);
        if (discr < 0)
            return {};
        const double q = -.5 * (b + std::copysign(std::sqrt(discr), b));
        if (q == 0)
            roots[count++] = projection;
        else {
            roots[count++] = q / a;
            roots[count++] = c / q;
            if (roots[0] > roots[1])
                std::swap(roots[0], roots[1]);
        }
    }
    for (int i = 0; i < count; ++i) {
        const double t = roots[i];
        if (!std::isfinite(t) || t < ray.t_min || t > ray.t_max)
            continue;
        double x = std::fma(t, dx, ox), y = std::fma(t, dy, oy), z = std::fma(t, dz, oz);
        if (s.kind == AnalyticKind::sphere) {
            const double scale = s.radius / std::hypot(x, y, z);
            x *= scale;
            y *= scale;
            z *= scale;
        }
        if (s.kind == AnalyticKind::cylinder) {
            const double scale = s.radius / std::hypot(x, y);
            x *= scale;
            y *= scale;
        }
        double phi = std::atan2(y, x);
        if (phi < 0)
            phi += 2 * std::numbers::pi;
        if (phi > s.phi_max)
            continue;
        double v;
        if (s.kind == AnalyticKind::disk) {
            const double r = std::hypot(x, y);
            if (r < s.inner_radius || r > s.radius)
                continue;
            v = (s.radius - r) / (s.radius - double(s.inner_radius));
        } else {
            if (z < s.z_min || z > s.z_max)
                continue;
            if (s.kind == AnalyticKind::sphere) {
                const double t0 = std::acos(double(s.z_min) / s.radius),
                             t1 = std::acos(double(s.z_max) / s.radius);
                v = (std::acos(std::clamp(z / s.radius, -1., 1.)) - t0) / (t1 - t0);
            } else
                v = (z - s.z_min) / (s.z_max - double(s.z_min));
        }
        return {true,
                t,
                {static_cast<float>(phi / s.phi_max), static_cast<float>(std::clamp(v, 0., 1.))}};
    }
    return {};
}
} // namespace rypath
