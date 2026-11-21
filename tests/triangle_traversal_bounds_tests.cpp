// SPDX-License-Identifier: GPL-3.0-only
#include "triangle_traversal_bounds.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
using namespace triangle_traversal;
W bits(float value) { return std::bit_cast<W>(value); }
double value(Float value) {
    return std::ldexp(static_cast<double>(value.mantissa) * (value.negative ? -1 : 1),
                      value.exponent);
}
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool same_fields(Float a, Float b) {
    return a.mantissa == b.mantissa && a.exponent == b.exponent && a.negative == b.negative &&
           a.valid == b.valid;
}
void check_world_box(Input input) {
    const auto fast = world_box_fp32(input), reference = world_box_e24(input);
    if (!fast.valid)
        return;
    require(reference.valid, "Fast world box must have a supported original E24 box");
    for (unsigned axis = 0; axis < 3; ++axis) {
        require(same_fields(fast.lower[axis], reference.lower[axis]),
                "Fast lower world bound must preserve all E24 fields");
        require(same_fields(fast.upper[axis], reference.upper[axis]),
                "Fast upper world bound must preserve all E24 fields");
    }
}
void float_boundaries() {
    using namespace triangle_extended;
    const W words[]{
        0u,          0x80000000u,      1u,          0x007fffffu, 0x00800000u, 23u << 23,
        24u << 23,   (24u << 23) | 1u, 0x3f800000u, 0xbf800000u, 0x3f800001u, 0xbf800001u,
        0x7effffffu, 0xfeffffffu,      254u << 23,  0x7f7fffffu, 0x7f800000u, 0x7fc00000u};
    for (W a : words) {
        const auto x = box_float(a);
        for (bool upward : {false, true}) {
            const auto result = box_outward(x, upward);
            if (result.valid)
                require(same_fields(from_bits(bits(result.value)), outward(from_bits(a), upward)),
                        "Float adjacent encoding must match E24 outward rounding");
        }
        for (W b : words) {
            const auto y = box_float(b), product = box_mul(x, y), sum = box_add(x, y);
            if (product.valid)
                require(
                    same_fields(from_bits(bits(product.value)), mul(from_bits(a), from_bits(b))),
                    "Accepted float multiplication must preserve all E24 fields");
            if (sum.valid)
                require(same_fields(from_bits(bits(sum.value)), add(from_bits(a), from_bits(b))),
                        "Accepted float addition must preserve all E24 fields");
        }
    }
    require(box_float(0x80000000u).valid && bits(box_float(0x80000000u).value) == 0,
            "World-box signed zero must canonicalize exactly");
    require(!box_mul(box_float(75u << 23), box_float(75u << 23)).valid &&
                box_mul(box_float(75u << 23), box_float(76u << 23)).valid,
            "Product exponent sums 150/151 must separate fallback and safe domains");
    require(!box_add(box_float((24u << 23) | 1u), box_float(0x80000000u | (24u << 23))).valid,
            "Minimum-normal cancellation must fall back outside the stored domain");
    require(!box_outward(box_float(24u << 23), false).valid &&
                !box_outward(box_float(0x7effffffu), true).valid,
            "Outward steps crossing either stored-domain binade must fall back");
}
Input plane() {
    Input input{};
    const float corners[]{-8, -8, 1, 8, 8, 1, 8, -8, 1};
    for (unsigned i = 0; i < 9; ++i)
        input.corners[i] = bits(corners[i]);
    for (unsigned axis = 0; axis < 3; ++axis)
        input.transform[axis * 4 + axis] = bits(1);
    input.direction[2] = bits(1);
    return input;
}
} // namespace

int main() try {
    using namespace triangle_extended;
    require(std::fegetround() == FE_TONEAREST, "Float bounds require RN arithmetic");
    float_boundaries();
    auto input = plane();
    require(world_box_fp32(input).valid, "Ordinary plane must exercise the float graph");
    check_world_box(input);
    for (W word : {1u, 23u << 23, 254u << 23, 0x7fc00000u}) {
        auto fallback = input;
        fallback.transform[0] = word;
        require(!world_box_fp32(fallback).valid, "Unsafe source must abandon the float graph");
    }
    for (W word : {0u, 0x80000000u, 24u << 23, (24u << 23) | 1u, 0x3f800001u, 0xbf800001u,
                   0x7effffffu, 0xfeffffffu}) {
        auto boundary = input;
        boundary.transform[3] = word;
        check_world_box(boundary);
        boundary.corners[0] = word;
        check_world_box(boundary);
    }
    auto bound = exit_upper(input);
    require(bound.valid && value(bound.value) >= 1 && value(bound.value) < 1.00001,
            "Planar world-box exit must enclose the true plane tightly");
    const auto one = pair_from_bits(bits(1));
    auto cap = pruning_cap(bound, one, 0, bits(INFINITY));
    require(cap.finite && std::bit_cast<float>(cap.bits) > value(bound.value),
            "Traversal cap must lie strictly beyond the box exit");
    require(!pruning_cap(bound, one, 0, cap.bits).finite,
            "An unchanged cap must not reset traversal");
    require(!pruning_cap(bound, one, cap.bits, bits(INFINITY)).finite,
            "A cap at or below the original minimum must not reset traversal");
    require(!pruning_cap({}, one, 0, bits(INFINITY)).finite,
            "Unsupported bounds must abstain from pruning");
    for (W invalid_maximum : {0x80000000u, 0xbf800000u, 0x7fc00000u, 1u})
        require(!pruning_cap(bound, one, 0, invalid_maximum).finite,
                "Invalid, subnormal or zero traversal limits must not be enlarged");
    const auto later = pair_from_bits(bits(2), bits(0x1p-40f));
    cap = pruning_cap(bound, later, 0, bits(INFINITY));
    require(cap.finite && compare(pair_from_bits(cap.bits), later).order > 0,
            "Cap must retain the complete represented distance too");
    auto tiny = scale(from_bits(bits(1)), -200);
    cap = pruning_cap({tiny, true}, pair(tiny), 0, bits(INFINITY));
    require(cap.finite && cap.bits == 0x00800000u, "Traversal cap must avoid FTZ endpoints");
    auto huge = scale(from_bits(bits(1)), 200);
    require(!pruning_cap({huge, true}, pair(huge), 0, bits(INFINITY)).finite,
            "Unrepresentable bounds must abstain from pruning");
    input.direction[2] = bits(-1);
    input.origin[2] = bits(3);
    bound = exit_upper(input);
    require(bound.valid && value(bound.value) >= 2 && value(bound.value) < 2.00001,
            "Negative-direction slab division must remain outward");
    input.origin[0] = bits(20);
    require(!exit_upper(input).valid, "Parallel ray outside a slab must not produce a cap");
    input = plane();
    input.direction[2] = 0;
    require(!exit_upper(input).valid, "Zero direction has no finite exit");
    input.transform[0] = 0x7fc00000u;
    require(!exit_upper(input).valid, "Invalid transform must not produce a cap");

    // Dyadic integer-grid vertices/matrices make every forward vertex and its
    // subtraction from the origin exact in binary64. Only the independent final
    // quotient rounds; our binary24 outward margin is much larger than its ULP.
    std::mt19937 random(0x79a314u);
    auto integer = [&] { return static_cast<int>(random() % 2049) - 1024; };
    for (unsigned test = 0; test < 4000; ++test) {
        input = {};
        for (auto &corner : input.corners)
            corner = bits(static_cast<float>(integer()) / 8);
        for (auto &coefficient : input.transform)
            coefficient = bits(static_cast<float>(integer()) / 16);
        for (unsigned axis = 0; axis < 3; ++axis) {
            input.origin[axis] = bits(static_cast<float>(integer()) / 8);
            int direction = integer();
            input.direction[axis] = bits(static_cast<float>(direction ? direction : 1) / 64);
        }
        double expected = std::numeric_limits<double>::infinity();
        for (unsigned axis = 0; axis < 3; ++axis) {
            double lower = std::numeric_limits<double>::infinity();
            double upper = -std::numeric_limits<double>::infinity();
            for (unsigned corner = 0; corner < 3; ++corner) {
                double world = std::bit_cast<float>(input.transform[axis * 4 + 3]);
                for (unsigned j = 0; j < 3; ++j)
                    world += double(std::bit_cast<float>(input.transform[axis * 4 + j])) *
                             std::bit_cast<float>(input.corners[corner * 3 + j]);
                lower = std::min(lower, world);
                upper = std::max(upper, world);
            }
            const double d = std::bit_cast<float>(input.direction[axis]);
            const double o = std::bit_cast<float>(input.origin[axis]);
            expected = std::min(expected, ((d < 0 ? lower : upper) - o) / d);
        }
        check_world_box(input);
        bound = exit_upper(input);
        require(bound.valid && value(bound.value) >= expected,
                "Directed world-box exit must enclose independent dyadic geometry oracle");
    }
    std::cout << "PASS conservative world-box exits: 4000 affine cases and endpoint controls\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
