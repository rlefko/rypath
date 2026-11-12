// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pbrt.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

namespace rypath::pbrt {

// Exact sign of the linear determinant of a finite binary64 matrix.
// Each input is a signed integer significand times 2^exponent. The six triple
// products span bits -3222 through 3071; their sums fit in 200 32-bit limbs.
// Separate positive/negative magnitudes avoid rounding and cancellation. This
// calculation imposes no determinant threshold and does not compute an inverse.
inline int exact_linear_determinant_sign(const Transform &transform) {
    static_assert(std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53 && sizeof(double) == 8);
    using Magnitude = std::array<std::uint32_t, 200>;
    using Product = std::array<std::uint32_t, 6>;
    Magnitude positive{};
    Magnitude negative{};
    constexpr std::array permutations{std::array{0, 1, 2}, std::array{1, 2, 0},
                                      std::array{2, 0, 1}, std::array{0, 2, 1},
                                      std::array{1, 0, 2}, std::array{2, 1, 0}};
    const auto add_word = [](Magnitude &magnitude, std::size_t index, std::uint32_t word) {
        std::uint64_t carry = word;
        while (carry != 0 && index < magnitude.size()) {
            carry += magnitude[index];
            magnitude[index++] = static_cast<std::uint32_t>(carry);
            carry >>= 32;
        }
    };
    for (std::size_t term = 0; term < permutations.size(); ++term) {
        Product product{1};
        int exponent = 0;
        bool sign = term >= 3;
        for (std::size_t row = 0; row < 3; ++row) {
            const auto bits = std::bit_cast<std::uint64_t>(
                transform.values[row * 4 + static_cast<std::size_t>(permutations[term][row])]);
            const auto encoded_exponent = static_cast<int>((bits >> 52) & 0x7ff);
            auto significand = bits & ((std::uint64_t{1} << 52) - 1);
            if (encoded_exponent != 0) {
                significand |= std::uint64_t{1} << 52;
            }
            exponent += encoded_exponent == 0 ? -1074 : encoded_exponent - 1075;
            sign = sign != ((bits >> 63) != 0);
            const std::array factor{static_cast<std::uint32_t>(significand),
                                    static_cast<std::uint32_t>(significand >> 32)};
            Product next{};
            for (std::size_t i = 0; i < product.size(); ++i) {
                std::uint64_t carry = 0;
                std::size_t j = 0;
                for (; j < factor.size() && i + j < next.size(); ++j) {
                    const std::uint64_t value =
                        static_cast<std::uint64_t>(product[i]) * factor[j] + next[i + j] + carry;
                    next[i + j] = static_cast<std::uint32_t>(value);
                    carry = value >> 32;
                }
                for (std::size_t k = i + j; carry != 0 && k < next.size(); ++k) {
                    carry += next[k];
                    next[k] = static_cast<std::uint32_t>(carry);
                    carry >>= 32;
                }
            }
            product = next;
        }
        const auto shift = static_cast<unsigned>(exponent + 3222);
        auto &magnitude = sign ? negative : positive;
        for (std::size_t i = 0; i < product.size(); ++i) {
            const std::uint64_t shifted = static_cast<std::uint64_t>(product[i]) << (shift % 32);
            const auto index = shift / 32 + i;
            add_word(magnitude, index, static_cast<std::uint32_t>(shifted));
            add_word(magnitude, index + 1, static_cast<std::uint32_t>(shifted >> 32));
        }
    }
    for (std::size_t i = positive.size(); i > 0; --i) {
        if (positive[i - 1] != negative[i - 1]) {
            return positive[i - 1] < negative[i - 1] ? -1 : 1;
        }
    }
    return 0;
}

inline bool exact_singular_linear(const Transform &transform) {
    return exact_linear_determinant_sign(transform) == 0;
}

} // namespace rypath::pbrt
