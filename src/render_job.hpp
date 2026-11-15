// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "checkpoint.hpp"
#include "metal_render.hpp"

namespace rypath {

struct CheckpointOptions {
    std::filesystem::path output;
    std::filesystem::path resume;
    std::uint32_t interval_seconds = 60;
    bool interval_supplied = false;
};

struct RenderJobResult {
    RenderResult render;
    int signal = 0;
    std::string identity;
    std::vector<CheckpointSegment> segments;
};

// Checks normalized paths and existing filesystem aliases before any publication.
std::string validate_checkpoint_paths(const CheckpointOptions &options,
                                      const std::filesystem::path &image,
                                      std::span<const std::filesystem::path> inputs);

// The CLI owns the process signal handlers for the duration of this synchronous job.
RenderJobResult run_render_job(const RenderScene &scene, const RenderSettings &settings,
                               const CheckpointOptions &options, const std::filesystem::path &image,
                               std::span<const std::filesystem::path> inputs, std::string build,
                               const std::function<void(std::uint64_t)> &progress = {});
std::string checkpoint_provenance(std::span<const CheckpointSegment> segments);

} // namespace rypath
