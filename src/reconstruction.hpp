// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "frame_session.hpp"
#include "gpu_diagnostics.hpp"

namespace rypath {
enum class ReconstructionMode { raw_rgb, metalfx, temporal };
// Appearance guides are an explicit MetalFX approximation, pending image qualification.
enum class MetalFXGuidePolicy { primary_surface, appearance };
struct ReconstructionSubmitOptions {
    bool reset_history = false;
    // Display exposure hint only; never scales raw or reconstructed pixels.
    float exposure = 1;
};
inline constexpr std::uint32_t reconstruction_dimension_limit = 16384;
struct ReconstructionConfig {
    bool profile_stages = false;
    ReconstructionMode mode = ReconstructionMode::raw_rgb;
    std::uint32_t output_width = 0, output_height = 0; // Zero preserves input dimensions.
    double near_plane = .001; // Guides only; raw transport has no new cutoff.
    // Native signed-color fallback. Zero spatial passes selects temporal only.
    std::uint32_t history_limit = 64, spatial_iterations = 5;
    MetalFXGuidePolicy metalfx_guide_policy = MetalFXGuidePolicy::primary_surface;
    bool cache_surface_guides = true; // Reuse guides when resolved inputs match.
};
enum class ReconstructionStatus { pending, complete, failed };
struct ReconstructionCounters {
    std::uint32_t failure = 0, completed = 0, color_negative = 0, color_high = 0;
    std::uint32_t albedo_clamped = 0, roughness_clamped = 0, mix_fallback = 0,
                  measured_fallback = 0;
    std::uint32_t fourier_fallback = 0, layered_approximation = 0, transmission_approximation = 0,
                  normal_approximation = 0;
    std::uint32_t passthrough = 0, media_approximation = 0, invalid_previous = 0, background = 0;
    std::uint32_t directional_approximation = 0, appearance_replacement = 0, appearance_bounded = 0,
                  appearance_fallback = 0;
};
struct ReconstructionInfo {
    GPUStageDiagnostics gpu_stages{};
    ReconstructionStatus status = ReconstructionStatus::pending;
    FrameInfo source{};
    ReconstructionMode mode = ReconstructionMode::raw_rgb;
    std::uint32_t width = 0, height = 0;
    float exposure = 1; // Actual half-float MetalFX metadata, not a pixel multiplier.
    bool history_reset = true;
    bool guide_cache_hit = false;
    // Surface pixel lanes dispatched this job; zero on hits and in color-only mode.
    std::uint64_t generated_surface_pixels = 0;
    bool defocus_approximation = false; // Reconstruction uses pinhole guides for lens rendering.
    double gpu_seconds = 0;
    bool gpu_time_valid = false;
    std::uint64_t command_count = 0;
    ReconstructionCounters counters{};
    // Signed temporal fallback only; accepted+rejected equals the input pixels.
    std::uint32_t history_accepted = 0, history_rejected = 0;
    std::string diagnostic;
};
struct ReconstructionCapture {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::uint32_t width = 0, height = 0;
    // Unexposed linear RGB from separate presentation storage. Temporal mode
    // preserves signed float32 values; raw_rgb/MetalFX use clamped half inputs.
    // Raw XYZ is unchanged by every reconstruction mode.
    std::vector<std::array<float, 3>> linear_rgb{};
};
class ReconstructionStream;
class MetalReconstructedFrameLease;
struct MetalReconstructedFrameAccess;
class ReconstructedFrame {
  public:
    ~ReconstructedFrame();
    ReconstructedFrame(ReconstructedFrame &&) noexcept;
    ReconstructedFrame &operator=(ReconstructedFrame &&) noexcept;
    ReconstructedFrame(const ReconstructedFrame &) = delete;
    ReconstructedFrame &operator=(const ReconstructedFrame &) = delete;
    ReconstructionInfo poll() const;
    ReconstructionInfo wait() const;
    ReconstructionCapture capture() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit ReconstructedFrame(std::shared_ptr<Impl>);
    friend class ReconstructionStream;
    friend class MetalReconstructedFrameLease;
    friend struct MetalReconstructedFrameAccess;
};
struct ReconstructionSubmitResult {
    GeometryStatus status = GeometryStatus::failure;
    FrameStatus source_status = FrameStatus::failed;
    bool busy = false;
    std::string diagnostic;
    std::unique_ptr<ReconstructedFrame> frame{};
};
struct ReconstructionBuildResult;
// Externally serialize submit(). One submission may be pending, while two result
// slots may remain retained. Results and GPU consumers survive stream destruction.
class ReconstructionStream {
  public:
    ~ReconstructionStream();
    ReconstructionStream(ReconstructionStream &&) noexcept;
    ReconstructionStream &operator=(ReconstructionStream &&) noexcept;
    ReconstructionStream(const ReconstructionStream &) = delete;
    ReconstructionStream &operator=(const ReconstructionStream &) = delete;
    ReconstructionSubmitResult submit(const Frame &, bool reset_history = false);
    ReconstructionSubmitResult submit(const Frame &, const ReconstructionSubmitOptions &);

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit ReconstructionStream(std::shared_ptr<Impl>);
    friend struct MetalReconstructionStreamProbe;
    friend ReconstructionBuildResult build_reconstruction_stream(const Frame &,
                                                                 const ReconstructionConfig &);
};
struct ReconstructionBuildResult {
    GeometryStatus status = GeometryStatus::failure;
    FrameStatus source_status = FrameStatus::failed;
    std::string diagnostic;
    std::unique_ptr<ReconstructionStream> stream{};
};
ReconstructionBuildResult build_reconstruction_stream(const Frame &,
                                                      const ReconstructionConfig & = {});
} // namespace rypath
