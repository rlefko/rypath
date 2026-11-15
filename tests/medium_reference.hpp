// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
namespace medium_reference {
inline constexpr double pi = 3.141592653589793238462643383279502884;
// Independent high precision equations; no shader helper or inverse CDF is used.
inline long double phase(long double cosine, long double g) {
    g = std::clamp(g, -0.99L, 0.99L);
    const auto denominator = 1 + g * g + 2 * g * cosine;
    return (1 - g * g) / (4 * static_cast<long double>(pi) * denominator * std::sqrt(denominator));
}
inline long double cdf(long double cosine, long double g) {
    g = std::clamp(g, -0.99L, 0.99L);
    if (std::abs(g) >= 1e-4L) {
        return (1 - g * g) / (2 * g) * (1 / (1 - g) - 1 / std::sqrt(1 + g * g + 2 * g * cosine));
    }
    // Smooth small-g integral, deliberately independent of inverse algebra.
    constexpr int intervals = 32;
    const long double h = (cosine + 1) / intervals;
    long double sum = phase(-1, g) + phase(cosine, g);
    for (int i = 1; i < intervals; ++i)
        sum += (i % 2 ? 4 : 2) * phase(-1 + h * i, g);
    return 2 * static_cast<long double>(pi) * h * sum / 3;
}
inline long double emission(long double a, long double s, long double le, long double d) {
    if (a == 0 || le == 0 || d == 0)
        return 0;
    return le * a / (a + s) * -std::expm1(-(a + s) * d);
}
} // namespace medium_reference
