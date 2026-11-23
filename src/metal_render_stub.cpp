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

RenderSamplesResult MetalRenderScene::sample(const RenderRequest &request, RenderTimings &) {
    return sample(request);
}
RenderBuildResult build_metal_renderer(const RenderScene &scene,
                                       const std::filesystem::path &library, RenderTimings *) {
    return build_metal_renderer(scene, library);
}

RenderBuildResult build_metal_renderer(const RenderScene &scene,
                                       const std::filesystem::path &library,
                                       RenderBuildOptions options, RenderTimings *) {
    auto result = build_metal_renderer(scene, library);
    result.shadow_traversal.requested = options.shadow_traversal;
    result.acceleration_build.requested = options.acceleration_build_policy;
    result.acceleration_build.diagnostic = result.diagnostic;
    return result;
}

ShadingNormalProbeResult probe_shading_normal(const std::vector<shading_normal_probe::Query> &,
                                              const std::filesystem::path &) {
    return {.status = MetalStatus::not_built,
            .diagnostic = "Metal backend was not built",
            .values = {},
            .guard_count = 0};
}

} // namespace rypath

#include "frame_session.hpp"
namespace rypath {
struct Frame::Impl {};
struct FrameSession::Impl {};
Frame::Frame(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Frame::~Frame() = default;
Frame::Frame(Frame &&) noexcept = default;
Frame &Frame::operator=(Frame &&) noexcept = default;
FrameInfo Frame::poll() const {
    return {.status = FrameStatus::failed, .diagnostic = "Metal backend was not built"};
}
FrameInfo Frame::wait() const { return poll(); }
FrameCapture Frame::capture() const {
    return {.status = GeometryStatus::not_built,
            .diagnostic = "Metal backend was not built",
            .raw_xyz = {}};
}
FrameSession::FrameSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FrameSession::~FrameSession() = default;
FrameSession::FrameSession(FrameSession &&) noexcept = default;
FrameSession &FrameSession::operator=(FrameSession &&) noexcept = default;
FrameSubmitResult FrameSession::submit(const FrameRequest &) {
    return {.status = GeometryStatus::not_built,
            .diagnostic = "Metal backend was not built",
            .frame = {}};
}
FrameSessionBuildResult build_frame_session(const RenderScene &, const FrameConfig &config,
                                            const std::filesystem::path &) {
    FrameSessionBuildResult result;
    result.status = GeometryStatus::not_built;
    result.diagnostic = "Metal backend was not built";
    result.shadow_traversal.requested = config.shadow_traversal;
    result.acceleration_build.requested = config.acceleration_build_policy;
    result.acceleration_build.diagnostic = result.diagnostic;
    return result;
}
} // namespace rypath
