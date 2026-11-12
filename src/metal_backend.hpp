// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace rypath {

enum class MetalStatus { success, not_built, no_device, failure };

struct MetalDeviceInfo {
    std::string name;
    std::string architecture;
    std::uint64_t registry_id = 0;
    bool unified_memory = false;
    bool compute_ray_tracing = false;
    bool render_ray_tracing = false;
};

struct MetalResult {
    MetalStatus status = MetalStatus::failure;
    MetalDeviceInfo device;
    std::string diagnostic;
    std::size_t dispatch_count = 0;
    std::size_t element_count = 0;
    std::size_t guard_count = 0;
};

MetalResult query_metal_device();

// An empty path loads rypath.metallib beside the canonical executable path.
// An explicit path is used by library-loading failure tests.
MetalResult run_metal_smoke_test(const std::filesystem::path &library_path = {});

} // namespace rypath
