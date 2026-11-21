// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "triangle_math_optimization_cases.hpp"
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
// Independent review examples include a supported-order union recovery and
// a multiply whose old two ordered graphs return different low components.
inline std::vector<Case> symmetry_cases() {
    std::vector<Case> result;
    const Case known[] = {{651559871u, 2589328319u, 650339161u, 440623960u},
                          {650865265u, 2588633713u, 647442266u, 437727067u},
                          {1066609694u, 3005770705u, 1066998364u, 858304568u}};
    for (auto c : known)
        for (Word signs = 0; signs < 4; ++signs) {
            if (signs & 1u) {
                c.high_a ^= 0x80000000u;
                c.low_a ^= 0x80000000u;
            }
            if (signs & 2u) {
                c.high_b ^= 0x80000000u;
                c.low_b ^= 0x80000000u;
            }
            result.push_back(c);
            // Restore the independent base case for the next sign combination.
            if (signs & 1u) {
                c.high_a ^= 0x80000000u;
                c.low_a ^= 0x80000000u;
            }
            if (signs & 2u) {
                c.high_b ^= 0x80000000u;
                c.low_b ^= 0x80000000u;
            }
        }
    return result;
}
using RoundedCase = std::array<Word, 2>;
inline std::vector<RoundedCase> rounded_cases() {
    std::vector<RoundedCase> out;
    const Word patterns[] = {0u, 1u, 2u, 0x3fffffu, 0x7ffffeu, 0x7fffffu};
    for (Word sum = 101; sum <= 106; ++sum)
        for (Word ma : patterns)
            for (Word mb : patterns)
                for (Word sign = 0; sign < 4; ++sign)
                    out.push_back({(52u << 23) | ma | ((sign & 1u) ? 0x80000000u : 0u),
                                   ((sum - 52u) << 23) | mb | ((sign & 2u) ? 0x80000000u : 0u)});
    // Actual point-light invalid cross term, all product signs.
    for (Word sign = 0; sign < 4; ++sign)
        out.push_back({(2298478592u & 0x7fffffffu) | ((sign & 1u) ? 0x80000000u : 0u),
                       330660155u | ((sign & 2u) ? 0x80000000u : 0u)});
    for (Word zero : {0u, 0x80000000u})
        for (Word other : {0u, 0x80000000u, 0x3f800000u, 0xbf800000u})
            out.push_back({zero, other});
    for (Word invalid : {1u, 0x80000001u, 0x007fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u})
        for (Word other : {0u, 0x80000000u, 0x3f800000u}) {
            out.push_back({invalid, other});
            out.push_back({other, invalid});
        }
    for (auto control :
         {RoundedCase{0x00800000u, 0x3f800000u}, RoundedCase{0x00800000u, 0x3f000000u},
          RoundedCase{0x7f7fffffu, 0x40000000u}, RoundedCase{0x3f800000u, 0x3f800000u}})
        out.push_back(control);
    return out;
}
inline bool rounded_zero_oracle(Word a, Word b) {
    const auto supported = [](float x) { return x == 0 || std::isnormal(x); };
    if (!supported(value(a)) || !supported(value(b)))
        return false;
    return std::abs(double(value(a)) * double(value(b))) <= std::ldexp(1., -150);
}
inline void check_rounded_cpu() {
    for (auto c : rounded_cases())
        if (triangle_numeric::rounded_product_is_zero(c[0], c[1]) !=
            rounded_zero_oracle(c[0], c[1]))
            throw std::runtime_error("Rounded-zero predicate differs from exact double oracle");
}
inline void check_cpu() {
    triangle_math::check_residual_cpu();
    triangle_math::check_normalized_product_cpu();
    check_rounded_cpu();
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
