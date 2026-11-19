// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"

namespace rypath {

struct MetalRenderScene::Impl {};

MetalRenderScene::MetalRenderScene(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalRenderScene::~MetalRenderScene() = default;
MetalRenderScene::MetalRenderScene(MetalRenderScene &&) noexcept = default;
MetalRenderScene &MetalRenderScene::operator=(MetalRenderScene &&) noexcept = default;

RenderSamplesResult MetalRenderScene::sample(const RenderRequest &) {
    return {
        .status = GeometryStatus::not_built,
        .diagnostic = "Metal backend was not built",
        .samples = {},
        .guard_count = 0,
        .packet_samples = {},
        .packet_splats = {},
    };
}

RenderBuildResult build_metal_renderer(const RenderScene &, const std::filesystem::path &) {
    return {
        .status = GeometryStatus::not_built,
        .device = {},
        .diagnostic = "Metal backend was not built",
        .scene = {},
    };
}

ShadingNormalProbeResult probe_shading_normal(const std::vector<shading_normal_probe::Query> &,
                                              const std::filesystem::path &) {
    return {.status = MetalStatus::not_built,
            .diagnostic = "Metal backend was not built",
            .values = {},
            .guard_count = 0};
}

} // namespace rypath
