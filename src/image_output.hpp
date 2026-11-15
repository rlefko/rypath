// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "exr_image.hpp"
#include "render.hpp"

#include <filesystem>

namespace rypath {
// Empty diagnostic on success. Publishes only after complete PNG encoding.
std::string write_png(const Film &film, const std::filesystem::path &path);
// Linear sRGB, without display clipping or transfer encoding. The raw film is unchanged.
// Publishes atomically after complete encoding and successful close.
std::string write_exr(const Film &film, const std::filesystem::path &path,
                      ExrPrecision precision = ExrPrecision::float32,
                      std::span<const std::pair<std::string, std::string>> metadata = {});
} // namespace rypath
