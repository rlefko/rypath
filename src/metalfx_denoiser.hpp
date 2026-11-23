// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Internal Objective-C++ interface; do not include in portable renderer headers.
#import <Metal/Metal.h>
#include <simd/simd.h>

#include <cstdint>
#include <memory>
#include <string>

namespace rypath {

struct MetalFXDenoiserConfig {
    std::uint32_t input_width = 0, input_height = 0;
    std::uint32_t output_width = 0, output_height = 0;
};

struct MetalFXTextureRequirement {
    MTLPixelFormat format = MTLPixelFormatInvalid;
    MTLTextureUsage usage = MTLTextureUsageUnknown;
    NSUInteger width = 0, height = 0;
};

struct MetalFXDenoiserRequirements {
    MetalFXTextureRequirement color, depth, motion, diffuse_albedo, specular_albedo;
    // Output additionally requires private storage. Exposure is fixed by this
    // adapter to shader-read usage; other usage masks are queried from MetalFX.
    MetalFXTextureRequirement normal, roughness, exposure, output;
};

struct MetalFXDenoiserInputs {
    id<MTLTexture> color = nil, depth = nil, motion = nil;
    // Noise-free linear RGB material guides, signed world-space normals, and
    // linear roughness. Depth must match view_to_clip and depth_reversed.
    id<MTLTexture> diffuse_albedo = nil, specular_albedo = nil;
    id<MTLTexture> normal = nil, roughness = nil, exposure = nil, output = nil;
    simd_float4x4 world_to_view = matrix_identity_float4x4;
    simd_float4x4 view_to_clip = matrix_identity_float4x4;
    // Current-to-previous motion, converted to input pixels by these scales.
    float motion_scale_x = 1, motion_scale_y = 1;
    // Only a GLOBAL projection offset, in input pixels. Zero is truthful when
    // film/lens Monte Carlo samples vary per path without global camera jitter.
    float jitter_x = 0, jitter_y = 0;
    bool depth_reversed = true, reset_history = true;
    // Optional synchronization for untracked textures, managed by the caller.
    id<MTLFence> fence = nil;
};

struct MetalFXDenoiserCreateResult;

// Externally serialize creation/use of a reconstruction stream. This adapter
// neither submits nor waits for command buffers and never reads raw accumulation.
// The caller ends its producer encoder and establishes GPU dependencies before
// encode(), orders temporal frames, and retains textures/frame slots through GPU
// completion. Every texture must have standalone storage (no views, buffer or
// heap backing, or IOSurfaces). Distinct output storage must not alias an input.
class MetalFXDenoiser {
  public:
    ~MetalFXDenoiser();
    MetalFXDenoiser(const MetalFXDenoiser &) = delete;
    MetalFXDenoiser &operator=(const MetalFXDenoiser &) = delete;
    const MetalFXDenoiserRequirements &requirements() const;
    // Empty diagnostic means encoding succeeded, not that GPU execution passed.
    // Inputs are linear RGB (not XYZ), un-pre-exposed, with opaque color alpha=1.
    // Exposure is a caller-populated positive finite 1x1 R16Float display hint. Contents
    // cannot be verified without GPU work/readback. Optional guides are disabled.
    std::string encode(id<MTLCommandBuffer> command, const MetalFXDenoiserInputs &inputs);
    static MetalFXDenoiserCreateResult create(id<MTLDevice> device,
                                              const MetalFXDenoiserConfig &config);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MetalFXDenoiser(std::unique_ptr<Impl> impl);
};

struct MetalFXDenoiserCreateResult {
    std::unique_ptr<MetalFXDenoiser> denoiser;
    std::string diagnostic;
};

} // namespace rypath
