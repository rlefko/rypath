// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "triangle_extended.h"

namespace triangle_traversal {
using triangle_extended::Float;
using triangle_extended::Pair;
using triangle_extended::W;

// A vertex and every matrix coefficient are exact binary32 inputs. Matrix
// entries are row-major; the shader adapter explicitly copies Metal's columns.
struct Input {
    W corners[9];
    W transform[12];
    W origin[3];
    W direction[3];
};
struct Bound {
    Float value;
    bool valid = false;
};

inline int order(Float a, Float b) {
    const int sa = triangle_extended::sign(a), sb = triangle_extended::sign(b);
    if (sa != sb)
        return sa < sb ? -1 : 1;
    return sa * triangle_extended::magnitude(a, b);
}

// Adjacent values in the extended binary24 format. Zero produced by its finite
// add/multiply/divide operations is exact: this format has no gradual underflow.
// Thus an exact-zero operation needs no artificial smallest-positive padding.
inline Float outward(Float a, bool upward) {
    if (!triangle_extended::supported(a))
        return {0, 0, false, false};
    if (!a.mantissa)
        return a;
    const bool increase_magnitude = upward != a.negative;
    if (increase_magnitude) {
        ++a.mantissa;
        if (a.mantissa == (triangle_extended::U{1} << 24)) {
            a.mantissa >>= 1;
            ++a.exponent;
        }
    } else if (a.mantissa == (triangle_extended::U{1} << 23)) {
        a.mantissa = (triangle_extended::U{1} << 24) - 1;
        --a.exponent;
    } else {
        --a.mantissa;
    }
    a.valid = triangle_extended::supported(a);
    return a;
}

// All arithmetic remains binary32 and canonical zero. This is only a
// sufficient domain: any failed guard abandons the entire fast world box.
inline W fp32_bits(float a) {
#if defined(__METAL_VERSION__)
    return as_type<W>(a);
#else
    return std::bit_cast<W>(a);
#endif
}
inline float fp32_value(W a) {
#if defined(__METAL_VERSION__)
    return as_type<float>(a);
#else
    return std::bit_cast<float>(a);
#endif
}
inline bool fp32_domain(W a) {
    const W mag = a & 0x7fffffffu, e = mag >> 23;
    return mag == 0 || (e >= 24 && e <= 253);
}
struct BoxFloat {
    float value = 0;
    bool valid = false;
};
inline BoxFloat box_float(W a) { return {fp32_value((a & 0x7fffffffu) ? a : 0), fp32_domain(a)}; }
inline BoxFloat box_mul(BoxFloat a, BoxFloat b) {
#if defined(__METAL_VERSION__)
#pragma METAL fp contract(off)
#endif
    if (!a.valid || !b.valid)
        return {};
    const W ea = (fp32_bits(a.value) >> 23) & 255u;
    const W eb = (fp32_bits(b.value) >> 23) & 255u;
    if (!ea || !eb)
        return {0, true};
    // The exact product is at least 2^-103; binary32 cannot flush it to zero.
    if (ea + eb < 151)
        return {};
    const float product = a.value * b.value;
    return box_float(fp32_bits(product));
}
inline BoxFloat box_add(BoxFloat a, BoxFloat b) {
#if defined(__METAL_VERSION__)
#pragma METAL fp contract(off)
#endif
    if (!a.valid || !b.valid)
        return {};
    // Every operand is a multiple of 2^-126: a nonzero exact sum is normal.
    const float sum = a.value + b.value;
    return box_float(fp32_bits(sum));
}
inline BoxFloat box_outward(BoxFloat a, bool upward) {
    if (!a.valid)
        return {};
    W bits = fp32_bits(a.value);
    if (!(bits & 0x7fffffffu))
        return {0, true};
    if (upward != bool(bits >> 31))
        ++bits;
    else
        --bits;
    return box_float(bits);
}
struct WorldBox {
    Float lower[3], upper[3];
    bool valid = false;
};
inline WorldBox world_box_e24(Input input) {
    using namespace triangle_extended;
    Float lower[3], upper[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        for (unsigned corner = 0; corner < 3; ++corner) {
            Float lo = from_bits(input.transform[axis * 4 + 3]), hi = lo;
            for (unsigned j = 0; j < 3; ++j) {
                const auto product = mul(from_bits(input.transform[axis * 4 + j]),
                                         from_bits(input.corners[corner * 3 + j]));
                lo = outward(add(lo, outward(product, false)), false);
                hi = outward(add(hi, outward(product, true)), true);
            }
            if (!supported(lo) || !supported(hi))
                return {};
            if (corner == 0 || order(lo, lower[axis]) < 0)
                lower[axis] = lo;
            if (corner == 0 || order(hi, upper[axis]) > 0)
                upper[axis] = hi;
        }
    }
    WorldBox result;
    for (unsigned axis = 0; axis < 3; ++axis) {
        result.lower[axis] = lower[axis];
        result.upper[axis] = upper[axis];
    }
    result.valid = true;
    return result;
}
inline WorldBox world_box_fp32(Input input) {
    float lower[3], upper[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        for (unsigned corner = 0; corner < 3; ++corner) {
            BoxFloat lo = box_float(input.transform[axis * 4 + 3]), hi = lo;
            for (unsigned j = 0; j < 3; ++j) {
                const auto product = box_mul(box_float(input.transform[axis * 4 + j]),
                                             box_float(input.corners[corner * 3 + j]));
                lo = box_outward(box_add(lo, box_outward(product, false)), false);
                hi = box_outward(box_add(hi, box_outward(product, true)), true);
            }
            if (!lo.valid || !hi.valid)
                return {};
            if (corner == 0 || lo.value < lower[axis])
                lower[axis] = lo.value;
            if (corner == 0 || hi.value > upper[axis])
                upper[axis] = hi.value;
        }
    }
    WorldBox result;
    for (unsigned axis = 0; axis < 3; ++axis) {
        result.lower[axis] = triangle_extended::from_bits(fp32_bits(lower[axis]));
        result.upper[axis] = triangle_extended::from_bits(fp32_bits(upper[axis]));
    }
    result.valid = true;
    return result;
}
// Bound the EXIT of a world-space box enclosing all forward-transformed
// vertices. This is deliberately looser than the computed intersection t.
// Each RN operation is rounded outward before it participates in the next one.
inline Bound exit_upper(Input input) {
    using namespace triangle_extended;
    // Abandon the entire float graph on any guard failure, never a partial box.
    const auto fast = world_box_fp32(input);
    const auto box = fast.valid ? fast : world_box_e24(input);
    if (!box.valid)
        return {};
    Float lower[3], upper[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        lower[axis] = box.lower[axis];
        upper[axis] = box.upper[axis];
    }
    Bound result;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto direction = from_bits(input.direction[axis]);
        const auto origin = from_bits(input.origin[axis]);
        if (!supported(origin) || !supported(direction))
            return {};
        if (!direction.mantissa) {
            if (order(origin, lower[axis]) < 0 || order(origin, upper[axis]) > 0)
                return {};
            continue;
        }
        // For negative d, a lower numerator gives an upper quotient.
        const auto edge = direction.negative ? lower[axis] : upper[axis];
        const auto numerator = outward(sub(edge, origin), !direction.negative);
        const auto far = outward(div(numerator, direction), true);
        if (!supported(far))
            return {};
        if (!result.valid || order(far, result.value) < 0)
            result = {far, true};
    }
    return result;
}

// A strict exterior binary32 endpoint also retains the represented candidate
// distance. Failure means abstain from pruning, never discard a candidate.
inline triangle_extended::Conversion pruning_cap(Bound bound, Pair distance, W minimum, W maximum) {
    using namespace triangle_extended;
    if (((minimum & 0x80000000u) && (minimum & 0x7fffffffu)) ||
        ((maximum & 0x80000000u) && (maximum & 0x7fffffffu)))
        return {0, false};
    minimum &= 0x7fffffffu;
    maximum &= 0x7fffffffu;
    if (minimum >= 0x7f800000u || maximum > 0x7f800000u || maximum < minimum ||
        (minimum && minimum < 0x00800000u) || (maximum && maximum < 0x00800000u))
        return {0, false};
    if (!bound.valid || !supported(bound.value) || !valid(distance))
        return {0, false};
    const auto rounded_distance = add(distance.high, distance.low);
    if (!supported(rounded_distance))
        return {0, false};
    auto limit = order(bound.value, rounded_distance) < 0 ? rounded_distance : bound.value;
    if (limit.negative)
        return {0, false};
    auto converted = to_bits(limit);
    if (!converted.finite || converted.bits >= 0x7f7fffffu)
        return {0, false};
    // Avoid a subnormal traversal limit on FTZ hardware. Every normal result is
    // exact binary32, and its successor is strictly above the extended bound.
    const W cap = converted.bits < 0x00800000u ? 0x00800000u : converted.bits + 1;
    if (cap <= minimum || cap >= maximum)
        return {0, false};
    return {cap, true};
}
} // namespace triangle_traversal
