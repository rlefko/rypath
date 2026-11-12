// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "metal_geometry.hpp"

#import <Metal/Metal.h>

namespace rypath {

// Only native adapters may use the resources owned by a live geometry scene.
struct MetalGeometryAccess {
    static id<MTLDevice> device(const MetalGeometryScene &scene);
    static id<MTLCommandQueue> queue(const MetalGeometryScene &scene);
    static std::size_t instance_count(const MetalGeometryScene &scene);
    static std::uint32_t source_instance(const MetalGeometryScene &scene, std::size_t ordinal);
    static void bind(const MetalGeometryScene &scene, id<MTLComputeCommandEncoder> encoder,
                     NSUInteger slot);
};

} // namespace rypath
