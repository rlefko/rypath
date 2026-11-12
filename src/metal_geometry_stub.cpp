// SPDX-License-Identifier: GPL-3.0-only

#include "metal_geometry.hpp"

namespace rypath {

struct MetalGeometryScene::Impl {};

MetalGeometryScene::MetalGeometryScene(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalGeometryScene::~MetalGeometryScene() = default;
MetalGeometryScene::MetalGeometryScene(MetalGeometryScene &&) noexcept = default;
MetalGeometryScene &MetalGeometryScene::operator=(MetalGeometryScene &&) noexcept = default;

GeometryClosestResult MetalGeometryScene::closest_hits(std::span<const GeometryRay>) {
    return {.status = GeometryStatus::not_built,
            .diagnostic = "Metal backend was not built",
            .hits = {}};
}

GeometryOcclusionResult MetalGeometryScene::occlusion(std::span<const GeometryRay>) {
    return {.status = GeometryStatus::not_built,
            .diagnostic = "Metal backend was not built",
            .occluded = {}};
}

GeometryBuildResult build_metal_geometry(const GeometryData &, const std::filesystem::path &) {
    return {
        .status = GeometryStatus::not_built,
        .device = {},
        .diagnostic = "Metal backend was not built",
        .scene = {},
    };
}

} // namespace rypath
