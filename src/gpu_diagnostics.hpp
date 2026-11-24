// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
namespace rypath {
struct GPUStageTiming {
    std::string name;
    double seconds = 0;
    std::uint64_t dispatches = 0;
    bool valid = true;
};
struct GPUStageDiagnostics {
    bool requested = false;
    bool complete = true;
    std::string status = "disabled";
    std::string sampling_mode = "disabled";
    bool structural_overhead = false;
    // Dispatch intervals may overlap other work. Never use their sum as frame time.
    std::vector<GPUStageTiming> stages;
};
inline bool valid_gpu_stage_interval(std::uint64_t begin, std::uint64_t end) {
    return begin != 0 && begin != UINT64_MAX && end != UINT64_MAX && end >= begin;
}
inline void add_gpu_stage(GPUStageDiagnostics &out, const std::string &name, std::uint64_t begin,
                          std::uint64_t end, double nanoseconds_per_tick = 1) {
    auto *stage = [&]() -> GPUStageTiming * {
        for (auto &s : out.stages)
            if (s.name == name)
                return &s;
        if (out.stages.size() >= 64)
            return nullptr;
        out.stages.push_back({.name = name});
        return &out.stages.back();
    }();
    if (!stage) {
        out.complete = false;
        out.status = "unavailable: frame stage cap exceeded";
        for (auto &s : out.stages) {
            s.valid = false;
            s.seconds = 0;
        }
        return;
    }
    ++stage->dispatches;
    stage->valid &= out.complete && valid_gpu_stage_interval(begin, end) &&
                    std::isfinite(nanoseconds_per_tick) && nanoseconds_per_tick > 0;
    if (stage->valid) {
        stage->seconds += static_cast<double>(end - begin) * nanoseconds_per_tick * 1e-9;
        stage->valid &= std::isfinite(stage->seconds);
    }
    if (!stage->valid) {
        stage->seconds = 0;
        if (out.complete)
            out.status = "invalid timestamp samples";
        out.complete = false;
        for (auto &s : out.stages) {
            s.valid = false;
            s.seconds = 0;
        }
    }
}
inline void invalidate_gpu_stages(GPUStageDiagnostics &out, const char *reason) {
    if (!out.requested)
        return;
    out.complete = false;
    out.status = reason;
    for (auto &stage : out.stages) {
        stage.valid = false;
        stage.seconds = 0;
    }
}
} // namespace rypath
