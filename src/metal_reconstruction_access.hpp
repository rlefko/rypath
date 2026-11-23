// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "reconstruction.hpp"
#import <Metal/Metal.h>
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
} // namespace rypath
