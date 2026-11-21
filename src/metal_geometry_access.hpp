// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "metal_geometry.hpp"

#import <Metal/Metal.h>

namespace rypath {

// Only native adapters may use the resources owned by a live geometry scene.
struct NativeHintStats {
    std::uint64_t acceleration_bytes = 0;
    std::uint64_t scratch_peak_bytes = 0;
    std::uint64_t input_peak_bytes = 0;
    std::uint64_t shared_record_bytes = 0;
    std::uint64_t native_blas_count = 0;
    std::uint64_t instance_count = 0;
    std::uint64_t eligible_instance_count = 0;
};
struct MetalGeometryAccess {
    static bool has_hint(const MetalGeometryScene &scene);
    static NativeHintStats hint_stats(const MetalGeometryScene &scene);
    static void bind_hint(const MetalGeometryScene &scene, id<MTLComputeCommandEncoder> encoder,
                          NSUInteger slot);
    static id<MTLDevice> device(const MetalGeometryScene &scene);
    static id<MTLCommandQueue> queue(const MetalGeometryScene &scene);
    // A mixed source instance has a native ordinal for each primitive kind.
    static std::size_t instance_count(const MetalGeometryScene &scene);
    static std::uint32_t source_instance(const MetalGeometryScene &scene, std::size_t ordinal);
    static PrimitiveKind primitive_kind(const MetalGeometryScene &scene, std::size_t ordinal);
    static void bind(const MetalGeometryScene &scene, id<MTLComputeCommandEncoder> encoder,
                     NSUInteger slot);
};

} // namespace rypath
