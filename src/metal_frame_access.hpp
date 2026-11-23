// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Internal Objective-C++ bridge. Portable frame headers expose no Metal types.
#include "frame_session.hpp"
#include "render_wire.h"
#import <Metal/Metal.h>

namespace rypath {
class MetalFrameLease;
struct MetalFramePipelineResult;
class MetalFramePipeline {
  public:
    ~MetalFramePipeline();
    MetalFramePipeline(MetalFramePipeline &&) noexcept;
    MetalFramePipeline &operator=(MetalFramePipeline &&) noexcept;
    MetalFramePipeline(const MetalFramePipeline &) = delete;
    MetalFramePipeline &operator=(const MetalFramePipeline &) = delete;
    id<MTLComputePipelineState> pipeline() const;
    id<MTLIntersectionFunctionTable> intersection_table() const;
    // Only for entries declaring the guide argument buffer at index 1.
    // Returns a fresh encoder so independently serialized streams do not share
    // its mutable argument-buffer binding.
    id<MTLArgumentEncoder> argument_encoder() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MetalFramePipeline(std::unique_ptr<Impl>);
    friend class MetalFrameLease;
};
struct MetalFramePipelineResult {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::unique_ptr<MetalFramePipeline> pipeline;
};
// Pins the entire completed frame and its slot, independently of Frame/Session.
// Keep this lease until every GPU consumer completes, or call retain_until_completed().
// Retaining a raw buffer alone does not prevent the renderer from reusing its slot.
// Only destruction/reassignment is supported on a moved-from lease, except the
// diagnostic-returning bind/retention methods and nullable device/buffer accessors.
class MetalFrameLease {
  public:
    ~MetalFrameLease();
    MetalFrameLease(MetalFrameLease &&) noexcept;
    MetalFrameLease &operator=(MetalFrameLease &&) noexcept;
    MetalFrameLease(const MetalFrameLease &) = delete;
    MetalFrameLease &operator=(const MetalFrameLease &) = delete;
    id<MTLDevice> device() const;
    id<MTLCommandQueue> queue() const;
    id<MTLBuffer> raw_xyz() const;
    const FrameInfo &info() const;
    const render_wire::Parameters &parameters() const;
    const render_wire::TextureParameters &texture_parameters() const;
    const FrameCamera &camera() const;
    IntersectionProfile intersection_profile() const;
    // Scene-specialized pipelines are reusable across leases from this session;
    // they do not pin the frame that created them. Uses the frame's loaded library.
    MetalFramePipelineResult create_guide_pipeline(NSString *entry) const;
    // Binds and selects the pipeline without dispatching. The guide signature
    // uses the resident render layout, full geometry, and caller arguments at 1.
    // A local full-film parameter copy disables packet/BDPT/lens rendering.
    std::string bind_guides(id<MTLComputeCommandEncoder> encoder,
                            const MetalFramePipeline &pipeline, id<MTLBuffer> arguments) const;
    // Attach before commit. No queue ordering is assumed: acquisition requires
    // producer completion; a consumer command on any same-device queue is valid.
    std::string retain_until_completed(id<MTLCommandBuffer> command) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MetalFrameLease(std::unique_ptr<Impl>);
    friend struct MetalFrameAccess;
};
struct MetalFrameAcquireResult {
    FrameStatus status = FrameStatus::failed;
    FrameInfo info;
    std::string diagnostic;
    std::unique_ptr<MetalFrameLease> lease;
};
struct MetalFrameAccess {
    // Reads completion metadata only; never waits, encodes GPU work, or reads pixels.
    static MetalFrameAcquireResult acquire(const Frame &frame);
};
} // namespace rypath
