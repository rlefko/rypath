// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "frame_work_diagnostics.hpp"
#include "gpu_diagnostics.hpp"
#include "metal_render.hpp"

namespace rypath {
// Frames contribute one fresh sample per pixel. Area replay additionally
// evaluates reused candidates and emits one properly weighted raw estimate.
// Footprints keep a fixed budget even when sample_index exceeds it; output is unclamped.
enum class FrameExecution { megakernel, wavefront };
enum class FramePacketLayout { packed, hot_cold, split_trace };
enum class FrameEstimator { independent, area_replay };
enum class FrameAreaPathDomain { summed = 0, contribution = 1, enhanced = 2 };
inline constexpr bool frame_area_contribution_family(FrameAreaPathDomain domain) {
    return domain == FrameAreaPathDomain::contribution || domain == FrameAreaPathDomain::enhanced;
}
inline constexpr std::uint32_t frame_area_canonical_version(FrameAreaPathDomain domain) {
    return domain == FrameAreaPathDomain::enhanced ? 4u : 0u;
}
inline constexpr std::uint32_t frame_area_discovery_version(FrameAreaPathDomain domain) {
    return domain == FrameAreaPathDomain::enhanced ? 1u : 0u;
}
inline constexpr const char *frame_area_path_domain_name(FrameAreaPathDomain domain) {
    switch (domain) {
    case FrameAreaPathDomain::summed:
        return "summed";
    case FrameAreaPathDomain::contribution:
        return "contribution";
    case FrameAreaPathDomain::enhanced:
        return "enhanced";
    }
    return "invalid";
}
// Full traversal remains the oracle until selected-prefix replay is qualified.
enum class FrameContributionReplay { full = 0, prefix = 1 };
inline constexpr const char *frame_contribution_replay_name(FrameContributionReplay replay) {
    switch (replay) {
    case FrameContributionReplay::full:
        return "full";
    case FrameContributionReplay::prefix:
        return "prefix";
    }
    return "invalid";
}
enum class FrameMutationPolicy { automatic, legacy, spectral_refresh };
enum class FrameAreaStencil { automatic, adjacent, reciprocal16 };
inline constexpr const char *frame_mutation_policy_name(FrameMutationPolicy policy) {
    switch (policy) {
    case FrameMutationPolicy::automatic:
        return "auto";
    case FrameMutationPolicy::legacy:
        return "legacy";
    case FrameMutationPolicy::spectral_refresh:
        return "spectral-refresh";
    }
    return "invalid";
}
inline constexpr const char *frame_area_stencil_name(FrameAreaStencil stencil) {
    switch (stencil) {
    case FrameAreaStencil::automatic:
        return "auto";
    case FrameAreaStencil::adjacent:
        return "adjacent";
    case FrameAreaStencil::reciprocal16:
        return "reciprocal16";
    }
    return "invalid";
}
enum class FrameAreaOutput { selected, conditional_mean };
struct FrameConfig {
    bool profile_stages = false;
    std::uint32_t width = 0, height = 0;
    std::uint64_t seed = 1;
    std::int32_t material_seed = 0;
    std::uint64_t footprint_samples = 1;
    std::uint64_t transient_byte_limit = 8ull * 1024 * 1024 * 1024;
    // Immutable numerical profile for this session; offline builders retain
    // their separate compensated default.
    IntersectionProfile intersection_profile = IntersectionProfile::native;
    ShadowTraversal shadow_traversal = ShadowTraversal::query;
    AccelerationBuildPolicy acceleration_build_policy = AccelerationBuildPolicy::default_build;
    FrameExecution execution = FrameExecution::wavefront;
    std::uint32_t capacity = 65536;
    // Rounds per asynchronous command, never a path-depth or boundary limit.
    std::uint32_t quantum = 8;
    std::uint32_t group_width = 32;
    // Semantic packet events per state publication; scalar scheduling is unchanged.
    std::uint32_t packet_events = 1;
    FramePacketLayout packet_layout = FramePacketLayout::packed;
    FrameEstimator estimator = FrameEstimator::independent;
    // Opt-in finite contribution slots; summed preserves the full-path oracle.
    FrameAreaPathDomain area_path_domain = FrameAreaPathDomain::summed;
    FrameContributionReplay contribution_replay = FrameContributionReplay::full;
    // Nonzero importance: L1(XYZ)/4 plus a positive scale-dependent floor.
    // Enhanced assigns exact zero XYZ a zero target; legacy retains the floor.
    // Source-aware MIS handles reuse; neither radiance nor weights are clipped.
    float target_scale = 1.f;
    // Conditional mean removes final spatial selection noise while preserving
    // the same selected reservoir for subsequent reuse. Selected is an ablation.
    FrameAreaOutput area_output = FrameAreaOutput::conditional_mean;
    // Exact temporal memoization only for complete identical evaluator contexts.
    // Enhanced retains the selected owned record/scale; camera/context changes
    // replay both directions. False provides a full-replay validation control.
    bool cache_temporal_evaluations = true;
    // Effective only for Enhanced Area prefix replay; disabled mode replays suffixes.
    bool enhanced_suffix_cache = true;
    // Corrected MH steps after temporal reuse; independent rendering applies zero.
    std::uint32_t mutation_steps = 1;
    // Automatic retains the legacy/adjacent defaults until a quality decision.
    FrameMutationPolicy mutation_policy = FrameMutationPolicy::automatic;
    FrameAreaStencil area_stencil = FrameAreaStencil::automatic;
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
    GPUStageDiagnostics gpu_stages{};
    FrameWorkDiagnostics path_work{};
    std::uint32_t width = 0, height = 0;
    std::uint64_t sample_index = 0, epoch = 0;
    FrameStatus status = FrameStatus::pending;
    double gpu_seconds = 0;
    bool gpu_time_valid = false;
    std::uint64_t completed_samples = 0;
    std::uint64_t command_count = 0;
    std::uint64_t area_serial = 0;
    bool history_used = false;
    bool temporal_evaluation_cache_used = false;
    bool enhanced_suffix_cache = false;
    std::array<std::uint64_t, 3> enhanced_suffix_hits{};
    std::uint64_t enhanced_suffix_misses = 0;
    FrameMutationPolicy mutation_policy = FrameMutationPolicy::legacy;
    FrameAreaStencil area_stencil = FrameAreaStencil::adjacent;
    FrameAreaPathDomain area_path_domain = FrameAreaPathDomain::summed;
    FrameContributionReplay contribution_replay = FrameContributionReplay::full;
    std::uint32_t contribution_depth = 0;
    std::uint32_t canonical_transport_version = 0, fresh_discovery_version = 0;
    // Resolved immutable stencil, ordered d0.x, d0.y, d1.x, d1.y.
    std::array<std::int32_t, 4> area_stencil_offsets{};
    std::uint64_t history_generation = 0;
    // Per-pixel pool sizes, including cached fresh/center candidates.
    std::uint32_t temporal_candidates = 0, spatial_candidates = 0;
    // Physical path evaluations; analytic null owners and cached responses are separate.
    std::uint64_t completed_evaluations = 0, fresh_candidates = 0;
    std::uint64_t cached_evaluations = 0, analytic_null_evaluations = 0, logical_evaluations = 0;
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
struct FrameSessionTestAccess;
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
    friend struct FrameSessionTestAccess;
};
struct FrameSessionBuildResult {
    GeometryStatus status = GeometryStatus::failure;
    MetalDeviceInfo device;
    std::string diagnostic;
    std::unique_ptr<FrameSession> session;
    ShadowTraversalSelection shadow_traversal{};
    AccelerationBuildInfo acceleration_build{};
};
// PT only. Callers explicitly copy/override authored BDPT before building.
FrameSessionBuildResult build_frame_session(const RenderScene &, const FrameConfig &,
                                            const std::filesystem::path &library_path = {});
} // namespace rypath
