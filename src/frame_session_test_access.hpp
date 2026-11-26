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
    // Opaque identities only: callers cannot mutate the shared owned-path
    // storage or treat it as a completed-frame snapshot.
    std::array<std::uintptr_t, 2> enhanced_record_arenas{}, enhanced_evaluations{};
    bool independent_owned_records = false;
    ShadowTraversalSelection shadow_traversal{};
    AccelerationBuildInfo acceleration_build{};
};
struct FrameSessionTestAccess {
    // Ordinary build_frame_session validates/builds the first session. The
    // second uses the exact same renderer; only the cache flag may differ.
    // Enhanced also uses exact caching when the complete evaluator context matches.
    // Callers must serialize both sessions, including their GPU consumers.
    static SharedSceneFrameSessions
    build_shared_scene(const RenderScene &, const FrameConfig &, const FrameConfig &,
                       const std::filesystem::path &library_path = {});
};
} // namespace rypath
