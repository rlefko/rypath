// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "acceleration_build_policy.hpp"
#include "geometry.hpp"
#include "metal_backend.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>

namespace rypath {

// Numerical intersection contract; compensated remains the offline default.
enum class IntersectionProfile { compensated, native };
struct GeometryBuildOptions {
    IntersectionProfile intersection_profile = IntersectionProfile::compensated;
    AccelerationBuildPolicy acceleration_build_policy = AccelerationBuildPolicy::default_build;
};

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
    friend // Native adapters may request supplementary opaque triangles; public queries
           // retain procedural traversal and the original source/primitive identities.
        GeometryBuildResult
        build_metal_geometry_with_hints(const GeometryData &, const std::filesystem::path &, bool);
    friend GeometryBuildResult
    build_metal_geometry(const GeometryData &, const std::filesystem::path &, GeometryBuildOptions);
    friend GeometryBuildResult build_metal_geometry_with_hints(const GeometryData &,
                                                               const std::filesystem::path &, bool,
                                                               GeometryBuildOptions);
    friend struct MetalGeometryAccess;
};

struct GeometryBuildResult {
    GeometryStatus status = GeometryStatus::failure;
    MetalDeviceInfo device;
    std::string diagnostic;
    std::unique_ptr<MetalGeometryScene> scene;
    AccelerationBuildInfo acceleration_build;
};

// Supplementary hints apply only to the compensated profile. Explicit native
// builds reject hints; both profiles preserve source/primitive identities.
GeometryBuildResult build_metal_geometry_with_hints(const GeometryData &,
                                                    const std::filesystem::path &, bool);

GeometryBuildResult build_metal_geometry_with_hints(const GeometryData &,
                                                    const std::filesystem::path &, bool,
                                                    GeometryBuildOptions);
GeometryBuildResult build_metal_geometry(const GeometryData &, const std::filesystem::path &,
                                         GeometryBuildOptions);

// An empty path uses the same executable-relative library as the compute check.
GeometryBuildResult build_metal_geometry(const GeometryData &geometry,
                                         const std::filesystem::path &library_path = {});

} // namespace rypath
