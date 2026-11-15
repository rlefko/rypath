// SPDX-License-Identifier: Apache-2.0
// Double VNDF adaptation from PBRT v4 b0bdf461; other reference equations independent.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Modified for standalone validation. See third_party/pbrt/LICENSE.txt.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
namespace dielectric_reference {
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
struct P {
    double eta, ax, ay;
    bool smooth;
};
inline P prepare(double eta, double ax, double ay, bool remap) {
    if (eta == 0)
        eta = 1;
    if (remap) {
        ax = std::sqrt(ax);
        ay = std::sqrt(ay);
    }
    bool smooth = eta == 1 || std::max(ax, ay) < double(1e-3f);
    if (!smooth) {
        ax = std::max(ax, double(1e-4f));
        ay = std::max(ay, double(1e-4f));
    }
    return {eta, ax, ay, smooth};
}
inline double fresnel(double c, double eta) {
    if (c < 0) {
        c = -c;
        eta = 1 / eta;
    }
    c = std::min(c, 1.0);
    if (eta == 1)
        return 0;
    double st2 = (1 - c * c) / (eta * eta);
    if (st2 >= 1)
        return 1;
    double ct = std::sqrt(1 - st2);
    double rp = (eta * c - ct) / (eta * c + ct), rs = (c - eta * ct) / (c + eta * ct);
    return .5 * (rp * rp + rs * rs);
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
    if (p.smooth || o.z == 0 || i.z == 0)
        return {};
    bool reflection = o.z * i.z > 0;
    double ep = reflection ? 1 : (o.z > 0 ? p.eta : 1 / p.eta);
    V h = o + ep * i;
    if (dot(h, h) == 0)
        return {};
    V m = unit(h);
    if (m.z < 0)
        m = -m;
    double co = dot(o, m), ci = dot(i, m);
    if (co * o.z < 0 || ci * i.z < 0)
        return {};
    double f = fresnel(co, p.eta), d = D(p, m), lo = lambda(p, o), li = lambda(p, i);
    if (reflection)
        return {d * f / (4 * std::abs(o.z * i.z) * (1 + lo + li)),
                d * f / (4 * std::abs(o.z) * (1 + lo))};
    double den = ci + co / ep;
    return {d * (1 - f) * std::abs(ci * co / (o.z * i.z * den * den)) / ((1 + lo + li) * ep * ep),
            d * std::abs(co * ci) * (1 - f) / (std::abs(o.z) * (1 + lo) * den * den)};
}
struct S {
    V i;
    double weight, pdf;
    bool accepted;
};
inline S sample(P p, V o, double uc, double u, double v) {
    if (o.z == 0)
        return {};
    V m{0, 0, 1};
    if (!p.smooth) {
        V w = unit({p.ax * o.x, p.ay * o.y, o.z});
        if (w.z < 0)
            w = -w;
        V t1 = w.z < .99999 ? unit(cross({0, 0, 1}, w)) : V{1, 0, 0}, t2 = cross(w, t1);
        double x = std::sqrt(u) * std::cos(2 * pi * v), y = std::sqrt(u) * std::sin(2 * pi * v);
        double h = std::sqrt(std::max(0.0, 1 - x * x)), a = (1 + w.z) / 2;
        y = (1 - a) * h + a * y;
        V nh = x * t1 + y * t2 + std::sqrt(std::max(0.0, 1 - x * x - y * y)) * w;
        m = unit({p.ax * nh.x, p.ay * nh.y, std::max(1e-6, nh.z)});
    }
    double co = dot(o, m), F = fresnel(co, p.eta), ep = co < 0 ? 1 / p.eta : p.eta;
    bool reflection = uc < F;
    V i;
    if (reflection)
        i = -o + 2 * co * m;
    else {
        double st2 = std::max(0.0, 1 - co * co) / (ep * ep);
        if (st2 >= 1)
            return {};
        double ct = std::sqrt(1 - st2);
        i = (-1 / ep) * o + (std::abs(co) / ep - ct) * (co < 0 ? -m : m);
    }
    if (i.z == 0 || ((i.z * o.z > 0) != reflection))
        return {i, 0, 0, false};
    double scale = reflection ? 1 : 1 / (ep * ep);
    if (p.smooth)
        return {i, scale, reflection ? F : 1 - F, true};
    double lo = lambda(p, o), li = lambda(p, i), d = D(p, m);
    if (d == 0)
        return {i, 0, 0, false};
    double pdf;
    if (reflection)
        pdf = d * F / (4 * std::abs(o.z) * (1 + lo));
    else {
        double ci = dot(i, m), den = ci + co / ep;
        pdf = d * std::abs(co * ci) * (1 - F) / (std::abs(o.z) * (1 + lo) * den * den);
    }
    return {i, scale * (1 + lo) / (1 + lo + li), pdf, true};
}
// Full-sphere midpoint solid-angle quadrature independent of the VNDF map.
inline E integrate(P p, V o, int n) {
    E total{};
    for (int z = 0; z < 2 * n; ++z) {
        double c = -1 + (z + .5) / n, s = std::sqrt(1 - c * c);
        for (int a = 0; a < 2 * n; ++a) {
            double phi = 2 * pi * (a + .5) / (2 * n);
            auto e = evaluate(p, o, {s * std::cos(phi), s * std::sin(phi), c});
            total.f += e.f * std::abs(c);
            total.pdf += e.pdf;
        }
    }
    double scale = pi / (n * double(n));
    return {total.f * scale, total.pdf * scale};
}
} // namespace dielectric_reference
