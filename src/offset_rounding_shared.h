// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#else
#include <cstdint>
#endif

namespace rypath_offset_rounding {
#if defined(__METAL_VERSION__)
using Word = uint;
#else
using Word = std::uint32_t;
#endif

struct Rounded {
    Word bits;
    bool valid;
};

// Strict outward neighbor among signed zero and finite binary32 normals.
// Integer input preserves subnormal words independently of Metal FTZ. Toward
// zero keeps the input sign; departing zero takes the requested sign. Finite
// outward overflow returns infinity/invalid, never a silently saturated value.
// Nonfinite inputs are preserved and invalid. Callers keep their failure checks.
inline Rounded outward(Word bits, bool toward_negative) {
    constexpr Word sign = 0x80000000u, magnitude = 0x7fffffffu;
    constexpr Word minimum = 0x00800000u, maximum = 0x7f7fffffu;
    const Word m = bits & magnitude, s = bits & sign;
    if (m > maximum)
        return {bits, false};
    if (m == 0)
        return {(toward_negative ? sign : 0u) | minimum, true};
    const bool away = ((s != 0) == toward_negative);
    if (away) {
        const Word next = m < minimum ? minimum : m + 1u;
        return {s | next, next <= maximum};
    }
    return {s | (m <= minimum ? 0u : m - 1u), true};
}
} // namespace rypath_offset_rounding
