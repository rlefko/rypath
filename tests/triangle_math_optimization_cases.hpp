// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "triangle_product_guard.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
namespace rypath::test::triangle_math {
using Word = std::uint32_t;
using Input = std::array<Word, 4>;
inline float value(Word bits) { return std::bit_cast<float>(bits); }
inline std::vector<Input> residual_cases() {
    std::vector<Input> out;
    const auto append = [&](Word a, Word b, Word p) {
        for (Word parent : {0u, 1u})
            out.push_back({a, b, p, parent});
    };
    append((87u << 23) | 1u, (87u << 23) | 1u, (47u << 23) | 2u);
    append((86u << 23) | 1u, (87u << 23) | 1u, (46u << 23) | 2u);
    append(0x3f800001u, 0x3fc00000u, 0x3fc00001u);
    append(0x3f800001u, 0x3fc00000u, 0x3fc00002u);
    // H +/- the residual quantum; both residual and product signs, sums 173/174.
    for (Word eb : {86u, 87u})
        for (int delta : {-1, 1})
            for (Word upper : {0u, 1u})
                for (Word sign : {0u, 0x80000000u}) {
                    const Word ma = 0x800001u, mb = Word(0xc00000 + delta);
                    const Word mp = Word((std::uint64_t(ma) * mb) >> 23) + upper;
                    append((87u << 23) | 1u | sign, (eb << 23) | (mb & 0x7fffffu),
                           ((87u + eb - 127u) << 23) | (mp & 0x7fffffu) | sign);
                }
    for (Word shift : {22u, 23u, 24u, 25u})
        append(87u << 23, 87u << 23, (24u + shift) << 23);
    const Word edges[]{0u,          0x80000000u, 1u,          0x007fffffu, 0x00800000u,
                       0x3f800000u, 0x7f7fffffu, 0x7f800000u, 0x7fc00000u};
    for (Word a : edges)
        for (Word b : edges)
            for (Word p : edges)
                append(a, b, p);
    return out;
}
inline void check_residual_cpu() {
    using namespace triangle_numeric;
    if (!normal_product_residual_domain((87u << 23) | 1u, (87u << 23) | 1u, (47u << 23) | 2u)
             .supported ||
        normal_product_residual_domain((86u << 23) | 1u, (87u << 23) | 1u, (46u << 23) | 2u)
            .supported)
        throw std::runtime_error("FMA residual lattice boundary must select fast/fallback domains");
    for (Word primary : {0x3fc00001u, 0x3fc00002u})
        if (!normal_product_residual_domain(0x3f800001u, 0x3fc00000u, primary).supported ||
            product_residual_range(0x3f800001u, 0x3fc00000u, primary) == unsupported)
            throw std::runtime_error(
                "Both halfway primaries preserve the original acceptance rule");
    for (auto c : residual_cases()) {
        const auto domain = normal_product_residual_domain(c[0], c[1], c[2]);
        if (!domain.supported)
            continue;
        // In this shift domain the exact product and subtraction fit binary64.
        const double error = double(value(c[0])) * double(value(c[1])) - double(value(c[2]));
        const bool accepted = std::abs(error) <= double(value(domain.half_ulp_bits));
        const bool reference = product_residual_range(c[0], c[1], c[2]) != unsupported;
        const float residual = std::fma(value(c[0]), value(c[1]), -value(c[2]));
        if (accepted != reference || (accepted && double(residual) != error) ||
            !std::isnormal(value(domain.half_ulp_bits)))
            throw std::runtime_error("Guarded FMA differs from exact residual/half-ULP oracle");
    }
}
inline std::vector<Input> exponent_cases() {
    std::vector<Input> out;
    const auto append = [&](Word bits, int scale) {
        for (Word parent : {0u, 1u})
            out.push_back({bits, std::bit_cast<Word>(scale), parent, 0});
    };
    for (Word exponent = 1; exponent <= 254; ++exponent)
        for (Word mantissa : {0u, 1u, 0x7fffffu})
            for (Word sign : {0u, 0x80000000u})
                for (int scale : {1 - int(exponent), 254 - int(exponent), -int(exponent),
                                  255 - int(exponent), -1, 0, 1})
                    append(sign | (exponent << 23) | mantissa, scale);
    for (Word bits : {0u, 0x80000000u, 1u, 0x80000001u, 0x7fffffu, 0x807fffffu, 0x7f7fffffu,
                      0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u})
        for (int scale : {-255, -127, -1, 0, 1, 127, 255, std::numeric_limits<int>::max()})
            append(bits, scale);
    // Original fallback negates the scale: INT_MIN is only defined on the zero early return.
    append(0u, std::numeric_limits<int>::min());
    append(0x80000000u, std::numeric_limits<int>::min());
    return out;
}
} // namespace rypath::test::triangle_math
