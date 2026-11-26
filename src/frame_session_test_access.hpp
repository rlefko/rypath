// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "frame_session.hpp"

namespace rypath {
// Internal diagnostic bridge, available only from the Metal backend. These
// sessions share the immutable renderer/AS, never their frame or history state.
struct SharedSceneFrameSessions {
    GeometryStatus status = GeometryStatus::failure;
    MetalDeviceInfo device;
    std::string diagnostic;
    std::array<std::unique_ptr<FrameSession>, 2> sessions;
    bool shared_renderer = false;
    bool independent_history = false;
    ShadowTraversalSelection shadow_traversal{};
    AccelerationBuildInfo acceleration_build{};
};
struct FrameSessionTestAccess {
    // Ordinary build_frame_session validates/builds the first session. The
    // second uses the exact same renderer; only the cache flag may differ.
    // Callers must serialize both sessions, including their GPU consumers.
    static SharedSceneFrameSessions
    build_shared_scene(const RenderScene &, const FrameConfig &, const FrameConfig &,
                       const std::filesystem::path &library_path = {});
};
} // namespace rypath
