// SPDX-License-Identifier: GPL-3.0-only
#include "frame_statistics.hpp"
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>

int main() try {
    using namespace rypath;
    const auto require = [](bool value) {
        if (!value) {
            std::cerr << "frame statistics contract failed\n";
            std::exit(1);
        }
    };
    const auto empty = summarize_frames(std::vector<FrameStatisticsSample>{});
    require(empty.cpu.count == 0 && !empty.cpu.median_seconds && !empty.gpu.p95_seconds);
    std::vector<FrameStatisticsSample> frames{{true, true, 1000, 1000}, {false, false, 1000, 1000}};
    require(summarize_frames(frames).cpu.count == 0);
    for (int i = 20; i >= 1; --i)
        frames.push_back({false, true, double(i), double(i) / 2});
    auto stats = summarize_frames(frames);
    require(stats.cpu.count == 20 && stats.cpu.median_seconds == 10 && stats.cpu.p95_seconds == 19);
    require(stats.gpu.count == 20 && stats.gpu.median_seconds == 5 && stats.gpu.p95_seconds == 9.5);
    frames.push_back({false, true, 0, {}});
    frames.push_back({false, true, std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN()});
    frames.push_back({false, true, -1, -1});
    stats = summarize_frames(frames);
    require(stats.cpu.count == 21 && stats.gpu.count == 20 && stats.missing_gpu_timestamps == 3);
    const auto single = summarize_frame_metric({7});
    require(single.count == 1 && single.median_seconds == 7 && single.p95_seconds == 7);
    std::cout << "frame statistics checks passed\n";
} catch (const std::exception &error) {
    std::cerr << "frame statistics: " << error.what() << '\n';
    return 1;
}
