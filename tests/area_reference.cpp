// SPDX-License-Identifier: GPL-3.0-only
#include "area_reference.hpp"

#include "cie_xyz_data.hpp"

#include <cmath>
#include <numbers>

namespace area_reference {
namespace {

using Vector = std::array<double, 3>;
using ReceiverPoint = std::array<double, 2>;

struct Node {
    double x;
    double weight;
};

// Eight-point Gauss-Legendre rule mapped onto [0,1].
constexpr std::array<Node, 8> nodes{{
    {0.0198550717512319, 0.0506142681451881},
    {0.101666761293187, 0.111190517226687},
    {0.237233795041836, 0.156853322938944},
    {0.408282678752175, 0.181341891689181},
    {0.591717321247825, 0.181341891689181},
    {0.762766204958164, 0.156853322938944},
    {0.898333238706813, 0.111190517226687},
    {0.980144928248768, 0.0506142681451881},
}};
constexpr double triangle_area = 0.625;

Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double dot(const Vector &a, const Vector &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vector unit(Vector v) {
    const double length = std::sqrt(dot(v, v));
    for (auto &component : v) {
        component /= length;
    }
    return v;
}

// The spherical edge integral gives the vector area of the triangle's directional
// footprint. Its projection onto the receiver normal yields the cosine integral.
double directional_mean(ReceiverPoint receiver) {
    const auto [x, y] = receiver;
    const std::array directions{
        unit({0.25 - x, -0.5 - y, -1}),
        unit({1.5 - x, -0.5 - y, -1}),
        unit({0.25 - x, 0.5 - y, -1}),
    };
    double projection = 0;
    for (std::size_t i = 0; i < directions.size(); ++i) {
        const auto &a = directions[i];
        const auto &b = directions[(i + 1) % directions.size()];
        const auto normal = cross(a, b);
        const double sine = std::sqrt(dot(normal, normal));
        projection += normal[2] * std::atan2(sine, dot(a, b)) / sine;
    }
    return std::abs(projection) / (2 * std::numbers::pi);
}

struct DirectionalMoments {
    double mean;
    double second;
};

DirectionalMoments duffy_moments(ReceiverPoint receiver) {
    const auto [x, y] = receiver;
    double nee_mean = 0;
    double nee_second = 0;
    double bsdf_mean = 0;
    double bsdf_second = 0;
    for (const auto [u, wu] : nodes) {
        for (const auto [v, wv] : nodes) {
            // Duffy map from the unit square to the emitter triangle.
            const double dx = 0.25 + 1.25 * u - x;
            const double dy = -0.5 + (1 - u) * v - y;
            const double distance_squared = 1 + dx * dx + dy * dy;
            // Lambertian directional density divided by the area sampler's
            // directional density, using cos(theta) = 1 / distance at both ends.
            const double ratio =
                triangle_area / (std::numbers::pi * distance_squared * distance_squared);
            const double ratio_squared = ratio * ratio;
            const double nee = ratio / (1 + ratio_squared);
            const double bsdf = ratio_squared / (1 + ratio_squared);
            const double area_weight = 2 * (1 - u) * wu * wv;
            nee_mean += area_weight * nee;
            nee_second += area_weight * nee * nee;
            bsdf_mean += area_weight * ratio * bsdf;
            bsdf_second += area_weight * ratio * bsdf * bsdf;
        }
    }
    // The two samples are independent conditional on the receiver location.
    return {nee_mean + bsdf_mean, nee_second + bsdf_second + 2 * nee_mean * bsdf_mean};
}

double material(double wavelength) {
    const std::size_t left = wavelength < wavelengths[1] ? 0 : 1;
    const double t = (wavelength - wavelengths[left]) / (wavelengths[left + 1] - wavelengths[left]);
    return std::lerp(reflectance[left], reflectance[left + 1], t);
}

void add_interval(Moments &moments, std::size_t index, std::array<double, 2> interval,
                  AngularMoments angular) {
    // On this half-nanometer interval the observer and emission are constant.
    // Splitting at material knots makes the spectral factor linear.
    constexpr double span = 470;
    const auto [lo, hi] = interval;
    const double a = material(lo);
    const double b = material(hi);
    const double width = hi - lo;
    const double angular_second = angular.variance + angular.mean * angular.mean;
    for (std::size_t c = 0; c < moments.mean.size(); ++c) {
        const double factor =
            emission(360 + static_cast<int>(index)) * rypath::detail::cie_xyz[index][c];
        moments.mean[c] += width * (a + b) / 2 * factor * angular.mean;
        moments.second[c] +=
            width * (a * a + a * b + b * b) / 3 * factor * factor * span * angular_second;
    }
}

} // namespace

double emission(int wavelength_nm) {
    return 0.125 + static_cast<double>(wavelength_nm - 360) / 512;
}

AngularMoments angular_moments() {
    constexpr double extent = 0.25;
    double mean = 0;
    double duffy_mean = 0;
    double second = 0;
    for (const auto [u, wu] : nodes) {
        for (const auto [v, wv] : nodes) {
            const ReceiverPoint receiver{(u - 0.5) * extent, (v - 0.5) * extent};
            const auto duffy = duffy_moments(receiver);
            mean += wu * wv * directional_mean(receiver);
            duffy_mean += wu * wv * duffy.mean;
            second += wu * wv * duffy.second;
        }
    }
    return {.mean = mean, .variance = second - mean * mean, .duffy_mean = duffy_mean};
}

Moments xyz_moments(AngularMoments angular) {
    Moments moments;
    for (std::size_t index = 0; index < rypath::detail::cie_xyz.size(); ++index) {
        const double center = 360 + static_cast<double>(index);
        for (const auto half : {0, 1}) {
            if ((index == 0 && half == 0) ||
                (index + 1 == rypath::detail::cie_xyz.size() && half == 1)) {
                continue;
            }
            const double lo = center + (half == 0 ? -0.5 : 0);
            const double hi = lo + 0.5;
            add_interval(moments, index, {lo, hi}, angular);
        }
    }
    return moments;
}

} // namespace area_reference
