// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "metal_render.hpp"

namespace rypath {
// Frames contribute one fresh sample per pixel. Area replay additionally
// evaluates reused candidates and emits one reservoir-weighted raw estimate.
// Footprints keep a fixed budget even when sample_index exceeds it; output is unclamped.
enum class FrameExecution { megakernel, wavefront };
enum class FrameEstimator { independent, area_replay };
struct FrameConfig {
    std::uint32_t width = 0, height = 0;
    std::uint64_t seed = 1;
    std::int32_t material_seed = 0;
    std::uint64_t footprint_samples = 1;
    std::uint64_t transient_byte_limit = 8ull * 1024 * 1024 * 1024;
    // Immutable numerical profile for this session; offline builders retain
    // their separate compensated default.
    IntersectionProfile intersection_profile = IntersectionProfile::native;
    FrameExecution execution = FrameExecution::wavefront;
    std::uint32_t capacity = 65536;
    // Rounds per asynchronous command, never a path-depth or boundary limit.
    std::uint32_t quantum = 8;
    std::uint32_t group_width = 32;
    FrameEstimator estimator = FrameEstimator::independent;
    // Importance proxy only: 1 + .1*L1(XYZ)/(target_scale+L1(XYZ)).
    // Bounded contrast controls history weights without changing raw radiance.
    float target_scale = 1.f;
    // Corrected MH steps after temporal reuse; independent rendering applies zero.
    std::uint32_t mutation_steps = 1;
};
struct FrameCamera {
    RenderCamera camera;
    // Preserve the scene medium unless specified. Callers must update this on
    // boundary crossings; the session does not classify the camera position.
    std::optional<std::uint32_t> camera_medium;
    std::optional<RasterCamera> raster_camera;
    std::optional<PerspectiveLens> perspective_lens;
    // Required for changed cameras when textured fallback footprints are used.
    std::optional<TextureCamera> texture_camera;
};
struct FrameRequest {
    std::uint64_t sample_index = 0, epoch = 0;
    std::optional<FrameCamera> camera;
};
enum class FrameStatus { pending, complete, failed };
struct FrameInfo {
    std::uint32_t width = 0, height = 0;
    std::uint64_t sample_index = 0, epoch = 0;
    FrameStatus status = FrameStatus::pending;
    double gpu_seconds = 0;
    bool gpu_time_valid = false;
    std::uint64_t completed_samples = 0;
    std::uint64_t command_count = 0;
    std::uint64_t area_serial = 0;
    bool history_used = false;
    std::uint64_t history_generation = 0;
    // Per-pixel pool sizes, including cached fresh/center candidates.
    std::uint32_t temporal_candidates = 0, spatial_candidates = 0;
    // Full successful path evaluations; cached candidates add no evaluations.
    std::uint64_t completed_evaluations = 0, fresh_candidates = 0;
    std::uint64_t mutation_attempts = 0, mutation_accepts = 0;
    // Camera translation may upload rebased graph/surface records. Repeated
    // origins in the same free slot reuse those records without uploading.
    std::uint64_t camera_upload_bytes = 0;
    std::string diagnostic;
};
struct FrameCapture {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::uint32_t width = 0, height = 0;
    std::vector<std::array<float, 3>> raw_xyz;
};
class FrameSession;
class MetalFrameLease;
struct MetalFrameAccess;
class Frame {
  public:
    ~Frame();
    Frame(Frame &&) noexcept;
    Frame &operator=(Frame &&) noexcept;
    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;
    FrameInfo poll() const;
    FrameInfo wait() const;
    // Waits once, then explicitly copies the complete GPU-resident raw XYZ.
    FrameCapture capture() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit Frame(std::shared_ptr<Impl>);
    friend class FrameSession;
    friend class MetalFrameLease;
    friend struct MetalFrameAccess;
};
struct FrameSubmitResult {
    GeometryStatus status = GeometryStatus::failure;
    bool busy = false;
    std::string diagnostic;
    std::unique_ptr<Frame> frame;
};
struct FrameSessionBuildResult;
// Externally serialize submit(). A retained Frame reserves one of two slots,
// including after completion. Frames survive session destruction safely.
class FrameSession {
  public:
    ~FrameSession();
    FrameSession(FrameSession &&) noexcept;
    FrameSession &operator=(FrameSession &&) noexcept;
    FrameSession(const FrameSession &) = delete;
    FrameSession &operator=(const FrameSession &) = delete;
    FrameSubmitResult submit(const FrameRequest &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit FrameSession(std::unique_ptr<Impl>);
    friend FrameSessionBuildResult build_frame_session(const RenderScene &, const FrameConfig &,
                                                       const std::filesystem::path &);
};
struct FrameSessionBuildResult {
    GeometryStatus status = GeometryStatus::failure;
    MetalDeviceInfo device;
    std::string diagnostic;
    std::unique_ptr<FrameSession> session;
};
// PT only. Callers explicitly copy/override authored BDPT before building.
FrameSessionBuildResult build_frame_session(const RenderScene &, const FrameConfig &,
                                            const std::filesystem::path &library_path = {});
} // namespace rypath
