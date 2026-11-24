// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "metal_texture_snapshot.hpp"
#include "reconstruction.hpp"
#import <Metal/Metal.h>
#include <cstddef>
namespace rypath {
// Pins a completed output slot. Its texture is read-only to consumers and never
// aliases a reconstruction input. Attach retention before committing GPU work.
class MetalReconstructedFrameLease {
  public:
    ~MetalReconstructedFrameLease();
    MetalReconstructedFrameLease(MetalReconstructedFrameLease &&) noexcept;
    MetalReconstructedFrameLease &operator=(MetalReconstructedFrameLease &&) noexcept;
    MetalReconstructedFrameLease(const MetalReconstructedFrameLease &) = delete;
    MetalReconstructedFrameLease &operator=(const MetalReconstructedFrameLease &) = delete;
    id<MTLDevice> device() const;
    id<MTLCommandQueue> queue() const;
    id<MTLTexture> output_texture() const;
    std::string retain_until_completed(id<MTLCommandBuffer>) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MetalReconstructedFrameLease(std::unique_ptr<Impl>);
    friend struct MetalReconstructedFrameAccess;
};
struct MetalReconstructedFrameAcquireResult {
    ReconstructionStatus status = ReconstructionStatus::failed;
    ReconstructionInfo info;
    std::string diagnostic;
    std::unique_ptr<MetalReconstructedFrameLease> lease;
};
struct MetalReconstructedFrameAccess {
    static MetalReconstructedFrameAcquireResult acquire(const ReconstructedFrame &);
};
// Explicit diagnostic readback, externally serialized with submit(). Copies
// surface guides only after completion; never exposes stream-owned textures.
struct MetalReconstructionGuideSnapshot {
    std::string diagnostic;
    std::array<std::vector<std::byte>, 12> textures; // Last four are temporal-only spatial guides.
};
struct MetalReconstructionSnapshot {
    std::string diagnostic;
    ReconstructionInfo info;
    std::vector<MetalReconstructionTextureSnapshot> textures;
};
struct MetalReconstructionStreamProbe {
    static MetalReconstructionGuideSnapshot capture_guides(const ReconstructionStream &);
    // Requires opt-in, a retained last output and no subsequent submission.
    // Externally serialize with submit; copies actual input/history/pass values.
    static MetalReconstructionSnapshot capture_diagnostics(const ReconstructionStream &);
};
} // namespace rypath
