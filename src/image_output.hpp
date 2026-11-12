// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "render.hpp"

#include <filesystem>

namespace rypath {
// Empty diagnostic on success. Publishes only after complete PNG encoding.
std::string write_png(const Film &film, const std::filesystem::path &path);
} // namespace rypath
