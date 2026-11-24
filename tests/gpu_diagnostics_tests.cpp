// SPDX-License-Identifier: GPL-3.0-only
#include "gpu_diagnostics.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
int main() {
    using namespace rypath;
    const auto require = [](bool value) {
        if (!value)
            std::abort();
    };
    GPUStageDiagnostics d;
    require(!d.requested && d.status == "disabled" && d.stages.empty());
    d.requested = true;
    add_gpu_stage(d, "trace", 100, 200, 2);
    add_gpu_stage(d, "trace", 200, 500, 2);
    require(d.stages.size() == 1 && d.stages[0].dispatches == 2 &&
            std::abs(d.stages[0].seconds - 8e-7) < 1e-20);
    add_gpu_stage(d, "shade", 500, 500);
    require(d.stages[1].valid && d.stages[1].seconds == 0);
    add_gpu_stage(d, "trace", UINT64_MAX, 500);
    require(!d.complete && !d.stages[0].valid && !d.stages[1].valid);
    add_gpu_stage(d, "refill", 100, 200);
    require(!d.stages.back().valid && d.stages.back().seconds == 0);
    GPUStageDiagnostics unavailable;
    unavailable.requested = true;
    invalidate_gpu_stages(unavailable, "unavailable");
    add_gpu_stage(unavailable, "later stage", 1, 10);
    require(!unavailable.stages[0].valid && !unavailable.complete);
    for (const auto scale : {0., -1., std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        GPUStageDiagnostics bad;
        add_gpu_stage(bad, "invalid calibration", 1, 2, scale);
        require(!bad.complete && !bad.stages[0].valid);
    }
    require(!valid_gpu_stage_interval(0, 2) && !valid_gpu_stage_interval(3, 2) &&
            !valid_gpu_stage_interval(1, UINT64_MAX));
    GPUStageDiagnostics bounded;
    for (unsigned i = 0; i < 100; ++i)
        add_gpu_stage(bounded, std::to_string(i), 1, 2);
    require(bounded.stages.size() == 64 && !bounded.complete);
    std::cout << "GPU stage diagnostics checks passed\n";
}
