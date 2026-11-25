// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#import <Metal/Metal.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rypath {
// Internal diagnostic borrowing only. The owner must remain externally
// serialized until readback completes. Never expose these handles to consumers.
struct NamedMetalTexture {
    std::string name;
    id<MTLTexture> texture = nil;
};
struct MetalReconstructionTextureSnapshot {
    std::string name;
    std::uint32_t width = 0, height = 0;
    MTLPixelFormat format = MTLPixelFormatInvalid;
    // Native little-endian component representation, tightly packed rows.
    // Retain quantized guide values; converting them to RGB loses replay data.
    std::vector<std::byte> bytes;
};
} // namespace rypath
