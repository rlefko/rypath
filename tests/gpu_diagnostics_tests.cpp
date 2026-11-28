// SPDX-License-Identifier: GPL-3.0-only
#include "frame_work_diagnostics.hpp"
#include "gpu_diagnostics.hpp"
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
int main() try {
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
    FrameWorkDiagnostics work;
    add_frame_work(work, "fresh", 0, 8, 10, 2, .1, .01, .001, true);
    require(work.passes.empty());
    work.requested = true;
    add_frame_work(work, "fresh", 0, 8, 10, 2, .1, 0, .001, true);
    add_frame_work(work, "fresh", 0, 8, 11, 0, .2, .01, .002, true);
    add_frame_work(work, "spatial", 1, 8, 21, 0, .3, .02, .003, true);
    require(work.passes.size() == 2 && work.passes[0].commands == 2 &&
            work.passes[0].encoded_rounds == 16 && work.passes[0].completed_evaluations == 21 &&
            work.passes[0].boundary_active_sum == 2 && work.passes[0].boundary_active_max == 2 &&
            std::abs(work.passes[0].gpu_seconds - .3) < 1e-15);
    add_frame_work(work, "finalize", 0, 0, 0, 0, 0, 0, .001, false);
    require(!work.complete && !work.passes[0].gpu_time_valid);
    add_frame_work(work, "spatial", 2, 8, 21, 0, .3, .02, .003, true);
    require(!work.passes.back().gpu_time_valid);
    FrameWorkDiagnostics capped;
    capped.requested = true;
    for (unsigned i = 0; i < 13; ++i)
        add_frame_work(capped, "mutation", i, 8, 0, 0, .1, 0, 0, true);
    require(capped.passes.size() == 12 && !capped.complete);
    std::uint64_t previous = 0, delta = 999;
    require(frame_packet_step_delta(0, 0, false, previous, delta) && delta == 0);
    require(frame_packet_step_delta(17, 7, false, previous, delta) && delta == 10);
    require(frame_packet_step_delta(20, 8, false, previous, delta) && delta == 2);
    require(frame_packet_step_delta(20, 8, false, previous, delta) && delta == 0);
    require(!frame_packet_step_delta(19, 8, false, previous, delta) && previous == 12);
    require(!frame_packet_step_delta(7, 8, false, previous, delta) && previous == 12);
    previous = 0; // New compacted leaf: nulls were never queued.
    require(frame_packet_step_delta(10, 7, true, previous, delta) && delta == 10);
    require(!frame_packet_step_delta(9, 7, true, previous, delta));
    FrameWorkDiagnostics exact;
    exact.requested = exact.packet_step_invocations_supported = true;
    add_frame_work(exact, "fresh", 0, 8, 1, 0, .1, 0, 0, true, UINT64_MAX);
    add_frame_work(exact, "finalize", 0, 0, 0, 0, .1, 0, 0, true);
    require(exact.complete && exact.passes[0].packet_step_invocations == UINT64_MAX &&
            exact.passes[1].packet_step_invocations == 0);
    add_frame_work(exact, "fresh", 0, 8, 1, 0, .1, 0, 0, true, 1);
    require(!exact.complete && exact.status == "invalid: packet step invocation overflow");
    std::cout << "GPU stage diagnostics checks passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
