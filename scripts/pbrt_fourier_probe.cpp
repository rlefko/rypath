// SPDX-License-Identifier: GPL-3.0-only
// Link to pinned, unmodified PBRT v3 RGBSpectrum angular BSDF implementation.
#include "reflection.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
namespace pbrt {
void Error(const char *, ...) { std::abort(); }
void Warning(const char *, ...) {}
} // namespace pbrt
namespace {
constexpr double pi = 3.14159265358979323846;
struct Weights {
    int offset;
    std::array<double, 4> value;
};
// Independent double evaluation of the nonuniform cubic basis and its integral.
// No upstream Sample_f result or authored CDF is used as a quantile oracle.
Weights weights(const pbrt::FourierBSDFTable &t, double mu) {
    int i = int(std::upper_bound(t.mu, t.mu + t.nMu, mu) - t.mu) - 1;
    i = std::clamp(i, 0, t.nMu - 2);
    double x0 = t.mu[i], x1 = t.mu[i + 1], h = x1 - x0;
    double u = std::clamp((mu - x0) / h, 0., 1.), u2 = u * u, u3 = u2 * u;
    Weights w{i - 1, {0, 2 * u3 - 3 * u2 + 1, -2 * u3 + 3 * u2, 0}};
    double a = u3 - 2 * u2 + u;
    if (i > 0) {
        a *= h / (x1 - t.mu[i - 1]);
        w.value[0] = -a;
        w.value[2] += a;
    } else {
        w.value[1] -= a;
        w.value[2] += a;
    }
    a = u3 - u2;
    if (i + 2 < t.nMu) {
        a *= h / (t.mu[i + 2] - x0);
        w.value[1] -= a;
        w.value[3] = a;
    } else {
        w.value[1] -= a;
        w.value[2] += a;
    }
    return w;
}
std::array<double, 2> quantiles(const pbrt::FourierBSDFTable &t, const pbrt::Vector3f &wo,
                                const pbrt::Vector3f &wi) {
    auto o = weights(t, wo.z), in = weights(t, -wi.z);
    std::vector<double> row(t.nMu), coefficients(t.mMax);
    for (int b = 0; b < 4; ++b) {
        if (o.value[b] == 0)
            continue;
        for (int x = 0; x < t.nMu; ++x)
            row[x] += o.value[b] * t.a0[(o.offset + b) * t.nMu + x];
        for (int a = 0; a < 4; ++a) {
            if (in.value[a] == 0)
                continue;
            int cell = (o.offset + b) * t.nMu + in.offset + a;
            for (int k = 0; k < t.m[cell]; ++k)
                coefficients[k] += o.value[b] * in.value[a] * t.a[t.aOffset[cell] + k];
        }
    }
    double total = 0, partial = 0;
    for (int i = 0; i + 1 < t.nMu; ++i) {
        double x0 = t.mu[i], x1 = t.mu[i + 1], h = x1 - x0;
        if (h == 0)
            continue;
        double f0 = row[i], f1 = row[i + 1];
        double d0 = i > 0 ? h * (f1 - row[i - 1]) / (x1 - t.mu[i - 1]) : f1 - f0;
        double d1 = i + 2 < t.nMu ? h * (row[i + 2] - f0) / (t.mu[i + 2] - x0) : f1 - f0;
        const auto integral = [&](double u) {
            return h * (f0 * u + d0 * u * u / 2 + (3 * (f1 - f0) - 2 * d0 - d1) * u * u * u / 3 +
                        (2 * (f0 - f1) + d0 + d1) * u * u * u * u / 4);
        };
        total += integral(1);
        partial += integral(std::clamp((-double(wi.z) - x0) / h, 0., 1.));
    }
    double phi = std::atan2(-double(wi.y), -double(wi.x)) - std::atan2(double(wo.y), double(wo.x));
    if (phi < 0)
        phi += 2 * pi;
    double angular = coefficients[0] * phi;
    for (int k = 1; k < t.mMax; ++k)
        angular += coefficients[k] * std::sin(k * phi) / k;
    return {partial / total, angular / (2 * pi * coefficients[0])};
}
} // namespace
int main(int argc, char **argv) {
    using namespace pbrt;
    if (argc != 3)
        return 1;
    FourierBSDFTable table;
    if (!FourierBSDFTable::Read(argv[1], &table))
        return 1;
    FourierBSDF bsdf(table, TransportMode::Radiance);
    std::ifstream queries(argv[2]);
    std::cout << std::setprecision(10);
    for (unsigned id = 0; id < 31 * 31; ++id) {
        auto wo = Normalize(Vector3f(.2f, .3f, id % 2 == 0 ? 1.f : -1.f));
        Vector3f wi;
        unsigned query_id;
        int accepted;
        if (!(queries >> query_id >> accepted >> wi.x >> wi.y >> wi.z) || query_id != id)
            return 1;
        auto e = bsdf.f(wo, wi);
        auto p = bsdf.Pdf(wo, wi);
        auto q = accepted ? quantiles(table, wo, wi) : std::array<double, 2>{0, 0};
        std::cout << accepted << ' ' << wi.x << ' ' << wi.y << ' ' << wi.z << ' ' << e[0] << ' '
                  << p << ' ' << e[1] << ' ' << e[2] << ' ' << q[0] << ' ' << q[1] << '\n';
    }
}
