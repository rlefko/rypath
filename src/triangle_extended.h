// SPDX-License-Identifier: GPL-3.0-only
// Finite binary24 pairs with independent exponents. Integer arithmetic follows
// round-to-nearest, ties-to-even; these remain approximate predicates.
#pragma once
#if defined(__METAL_VERSION__)
#include <metal_stdlib>
namespace triangle_extended {
using U = metal::ulong;
using W = metal::uint;
} // namespace triangle_extended
#else
#include <bit>
#include <cstdint>
namespace triangle_extended {
using U = std::uint64_t;
using W = std::uint32_t;
} // namespace triangle_extended
#endif
namespace triangle_extended {
struct Float {
    U mantissa = 0;
    int exponent = 0;
    bool negative = false, valid = true;
};
// Callers establish a nonzero integer before counting leading zeros.
inline unsigned top(U m) {
#if defined(__METAL_VERSION__)
    return 63u - unsigned(metal::clz(m));
#else
    return 63u - static_cast<unsigned>(std::countl_zero(m));
#endif
}
inline Float rounded(U m, int e, bool negative) {
    if (!m)
        return {};
    unsigned t = top(m);
    if (t < 23) {
        m <<= 23 - t;
        e -= int(23 - t);
    } else if (t > 23) {
        unsigned s = t - 23;
        U q = m >> s, tail = m & ((U{1} << s) - 1), halfway = U{1} << (s - 1);
        if (tail > halfway || (tail == halfway && (q & 1)))
            ++q;
        e += int(s);
        if (q == (U{1} << 24)) {
            q >>= 1;
            ++e;
        }
        m = q;
    }
    return {m, e, negative, e >= -100000 && e <= 100000};
}
inline Float from_bits(W bits) {
    W mag = bits & 0x7fffffffu, e = mag >> 23, m = mag & 0x7fffffu;
    if (e == 255)
        return {0, 0, false, false};
    if (!mag)
        return {};
    return e ? rounded(m | 0x800000u, int(e) - 150, bool(bits >> 31))
             : rounded(m, -149, bool(bits >> 31));
}
inline bool supported(Float a) {
    return a.valid && a.exponent >= -100000 && a.exponent <= 100000 &&
           (a.mantissa == 0 ? a.exponent == 0 && !a.negative
                            : a.mantissa >= (U{1} << 23) && a.mantissa < (U{1} << 24));
}
inline Float neg(Float a) {
    if (a.mantissa)
        a.negative = !a.negative;
    return a;
}
inline Float add(Float a, Float b) {
    if (!supported(a) || !supported(b))
        return {0, 0, false, false};
    if (!a.mantissa)
        return b;
    if (!b.mantissa)
        return a;
    if (a.exponent < b.exponent) {
        Float t = a;
        a = b;
        b = t;
    }
    unsigned gap = unsigned(a.exponent - b.exponent);
    if (gap > 39)
        return a;
    U x = a.mantissa << gap, y = b.mantissa;
    if (a.negative == b.negative)
        return rounded(x + y, b.exponent, a.negative);
    return x >= y ? rounded(x - y, b.exponent, a.negative) : rounded(y - x, b.exponent, b.negative);
}
inline Float sub(Float a, Float b) { return add(a, neg(b)); }
inline Float scale(Float a, int increment) {
    if (!supported(a) || increment > 200000 || increment < -200000)
        return {0, 0, false, false};
    if (!a.mantissa)
        return a;
    a.exponent += increment;
    a.valid = a.exponent >= -100000 && a.exponent <= 100000;
    return a;
}

inline Float mul(Float a, Float b) {
    if (!supported(a) || !supported(b))
        return {0, 0, false, false};
    return rounded(a.mantissa * b.mantissa, a.exponent + b.exponent, a.negative != b.negative);
}
// p must be the computed rounded mul(a,b), never an arbitrary supplied primary.
inline Float product_error(Float a, Float b, Float p) {
    if (!supported(a) || !supported(b) || !supported(p))
        return {0, 0, false, false};
    if (!a.mantissa || !b.mantissa)
        return {};
    U exact = a.mantissa * b.mantissa;
    int shift = p.exponent - a.exponent - b.exponent;
    if (shift < 0 || shift > 25)
        return {0, 0, false, false};
    U q = p.mantissa << unsigned(shift);
    return exact >= q ? rounded(exact - q, a.exponent + b.exponent, a.negative != b.negative)
                      : rounded(q - exact, a.exponent + b.exponent, a.negative == b.negative);
}
inline Float div(Float a, Float b) {
    if (!supported(a) || !supported(b) || !b.mantissa)
        return {0, 0, false, false};
    if (!a.mantissa)
        return {};
    U r = a.mantissa;
    int e = a.exponent - b.exponent;
    if (r < b.mantissa) {
        r <<= 1;
        --e;
    }
    U q = 1;
    r -= b.mantissa;
    for (unsigned i = 0; i < 25; ++i) {
        r <<= 1;
        q <<= 1;
        if (r >= b.mantissa) {
            r -= b.mantissa;
            q |= 1;
        }
    }
    U m = q >> 2, low = q & 3;
    if (low > 2 || (low == 2 && (r || (m & 1))))
        ++m;
    e -= 23;
    if (m == (U{1} << 24)) {
        m >>= 1;
        ++e;
    }
    return {m, e, a.negative != b.negative, e >= -100000 && e <= 100000};
}
inline int sign(Float a) { return a.mantissa ? (a.negative ? -1 : 1) : 0; }
inline int magnitude(Float a, Float b) {
    if (!a.mantissa || !b.mantissa)
        return a.mantissa ? 1 : b.mantissa ? -1 : 0;
    return a.exponent < b.exponent   ? -1
           : a.exponent > b.exponent ? 1
           : a.mantissa < b.mantissa ? -1
           : a.mantissa > b.mantissa ? 1
                                     : 0;
}
inline bool same(Float a, Float b) {
    return a.mantissa == b.mantissa && a.exponent == b.exponent && a.negative == b.negative &&
           a.valid == b.valid;
}
struct Pair {
    Float high, low;
};
inline Pair pair(Float a) { return {a, {}}; }
inline Pair pair_from_bits(W high, W low = 0) { return {from_bits(high), from_bits(low)}; }
inline Pair neg(Pair a) { return {neg(a.high), neg(a.low)}; }
inline Pair scale(Pair a, int e) { return {scale(a.high, e), scale(a.low, e)}; }
inline bool valid(Pair a) { return supported(a.high) && supported(a.low); }
// Full TwoSum is used for renormalization: no magnitude premise is required.
inline Pair two_sum(Float a, Float b) {
    Float s = add(a, b), part = sub(s, a);
    return {s, add(sub(a, sub(s, part)), sub(b, part))};
}
inline Pair add(Pair a, Pair b) {
    Float s = add(a.high, b.high), part = sub(s, a.high);
    Float error = add(add(sub(a.high, sub(s, part)), sub(b.high, part)), add(a.low, b.low));
    return two_sum(s, error);
}
inline Pair sub(Pair a, Pair b) { return add(a, neg(b)); }
inline bool before(Pair a, Pair b) {
    int h = magnitude(a.high, b.high);
    if (h)
        return h < 0;
    int l = magnitude(a.low, b.low);
    if (l)
        return l < 0;
    bool ar = a.low.mantissa && (a.high.negative != a.low.negative),
         br = b.low.mantissa && (b.high.negative != b.low.negative);
    return ar < br;
}
inline Pair mul(Pair a, Pair b) {
    if (before(b, a)) {
        Pair t = a;
        a = b;
        b = t;
    }
    Float p = mul(a.high, b.high),
          error =
              add(add(product_error(a.high, b.high, p), mul(a.high, b.low)), mul(a.low, b.high));
    return two_sum(p, error);
}
inline Pair div(Pair a, Pair b) {
    Float q = div(a.high, b.high);
    Float correction = div(sub(a, mul(b, pair(q))).high, b.high);
    return add(pair(q), pair(correction));
}
inline int sign(Pair a) { return sign(a.high) ? sign(a.high) : sign(a.low); }
inline bool same(Pair a, Pair b) { return same(a.high, b.high) && same(a.low, b.low); }
struct Conversion {
    W bits = 0;
    bool finite = true;
};
// Public binary32 output only. Ranking retains the complete Pair.
inline Conversion to_bits(Float a) {
    if (!supported(a))
        return {0, false};
    if (!a.mantissa)
        return {};
    W signbit = a.negative ? 0x80000000u : 0;
    if (a.exponent > 104)
        return {signbit | 0x7f800000u, false};
    if (a.exponent >= -149)
        return {signbit | (W(a.exponent + 150) << 23) | W(a.mantissa & 0x7fffffu), true};
    unsigned shift = unsigned(-149 - a.exponent);
    U q = 0;
    if (shift <= 24) {
        q = a.mantissa >> shift;
        U tail = a.mantissa & ((U{1} << shift) - 1), midpoint = U{1} << (shift - 1);
        if (tail > midpoint || (tail == midpoint && (q & 1)))
            ++q;
    }
    return {signbit | W(q), true};
}
struct Comparison {
    int order = 0;
    bool valid = true;
};
#if defined(__METAL_VERSION__)
using Signed = long;
#else
using Signed = std::int64_t;
#endif
// Exact comparison of four signed dyadics, including non-normalized/gapped
// pairs. No floating subtraction or fixed-significand collapse is involved.
inline Comparison compare(Pair a, Pair b) {
    if (!valid(a) || !valid(b))
        return {0, false};
    Float terms[4] = {a.high, a.low, neg(b.high), neg(b.low)};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = i + 1; j < 4; ++j)
            if ((!terms[i].mantissa && terms[j].mantissa) ||
                (terms[j].mantissa && terms[j].exponent > terms[i].exponent)) {
                Float t = terms[i];
                terms[i] = terms[j];
                terms[j] = t;
            }
    Signed acc = 0;
    int exponent = 0;
    for (unsigned i = 0; i < 4; ++i) {
        Float t = terms[i];
        if (!t.mantissa)
            continue;
        if (acc == 0) {
            acc = t.negative ? -Signed(t.mantissa) : Signed(t.mantissa);
            exponent = t.exponent;
            continue;
        }
        U magnitude = acc < 0 ? U(-acc) : U(acc);
        unsigned leading = top(magnitude);
        if (int(leading) + exponent >= t.exponent + 26)
            return {acc < 0 ? -1 : 1, true};
        unsigned gap = unsigned(exponent - t.exponent);
        // The dominance check bounds leading+gap<26. Shift unsigned magnitude;
        // never left-shift a negative signed integer.
        U aligned = magnitude << gap;
        acc = acc < 0 ? -Signed(aligned) : Signed(aligned);
        acc += t.negative ? -Signed(t.mantissa) : Signed(t.mantissa);
        exponent = t.exponent;
    }
    return {acc < 0 ? -1 : acc > 0 ? 1 : 0, true};
}
} // namespace triangle_extended
