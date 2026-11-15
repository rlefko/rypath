// SPDX-License-Identifier: GPL-3.0-only
// Independent double equations for the literal glTF dielectric/metal branch mix.
// Target: Khronos glTF-Sample-Renderer 623715005b00f5dba155e7448706bf87202e553a.
// VNDF mapping: Heitz, JCGT 7(4), 2018, Listing 1; no production headers included.
#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>
namespace gltf_reference {
constexpr double pi = std::numbers::pi;
struct V {
    double x, y, z;
};
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V operator-(V a) { return {-a.x, -a.y, -a.z}; }
inline V operator*(double s, V v) { return {s * v.x, s * v.y, s * v.z}; }
inline double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V unit(V v) { return (1 / std::sqrt(dot(v, v))) * v; }
inline V cross(V a, V b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline V direction(double z, double phi = 0) {
    const double s = std::sqrt(std::max(0., 1 - z * z));
    return {s * std::cos(phi), s * std::sin(phi), z};
}
struct P {
    double color, metallic, alpha;
    double q() const { return color == 0 || metallic == 1 ? 1 : .5; }
};
inline double schlick(double f0, double mu) {
    return f0 + (1 - f0) * std::pow(1 - std::clamp(mu, 0., 1.), 5);
}
inline double amplitude(P p, double mu) {
    return (1 - p.metallic) * schlick(.04, mu) + p.metallic * schlick(p.color, mu);
}
inline double lambda(double alpha, V w) {
    const double slope = alpha * std::hypot(w.x, w.y) / w.z;
    return slope * slope / (2 * (std::hypot(1., slope) + 1));
}
struct E {
    double f, pdf;
};
inline E evaluate(P p, V o, V i) {
    if (o.z <= 0 || i.z <= 0)
        return {};
    const auto h = unit(o + i);
    const double mu = std::abs(dot(o, h));
    const double dielectric = (1 - schlick(.04, mu)) * p.color / pi;
    double f = (1 - p.metallic) * dielectric;
    double pdf = (1 - p.q()) * i.z / pi;
    if (p.alpha > 0) {
        const double a2 = p.alpha * p.alpha;
        const double denominator = h.x * h.x + h.y * h.y + a2 * h.z * h.z;
        const double d = a2 / (pi * denominator * denominator);
        const double lo = lambda(p.alpha, o), li = lambda(p.alpha, i);
        f += amplitude(p, mu) * d / (4 * o.z * i.z * (1 + lo + li));
        pdf += p.q() * d / (4 * o.z * (1 + lo));
    }
    return {f, pdf};
}
struct S {
    V i{};
    double weight = 0, pdf = 0;
    bool accepted = false, delta = false;
};
inline S sample(P p, V o, double choice, double u, double v) {
    if (o.z <= 0)
        return {};
    V i;
    if (choice < p.q()) {
        if (p.alpha == 0) {
            const double weight = amplitude(p, o.z) / p.q();
            return {{-o.x, -o.y, o.z}, weight, p.q(), weight > 0, true};
        }
        const V stretched = unit({p.alpha * o.x, p.alpha * o.y, o.z});
        const double lensq = stretched.x * stretched.x + stretched.y * stretched.y;
        const V t1 = lensq > 0
                         ? V{-stretched.y / std::sqrt(lensq), stretched.x / std::sqrt(lensq), 0}
                         : V{1, 0, 0};
        const V t2 = cross(stretched, t1);
        const double x = std::sqrt(u) * std::cos(2 * pi * v);
        double y = std::sqrt(u) * std::sin(2 * pi * v);
        const double blend = .5 * (1 + stretched.z);
        y = (1 - blend) * std::sqrt(std::max(0., 1 - x * x)) + blend * y;
        const auto nh = x * t1 + y * t2 + std::sqrt(std::max(0., 1 - x * x - y * y)) * stretched;
        const auto h = unit({p.alpha * nh.x, p.alpha * nh.y, std::max(0., nh.z)});
        i = -o + 2 * dot(o, h) * h;
        if (i.z <= 0)
            return {i, 0, 0, false, false};
    } else {
        const double radius = std::sqrt(u), phi = 2 * pi * v;
        i = {radius * std::cos(phi), radius * std::sin(phi), std::sqrt(1 - u)};
    }
    const auto e = evaluate(p, o, i);
    const double weight = e.f * i.z / e.pdf;
    return {i, weight, e.pdf, weight > 0, false};
}
// Midpoint solid-angle integration, independent of all sampling transformations.
inline E integrate(P p, V o, int resolution) {
    E sum{};
    for (int z = 0; z < resolution; ++z) {
        const double mu = (z + .5) / resolution;
        for (int a = 0; a < 2 * resolution; ++a) {
            const auto e = evaluate(p, o, direction(mu, 2 * pi * (a + .5) / (2 * resolution)));
            sum.f += e.f * mu;
            sum.pdf += e.pdf;
        }
    }
    const double measure = pi / (double(resolution) * resolution);
    sum.f *= measure;
    sum.pdf *= measure;
    if (p.alpha == 0) {
        sum.f += amplitude(p, o.z);
        // Include selected zero-energy delta events when checking proposal mass.
        sum.pdf += p.q();
    }
    return sum;
}
} // namespace gltf_reference
