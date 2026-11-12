// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "geometry.hpp"
#include "metal_backend.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>

namespace rypath {

enum class GeometryStatus { success, not_built, no_device, no_ray_tracing, failure };

struct GeometryClosestResult {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::vector<GeometryHit> hits;
    std::size_t guard_count = 0;
};

struct GeometryOcclusionResult {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::vector<std::uint8_t> occluded;
    std::size_t guard_count = 0;
};

struct GeometryBuildResult;
struct MetalGeometryAccess;

// Owns an immutable uploaded scene. Query calls are synchronous and require
// external serialization; no caller-owned scene storage is retained.
class MetalGeometryScene {
  public:
    ~MetalGeometryScene();
    MetalGeometryScene(MetalGeometryScene &&) noexcept;
    MetalGeometryScene &operator=(MetalGeometryScene &&) noexcept;
    MetalGeometryScene(const MetalGeometryScene &) = delete;
    MetalGeometryScene &operator=(const MetalGeometryScene &) = delete;

    GeometryClosestResult closest_hits(std::span<const GeometryRay> rays);
    GeometryOcclusionResult occlusion(std::span<const GeometryRay> rays);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MetalGeometryScene(std::unique_ptr<Impl> impl);
    friend GeometryBuildResult build_metal_geometry(const GeometryData &,
                                                    const std::filesystem::path &);
    friend struct MetalGeometryAccess;
};

struct GeometryBuildResult {
    GeometryStatus status = GeometryStatus::failure;
    MetalDeviceInfo device;
    std::string diagnostic;
    std::unique_ptr<MetalGeometryScene> scene;
};

// An empty path uses the same executable-relative library as the compute check.
GeometryBuildResult build_metal_geometry(const GeometryData &geometry,
                                         const std::filesystem::path &library_path = {});

} // namespace rypath
