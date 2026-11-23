// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace rypath {
struct FrameStatisticsSample {
    bool warmup = false, complete = false;
    double cpu_seconds = 0;
    std::optional<double> gpu_seconds;
};
struct FrameMetricSummary {
    std::size_t count = 0;
    std::optional<double> median_seconds, p95_seconds;
};
struct FrameStatistics {
    FrameMetricSummary cpu, gpu;
    std::size_t missing_gpu_timestamps = 0;
};
inline FrameMetricSummary summarize_frame_metric(std::vector<double> values) {
    std::erase_if(values, [](double value) { return !std::isfinite(value) || value < 0; });
    if (values.empty())
        return {};
    std::sort(values.begin(), values.end());
    return {values.size(), values[(values.size() - 1) / 2],
            values[static_cast<std::size_t>(std::ceil(.95 * double(values.size()))) - 1]};
}
template <typename Range> FrameStatistics summarize_frames(const Range &frames) {
    std::vector<double> cpu, gpu;
    FrameStatistics result;
    for (const auto &frame : frames) {
        if (frame.warmup || !frame.complete)
            continue;
        cpu.push_back(frame.cpu_seconds);
        if (frame.gpu_seconds && std::isfinite(*frame.gpu_seconds) && *frame.gpu_seconds >= 0)
            gpu.push_back(*frame.gpu_seconds);
        else
            ++result.missing_gpu_timestamps;
    }
    result.cpu = summarize_frame_metric(std::move(cpu));
    result.gpu = summarize_frame_metric(std::move(gpu));
    return result;
}
} // namespace rypath
