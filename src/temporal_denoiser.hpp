// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Internal Objective-C++ adapter. Raw Monte Carlo XYZ is never modified.
#include "metal_texture_snapshot.hpp"
#import <Metal/Metal.h>
#include <cstdint>
#include <memory>
#include <string>

namespace rypath {
class MetalFrameLease;
class MetalGPUStages;
struct TemporalDenoiserInputs {
    // Signed, unexposed RGBA32Float (alpha one), native resolution.
    id<MTLTexture> color = nil;
    id<MTLTexture> depth = nil, motion = nil; // R32Float, RG16Float; motion in pixels.
    id<MTLTexture> diffuse_albedo = nil, specular_albedo = nil, normal = nil; // RGBA16Float.
    // Current surface projected into the previous camera, not the current depth.
    id<MTLTexture> previous_depth = nil; // R32Float.
    id<MTLTexture> identity =
        nil;                     // RGBA32Uint(instance,primitive,kind,material), all UINT_MAX sky.
    id<MTLTexture> output = nil; // Distinct RGBA32Float, shader-write usage.
    bool reset_history = true;
};
struct TemporalDenoiserStats {
    std::uint32_t failure = 0, completed = 0, history_accepted = 0, history_rejected = 0;
};
struct TemporalDenoiserConfig {
    std::uint32_t width = 0, height = 0;
    std::uint32_t history_limit = 64, spatial_iterations = 5;
    bool capture_diagnostics = false;
};
struct TemporalDenoiserCreateResult;
// Externally serialized. Encode only one pending command; call complete after
// its GPU completion, even on failure. Caller pins adapter and all input/output
// textures through completion. All textures must be standalone, nonaliasing 2D
// allocations; no texture views, buffers, heaps or IOSurfaces.
// This signed SVGF-inspired adaptation is an approximate reconstruction, not a
// raw estimate or a confidence interval. Pre-spatial history avoids recurrent blur.
class TemporalDenoiser {
  public:
    ~TemporalDenoiser();
    TemporalDenoiser(const TemporalDenoiser &) = delete;
    TemporalDenoiser &operator=(const TemporalDenoiser &) = delete;
    static TemporalDenoiserCreateResult create(MetalFrameLease &, const TemporalDenoiserConfig &);
    // Conservative pre-allocation estimate: 16 textures at padded 64x64 extents,
    // 64 KiB overhead each, and two 64 KiB buffer allowances. Excludes caller
    // inputs/output and Metal pipeline objects. UINT64_MAX means invalid extent.
    static std::uint64_t owned_resource_bytes(std::uint32_t width, std::uint32_t height,
                                              bool capture_diagnostics = false);
    std::string encode(id<MTLCommandBuffer>, const TemporalDenoiserInputs &,
                       MetalGPUStages *stages = nullptr);
    // Empty means clean GPU arithmetic/counts and committed history; otherwise
    // invalidates history. Never publish output before successful completion.
    std::string complete(bool command_succeeded);
    TemporalDenoiserStats stats() const;
    // Empty unless opt-in and the last command completed successfully. Borrowed
    // until the next encode; caller must serialize and finish readback first.
    std::vector<NamedMetalTexture> diagnostic_textures() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit TemporalDenoiser(std::unique_ptr<Impl>);
};
struct TemporalDenoiserCreateResult {
    std::unique_ptr<TemporalDenoiser> denoiser;
    std::string diagnostic;
};
} // namespace rypath
