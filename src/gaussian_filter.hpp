// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rypath {

struct PixelFilter;

// Owned normalized PBRT midpoint-table distribution. Conditional rows have
// width+1 entries; the marginal has height+1 entries. The device representation
// concatenates conditional_cdf and marginal_cdf in that order.
struct GaussianFilterDistribution {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float x_radius = 0;
    float y_radius = 0;
    std::vector<float> conditional_cdf;
    std::vector<float> marginal_cdf;
};

struct GaussianFilterCompilation {
    GaussianFilterDistribution distribution;
    std::string diagnostic;
};

inline constexpr std::uint32_t gaussian_filter_max_axis = 4096;
inline constexpr std::uint32_t gaussian_filter_max_cells = 1048576;

GaussianFilterCompilation compile_gaussian_filter(const PixelFilter &filter);

// Samples the normalized table with unit film weight. Inputs in [0,1) are the
// production contract; finite out-of-range inputs are clamped to that interval.
// Requires an unmodified successful compiler result. Nonfinite inputs return NaNs.
std::array<float, 2> sample_gaussian_filter(const GaussianFilterDistribution &distribution, float u,
                                            float v);

} // namespace rypath
