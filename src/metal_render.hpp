// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "metal_geometry.hpp"
#include "render.hpp"
#include "shading_normal_probe_wire.h"

namespace rypath {

class RenderTimings;
class MetalFrameLease;

struct RenderRequest {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t seed = 1;
    std::int32_t material_seed = 0;
    std::uint32_t pixel_begin = 0;
    std::uint32_t pixel_count = 0;
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_count = 0;
    // Fixed target budget for texture footprints and the ZSobol sequence,
    // independent of dispatch partitioning. Required for image-textured scenes
    // and every ZSobol request; otherwise zero remains valid for constant scenes.
    std::uint64_t total_samples = 0;
    // Zero chooses the device width. Other values support scheduling validation.
    std::uint32_t threadgroup_width = 0;
    std::uint32_t emergency_limit = render_emergency_limit;
    RenderSampler sampler{};
    std::uint32_t boundary_limit = medium_boundary_limit;
};
struct RenderSamplesResult {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::vector<RenderSample> samples;
    std::size_t guard_count = 0;
    std::vector<PacketSample> packet_samples;
    std::vector<PacketSplat> packet_splats;
};
struct RenderBuildOptions {
    IntersectionProfile intersection_profile = IntersectionProfile::compensated;
};
struct RenderBuildResult;
// Synchronous operations require external serialization, including sample().
// The owner reuses its output buffer and retains no caller-owned scene storage.
class MetalRenderScene {
  public:
    ~MetalRenderScene();
    MetalRenderScene(MetalRenderScene &&) noexcept;
    MetalRenderScene &operator=(MetalRenderScene &&) noexcept;
    MetalRenderScene(const MetalRenderScene &) = delete;
    MetalRenderScene &operator=(const MetalRenderScene &) = delete;
    RenderSamplesResult sample(const RenderRequest &request);
    RenderSamplesResult sample(const RenderRequest &request, RenderTimings &timings);

  private:
    friend class FrameSession;
    friend class MetalFrameLease;
    friend RenderBuildResult build_metal_renderer(const RenderScene &,
                                                  const std::filesystem::path &, RenderBuildOptions,
                                                  RenderTimings *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MetalRenderScene(std::unique_ptr<Impl> impl);
    friend RenderBuildResult build_metal_renderer(const RenderScene &,
                                                  const std::filesystem::path &);
    friend RenderBuildResult build_metal_renderer(const RenderScene &,
                                                  const std::filesystem::path &, RenderTimings *);
};
struct RenderBuildResult {
    GeometryStatus status = GeometryStatus::failure;
    MetalDeviceInfo device;
    std::string diagnostic;
    std::unique_ptr<MetalRenderScene> scene;
    double pipeline_seconds = 0;
    // Zero preserves the default packet workspace policy for synthetic sources.
    std::uint32_t packet_lane_capacity = 0;
};
RenderBuildResult build_metal_renderer(const RenderScene &scene,
                                       const std::filesystem::path &library_path = {});
RenderBuildResult build_metal_renderer(const RenderScene &scene,
                                       const std::filesystem::path &library_path,
                                       RenderTimings *timings);

RenderBuildResult build_metal_renderer(const RenderScene &scene,
                                       const std::filesystem::path &library_path,
                                       RenderBuildOptions options,
                                       RenderTimings *timings = nullptr);

// Fixed-input native numerical validation; no scene/AS or transport state changes.
struct ShadingNormalProbeResult {
    MetalStatus status = MetalStatus::failure;
    std::string diagnostic;
    std::vector<shading_normal_probe::Result> values;
    std::size_t guard_count = 0;
};
ShadingNormalProbeResult
probe_shading_normal(const std::vector<shading_normal_probe::Query> &queries,
                     const std::filesystem::path &library_path = {});

enum class RenderCompletion { complete, cancelled, failed };
struct RenderControl {
    const Film *initial_film = nullptr;
    std::function<bool()> cancelled;
    // Called after every committed batch and once before work. For completed
    // resumes and cancellation before GPU setup, device is empty.
    // A diagnostic fails the render while preserving the committed Film.
    std::function<std::string(const Film &, const MetalDeviceInfo &)> committed;
    std::function<void(std::uint64_t)> progress;
    RenderTimings *timings = nullptr;
};
struct RenderResult {
    GeometryStatus status = GeometryStatus::failure;
    RenderCompletion completion = RenderCompletion::failed;
    MetalDeviceInfo device;
    std::string diagnostic;
    Film film;
    // Wall time: preparation excludes the renderer pipeline build; rendering
    // includes dispatch, readback, accumulation and commit/progress callbacks.
    double preparation_seconds = 0;
    double pipeline_seconds = 0;
    double render_seconds = 0;
};
RenderResult render_progressive(const RenderScene &scene, const RenderSettings &settings,
                                const std::function<void(std::uint64_t)> &progress = {});
RenderResult render_progressive(const RenderScene &scene, const RenderSettings &settings,
                                const RenderControl &control);

} // namespace rypath
