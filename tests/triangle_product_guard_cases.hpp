// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "triangle_product_guard.h"
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
namespace rypath::test::product_guard {
using Word = std::uint32_t;
struct Case {
    Word high_a, low_a, high_b, low_b;
};
inline std::vector<Case> cases() {
    std::vector<Case> result{
        {0xb194d7a0u, 0, 0x1cce9497u, 0}, // Actual frozen-v6 night offending operands.
        {0x291e064bu, 0, 0x2961f9aeu, 0}, // Normal exact residual despite low product LSB.
        {0x21800001u, 0, 0x30800001u, 0}, // Genuine subnormal retained residual.
        {0x2e800000u, 0x21800001u, 0x30800001u, 0}, // Same tiny residual discarded in cross term.
        {0, 0, 0x3f800000u, 0},
        {0x80000000u, 0, 0x3f800000u, 0},
        {0x00800000u, 0, 0x3f800000u, 0},
        {0x00800000u, 0, 0x3f000000u, 0},
        {0x7f7fffffu, 0, 0x40000000u, 0},
        {0x3f800001u, 0, 0x3f800001u, 0},
        {0x3f800001u, 0, 0xbf800001u, 0}};
    // Invalid operand domains precede zero shortcuts, in either argument order.
    for (Word invalid : {1u, 0x80000001u, 0x007fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u})
        for (Word other : {0u, 0x80000000u, 0x3f800000u}) {
            result.push_back({invalid, 0, other, 0});
            result.push_back({other, 0, invalid, 0});
        }
    result.push_back({0x2b800001u, 0, 0x2b800001u, 0}); // Retained error 2^-126 is normal.
    result.push_back({0x2b800001u, 0, 0x2b000001u, 0}); // Retained error 2^-127 is unsupported.
    Word state = 143907u;
    auto next = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (unsigned i = 0; i < 5000; ++i) {
        auto normal = [&]() {
            Word x = next();
            return (x & 0x807fffffu) | ((1u + (next() % 254u)) << 23);
        };
        result.push_back({normal(), 0, normal(), 0});
    }
    return result;
}
inline float value(Word bits) { return std::bit_cast<float>(bits); }
inline Word bits(float value) { return std::bit_cast<Word>(value); }
// Independent oracle: every exact binary32 product fits binary64's 53 bits.
// Subtracting its rounded binary32 primary gives the exact residual here.
inline triangle_numeric::ProductResidual oracle(Word a_bits, Word b_bits) {
    const float a = value(a_bits), b = value(b_bits), p = a * b;
    const auto normal = [](float x) { return x == 0 || std::isnormal(x); };
    if (!normal(a) || !normal(b) || !normal(p) || !std::isfinite(p) || (a != 0 && b != 0 && p == 0))
        return triangle_numeric::unsupported;
    const double error = double(a) * double(b) - double(p);
    if (error == 0)
        return triangle_numeric::exact_product;
    return std::abs(error) >= std::numeric_limits<float>::min() ? triangle_numeric::normal_residual
                                                                : triangle_numeric::unsupported;
}
inline void check_cpu() {
    for (auto c : cases()) {
        float p = value(c.high_a) * value(c.high_b);
        if (triangle_numeric::product_residual_range(c.high_a, c.high_b, bits(p)) !=
            oracle(c.high_a, c.high_b))
            throw std::runtime_error(
                "Triangle product residual guard disagrees with independent double oracle");
    }
    // Malformed supplied primaries must never cause an invalid shift or clz(0).
    for (Word p : {0u, 0x80000000u, 0x7f800000u, 0x7fc00000u, 1u, 0xbf800000u, 0x7f000000u})
        if (triangle_numeric::product_residual_range(0x3f800000u, 0x3f800000u, p) !=
            triangle_numeric::unsupported)
            throw std::runtime_error("Triangle product residual guard accepts malformed primary");
}
} // namespace rypath::test::product_guard
