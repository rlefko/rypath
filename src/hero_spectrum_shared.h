// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "packet_spectrum_shared.h"
#if defined(__METAL_VERSION__)
#define RYPATH_HERO_THREAD thread
#else
#include <array>
#include <bit>
#include <cstdint>
#endif

// Wilkie et al. 2014, Eq. 11, in recursive balance-mixture form.
// P[k] is the likelihood of the observed prefix with wavelength k as hero;
// beta[j] = 4 F[j] / sum(P). Keeping scaled likelihoods (rather than FLOAT
// posteriors) preserves a small alternative that may dominate at a later event.
namespace rypath_hero {
#if defined(__METAL_VERSION__)
using Vector = metal::float4;
inline unsigned bits(float x) { return as_type<unsigned>(x); }
inline float from_bits(unsigned x) { return as_type<float>(x); }
inline float fraction(float x, RYPATH_HERO_THREAD int &e) { return metal::frexp(x, e); }
inline float scale(float x, int e) { return metal::ldexp(x, e); }
#else
#define RYPATH_HERO_THREAD
using Vector = std::array<float, 4>;
inline unsigned bits(float x) { return std::bit_cast<std::uint32_t>(x); }
inline float from_bits(unsigned x) { return std::bit_cast<float>(x); }
inline float fraction(float x, int &e) { return std::frexp(x, &e); }
inline float scale(float x, int e) { return std::ldexp(x, e); }
#endif
inline bool nonnegative_finite(float x) { return bits(x) < 0x7f800000u || bits(x) == 0x80000000u; }
struct Number {
    float fraction = 0;
    int exponent = 0;
};
inline Number normalized(float x, int exponent) {
    if (x == 0)
        return {};
    int e;
    const float m = fraction(x, e);
    return {m, exponent + e};
}
inline Number number(float x) {
    const unsigned word = bits(x) & 0x7fffffffu;
    if (word == 0)
        return {};
    if ((word & 0x7f800000u) == 0)
        return normalized(float(word), -149);
    return normalized(x, 0);
}
inline Number multiply(Number a, Number b) {
    if (a.fraction == 0 || b.fraction == 0)
        return {};
    return normalized(a.fraction * b.fraction, a.exponent + b.exponent);
}
inline Number divide(Number a, Number b) {
    return normalized(a.fraction / b.fraction, a.exponent - b.exponent);
}
inline Number add(Number a, Number b) {
    if (a.fraction == 0)
        return b;
    if (b.fraction == 0)
        return a;
    if (a.exponent < b.exponent) {
        const auto temporary = a;
        a = b;
        b = temporary;
    }
    const int gap = a.exponent - b.exponent;
    if (gap > 24)
        return a;
    return normalized(a.fraction + scale(b.fraction, -gap), a.exponent);
}
struct Scalar {
    float value = 0;
    bool valid = false;
};
inline Scalar materialize(Number x) {
    if (x.fraction == 0)
        return {0, true};
    if (!(x.fraction >= .5f && x.fraction < 1) || x.exponent > 128)
        return {};
    if (x.exponent >= -125) {
        const float value = scale(x.fraction, x.exponent);
        return {value, nonnegative_finite(value)};
    }
    if (x.exponent < -149)
        return {0, true};
    // Construct gradual underflow with integer RNE. Likelihoods themselves
    // remain scaled; materialized throughput has ordinary binary32 limits.
    const unsigned significand = (bits(x.fraction) & 0x007fffffu) | 0x00800000u;
    const unsigned shift = unsigned(-125 - x.exponent);
    unsigned units = significand >> shift;
    const unsigned rest = significand & ((1u << shift) - 1);
    const unsigned halfway = 1u << (shift - 1);
    units += rest > halfway || (rest == halfway && (units & 1));
    return {from_bits(units), true};
}
struct Density {
    Number lane[4];
};
inline Density initial_density() {
    Density result{};
    for (unsigned k = 0; k < 4; ++k)
        result.lane[k] = number(1);
    return result;
}
inline Number total(const RYPATH_HERO_THREAD Density &p) {
    return add(add(p.lane[0], p.lane[1]), add(p.lane[2], p.lane[3]));
}
inline Number mixture(const RYPATH_HERO_THREAD Density &p, Vector pdf) {
    Number terms[4];
    for (unsigned k = 0; k < 4; ++k)
        terms[k] = multiply(p.lane[k], number(pdf[k]));
    return add(add(terms[0], terms[1]), add(terms[2], terms[3]));
}
struct Update {
    Density density;
    Vector beta;
    bool valid = false;
};
inline Update advance(const RYPATH_HERO_THREAD Density &prefix, Vector beta, Vector numerator,
                      Vector pdf) {
    Update out{};
    for (unsigned k = 0; k < 4; ++k) {
        if (!nonnegative_finite(pdf[k]) || !nonnegative_finite(numerator[k]) ||
            !nonnegative_finite(beta[k]))
            return out;
        out.density.lane[k] = multiply(prefix.lane[k], number(pdf[k]));
    }
    const Number before = total(prefix), after = total(out.density);
    if (!(before.fraction > 0 && after.fraction > 0))
        return out;
    const Number ratio = divide(before, after);
    for (unsigned k = 0; k < 4; ++k) {
        const auto value =
            materialize(multiply(multiply(number(beta[k]), number(numerator[k])), ratio));
        if (!value.valid)
            return out;
        out.beta[k] = value.value;
    }
    out.valid = true;
    return out;
}
inline Scalar balance(const RYPATH_HERO_THREAD Density &prefix, Vector sampled, Vector other) {
    for (unsigned k = 0; k < 4; ++k)
        if (!nonnegative_finite(sampled[k]) || !nonnegative_finite(other[k]))
            return {};
    const Number a = mixture(prefix, sampled), b = mixture(prefix, other), sum = add(a, b);
    if (!(sum.fraction > 0))
        return {};
    return materialize(divide(a, sum));
}
struct Contribution {
    Vector value;
    bool valid = false;
};
// Apply the represented weight BEFORE rounding the contribution to FLOAT.
// A subnormal/underflowing weight can still produce a normal result when
// multiplied by a large path value; materializing the weight first loses it.
inline Contribution balance_contribution(const RYPATH_HERO_THREAD Density &prefix, Vector value,
                                         Vector sampled, Vector other) {
    Contribution result{};
    for (unsigned k = 0; k < 4; ++k)
        if (!nonnegative_finite(value[k]) || !nonnegative_finite(sampled[k]) ||
            !nonnegative_finite(other[k]))
            return result;
    const Number a = mixture(prefix, sampled), b = mixture(prefix, other), sum = add(a, b);
    if (!(sum.fraction > 0))
        return result;
    const Number weight = divide(a, sum);
    for (unsigned k = 0; k < 4; ++k) {
        const auto weighted = materialize(multiply(number(value[k]), weight));
        if (!weighted.valid)
            return result;
        result.value[k] = weighted.value;
    }
    result.valid = true;
    return result;
}
// An explicit hero-only fallback is a different operation from a dispersive
// Dirac update. Its compensation is 1/posterior(hero), not an unconditional 4.
inline Update collapse(const RYPATH_HERO_THREAD Density &prefix, Vector beta) {
    Update out{};
    if (!(prefix.lane[0].fraction > 0) || !nonnegative_finite(beta[0]))
        return out;
    const auto value =
        materialize(multiply(number(beta[0]), divide(total(prefix), prefix.lane[0])));
    if (!value.valid)
        return out;
    out.density.lane[0] = number(1);
    out.beta[0] = value.value;
    out.valid = true;
    return out;
}
inline rypath_packet_spectrum::Wavelengths sample_uniform(float u) {
    rypath_packet_spectrum::Wavelengths out{};
    if (!(u >= 0 && u < 1)) {
        for (unsigned k = 0; k < 4; ++k)
            out.lambda[k] = out.pdf[k] = NAN;
        return out;
    }
    for (unsigned k = 0; k < 4; ++k) {
        float shifted = u + float(k) * .25f;
        if (shifted >= 1)
            shifted -= 1;
        float wavelength = 360 + 470 * shifted;
        // Rounding the largest admissible uniform can land on the open bound.
        if (wavelength >= 830)
            wavelength = from_bits(bits(830.f) - 1);
        out.lambda[k] = wavelength;
        out.pdf[k] = 1.f / 470.f;
    }
    return out;
}
} // namespace rypath_hero
#undef RYPATH_HERO_THREAD
