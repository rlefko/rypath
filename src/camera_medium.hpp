// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "camera_medium_wire.h"
#include "frame_session.hpp"
#include <memory>
#include <string>
namespace rypath {
struct CameraMediumResult {
    GeometryStatus status = GeometryStatus::failure;
    camera_medium_wire::Status failure = camera_medium_wire::Status::invalid_config;
    std::string diagnostic;
    std::uint32_t medium = no_medium, crossings = 0;
    bool gpu_time_valid = false;
    double gpu_seconds = 0;
};
struct CameraMediumBuildResult;
// A bounded numerical classifier, not a watertightness/overlap proof. Authored
// interfaces must be consistent and provably opaque. Crossings are queried on
// the immutable original ray, without offset jumps or advancing t_min. The
// query extends past the destination so t=1 is not a native interval endpoint;
// certified side tests distinguish future interfaces from actual crossings.
// Distinct hits at the same represented distance are rejected, including
// ambiguous shared-edge crossings; native near-equality remains profile-defined.
// Destination is the actual float-packed origin to be submitted. Failure means retain the previous
// pose. Externally serialize calls. classify waits for one small GPU query; its cost belongs in
// viewer end-to-end latency. It never reads or mutates raw pixels.
class CameraMediumTracker {
  public:
    ~CameraMediumTracker();
    CameraMediumTracker(CameraMediumTracker &&) noexcept;
    CameraMediumTracker &operator=(CameraMediumTracker &&) noexcept;
    CameraMediumTracker(const CameraMediumTracker &) = delete;
    CameraMediumTracker &operator=(const CameraMediumTracker &) = delete;
    static CameraMediumBuildResult build(const Frame &completed_frame);
    // current must be complete and belong to the build session/profile. Its
    // resolved camera medium is the known starting state. Pass every successful
    // result.medium explicitly in ALL subsequent FrameCamera requests; sessions
    // do not remember the preceding request's medium override.
    CameraMediumResult classify(const Frame &current, GeometryPoint destination,
                                std::uint32_t crossing_limit = medium_boundary_limit);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit CameraMediumTracker(std::unique_ptr<Impl>);
};
struct CameraMediumBuildResult {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::unique_ptr<CameraMediumTracker> tracker;
};
} // namespace rypath
