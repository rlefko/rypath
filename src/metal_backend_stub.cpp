// SPDX-License-Identifier: GPL-3.0-only

#include "metal_backend.hpp"

namespace rypath {

MetalResult query_metal_device() {
    return {
        .status = MetalStatus::not_built,
        .device = {},
        .diagnostic = "Metal backend was not built",
    };
}

MetalResult run_metal_smoke_test(const std::filesystem::path &) { return query_metal_device(); }

} // namespace rypath
