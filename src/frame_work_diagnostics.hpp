// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
namespace rypath {
// One row per full-path evaluation pass, plus the final publication command.
// Boundary queue observations are not per-dispatch occupancy or bounce counts.
struct FrameWorkPass {
    std::string kind;
    std::uint32_t index = 0;
    std::uint64_t commands = 0, encoded_rounds = 0, completed_evaluations = 0;
    std::uint64_t boundary_active_sum = 0;
    std::uint32_t boundary_active_max = 0;
    double gpu_seconds = 0, inter_command_seconds = 0, encode_seconds = 0;
    bool gpu_time_valid = true;
};
struct FrameWorkDiagnostics {
    bool requested = false, complete = true;
    std::string status = "disabled";
    std::uint64_t state_pool_bytes = 0;
    std::vector<FrameWorkPass> passes;
};
inline void invalidate_frame_work(FrameWorkDiagnostics &out, const char *reason) {
    if (!out.requested)
        return;
    out.complete = false;
    out.status = reason;
    for (auto &pass : out.passes)
        pass.gpu_time_valid = false;
}
inline void add_frame_work(FrameWorkDiagnostics &out, const char *kind, std::uint32_t index,
                           std::uint64_t rounds, std::uint64_t completed, std::uint32_t active,
                           double gpu_seconds, double gap_seconds, double encode_seconds,
                           bool gpu_valid) {
    if (!out.requested)
        return;
    auto found = std::find_if(out.passes.begin(), out.passes.end(), [&](const auto &pass) {
        return pass.kind == kind && pass.index == index;
    });
    if (found == out.passes.end()) {
        // Four mutations, four spatial passes, fresh, two temporal passes, finalize.
        if (out.passes.size() == 12) {
            invalidate_frame_work(out, "invalid: path work pass cap exceeded");
            return;
        }
        out.passes.push_back({.kind = kind, .index = index});
        found = out.passes.end() - 1;
    }
    auto &pass = *found;
    ++pass.commands;
    pass.encoded_rounds += rounds;
    pass.completed_evaluations += completed;
    pass.boundary_active_sum += active;
    pass.boundary_active_max = std::max(pass.boundary_active_max, active);
    const auto finite_time = [](double seconds) { return std::isfinite(seconds) && seconds >= 0; };
    pass.gpu_time_valid &=
        out.complete && gpu_valid && finite_time(gpu_seconds) && finite_time(gap_seconds);
    if (pass.gpu_time_valid) {
        pass.gpu_seconds += gpu_seconds;
        pass.inter_command_seconds += gap_seconds;
    }
    if (finite_time(encode_seconds))
        pass.encode_seconds += encode_seconds;
    else
        invalidate_frame_work(out, "invalid: host encoding interval");
    if (!finite_time(pass.gpu_seconds) || !finite_time(pass.inter_command_seconds) ||
        !finite_time(pass.encode_seconds))
        invalidate_frame_work(out, "invalid: path work timing overflow");
    if (!pass.gpu_time_valid)
        invalidate_frame_work(out, "unavailable: path work GPU timestamps");
}
} // namespace rypath
