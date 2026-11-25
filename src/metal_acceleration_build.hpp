// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "acceleration_build_policy.hpp"
#import <Metal/Metal.h>
#include <chrono>
#include <limits>
namespace rypath::metal {
inline bool supports_fast_acceleration_build(id<MTLDevice> device) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *))
        return device.supportsRaytracing && [device supportsFamily:MTLGPUFamilyApple9];
#endif
    (void)device;
    return false;
}
// Called before sizing; the same descriptor is subsequently passed to build.
inline bool prefer_fast_intersection(MTLAccelerationStructureDescriptor *descriptor, bool fast) {
    if (!fast)
        return true;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) {
        descriptor.usage |= MTLAccelerationStructureUsagePreferFastIntersection;
        return true;
    }
#endif
    (void)descriptor;
    return false;
}
inline bool record_acceleration_build(AccelerationBuildInfo &info, id<MTLAccelerationStructure> as,
                                      bool blas, bool fast,
                                      std::chrono::steady_clock::time_point start) {
    auto &bytes = blas ? info.blas_bytes : info.tlas_bytes;
    if (as.size > std::numeric_limits<std::uint64_t>::max() - bytes)
        return false;
    bytes += as.size;
    ++(blas ? info.blas_count : info.tlas_count);
    if (fast)
        ++(blas ? info.fast_blas_count : info.fast_tlas_count);
    info.build_cpu_wall_seconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}
} // namespace rypath::metal
