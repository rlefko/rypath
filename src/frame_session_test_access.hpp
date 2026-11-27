// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "area_wire.h"
#include "frame_session.hpp"
#include <vector>

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
// Owned CPU copies from one completed publication, never views of mutable GPU
// arenas. Only records referenced by a valid tuple are initialized witnesses.
struct EnhancedSessionSnapshot {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::uint32_t width = 0, height = 0, front_bank = 0;
    std::uint64_t serial = 0, generation = 0, epoch = 0;
    std::vector<area_wire::History> history;
    std::vector<area_wire::Temporal> temporal;
    // Bank-major: records[bank * width * height + pixel].
    std::vector<enhanced_wire::Record> records;
};
struct FrameSessionTestAccess {
    // Wait for the expected frame to complete first; externally serialize all
    // submissions/consumers. Explicitly copies GPU storage, without changing it.
    // Requires the latest attempt to be that successful commit; a later failed
    // attempt invalidates full snapshots even when old history remains eligible.
    // Temporal bank3 is the fresh tuple only on a cold frame with zero mutations.
    static EnhancedSessionSnapshot snapshot_enhanced(FrameSession &, std::uint64_t expected_serial);
    // Ordinary build_frame_session validates/builds the first session. The
    // second uses the exact same renderer; only the cache flag may differ.
    // Enhanced also uses exact caching when the complete evaluator context matches.
    // Callers must serialize both sessions, including their GPU consumers.
    static SharedSceneFrameSessions
    build_shared_scene(const RenderScene &, const FrameConfig &, const FrameConfig &,
                       const std::filesystem::path &library_path = {});
};
} // namespace rypath
