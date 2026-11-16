// SPDX-License-Identifier: Apache-2.0
// Gaussian tabulation and FastExp adapted from pbrt, Copyright (c) 1998-2020
// Matt Pharr, Wenzel Jakob, and Greg Humphreys. See third_party/pbrt/LICENSE.txt.

#include "gaussian_filter.hpp"

#include "render.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>

namespace rypath {
namespace {

float fast_exp(float x) {
    // Guard the conversion before evaluating PBRT's binary32 approximation.
    if (x < -100.f)
        return 0;
    const float xp = x * 1.442695041f;
    const float fxp = std::floor(xp);
    const float f = xp - fxp;
    const auto i = static_cast<int>(fxp);
    const float polynomial =
        std::fma(f, std::fma(f, std::fma(f, 0.0781455737f, 0.226173572f), 0.695556856f), 1.f);
    auto bits = std::bit_cast<std::uint32_t>(polynomial);
    const int exponent = static_cast<int>((bits >> 23) & 255) - 127 + i;
    if (exponent < -126)
        return 0;
    bits = (bits & 0x807fffffu) | (static_cast<std::uint32_t>(exponent + 127) << 23);
    return std::bit_cast<float>(bits);
}

float lerp(float t, float a, float b) { return (1 - t) * a + t * b; }

bool build_cdf(std::span<const float> values, float radius, std::span<float> cdf, float &integral) {
    cdf[0] = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        cdf[i + 1] = cdf[i] + values[i] * (2 * radius) / static_cast<float>(values.size());
        if (!std::isfinite(cdf[i + 1]))
            return false;
    }
    integral = cdf.back();
    if (integral > 0 && !std::isnormal(integral))
        return false;
    if (integral == 0) {
        // An inactive conditional row cannot be selected by the marginal.
        // Match PBRT's row representation without replacing a zero-mass filter.
        for (std::size_t i = 1; i < cdf.size(); ++i)
            cdf[i] = static_cast<float>(i) / static_cast<float>(values.size());
    } else {
        for (std::size_t i = 1; i < cdf.size(); ++i)
            cdf[i] /= integral;
    }
    for (std::size_t i = 1; i < cdf.size(); ++i) {
        const float gap = cdf[i] - cdf[i - 1];
        if (!std::isfinite(cdf[i]) || (cdf[i] > 0 && !std::isnormal(cdf[i])) ||
            (gap > 0 && !std::isnormal(gap)))
            return false;
    }
    return true;
}

// The scalar order follows the distribution domain, then its uniform variate.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
float sample_axis(std::span<const float> cdf, float radius, float u, std::size_t &cell) {
    u = std::clamp(u, 0.f, 0x1.fffffep-1f);
    // upper_bound skips leading and interior zero-mass plateaus, including u=0.
    cell = static_cast<std::size_t>(std::upper_bound(cdf.begin(), cdf.end(), u) - cdf.begin() - 1);
    const float du = (u - cdf[cell]) / (cdf[cell + 1] - cdf[cell]);
    const float t = (static_cast<float>(cell) + du) / static_cast<float>(cdf.size() - 1);
    return lerp(t, -radius, radius);
}

} // namespace

GaussianFilterCompilation compile_gaussian_filter(const PixelFilter &filter) {
    const auto fail = [](const char *message) { return GaussianFilterCompilation{{}, message}; };
    if (filter.kind != PixelFilterKind::gaussian)
        return fail("Gaussian compilation requires a Gaussian pixel filter");
    if (!std::isnormal(filter.x_radius) || filter.x_radius <= 0 ||
        !std::isnormal(filter.y_radius) || filter.y_radius <= 0 || !std::isnormal(filter.sigma) ||
        filter.sigma <= 0)
        return fail("Gaussian radii and sigma must be positive normal binary32 values");
    const float nx = 32 * filter.x_radius, ny = 32 * filter.y_radius;
    if (!(nx >= 1 && nx < gaussian_filter_max_axis + 1.f && ny >= 1 &&
          ny < gaussian_filter_max_axis + 1.f))
        return fail("Gaussian table axis must contain 1 to 4096 cells");
    const auto width = static_cast<std::uint32_t>(nx);
    const auto height = static_cast<std::uint32_t>(ny);
    if (std::uint64_t{width} * height > gaussian_filter_max_cells)
        return fail("Gaussian table exceeds the 1048576-cell limit");
    const float denominator = 2 * std::numbers::pi_v<float> * filter.sigma * filter.sigma;
    const float two_sigma_squared = 2 * filter.sigma * filter.sigma;
    const float scale = 1 / std::sqrt(denominator);
    if (!(denominator > 0 && std::isfinite(denominator) && two_sigma_squared > 0 &&
          std::isfinite(two_sigma_squared) && std::isfinite(scale)))
        return fail("Gaussian sigma arithmetic overflows or collapses in binary32");
    const auto gaussian = [&](float x) { return scale * fast_exp(-(x * x) / two_sigma_squared); };
    const float exp_x = gaussian(filter.x_radius), exp_y = gaussian(filter.y_radius);
    GaussianFilterDistribution distribution{width,
                                            height,
                                            filter.x_radius,
                                            filter.y_radius,
                                            std::vector<float>(std::size_t{height} * (width + 1)),
                                            std::vector<float>(height + 1)};
    std::vector<float> values(width), row_integrals(height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const float py = lerp((static_cast<float>(y) + 0.5f) / static_cast<float>(height),
                              -filter.y_radius, filter.y_radius);
        const float fy = std::max(0.f, gaussian(py) - exp_y);
        for (std::uint32_t x = 0; x < width; ++x) {
            const float px = lerp((static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                                  -filter.x_radius, filter.x_radius);
            values[x] = std::max(0.f, gaussian(px) - exp_x) * fy;
            if (!std::isfinite(values[x]))
                return fail("Gaussian table contains nonfinite binary32 values");
        }
        auto row = std::span(distribution.conditional_cdf)
                       .subspan(std::size_t{y} * (width + 1), width + 1);
        if (!build_cdf(values, filter.x_radius, row, row_integrals[y]))
            return fail("Gaussian conditional distribution overflows or collapses in binary32");
    }
    float integral;
    if (!build_cdf(row_integrals, filter.y_radius, distribution.marginal_cdf, integral) ||
        !(integral > 0))
        return fail("Gaussian table has a zero or nonfinite binary32 integral");
    distribution.integral = integral;
    return {std::move(distribution), {}};
}

std::array<float, 2> sample_gaussian_filter(const GaussianFilterDistribution &distribution, float u,
                                            float v) {
    if (!std::isfinite(u) || !std::isfinite(v)) {
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        return {nan, nan};
    }
    std::size_t row, cell;
    const float y = sample_axis(distribution.marginal_cdf, distribution.y_radius, v, row);
    const auto conditional = std::span(distribution.conditional_cdf)
                                 .subspan(row * (distribution.width + 1), distribution.width + 1);
    const float x = sample_axis(conditional, distribution.x_radius, u, cell);
    return {x, y};
}

} // namespace rypath
