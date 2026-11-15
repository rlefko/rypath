// SPDX-License-Identifier: Apache-2.0
// Double VNDF adaptation from PBRT v4 b0bdf461; other reference equations independent.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Modified for standalone validation. See third_party/pbrt/LICENSE.txt.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
namespace conductor_reference {
constexpr double pi = std::numbers::pi;
struct V {
    double x, y, z;
};
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V operator-(V a) { return {-a.x, -a.y, -a.z}; }
inline V operator*(double a, V b) { return {a * b.x, a * b.y, a * b.z}; }
inline double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V unit(V a) { return (1 / std::sqrt(dot(a, a))) * a; }
inline V cross(V a, V b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
// Independent complex Snell calculation, rather than the device real-square-root formula.
inline double fresnel(double c, double eta, double k) {
    if (eta == 1 && k == 0)
        return 0;
    std::complex<double> n(eta, k);
    auto ct = std::sqrt(1.0 - (1 - c * c) / (n * n));
    auto rp = (n * c - ct) / (n * c + ct), rs = (c - n * ct) / (c + n * ct);
    return .5 * (std::norm(rp) + std::norm(rs));
}
struct P {
    double eta, k, ax, ay;
    bool smooth;
};
inline P prepare(double eta, double k, double ax, double ay, bool remap) {
    if (remap) {
        ax = std::sqrt(ax);
        ay = std::sqrt(ay);
    }
    bool smooth = std::max(ax, ay) < 1e-3;
    if (!smooth) {
        ax = std::max(ax, 1e-4);
        ay = std::max(ay, 1e-4);
    }
    return {eta, k, ax, ay, smooth};
}
inline double lambda(P p, V w) {
    return .5 *
           (std::sqrt(1 + (p.ax * p.ax * w.x * w.x + p.ay * p.ay * w.y * w.y) / (w.z * w.z)) - 1);
}
inline double D(P p, V m) {
    if (std::pow(m.z, 4) < 1e-16)
        return 0;
    double t = m.z * m.z + m.x * m.x / (p.ax * p.ax) + m.y * m.y / (p.ay * p.ay);
    return 1 / (pi * p.ax * p.ay * t * t);
}
struct E {
    double f, pdf;
};
inline E evaluate(P p, V o, V i) {
    if (p.smooth || o.z * i.z <= 0)
        return {};
    V m = unit(o + i);
    double d = D(p, m), lo = lambda(p, o), li = lambda(p, i);
    return {d * fresnel(std::abs(dot(o, m)), p.eta, p.k) /
                (4 * std::abs(o.z * i.z) * (1 + lo + li)),
            d / (4 * std::abs(o.z) * (1 + lo))};
}
struct S {
    V i;
    double weight, pdf;
    bool accepted;
};
// Independent double transcription of pinned PBRT's Heitz VNDF mapping.
inline S sample(P p, V o, double u, double v) {
    if (o.z == 0)
        return {};
    if (p.smooth) {
        double f = fresnel(std::abs(o.z), p.eta, p.k);
        return {{-o.x, -o.y, o.z}, f, 1, f > 0};
    }
    V w = unit({p.ax * o.x, p.ay * o.y, o.z});
    if (w.z < 0)
        w = -w;
    V t1 = w.z < .99999 ? unit(cross({0, 0, 1}, w)) : V{1, 0, 0};
    V t2 = cross(w, t1);
    double x = std::sqrt(u) * std::cos(2 * pi * v), y = std::sqrt(u) * std::sin(2 * pi * v);
    double h = std::sqrt(std::max(0.0, 1 - x * x)), a = (1 + w.z) / 2;
    y = (1 - a) * h + a * y;
    V nh = x * t1 + y * t2 + std::sqrt(std::max(0.0, 1 - x * x - y * y)) * w;
    V m = unit({p.ax * nh.x, p.ay * nh.y, std::max(1e-6, nh.z)});
    V i = -o + 2 * dot(o, m) * m;
    if (o.z * i.z <= 0)
        return {i, 0, 0, false};
    double lo = lambda(p, o), li = lambda(p, i);
    double d = D(p, m);
    if (d == 0)
        return {i, 0, 0, false};
    double pdf = d / (4 * std::abs(o.z) * (1 + lo));
    double weight = fresnel(std::abs(dot(o, m)), p.eta, p.k) * (1 + lo) / (1 + lo + li);
    return {i, weight, pdf, weight > 0};
}
// Midpoint solid-angle quadrature, independent of VNDF mapping and its probability mass.
inline E integrate(P p, V o, int n) {
    double energy = 0, mass = 0;
    for (int z = 0; z < n; ++z) {
        double c = (z + .5) / n, s = std::sqrt(1 - c * c);
        for (int a = 0; a < 2 * n; ++a) {
            double phi = 2 * pi * (a + .5) / (2 * n);
            auto e = evaluate(p, o, {s * std::cos(phi), s * std::sin(phi), c});
            energy += e.f * c;
            mass += e.pdf;
        }
    }
    double scale = 2 * pi / (2.0 * n * n);
    return {energy * scale, mass * scale};
}
} // namespace conductor_reference
