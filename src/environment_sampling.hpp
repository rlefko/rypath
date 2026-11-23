// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "image_texture.hpp"

namespace rypath {

// Each table stores a marginal CDF followed by width conditional row CDFs,
// all with width + 1 entries. The full table starts at zero.
struct EnvironmentSampling {
    std::uint32_t width = 0;
    std::uint32_t compensated_offset = 0;
    std::vector<float> cdf;
};

// Includes both native-resolution tables. Does not allocate or inspect pixels.
std::optional<std::size_t> environment_sampling_bytes(std::uint32_t width);

// The caller supplies the available retained budget (including device limits).
// Black full distributions and constant compensated distributions use uniform
// tables. Negative channels are clamped exactly as in environment emission.
std::optional<EnvironmentSampling> build_environment_sampling(const ImagePyramid &image,
                                                              std::size_t byte_budget,
                                                              std::string &diagnostic);

} // namespace rypath
