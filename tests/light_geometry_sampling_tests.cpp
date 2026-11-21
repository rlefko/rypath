// SPDX-License-Identifier: GPL-3.0-only
#include "light_geometry_sampling_shared.h"
#include "offset_rounding_cases.hpp"
#include <array>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {
namespace lg = rypath_light_geometry_math;
using lg::V2;
using lg::V3;
using lg::Weights;

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(float value, float expected, float tolerance, const std::string &message) {
    require(std::isfinite(value) && std::abs(value - expected) <= tolerance,
            message + ": " + std::to_string(value) + " vs " + std::to_string(expected));
}
V3 interpolate(V3 a, V3 b, V3 c, V2 uv) { return a + uv.x * (b - a) + uv.y * (c - a); }

void offset_rounding() {
    for (const auto &test : offset_rounding_test::cases)
        for (bool negative : {false, true}) {
            const auto actual = rypath_offset_rounding::outward(test.input, negative);
            require(actual.bits == (negative ? test.negative : test.positive) &&
                        actual.valid == (negative ? test.negative_valid : test.positive_valid),
                    "Offset rounding boundary word " + std::to_string(test.input));
        }
}

void bilinear() {
    const Weights weights{.01f, .2f, .4f, 1};
    constexpr unsigned n = 256;
    double integral = 0, moment_x = 0, moment_y = 0;
    for (unsigned y = 0; y < n; ++y)
        for (unsigned x = 0; x < n; ++x) {
            const V2 u{(static_cast<float>(x) + .5f) / n, (static_cast<float>(y) + .5f) / n};
            integral += lg::bilinear_pdf(u, weights) / (n * n);
            const auto sample = lg::bilinear_sample(u, weights);
            moment_x += sample.x / (n * n);
            moment_y += sample.y / (n * n);
        }
    const double sum = .01 + .2 + .4 + 1;
    near(float(integral), 1, 2e-6f, "Bilinear quadrature normalization");
    near(float(moment_x), float((.01 + .4 + 2 * (.2 + 1)) / (3 * sum)), 2e-5f,
         "Bilinear first x moment");
    near(float(moment_y), float((.01 + .2 + 2 * (.4 + 1)) / (3 * sum)), 2e-5f,
         "Bilinear first y moment");
    const auto black = lg::bilinear_sample({.3f, .7f}, {0, 0, 0, 0});
    near(black.x, .3f, 1e-6f, "Zero-Jacobian fallback x");
    near(black.y, .7f, 1e-6f, "Zero-Jacobian fallback y");
}

void pinned_pbrt_fixtures() {
    // Generated from the actual pinned Float PBRT b0bdf461 library using
    // SampleSphericalTriangle / SampleSphericalRectangle, -ffp-contract=off.
    struct Fixture {
        V2 u, triangle, rectangle;
    };
    const std::array fixtures{
        Fixture{{.1f, .2f}, {.5372091532f, .1034439951f}, {.1900854707f, .2263269126f}},
        Fixture{{.3f, .7f}, {.1513626575f, .41564551f}, {.4083202481f, .7057790756f}},
        Fixture{{.5f, .5f}, {.2457033694f, .4997260273f}, {.5619937778f, .5281414986f}},
        Fixture{{.9f, .8f}, {.1148049459f, .8257148862f}, {.8792259097f, .7987345457f}}};
    const auto frame = lg::rectangle_frame({.2f, .1f, 1}, {-1, -.5f, 0}, {2, 0, 0}, {0, 1, 0});
    for (const auto &fixture : fixtures) {
        V2 triangle;
        require(lg::triangle_sample({-1, -1, 0}, {1, -1, 0}, {0, 1, 0}, {.2f, .3f, 1}, fixture.u,
                                    triangle),
                "Pinned PBRT triangle sample");
        near(triangle.x, fixture.triangle.x, 2e-5f, "Pinned PBRT triangle b1");
        near(triangle.y, fixture.triangle.y, 2e-5f, "Pinned PBRT triangle b2");
        const auto rectangle = lg::rectangle_sample(frame, fixture.u);
        near(rectangle.x, fixture.rectangle.x, 2e-5f, "Pinned PBRT rectangle u");
        near(rectangle.y, fixture.rectangle.y, 2e-5f, "Pinned PBRT rectangle v");
    }
}

void triangles() {
    const V3 a{-1, -1, 0}, b{1, -1, 0}, c{0, 1, 0};
    const std::array references{V3{.2f, .3f, 1}, V3{-.1f, .2f, -.3f}, V3{0, 0, 60}};
    for (V3 reference : references) {
        const float omega = lg::triangle_solid_angle(a, b, c, reference);
        require(lg::spherical_triangle(omega), "Triangle fixture must select spherical policy");
        // Independent area-domain midpoint quadrature of cos(theta)/distance^2.
        double quadrature = 0, quadrature_z = 0;
        constexpr unsigned qn = 512;
        const V3 normal = lg::unit(lg::cross(b - a, c - a));
        const float area = .5f * lg::length3(lg::cross(b - a, c - a));
        for (unsigned y = 0; y < qn; ++y)
            for (unsigned x = 0; x < qn; ++x) {
                const auto uv = lg::triangle_uniform_uv(
                    {(static_cast<float>(x) + .5f) / qn, (static_cast<float>(y) + .5f) / qn});
                const V3 delta = interpolate(a, b, c, uv) - reference;
                const double distance2 = lg::dot(delta, delta);
                const double density = static_cast<double>(area) *
                                       std::abs(lg::dot(normal, delta)) /
                                       (distance2 * std::sqrt(distance2) * qn * qn);
                quadrature += density;
                quadrature_z += density * delta.z / std::sqrt(distance2);
            }
        require(std::abs(quadrature - omega) < 2e-4 * omega,
                "Triangle solid angle versus independent area quadrature");
        double sampled_z = 0;
        for (unsigned y = 0; y < 48; ++y)
            for (unsigned x = 0; x < 48; ++x) {
                const V2 u{(static_cast<float>(x) + .5f) / 48, (static_cast<float>(y) + .5f) / 48};
                V2 uv;
                require(lg::triangle_sample(a, b, c, reference, u, uv),
                        "Spherical triangle sample");
                require(uv.x >= 0 && uv.y >= 0 && uv.x + uv.y <= 1.000001f,
                        "Sampled triangle barycentrics");
                const auto inverse = lg::triangle_inverse(
                    a, b, c, reference, lg::unit(interpolate(a, b, c, uv) - reference));
                // Float angular inversion is ill conditioned at the far threshold.
                const float tolerance = omega < .001f ? .004f : .0003f;
                near(inverse.x, u.x, tolerance, "Triangle inverse x");
                near(inverse.y, u.y, tolerance, "Triangle inverse y");
                sampled_z += lg::unit(interpolate(a, b, c, uv) - reference).z / (48 * 48);
            }
        near(float(sampled_z), float(quadrature_z / quadrature), .003f,
             "Triangle sampled direction moment versus independent quadrature");
    }
    require(!lg::spherical_triangle(std::nextafter(3e-4f, 0.f)) && lg::spherical_triangle(3e-4f) &&
                lg::spherical_triangle(6.22f) &&
                !lg::spherical_triangle(std::nextafter(6.22f, 7.f)),
            "Triangle policy threshold inclusivity");
    require(!lg::spherical_triangle(lg::triangle_solid_angle(a, b, c, {0, 0, 1000})),
            "Far triangle must use area proposal");
    require(!lg::spherical_triangle(lg::triangle_solid_angle(a, b, c, {0, 0, .00001f})),
            "Near-hemisphere triangle must use area proposal");
}

void rectangles() {
    const V3 a{-1, -.5f, 0}, ex{2, 0, 0}, ey{0, 1, 0};
    require(lg::rectangle(a, a + ex, a + ey, a + ex + ey), "Rectangle classification");
    require(!lg::rectangle(a, a + ex, a + V3{.2f, 1, 0}, a + ex + V3{.2f, 1, 0}),
            "Sheared parallelogram must retain bilinear area proposal");
    require(!lg::rectangle(a, a + ex, a + ey, a + ex + ey + V3{0, 0, .1f}),
            "Nonplanar patch must retain bilinear area proposal");
    for (V3 reference : {V3{.2f, .1f, 1}, V3{-.1f, .2f, -.03f}, V3{0, 0, 100}}) {
        const auto frame = lg::rectangle_frame(reference, a, ex, ey);
        require(frame.area > 1e-4f, "Rectangle fixture spherical policy");
        double quadrature = 0, quadrature_z = 0, sampled_z = 0;
        constexpr unsigned qn = 512;
        for (unsigned y = 0; y < qn; ++y)
            for (unsigned x = 0; x < qn; ++x) {
                const V3 delta = a + ((static_cast<float>(x) + .5f) / qn) * ex +
                                 ((static_cast<float>(y) + .5f) / qn) * ey - reference;
                const double d2 = lg::dot(delta, delta);
                const double density = 2 * std::abs(delta.z) / (d2 * std::sqrt(d2) * qn * qn);
                quadrature += density;
                quadrature_z += density * delta.z / std::sqrt(d2);
            }
        for (unsigned y = 0; y < 48; ++y)
            for (unsigned x = 0; x < 48; ++x) {
                const V2 u{(static_cast<float>(x) + .5f) / 48, (static_cast<float>(y) + .5f) / 48};
                const auto uv = lg::rectangle_sample(frame, u);
                require(uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1,
                        "Rectangle sample inside parameter domain");
                const auto inverse = lg::rectangle_inverse(frame, uv);
                const float tolerance = frame.area < .001f ? .015f : .0004f;
                near(inverse.x, u.x, tolerance, "Rectangle inverse x");
                near(inverse.y, u.y, tolerance, "Rectangle inverse y");
                sampled_z += lg::unit(a + uv.x * ex + uv.y * ey - reference).z / (48 * 48);
            }
        near(float(sampled_z), float(quadrature_z / quadrature), .003f,
             "Rectangle sampled direction moment versus independent quadrature");
    }
    const auto jacobians = lg::patch_jacobians(a, a + ex, a + ey, a + ex + ey + V3{0, 0, 1});
    require(jacobians.a > 0 && jacobians.b > 0 && jacobians.c > 0 && jacobians.d > 0,
            "Nonplanar patch corner Jacobians");
}

void spheres() {
    const V3 center{.2f, -.3f, .4f};
    constexpr float radius = .7f;
    for (V3 reference : {V3{0, 0, 2}, V3{2, -.3f, .4f}, V3{0, 0, 1000}}) {
        const float distance2 = lg::dot(center - reference, center - reference);
        const double sin2 = double(radius) * radius / distance2;
        const double exact_omega = 2 * std::numbers::pi * sin2 / (1 + std::sqrt(1 - sin2));
        require(std::abs(1 / lg::sphere_cone_pdf(radius, distance2) - exact_omega) <
                    exact_omega * 2e-4,
                "Sphere cone normalization versus rationalized double oracle");
        for (unsigned y = 0; y < 48; ++y)
            for (unsigned x = 0; x < 48; ++x) {
                const auto point = lg::sphere_cone_point(
                    center, radius, reference,
                    {(static_cast<float>(x) + .5f) / 48, (static_cast<float>(y) + .5f) / 48});
                near(lg::length3(point - center), radius, 3e-6f, "Sphere surface radius");
                require(lg::dot(point - center, reference - point) >= 0,
                        "Sphere cone must select visible near root");
            }
    }
}
} // namespace
int main() {
    try {
        offset_rounding();
        bilinear();
        pinned_pbrt_fixtures();
        triangles();
        rectangles();
        spheres();
        std::cout << "Incident geometry: bilinear moments, independent quadrature, inverse maps, "
                     "thresholds and sphere oracle passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
