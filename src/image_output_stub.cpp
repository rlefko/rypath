// SPDX-License-Identifier: GPL-3.0-only

#include "image_output.hpp"

namespace rypath {

std::string write_png(const Film &, const std::filesystem::path &) {
    return "PNG output was not built";
}

} // namespace rypath
