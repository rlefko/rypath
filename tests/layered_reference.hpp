// SPDX-License-Identifier: GPL-3.0-only
// Independent double path-throughput reference; uses the separately checked
// double interface equations, with independent random streams and HG frame.
#pragma once
#include "conductor_reference.hpp"
#include "dielectric_reference.hpp"
#include <random>
namespace layered_reference {
using V = dielectric_reference::V;
using dielectric_reference::dot;
using dielectric_reference::unit;
constexpr double pi = std::numbers::pi;
struct P {
    dielectric_reference::P top;
    conductor_reference::P base;
    bool conductor;
    double R, thickness, albedo, g;
    unsigned depth;
};
inline double hg(double cosine, double g) {
    g = std::clamp(g, -.99, .99);
    double den = 1 + g * g + 2 * g * cosine;
    return (1 - g * g) / (4 * pi * den * std::sqrt(den));
}
inline double thin_reflection(double c, double eta) {
    double R = dielectric_reference::fresnel(std::abs(c), eta);
    return R + (R < 1 ? (1 - R) * (1 - R) * R / (1 - R * R) : 0);
}
// Analytic normal/smooth-interface one-base-event NEE term, valid at depth=1.
inline double smooth_diffuse_first(P p, V o, V i) {
    if (o.z * i.z <= 0)
        return 0;
    double F_o = dielectric_reference::fresnel(std::abs(o.z), p.top.eta);
    double F_i = dielectric_reference::fresnel(std::abs(i.z), p.top.eta);
    double co = std::sqrt(1 - (1 - o.z * o.z) / (p.top.eta * p.top.eta));
    double ci = std::sqrt(1 - (1 - i.z * i.z) / (p.top.eta * p.top.eta));
    double t = std::max(p.thickness, double(std::numeric_limits<float>::min()));
    double Tr = t <= std::numeric_limits<float>::min() ? 1 : std::exp(-t / co - t / ci);
    return (1 - F_o) * (1 - F_i) * p.R / (pi * p.top.eta * p.top.eta) * Tr;
}
struct S {
    V wi{};
    double weight{};
    bool accepted{}, delta{};
};
struct Random {
    std::mt19937_64 generator;
    double next() { return std::generate_canonical<double, 53>(generator); }
};
inline V phase_sample(V incoming, double g, Random &rng) {
    // Draw physical scattering cosine relative to the propagation direction.
    g = std::clamp(g, -.99, .99);
    double u = rng.next(), c;
    if (std::abs(g) < 1e-3)
        c = 2 * u - 1;
    else {
        double s = (1 - g * g) / (1 - g + 2 * g * u);
        c = (1 + g * g - s * s) / (2 * g);
    }
    c = std::clamp(c, -1., 1.);
    auto x = unit(
        dielectric_reference::cross(std::abs(incoming.x) < .9 ? V{1, 0, 0} : V{0, 1, 0}, incoming));
    auto y = dielectric_reference::cross(incoming, x);
    double phi = 2 * pi * rng.next(), s = std::sqrt(1 - c * c);
    return (s * std::cos(phi)) * x + (s * std::sin(phi)) * y + c * incoming;
}
inline S sample(P p, V o, Random &rng) {
    bool flip = o.z < 0;
    if (flip)
        o = -o;
    double choice = rng.next(), u = rng.next(), v = rng.next();
    auto a = dielectric_reference::sample(p.top, o, choice, u, v);
    if (!a.accepted)
        return {};
    V w = a.i;
    double weight = a.weight, t = std::max(p.thickness, double(std::numeric_limits<float>::min())),
           z = t;
    bool delta = p.top.smooth;
    if (w.z > 0)
        return {flip ? -w : w, weight, true, delta};
    for (unsigned depth = 0; depth < p.depth; ++depth) {
        if (depth > 3 && weight < .25) {
            if (rng.next() >= weight)
                return {};
            weight = 1;
        }
        if (p.albedo > 0) {
            double distance = -std::log1p(-rng.next());
            double next = z + distance * w.z;
            if (next > 0 && next < t) {
                w = phase_sample(w, p.g, rng);
                weight *= p.albedo;
                z = next;
                delta = false;
                continue;
            }
            z = std::clamp(next, 0., t);
        } else {
            weight *= t <= std::numeric_limits<float>::min() ? 1 : std::exp(-t / std::abs(w.z));
            z = z == t ? 0 : t;
        }
        if (z == 0) {
            if (p.conductor) {
                conductor_reference::V wo{-w.x, -w.y, -w.z};
                u = rng.next();
                v = rng.next();
                auto b = conductor_reference::sample(p.base, wo, u, v);
                if (!b.accepted)
                    return {};
                weight *= b.weight;
                w = {b.i.x, b.i.y, b.i.z};
                delta = delta && p.base.smooth;
            } else {
                double r = std::sqrt(rng.next()), phi = 2 * pi * rng.next();
                w = {r * std::cos(phi), r * std::sin(phi), std::sqrt(1 - r * r)};
                weight *= p.R;
                delta = false;
            }
        } else {
            choice = rng.next();
            u = rng.next();
            v = rng.next();
            auto b = dielectric_reference::sample(p.top, -w, choice, u, v);
            if (!b.accepted)
                return {};
            weight *= b.weight;
            w = b.i;
            delta = delta && p.top.smooth;
            if (w.z > 0)
                return {flip ? -w : w, weight, true, delta};
        }
    }
    return {};
}
} // namespace layered_reference
