// SPDX-License-Identifier: GPL-3.0-only
#include "metal_execution.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using Words = std::array<std::uint32_t, 64>;
enum : unsigned { ok = 0, unsupported = 1, outside = 2, invalid = 3 };
struct Case {
    Words query{};
    unsigned status = ok, family = 0;
    bool support = false, tangent_direction = false;
    std::array<long double, 4> numerator{}, pdf{};
    std::array<long double, 3> direction{};
    long double r2 = 0, G = 0, cosine = 0, destination_G = 0, destination_cosine = 0;
    unsigned footprint_decision = 0, conductor_witness = 0;
    long double previous_q = 1;
    unsigned primary_ready = 1, needed = 0;
    float normal_offset = 0;
    bool duplicate_primary = false;
    unsigned geometric_status = ok, edge_status = ok;
    bool exact_ray = false;
};
void need(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
unsigned bits(float x) { return std::bit_cast<unsigned>(x); }
float f32(unsigned x) { return std::bit_cast<float>(x); }
long double scaled(unsigned fraction, unsigned exponent) {
    return std::ldexp(static_cast<long double>(f32(fraction)), std::bit_cast<int>(exponent));
}
void close(long double actual, long double expected, const std::string &label) {
    // 32eps bounds the small fixed strict-FLOAT BSDF/geometry expression; no MC
    //  noise is involved and zero expectations are exact. Selected BEFORE GPU.
    const auto tolerance = 32.L * std::numeric_limits<float>::epsilon() * std::abs(expected);
    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, label);
}
Case diffuse() {
    Case c;
    c.query[1] = 0;
    c.query[2] = 15;
    for (unsigned k = 0; k < 4; ++k)
        c.query[8 + k] = bits(.25f * float(k + 1));
    c.query[34] = c.query[38] = bits(1);
    c.support = true;
    const auto pi = std::acos(-1.L);
    for (unsigned k = 0; k < 4; ++k) {
        c.numerator[k] = (.25L * (k + 1)) / pi;
        c.pdf[k] = 1 / pi;
    }
    return c;
}
Case conductor() {
    auto c = diffuse();
    c.query[1] = 1;
    c.family = 1;
    c.query.fill(0);
    c.query[1] = 1;
    c.query[2] = 15;
    c.query[34] = c.query[38] = bits(1);
    const auto pi = std::acos(-1.L);
    for (unsigned k = 0; k < 4; ++k) {
        const float ax = .25f * float(k + 1), ay = .5f;
        const unsigned b = 8 + 5 * k;
        c.query[b] = bits(1);
        c.query[b + 1] = bits(2);
        c.query[b + 2] = bits(ax);
        c.query[b + 3] = bits(ay);
        // Independent normal-incidence Fresnel R=((eta-1)^2+k²)/((eta+1)^2+k²)=1/2.
        c.pdf[k] = 1 / (4 * pi * ax * ay);
        c.numerator[k] = .5L * c.pdf[k];
    }
    return c;
}
Case edge(std::array<float, 3> from, std::array<float, 3> to, std::array<float, 3> normal) {
    Case c;
    c.query[0] = 1;
    for (unsigned k = 0; k < 3; ++k) {
        c.query[4 + k] = bits(from[k]);
        c.query[8 + k] = bits(normal[k]);
        c.query[12 + k] = bits(to[k]);
        c.query[16 + k] = bits(normal[k]);
        c.direction[k] = static_cast<long double>(to[k]) - from[k];
        c.r2 += c.direction[k] * c.direction[k];
    }
    if (c.r2 == 0) {
        c.status = outside;
        return c;
    }
    const auto distance = std::sqrt(c.r2);
    for (unsigned k = 0; k < 3; ++k) {
        c.direction[k] /= distance;
        c.cosine += c.direction[k] * normal[k];
    }
    c.cosine = std::abs(c.cosine);
    c.G = c.destination_G = c.cosine / c.r2;
    c.destination_cosine = c.cosine;
    if (c.cosine == 0)
        c.status = outside;
    return c;
}
std::vector<Case> cases() {
    std::vector<Case> all;
    all.push_back(diffuse());
    auto c = diffuse();
    c.query[3] = 1;
    c.query[32] = bits(1);
    c.query[34] = 0;
    c.query[36] = bits(.8f);
    c.query[38] = bits(-.6f);
    for (unsigned k = 0; k < 4; ++k) {
        c.pdf[k] *= .8L;
        c.numerator[k] *= .8L;
    }
    all.push_back(c);
    c = diffuse();
    c.query[34] = c.query[38] = bits(-1);
    all.push_back(c);
    c = diffuse();
    c.query[38] = bits(-1);
    c.support = false;
    c.numerator = {};
    c.pdf = {};
    all.push_back(c);
    c = diffuse();
    c.query[36] = bits(1);
    c.query[38] = 0;
    c.support = false;
    c.numerator = {};
    c.pdf = {};
    all.push_back(c);
    c = diffuse();
    c.query[8] = 0;
    c.support = false;
    c.numerator[0] = c.pdf[0] = 0;
    all.push_back(c);
    all.push_back(conductor());
    c = conductor();
    c.query[32] = c.query[36] = bits(1);
    c.query[34] = c.query[38] = bits(1e-5f);
    c.support = false;
    c.numerator = c.pdf = {};
    c.conductor_witness = 15; // Valid native zero from m.z^4 < 1e-16 model cutoff.
    all.push_back(c);
    c = conductor();
    c.query[32] = bits(1);
    c.query[36] = bits(-1);
    c.query[34] = c.query[38] = bits(1e-4f);
    for (unsigned k = 0; k < 4; ++k) {
        c.query[8 + 5 * k + 2] = bits(1e30f);
        c.query[8 + 5 * k + 3] = bits(1e-4f);
    }
    c.status = invalid;
    c.conductor_witness = 7; // m=z, D>0, finite lambda: positive f/PDF underflow.
    all.push_back(c);
    c = conductor();
    c.query[12] = 1;
    c.status = unsupported;
    c.family = 3;
    all.push_back(c);
    for (unsigned kind : {2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 0xffffffffu}) {
        c = diffuse();
        c.query[1] = kind;
        c.status = unsupported;
        c.family = 3;
        for (unsigned k = 8; k < 32; ++k)
            c.query[k] = 0xa5a5a5a5u;
        all.push_back(c);
    }
    for (unsigned mask : {0u, 2u, 17u}) {
        c = diffuse();
        c.query[2] = mask;
        c.status = invalid;
        c.family = 3;
        all.push_back(c);
    }
    c = diffuse();
    c.query[9] = bits(std::numeric_limits<float>::quiet_NaN());
    c.status = invalid;
    c.family = 3;
    all.push_back(c);
    c = diffuse();
    c.query[2] = 1;
    c.query[9] = bits(std::numeric_limits<float>::quiet_NaN());
    for (unsigned k = 1; k < 4; ++k)
        c.numerator[k] = c.pdf[k] = 0;
    all.push_back(c);
    c = conductor();
    c.query[10] = bits(std::numeric_limits<float>::quiet_NaN());
    c.status = invalid;
    c.family = 3;
    all.push_back(c);
    c = conductor();
    c.query[17] = 2;
    c.status = invalid;
    c.family = 3;
    all.push_back(c);
    c = diffuse();
    c.query[3] = 2;
    c.status = invalid;
    all.push_back(c);
    c = diffuse();
    c.query[32] = bits(std::numeric_limits<float>::quiet_NaN());
    c.status = invalid;
    all.push_back(c);
    c = diffuse();
    c.query[38] = bits(2);
    c.status = invalid;
    all.push_back(c);
    c = diffuse();
    c.query[8] = bits(std::numeric_limits<float>::denorm_min());
    c.status = invalid;
    all.push_back(c);
    all.push_back(edge({0, 0, 0}, {0, 0, 2}, {0, 0, 1}));
    c = edge({0, 0, 0}, {0, 0, 1}, {0, 0, 1 + 0x1p-17f});
    c.cosine = c.destination_cosine = c.G = c.destination_G = 1;
    all.push_back(c); // Accepted near-unit literal normal has normalized cosine1.
    all.push_back(edge({0, 0, 0}, {3, 0, 4}, {0, 0, 1}));
    all.push_back(edge({3, 0, 4}, {0, 0, 0}, {0, 0, 1}));
    c = edge({0, 0, 0}, {3, 0, 4}, {0, 0, 1});
    c.query[16] = bits(1);
    c.query[18] = 0;
    c.destination_cosine = .6L;
    c.destination_G = .6L / 25;
    all.push_back(c); // Distinct source/destination normals catch swapped/fullG.

    all.push_back(edge({0, 0, 0}, {0, 0, 0}, {0, 0, 1}));
    all.push_back(edge({0, 0, 0}, {0, 0, 2}, {1, 0, 0}));
    c = edge({0, 0, 0}, {1, 1e-20f, 1}, {.70710677f, 1e-20f, -.70710677f});
    // Independent cancellation oracle: identical x/z products cancel exactly;
    // the positive y*y residual is materializable. A lossy dot must report
    // uncertain arithmetic, never physical outside-support.
    c.cosine = c.direction[1] * static_cast<long double>(1e-20f);
    need(c.cosine > 0 && static_cast<float>(c.cosine) > 0,
         "Cancellation fixture independently positive representable cosine");
    c.status = invalid;
    all.push_back(c);
    all.push_back(edge({0, 0, 0}, {0, 0, 0x1p-100f}, {0, 0, 1}));
    all.push_back(edge({0, 0, 0}, {0, 0, std::numeric_limits<float>::denorm_min()}, {0, 0, 1}));
    all.push_back(edge({std::numeric_limits<float>::max(), 0, 0},
                       {-std::numeric_limits<float>::max(), 0, 0}, {1, 0, 0}));
    c = edge({0, 0, 0}, {0, 0, 2}, {0, 0, 1});
    c.query[8] = bits(std::numeric_limits<float>::quiet_NaN());
    c.status = invalid;
    all.push_back(c);
    c = edge({0, 0, 0}, {0, 0, 2}, {0, 0, 1});
    c.query[18] = bits(2);
    c.status = invalid;
    all.push_back(c);
    c = edge({0, 0, 0},
             {std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min(), 0},
             {1, 0, 0});
    c.status = invalid;
    all.push_back(c);
    // Actual footprint input construction, with normal-incidence marginal PDFs.
    for (unsigned kind : {0u, 1u}) {
        c = kind == 0 ? diffuse() : conductor();
        c.query[0] = 2;
        c.query[4] = kind;
        c.query[32] = bits(1);
        c.query[33] = bits(1);
        all.push_back(c);
        c.query[33] = bits(1000);
        c.footprint_decision = kind == 0 ? 0u : 1u;
        all.push_back(c);
    }
    for (unsigned kind : {0u, 1u}) {
        c = kind == 0 ? diffuse() : conductor();
        c.query[0] = 2;
        c.query[4] = kind;
        c.query[3] = 4;
        c.previous_q = c.pdf[0];
        all.push_back(c); // Both footprint PDFs come from actual forced queries.
        if (kind == 1) {
            c.query[5] = bits(.01f);
            c.footprint_decision = 1;
            all.push_back(c); // Actual very-sharp pivot PDF rejects inverse test.
        }
    }
    c = conductor();
    c.query[0] = 2;
    c.query[4] = 2;
    c.query[3] = 1;
    c.query[32] = bits(1);
    c.query[33] = bits(std::numeric_limits<float>::quiet_NaN());
    all.push_back(c); // TerminalE bypass.
    c = diffuse();
    c.query[0] = 2;
    c.query[4] = 0;
    c.query[3] = 2;
    c.query[32] = c.query[33] = bits(1);
    c.status = invalid;
    all.push_back(c);
    // Direction-only common-law queries are independent of normal/material
    // eligibility. These append to, rather than replace, every prior oracle.
    c = edge({0, 0, 0}, {0, 0, 1}, {0, 0, 1});
    c.query[0] = 3;
    c.tangent_direction = true;
    all.push_back(c);
    c = edge({0, 0, 0}, {0, 0, -std::numeric_limits<float>::denorm_min()}, {0, 0, 1});
    c.query[0] = 3;
    c.tangent_direction = true;
    all.push_back(c);
    c = edge({0, 0, 0}, {1, -std::numeric_limits<float>::denorm_min(), 0}, {1, 0, 0});
    c.query[0] = 3;
    all.push_back(c); // Negative subnormal direction component must retain sign.
    c = edge({0, 0, 0}, {-3, 0, 4}, {0, 0, 1});
    c.query[0] = 3;
    all.push_back(c);
    c = edge({std::numeric_limits<float>::max(), 0, 0}, {-std::numeric_limits<float>::max(), 0, 0},
             {1, 0, 0});
    c.query[0] = 3;
    all.push_back(c);
    c = edge({0, 0, 0}, {0, 0, 0}, {0, 0, 1});
    c.query[0] = 3;
    all.push_back(c);
    c = edge({0, 0, 0},
             {std::numeric_limits<float>::max(), -std::numeric_limits<float>::denorm_min(), 0},
             {1, 0, 0});
    c.query[0] = 3;
    c.status = invalid;
    all.push_back(c); // Nonzero component below FLOAT representability is error.
    c = edge({0, 0, 0}, {0, 0, 1}, {0, 0, 1});
    c.query[0] = 3;
    c.query[4] = bits(std::numeric_limits<float>::quiet_NaN());
    c.status = invalid;
    all.push_back(c);
    c = edge({0, 0, 0}, {0, 0, 1}, {0, 0, 1});
    c.query[0] = 3;
    c.query[14] = bits(std::numeric_limits<float>::infinity());
    c.status = invalid;
    all.push_back(c);
    // Runtime utility witnesses append without changing prior geometry cases.
    c = edge({0, 0, 0}, {3, 0, 4}, {0, 0, 1});
    c.query[0] = 4;
    all.push_back(c);
    c = edge({0, 0, 0}, {0, 0, 2}, {0, 0, 1});
    c.query[0] = 4;
    c.query[3] = 1;
    c.duplicate_primary = true;
    all.push_back(c);
    c = edge({0, 0, 0}, {0, 0, 2}, {1, 0, 0});
    c.query[0] = 4;
    c.status = ok; // A ready primary with zero cosine admits no chart.
    all.push_back(c);
    c = edge({0, 0, 0}, {0, 0, 0}, {0, 0, 1});
    c.query[0] = 4;
    c.status = ok;
    all.push_back(c);
    c = edge({0, 0, 0}, {0, 0, 2}, {0, 0, 1});
    c.query[0] = 4;
    c.query[12] = bits(std::numeric_limits<float>::quiet_NaN());
    c.status = invalid;
    c.primary_ready = 0;
    all.push_back(c);
    c = edge({0, 0, 0}, {0, 0, 2}, {0, 0, 1});
    c.query[0] = 4;
    c.query[17] = bits(std::numeric_limits<float>::quiet_NaN());
    c.status = invalid;
    c.primary_ready = 0;
    all.push_back(c);
    for (unsigned axis : {0u, 2u})
        for (float sign : {-1.f, 1.f}) {
            c = {};
            c.query[0] = 5;
            const std::array<float, 3> position{{1, -2, 4}}, error{{.125f, .25f, .5f}};
            for (unsigned k = 0; k < 3; ++k) {
                c.query[4 + k] = bits(position[k]);
                c.query[8 + k] = bits(error[k]);
                c.direction[k] = position[k]; // Independent expected ray origin.
            }
            // Include a negative geometric normal; outgoing orientation still
            // determines the same requested outward offset direction.
            c.query[16 + axis] = bits(axis == 0 ? -1.f : 1.f);
            c.query[32 + axis] = bits(sign);
            c.normal_offset = error[axis];
            const float shifted = position[axis] + sign * error[axis];
            c.direction[axis] =
                std::nextafter(shifted, sign < 0 ? -std::numeric_limits<float>::infinity()
                                                 : std::numeric_limits<float>::infinity());
            all.push_back(c);
        }
    const auto lanes = [&](unsigned positive_P, std::array<float, 4> beta, unsigned expected) {
        Case value;
        value.query[0] = 6;
        value.needed = expected;
        for (unsigned k = 0; k < 4; ++k) {
            if (positive_P & (1u << k)) {
                value.query[8 + 2 * k] = bits(.5f);
                value.query[9 + 2 * k] = std::bit_cast<unsigned>(-1000);
            }
            value.query[32 + k] = bits(beta[k]);
        }
        all.push_back(value);
    };
    lanes(15, {0, 0, 0, 0}, 15); // beta0 still needs every positive-density lane.
    lanes(0, {0, 2, 0, 0}, 2);   // P0 still needs a positive-response lane.
    lanes(5, {0, 2, 0, 4}, 15);
    lanes(1, {1, 0, 0, 0}, 1); // Prior collapse removes companion requirements.
    lanes(1, {1, -0.f, -0.f, 0}, 1);
    lanes(0, {0, 0, std::numeric_limits<float>::denorm_min(), 0}, 4);
    lanes(0, {0, 0, 0, 0}, 0);
    // Actual rolling predicate, with independent hemisphere and normal-incidence
    // PDF arithmetic. No AS or candidate-builder oracle is used here.
    const auto certificate = [](Case value) {
        value.query[0] = 7;
        value.query[3] = 1;
        value.query[4] = 15;
        value.query[5] = 0;
        value.query[6] = 1;
        value.query[40] = value.query[44] = bits(.5f);
        value.query[41] = value.query[45] = 1;
        value.query[42] = value.query[46] = bits(1);
        value.query[47] = 1;
        value.needed = 1;
        return value;
    };
    c = certificate(diffuse());
    all.push_back(c);
    c.query[38] = bits(-1);
    c.needed = 0;
    all.push_back(c); // Diffuse finite hemisphere support is part of the veto.
    c.query[36] = bits(1);
    c.query[38] = 0;
    all.push_back(c); // Tangent has no continuous support.
    c = certificate(diffuse());
    c.query[3] = 0;
    c.query[5] = 1;
    c.needed = 0;
    all.push_back(c); // No base certificate means no earlier candidate.
    c.query[3] = 1;
    c.status = invalid;
    all.push_back(c); // A nonempty certificate binds its predecessor identity.
    c = certificate(diffuse());
    c.query[47] = 0;
    c.status = invalid;
    all.push_back(c);
    c = certificate(diffuse());
    c.query[4] = 0;
    c.status = invalid;
    all.push_back(c);
    c = certificate(diffuse());
    c.query[2] = 1;
    c.query[4] = 3;
    c.query[9] = bits(std::numeric_limits<float>::quiet_NaN());
    c.status = invalid;
    all.push_back(c); // Query mask must expose a needed lane absent from physical history.
    c = certificate(diffuse());
    c.query[2] = 1;
    c.query[4] = 3;
    c.query[9] = 0;
    all.push_back(c); // Zero reflectance is valid, and its likelihood is still queried.
    c = certificate(conductor());
    all.push_back(c);
    c.query[45] = std::bit_cast<unsigned>(-15); // D=2^-16.
    c.needed = 0;
    const long double q0 = 1 / (4 * std::acos(-1.L) * .25L * .5L);
    need(std::ldexp(1.L, -16) / q0 < .0002L * 4 * std::acos(-1.L),
         "Declared rough inverse-footprint negative has separated independent bound");
    all.push_back(c);
    // Incoming scattering survives a native hit whose two stored FLOAT
    // positions coincide. The independently specified ray is retained bit for
    // bit; geometric-only replay has no direction and would reject these cases.
    const auto incoming = [&](std::array<float, 3> current, std::array<float, 3> previous,
                              std::array<float, 3> ray, unsigned status, unsigned geometric,
                              unsigned chart_status, bool exact_ray) {
        auto value = edge(current, previous, {0, 0, 1});
        value.query[0] = 8;
        value.status = status;
        value.geometric_status = geometric;
        value.edge_status = chart_status;
        value.exact_ray = exact_ray;
        for (unsigned k = 0; k < 3; ++k) {
            value.query[32 + k] = bits(ray[k]);
            if (exact_ray)
                value.direction[k] = ray[k];
        }
        all.push_back(value);
    };
    incoming({0, 0, 0}, {0, 0, 0}, {0, 0, 1}, outside, outside, outside, true);
    incoming({1, -2, 4}, {1, -2, 4}, {1, -0.f, 0}, outside, outside, outside, true);
    incoming({0, -0.f, 0}, {-0.f, 0, -0.f}, {0, -1, -0.f}, outside, outside, outside, true);
    // Accepted near-unit ray must not be normalized or replaced by an epsilon
    // edge. Its literal length remains within the existing unit predicate.
    incoming({1, 2, 3}, {1, 2, 3}, {0, 0, 1 + 0x1p-18f}, outside, outside, outside, true);
    const auto maximum = std::numeric_limits<float>::max();
    incoming({maximum, -maximum, maximum}, {maximum, -maximum, maximum}, {0, 0, -1}, outside,
             outside, outside, true);
    for (const auto ray :
         {std::array{0.f, 0.f, 0.f}, std::array{0.f, 0.f, 2.f}, std::array{0.f, 0.f, 1 + 0x1p-15f},
          std::array{std::numeric_limits<float>::quiet_NaN(), 0.f, 1.f},
          std::array{0.f, std::numeric_limits<float>::infinity(), 1.f},
          std::array{maximum, maximum, maximum},
          std::array{0.f, 0.f, std::numeric_limits<float>::denorm_min()}})
        incoming({1, 2, 3}, {1, 2, 3}, ray, invalid, outside, outside, false);
    // Disagreeing valid rays cannot override a nonzero geometric edge. The
    // expected direction/distance come from the independent long-double oracle.
    incoming({0, 0, 0}, {0, 0, 2}, {1, 0, 0}, ok, ok, ok, false);
    incoming({1, 2, 3}, {4, 2, 7}, {0, 0, -1}, ok, ok, ok, false);
    incoming({0, 0, 0}, {0, 0, -std::numeric_limits<float>::denorm_min()}, {1, 0, 0}, ok, ok, ok,
             false);
    incoming({maximum, 0, 0}, {-maximum, 0, 0}, {0, 0, 1}, ok, ok, outside, false);
    // A valid fallback ray never masks malformed positions or a unit component
    // that cannot be materialized from a nonzero extreme geometric edge.
    incoming({std::numeric_limits<float>::quiet_NaN(), 0, 0}, {0, 0, 0}, {0, 0, 1}, invalid,
             invalid, invalid, false);
    incoming({std::numeric_limits<float>::infinity(), 0, 0},
             {std::numeric_limits<float>::infinity(), 0, 0}, {0, 0, 1}, invalid, invalid, invalid,
             false);
    incoming({0, 0, 0}, {maximum, std::numeric_limits<float>::denorm_min(), 0}, {0, 0, 1}, invalid,
             invalid, invalid, false);
    return all;
}
int run(const std::filesystem::path &path) {
    using namespace rypath;
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for pure hybrid geometry probe");
    NSError *error = nil;
    auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(path.c_str())] error:&error];
    need(library != nil, "Hybrid geometry library load failed");
    auto constants = [MTLFunctionConstantValues new];
    for (unsigned i = 0; i <= 18; ++i) {
        const bool yes = true;
        [constants setConstantValue:&yes type:MTLDataTypeBool atIndex:i];
    }
    const bool yes = true, no = false;
    [constants setConstantValue:&no type:MTLDataTypeBool atIndex:20];
    [constants setConstantValue:&yes type:MTLDataTypeBool atIndex:25];
    std::string diagnostic;
    auto pipeline = metal::load_pipeline(device, library, @"rypath_enhanced_hybrid_geometry_probe",
                                         diagnostic, nil, constants);
    need(pipeline != nil, diagnostic);
    const auto fixtures = cases();
    const unsigned count = unsigned(fixtures.size());
    std::vector<Words> queries;
    for (const auto &c : fixtures)
        queries.push_back(c.query);
    auto input = [device newBufferWithBytes:queries.data()
                                     length:queries.size() * sizeof(Words)
                                    options:MTLResourceStorageModeShared];
    constexpr unsigned guard = 256;
    auto output = [device newBufferWithLength:count * sizeof(Words) + 2 * guard
                                      options:MTLResourceStorageModeShared];
    need(input && output, "Hybrid geometry buffers");
    std::memset(output.contents, 0xa5, output.length);
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    need(queue && command && encoder, "Hybrid geometry command");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:guard atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Pure hybrid geometry probe");
    need(diagnostic.empty(), diagnostic);
    const auto *data = reinterpret_cast<const Words *>(
        static_cast<const unsigned char *>(output.contents) + guard);
    for (unsigned i = 0; i < count; ++i) {
        const auto &c = fixtures[i];
        const auto &v = data[i];
        const std::string label = "Hybrid geometry case" + std::to_string(i);
        need(v[0] == c.status, label + " status");
        if (c.query[0] == 0) {
            need(v[1] == c.family, label + " family");
            for (unsigned k = 0; k < 4; ++k)
                need(v[12 + k] == bits(1), label + " eta1");
            need(v[16] == 0, label + " no transmission substitution");
            if (c.conductor_witness != 0)
                need(v[18] == c.conductor_witness, label + " native cutoff/range witness");
            if (c.status != ok)
                continue;
            need(v[3] == unsigned(c.support) && v[17] == 0, label + " support/native differential");
            for (unsigned k = 0; k < 4; ++k) {
                close(f32(v[4 + k]), c.numerator[k], label + " independent numerator");
                close(f32(v[8 + k]), c.pdf[k], label + " independent marginal PDF");
            }
            close(f32(v[2]), c.family == 0 ? 1.L : .25L, label + " hero0 native NDF width");
        } else if (c.query[0] == 1) {
            if (c.status != ok)
                continue;
            for (unsigned k = 0; k < 3; ++k)
                close(f32(v[20 + k]), c.direction[k], label + " unit direction");
            close(scaled(v[24], v[25]), c.r2, label + " scaled distance²");
            close(scaled(v[26], v[27]), c.destination_G, label + " destinationG");
            close(scaled(v[28], v[29]), c.G, label + " sourceG");
            close(f32(v[30]), c.cosine, label + " source cosine");
            close(f32(v[31]), c.destination_cosine, label + " destination cosine");
        } else if (c.query[0] == 3) {
            if (c.status != ok)
                continue;
            for (unsigned k = 0; k < 3; ++k)
                close(f32(v[20 + k]), c.direction[k], label + " independent physical direction");
            close(scaled(v[24], v[25]), c.r2, label + " independent physical distance squared");
            if (c.tangent_direction)
                need(v[32] == outside, label + " direction survives normal-cosine chart rejection");
        } else if (c.query[0] == 8) {
            need(v[32] == c.geometric_status && v[33] == c.edge_status,
                 label + " independent geometric direction and chart status");
            if (c.status == outside) {
                need(c.exact_ray && v[24] == 0 && v[25] == 0,
                     label + " coincident fallback retains zero geometric distance");
                bool differs_from_geometric = false;
                for (unsigned k = 0; k < 3; ++k) {
                    need(v[20 + k] == c.query[32 + k],
                         label + " coincident incoming preserves exact traced ray bits");
                    need(v[36 + k] == 0,
                         label + " geometric-only negative control has no direction");
                    differs_from_geometric |= v[20 + k] != v[36 + k];
                }
                need(differs_from_geometric && v[33] == outside,
                     label + " ray fallback restores scattering without admitting zero-edge chart");
            } else if (c.status == ok) {
                for (unsigned k = 0; k < 3; ++k) {
                    close(f32(v[20 + k]), c.direction[k],
                          label + " independent incoming direction");
                    need(v[20 + k] == v[36 + k],
                         label + " nonzero incoming retains geometric direction bits");
                }
                close(scaled(v[24], v[25]), c.r2, label + " independent incoming distance squared");
            }
        } else if (c.query[0] == 4) {
            need(v[31] == c.primary_ready, label + " primary readiness");
            if (c.status != ok) {
                need(v[24] == 0 && v[25] == 0 && v[30] == 0,
                     label + " invalid primary never publishes footprint");
                continue;
            }
            close(scaled(v[24], v[25]), c.r2, label + " independent primary distance squared");
            close(f32(v[30]), c.cosine, label + " independent primary cosine");
            need(v[38] == 0 && v[37] == (c.cosine == 0 ? 3u : 0u),
                 label + " zero-footprint primary is not an eligible chart");
            if (c.duplicate_primary)
                need(v[36] == invalid, label + " duplicate primary is numerical failure");
        } else if (c.query[0] == 5) {
            need(v[35] == 0 && v[24] == bits(c.normal_offset),
                 label + " captured original full-point error offset");
            for (unsigned k = 0; k < 3; ++k)
                need(v[20 + k] == bits(static_cast<float>(c.direction[k])) &&
                         v[20 + k] == v[32 + k],
                     label + " exact original offset and independent CPU outward rounding");
        } else if (c.query[0] == 6) {
            need(v[40] == c.needed, label + " union of positive density and beta requirements");
        } else if (c.query[0] == 7) {
            need(v[40] == (c.status == ok ? c.needed : 0),
                 label + " actual rolling predecessor support/inverse-footprint predicate");
        } else if (c.status == ok) {
            need(v[40] == 0 && v[41] == c.footprint_decision && v[42] == 1,
                 label + " actual footprint classifier");
            close(scaled(v[48], v[49]), c.previous_q, label + " queried previous q");
            close(f32(v[45]), 1, label + " primary cosine");
        }
    }
    const auto *bytes = static_cast<const unsigned char *>(output.contents);
    for (unsigned i = 0; i < guard; ++i)
        need(bytes[i] == 0xa5 && bytes[output.length - guard + i] == 0xa5,
             "Hybrid geometry output guards");
    std::cout << count
              << " pure hybrid geometry/family/forced-query cases passed; strict-FP, no AS/trace\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Expected hybrid geometry metallib");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
