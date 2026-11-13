// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "metal_geometry.hpp"
#include "render.hpp"

namespace rypath {

struct RenderRequest {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t seed = 1;
    std::uint32_t pixel_begin = 0;
    std::uint32_t pixel_count = 0;
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_count = 0;
    // Zero chooses the device width. Other values support scheduling validation.
    std::uint32_t threadgroup_width = 0;
    std::uint32_t emergency_limit = render_emergency_limit;
    RenderSampler sampler{};
};
struct RenderSamplesResult {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::vector<RenderSample> samples;
    std::size_t guard_count = 0;
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

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MetalRenderScene(std::unique_ptr<Impl> impl);
    friend RenderBuildResult build_metal_renderer(const RenderScene &,
                                                  const std::filesystem::path &);
};
struct RenderBuildResult {
    GeometryStatus status = GeometryStatus::failure;
    MetalDeviceInfo device;
    std::string diagnostic;
    std::unique_ptr<MetalRenderScene> scene;
};
RenderBuildResult build_metal_renderer(const RenderScene &scene,
                                       const std::filesystem::path &library_path = {});

struct RenderResult {
    GeometryStatus status = GeometryStatus::failure;
    MetalDeviceInfo device;
    std::string diagnostic;
    Film film;
};
RenderResult render_progressive(const RenderScene &scene, const RenderSettings &settings,
                                const std::function<void(std::uint64_t)> &progress = {});

} // namespace rypath
