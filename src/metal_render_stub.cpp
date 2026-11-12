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

} // namespace rypath
