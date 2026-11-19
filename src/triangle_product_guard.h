// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#else
#include <bit>
#include <cstdint>
#endif
namespace rypath {
namespace triangle_numeric {
#if defined(__METAL_VERSION__)
using Word = uint;
using Wide = ulong;
#else
using Word = std::uint32_t;
using Wide = std::uint64_t;
#endif
enum ProductResidual : Word { unsupported = 0, exact_product = 1, normal_residual = 2 };
inline bool normal_or_zero(Word bits) {
    const Word magnitude = bits & 0x7fffffffu, exponent = magnitude >> 23;
    return magnitude == 0 || (exponent > 0 && exponent < 255);
}
inline unsigned highest_bit(Wide value) {
    // Callers establish nonzero before counting; no undefined clz(0).
#if defined(__METAL_VERSION__)
    return 63u - unsigned(metal::clz(value));
#else
    return 63u - static_cast<unsigned>(std::countl_zero(value));
#endif
}
// p must be the actual rounded binary32 product a*b. For normal inputs,
// a=ma*2^ea,b=mb*2^eb,p=mp*2^ep. Their integer difference identifies the
// actual FMA error a*b-p. Its leading bit determines normality: normal floats
// can have mantissa bits below 2^-126.
inline ProductResidual product_residual_range(Word a, Word b, Word p) {
    if (!normal_or_zero(a) || !normal_or_zero(b) || !normal_or_zero(p))
        return unsupported;
    const Word aa = a & 0x7fffffffu, bb = b & 0x7fffffffu, pp = p & 0x7fffffffu;
    if (aa == 0 || bb == 0)
        return pp == 0 && ((a ^ b ^ p) & 0x80000000u) == 0 ? exact_product : unsupported;
    if (pp == 0 || ((a ^ b ^ p) & 0x80000000u) != 0)
        return unsupported;
    const Wide ma = (aa & 0x7fffffu) | 0x800000u, mb = (bb & 0x7fffffu) | 0x800000u,
               mp = (pp & 0x7fffffu) | 0x800000u;
    const int ea = int(aa >> 23) - 150, eb = int(bb >> 23) - 150, ep = int(pp >> 23) - 150;
    const int shift = ep - ea - eb;
    // Actual normal rounded products have shifts 23 or 24. Reject malformed
    // products before shifting a 24-bit integer into the wide word.
    if (shift < 23 || shift > 24)
        return unsupported;
    const Wide product = ma * mb, rounded = mp << unsigned(shift);
    const Wide difference = product >= rounded ? product - rounded : rounded - product;
    if (difference == 0)
        return exact_product;
    // A correct rounded primary differs by at most half its unit, bounding the
    // residual to at most 24 significant bits, exactly representable by binary32.
    if (difference > (Wide{1} << unsigned(shift - 1)))
        return unsupported;
    const int leading = ea + eb + int(highest_bit(difference));
    return leading >= -126 && leading <= 127 ? normal_residual : unsupported;
}
// Only normal-or-zero raw inputs are supported. The exact product of two
// binary32 significands fits 48 bits. RN-even yields zero iff its magnitude
// is at most half of the minimum subnormal (2^-150), including the zero tie.
// This applies to deliberately rounded cross terms, not retained product errors.
inline bool rounded_product_is_zero(Word a, Word b) {
    if (!normal_or_zero(a) || !normal_or_zero(b))
        return false;
    const Word aa = a & 0x7fffffffu, bb = b & 0x7fffffffu;
    if (aa == 0 || bb == 0)
        return true;
    const Word sum = (aa >> 23) + (bb >> 23);
    if (sum <= 102)
        return true;
    if (sum >= 105)
        return false;
    const Word ma = (aa & 0x7fffffu) | 0x800000u;
    const Word mb = (bb & 0x7fffffu) | 0x800000u;
    if (sum == 104)
        return ma == 0x800000u && mb == 0x800000u;
    return Wide(ma) * Wide(mb) <= (Wide{1} << 47);
}
} // namespace triangle_numeric
} // namespace rypath
