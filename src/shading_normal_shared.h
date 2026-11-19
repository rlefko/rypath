// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#else
#include <cmath>
#include <limits>
#endif

// The importance adjoint for the renderer's shading-normal cosine convention.
// See Veach, section 5.3.2, and PBRT 3's path-space measurement equation.
// This changes weights, not sampling densities or the radiance-mode BSDF.
namespace rypath_shading_normal {
#if defined(__METAL_VERSION__)
namespace math = metal;
#else
namespace math = std;
#endif

#if defined(__METAL_VERSION__)
inline float fraction(float value, thread int &exponent) { return metal::frexp(value, exponent); }
#else
inline float fraction(float value, int &exponent) { return std::frexp(value, &exponent); }
#endif

inline float invalid_value() {
#if defined(__METAL_VERSION__)
    return NAN;
#else
    return std::numeric_limits<float>::quiet_NaN();
#endif
}

struct Correction {
    float scale;
    int exponent;
    bool valid;
};

inline Correction correction(float wo_ns, float wi_ns, float wo_ng, float wi_ng) {
    if (!math::isfinite(wo_ns) || !math::isfinite(wi_ns) || !math::isfinite(wo_ng) ||
        !math::isfinite(wi_ng))
        return {0, 0, false};
    wo_ns = math::abs(wo_ns);
    wi_ns = math::abs(wi_ns);
    wo_ng = math::abs(wo_ng);
    wi_ng = math::abs(wi_ng);
    // Exact singular directions use the zero convention; no near-grazing cutoff.
    if (wo_ng == 0 || wi_ns == 0 || wo_ns == 0 || wi_ng == 0)
        return {0, 0, true};
    if (wo_ns == wo_ng && wi_ng == wi_ns)
        return {1, 0, true};
    int a, b, c, d;
    const float ma = fraction(wo_ns, a), mb = fraction(wi_ng, b);
    const float mc = fraction(wo_ng, c), md = fraction(wi_ns, d);
    return {(ma * mb) / (mc * md), a + b - c - d, true};
}

// An importance endpoint always includes the incident shading cosine. Cancel
// it algebraically before forming a weight, so a removable grazing singularity
// cannot overflow f*C before multiplication by |wi.ns|. Preserve the exact-zero
// convention of correction(), including a zero incident shading cosine.
inline Correction cosine_correction(float wo_ns, float wi_ns, float wo_ng, float wi_ng) {
    if (!math::isfinite(wo_ns) || !math::isfinite(wi_ns) || !math::isfinite(wo_ng) ||
        !math::isfinite(wi_ng))
        return {0, 0, false};
    wo_ns = math::abs(wo_ns);
    wi_ns = math::abs(wi_ns);
    wo_ng = math::abs(wo_ng);
    wi_ng = math::abs(wi_ng);
    if (wo_ng == 0 || wi_ns == 0 || wo_ns == 0 || wi_ng == 0)
        return {0, 0, true};
    if (wo_ns == wo_ng && wi_ng == wi_ns) {
        int exponent;
        const float scale = fraction(wi_ns, exponent);
        return {scale, exponent, true};
    }
    int a, b, c;
    const float ma = fraction(wo_ns, a), mb = fraction(wi_ng, b);
    const float mc = fraction(wo_ng, c);
    return {(ma * mb) / mc, a + b - c, true};
}

inline bool valid_correction(Correction c) {
    // Four finite binary32 frexp exponents give this exact bounded range.
    return c.valid && math::isfinite(c.scale) && c.scale >= 0 && c.exponent >= -552 &&
           c.exponent <= 552;
}

inline float apply(float value, Correction c) {
    if (!valid_correction(c) || !math::isfinite(value) || value < 0)
        return invalid_value();
    if (c.scale == 1 && c.exponent == 0)
        return value;
    if (value == 0 || c.scale == 0)
        return 0;
    int e;
    const float m = fraction(value, e);
    return math::ldexp(m * c.scale, e + c.exponent);
}

inline float multiply(float a, float b, Correction c) {
    if (!valid_correction(c) || !math::isfinite(a) || !math::isfinite(b) || a < 0 || b < 0)
        return invalid_value();
    if (c.scale == 1 && c.exponent == 0)
        return a * b;
    if (a == 0 || b == 0 || c.scale == 0)
        return 0;
    int ea, eb;
    const float ma = fraction(a, ea), mb = fraction(b, eb);
    // Keep representable final weights when an intermediate factor or product
    // would overflow or underflow. A nonfinite final weight still fails visibly.
    return math::ldexp((ma * mb) * c.scale, ea + eb + c.exponent);
}
} // namespace rypath_shading_normal
