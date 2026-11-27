// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "area_wire.h"
#include "enhanced_suffix_tape_shared.h"
#include "frame_session.hpp"
#include <optional>
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
    std::array<std::uintptr_t, 2> enhanced_suffix_arenas{};
    bool independent_suffix_storage = false;
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
    bool suffix_cache_enabled = false;
    std::uint64_t suffix_context = 0;
    // Populated only when explicitly requested and suffix caching is enabled.
    std::vector<rypath_enhanced_suffix_tape::Entry> enhanced_suffix;
};
// CPU allocator diagnostics only; these identities do not expose GPU scratch.
// Successful state reads always have no pending frame.
struct EnhancedSuffixContextState {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::array<std::uint64_t, 2> contexts{};
    unsigned front = 0;
    std::uint64_t next_context = 0, generation = 0, next_serial = 0, pending_serial = 0;
    std::optional<std::uint64_t> committed_serial;
    bool exhausted = false, valid = false;
};
struct FrameSessionTestAccess {
    // Externally serialize submissions and consumers. Both methods require
    // enabled Enhanced prefix caching and reject pending/moved-from sessions.
    // A failed GPU attempt may be inspected without granting a full snapshot.
    static EnhancedSuffixContextState suffix_context_state(FrameSession &);
    // Test-only allocator initialization: nonzero seed, before any submission,
    // epoch snapshot, publication or previous allocator change. Returns the
    // resulting state on success; rejection leaves allocator state unchanged.
    static EnhancedSuffixContextState seed_next_suffix_context(FrameSession &,
                                                               std::uint64_t next_context);

    // Wait for the expected frame to complete first; externally serialize all
    // submissions/consumers. Explicitly copies GPU storage, without changing it.
    // Requires the latest attempt to be that successful commit; a later failed
    // attempt invalidates full snapshots even when old history remains eligible.
    // Temporal bank3 is the fresh tuple only on a cold frame with zero mutations.
    static EnhancedSessionSnapshot snapshot_enhanced(FrameSession &, std::uint64_t expected_serial,
                                                     bool include_suffix = false);
    // Ordinary build_frame_session validates/builds the first session. The
    // second uses the exact same renderer; only temporal/suffix cache flags and
    // experimental Enhanced pair compaction may differ.
    // Enhanced also uses exact caching when the complete evaluator context matches.
    // Callers must serialize both sessions, including their GPU consumers.
    static SharedSceneFrameSessions
    build_shared_scene(const RenderScene &, const FrameConfig &, const FrameConfig &,
                       const std::filesystem::path &library_path = {});
};
} // namespace rypath
