// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "checkpoint_contract.h"
#include "render.hpp"
#include "sha256.hpp"
#include <filesystem>
namespace rypath {
struct CheckpointSegment {
    std::uint64_t begin = 0, end = 0;
    std::string build, device, architecture, os;
};
struct RenderCheckpoint {
    Film film;
    RenderSettings settings;
    Sha256Digest identity{};
    std::vector<CheckpointSegment> segments;
};
struct CheckpointReadResult {
    std::optional<RenderCheckpoint> value;
    std::string diagnostic;
};
CheckpointReadResult read_checkpoint(const std::filesystem::path &path);
std::string write_checkpoint(const RenderCheckpoint &checkpoint, const std::filesystem::path &path);
std::string validate_checkpoint(const RenderCheckpoint &checkpoint);
} // namespace rypath
