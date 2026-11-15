// SPDX-License-Identifier: GPL-3.0-only
// Homogeneous scalar transport; HG uses PBRT's outward wo/wi convention.
#pragma once
#include "conductor.h"
namespace rypath_medium {
struct Segment {
    float transmission, emission;
    bool valid;
};
// Three positive factors divided by a fourth, with no lossy intermediate range.
inline float product_ratio(float a, float b, float c, float d) {
    if (a == 0 || b == 0 || c == 0)
        return 0;
    int ea, eb, ec, ed;
    float ma = frexp(a, ea), mb = frexp(b, eb), mc = frexp(c, ec), md = frexp(d, ed);
    return ldexp(((ma * mb) * mc) / md, ea + eb + ec - ed);
}
// (1-exp(-x))/x. Metal has no expm1; the series avoids cancellation at zero.
inline float attenuation_integral_ratio(float x) {
    if (x <= 0.125f) {
        float value = 1.0f / 40320.0f;
        value = fma(value, -x, 1.0f / 5040.0f);
        value = fma(value, -x, 1.0f / 720.0f);
        value = fma(value, -x, 1.0f / 120.0f);
        value = fma(value, -x, 1.0f / 24.0f);
        value = fma(value, -x, 1.0f / 6.0f);
        value = fma(value, -x, 0.5f);
        return fma(value, -x, 1.0f);
    }
    return (1.0f - exp(-x)) / x;
}
inline Segment segment(float sigma_a, float sigma_s, float Le, float distance) {
    if (!isfinite(sigma_a) || !isfinite(sigma_s) || !isfinite(Le) || sigma_a < 0 || sigma_s < 0 ||
        Le < 0 || isnan(distance) || distance < 0)
        return {0, 0, false};
    const float sigma_t = sigma_a + sigma_s;
    if (!isfinite(sigma_t))
        return {0, 0, false};
    if (distance == 0 || sigma_t == 0)
        return {1, 0, true};
    const float depth = sigma_t * distance;
    const float transmission = exp(-depth);
    float emission = 0;
    if (Le > 0 && sigma_a > 0) {
        if (depth <= 0.125f) {
            // Keep distance separate: sigma_t*distance can underflow while the
            // final Le*sigma_a*distance is still representable.
            emission =
                product_ratio(Le, sigma_a, distance, 1.0f / attenuation_integral_ratio(depth));
        } else {
            const float loss = isinf(depth) ? 1.0f : 1.0f - transmission;
            emission = product_ratio(Le, sigma_a, loss, sigma_t);
        }
    }
    return {transmission, emission, isfinite(emission) && emission >= 0};
}
inline float flight(float sigma_t, float u) {
    if (!isfinite(sigma_t) || sigma_t < 0 || !isfinite(u) || u < 0 || u > 1)
        return NAN;
    if (sigma_t == 0 || u == 1)
        return INFINITY;
    if (u == 0)
        return 0;
    float depth;
    if (u <= 0.125f) {
        // -log(1-u)/u; remainder is below binary32 precision on this interval.
        float value = 1.0f / 10.0f;
        for (int n = 9; n >= 2; --n)
            value = fma(value, u, 1.0f / float(n));
        depth = u * fma(value, u, 1.0f);
    } else {
        depth = -log(1.0f - u);
    }
    return depth / sigma_t;
}
inline float phase(float3 wo, float3 wi, float g) {
    if (!all(isfinite(wo)) || !all(isfinite(wi)) || !isfinite(g) ||
        abs(dot(wo, wo) - 1.0f) > 2e-5f || abs(dot(wi, wi) - 1.0f) > 2e-5f)
        return NAN;
    g = clamp(g, -0.99f, 0.99f);
    const float cosine = clamp(dot(wo, wi), -1.0f, 1.0f);
    // Sum of nonnegative terms avoids cancellation at the narrow HG peak.
    const float base = 1.0f - abs(g);
    const float denominator = fma(2.0f * abs(g), 1.0f + (g >= 0 ? cosine : -cosine), base * base);
    return (1.0f / (4.0f * M_PI_F)) * ((1.0f - g) * (1.0f + g)) / (denominator * sqrt(denominator));
}
inline float3 phase_sample(float3 wo, float g, float2 u) {
    if (!all(isfinite(wo)) || !isfinite(g) || !all(isfinite(u)) || any(u < 0) || any(u > 1) ||
        abs(dot(wo, wo) - 1.0f) > 2e-5f)
        return float3(NAN);
    g = clamp(g, -0.99f, 0.99f);
    // Exact inverse CDF, algebraically factored to stay continuous at g=0.
    // cos(wo,wi) = 2*u*(1-g)^2*(1+g*(1-u))/(1+g*(1-2*u))^2 - 1.
    // Reflect negative g about the equator so the narrow peak always comes
    // from a small nonnegative term minus one, never cancellation of O(1) terms.
    const float h = abs(g), v = g < 0 ? 1.0f - u.x : u.x;
    const float d = fma(h, 1.0f - 2.0f * v, 1.0f);
    const float ratio = (1.0f - h) / d;
    const float c =
        clamp(fma(2.0f * v * ratio * ratio, fma(h, 1.0f - v, 1.0f), -1.0f), -1.0f, 1.0f);
    const float cosine = g < 0 ? -c : c;
    const float sine = sqrt(max(0.0f, (1.0f - cosine) * (1.0f + cosine)));
    float3 x;
    if (!rypath_conductor::unit(cross(abs(wo.z) < 0.9f ? float3(0, 0, 1) : float3(0, 1, 0), wo), x))
        return float3(NAN);
    const float3 y = cross(wo, x);
    const float phi = 2.0f * M_PI_F * u.y;
    float3 result;
    if (!rypath_conductor::unit(cosine * wo + sine * (cos(phi) * x + sin(phi) * y), result))
        return float3(NAN);
    return result;
}
} // namespace rypath_medium
