// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "image_texture.hpp"

#include <functional>
#include <utility>

namespace rypath {

enum class ExrPrecision { half, float32 };

struct ExrImageView {
    std::uint32_t width;
    std::uint32_t height;
    std::span<const float> rgb;
    ExrPrecision precision = ExrPrecision::float32;
    std::span<const std::pair<std::string, std::string>> attributes{};
};

// Positioned writes to a caller-owned unpublished destination. Empty diagnostic
// means all chunks and the context were finalized successfully.
using ExrWrite = std::function<bool(std::uint64_t, std::span<const std::uint8_t>)>;
std::string encode_exr(const ExrImageView &image, const ExrWrite &write,
                       const ImageLimits &limits = {});

} // namespace rypath
