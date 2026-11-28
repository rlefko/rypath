// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "viewer_options.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

namespace rypath::viewer {
struct WorkloadPose {
    const char *phase = "interactive";
    double translation = 0, rotation = 0;
    bool cut = false;
};
// Absolute poses prevent timing-dependent camera drift. The cut is one frame;
// subsequent frames at the authored pose belong to the separate settled phase.
inline WorkloadPose workload_pose(const Options &o, std::uint64_t sample) {
    if (o.workload == "interactive")
        return {};
    if (sample < o.warmup)
        return {"startup"};
    const auto index = sample - o.warmup, count = o.frames - o.warmup;
    if (o.workload == "static")
        return {"static"};
    if (o.workload == "translation" || o.workload == "rotation") {
        double t = count > 1 ? double(index) / double(count - 1) : 0;
        if (o.motion_period) {
            const auto phase = index % o.motion_period;
            t = double(std::min(phase, o.motion_period - phase)) / double(o.motion_period / 2);
        }
        return {o.workload == "translation" ? "translation" : "rotation",
                o.workload == "translation" ? o.motion_distance * t : 0,
                o.workload == "rotation" ? o.rotation_radians * t : 0};
    }
    const auto quarter = count / 4;
    if (index < quarter)
        return {"static"};
    if (index < 2 * quarter)
        return {"translation", o.motion_distance * double(index - quarter + 1) / double(quarter)};
    if (index < 3 * quarter)
        return {"rotation", o.motion_distance,
                o.rotation_radians * double(index - 2 * quarter + 1) / double(quarter)};
    return {index == 3 * quarter ? "cut" : "settled", 0, 0, index == 3 * quarter};
}
inline std::string json_string(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (c < 32)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c)
                << std::dec;
        else
            out << c;
    }
    out << '"';
    return out.str();
}
struct PipelineObservation {
    std::uint64_t sample = 0;
    double completed_time = 0, pipeline_seconds = 0;
    std::string phase;
    bool startup = false;
    std::uint32_t output_width = 0, output_height = 0;
    std::uint64_t epoch = 0;
    std::uint32_t camera_medium = 0;
    std::array<double, 3> camera_origin{}, camera_forward{}, camera_up{};
    FrameInfo effective{};
    bool reconstruction_reset = false;
    ReconstructionMode mode = ReconstructionMode::raw_rgb;
    std::optional<double> reconstruction_gpu_seconds{};
};
struct DrawableObservation {
    std::uint64_t sample = 0;
    double submitted_time = 0, presented_time = 0;
    bool callback = false;
    std::uint32_t width = 0, height = 0;
};
inline void write_interval_summary(std::ostream &out, std::vector<double> intervals) {
    out << "{\"interval_count\":" << intervals.size();
    if (intervals.empty())
        out << ",\"mean_ms\":null,\"p95_ms\":null}";
    else {
        std::sort(intervals.begin(), intervals.end());
        out << ",\"mean_ms\":"
            << 1000 * std::accumulate(intervals.begin(), intervals.end(), 0.) /
                   double(intervals.size())
            << ",\"p95_ms\":"
            << 1000 * intervals[std::size_t(std::ceil(.95 * double(intervals.size()))) - 1] << '}';
    }
}
// Only distinct source frames with valid presentation timestamps enter intervals.
// Redraws remain in the event log; missing callbacks and zero presentedTime are
// explicit, never converted into successful display observations.
inline std::string measurement_report(const Options &o,
                                      const std::vector<PipelineObservation> &frames,
                                      const std::vector<DrawableObservation> &drawables,
                                      std::string_view status, double elapsed,
                                      std::string_view device = {}, std::string_view os = {},
                                      std::string_view spectral = {}, std::string_view hero = {}) {
    std::vector<const DrawableObservation *> displayed;
    std::set<std::uint64_t> submitted, seen;
    std::map<std::uint64_t, const PipelineObservation *> indexed_frames;
    for (const auto &f : frames)
        indexed_frames.emplace(f.sample, &f);
    std::uint64_t missing = 0, dropped = 0, repeats = 0;
    for (const auto &d : drawables) {
        submitted.insert(d.sample);
        missing += !d.callback;
        dropped += d.callback && !(d.presented_time > 0 && std::isfinite(d.presented_time));
        if (d.callback && d.presented_time > 0 && std::isfinite(d.presented_time))
            displayed.push_back(&d);
    }
    std::sort(displayed.begin(), displayed.end(),
              [](auto a, auto b) { return a->presented_time < b->presented_time; });
    std::erase_if(displayed, [&](auto d) {
        if (!seen.insert(d->sample).second) {
            ++repeats;
            return true;
        }
        return false;
    });
    const auto observation = [&](std::uint64_t sample) -> const PipelineObservation * {
        const auto found = indexed_frames.find(sample);
        return found == indexed_frames.end() ? nullptr : found->second;
    };
    std::vector<double> measured, startup;
    double measured_first = 0, measured_last = 0;
    std::size_t measured_displayed = 0;
    for (const auto d : displayed) {
        const auto f = observation(d->sample);
        if (f && !f->startup) {
            if (!measured_displayed)
                measured_first = d->presented_time;
            measured_last = d->presented_time;
            ++measured_displayed;
        }
    }
    std::map<std::string, std::vector<double>> phases;
    for (const auto &f : frames)
        if (!f.startup)
            phases.try_emplace(f.phase);
    for (std::size_t i = 1; i < displayed.size(); ++i) {
        const auto a = observation(displayed[i - 1]->sample), b = observation(displayed[i]->sample);
        if (!a || !b)
            continue;
        const double interval = displayed[i]->presented_time - displayed[i - 1]->presented_time;
        if (a->startup && b->startup)
            startup.push_back(interval);
        if (a->startup || b->startup)
            continue;
        measured.push_back(interval);
        if (a->phase == b->phase)
            phases[b->phase].push_back(interval);
    }
    std::size_t unsubmitted = 0, undisplayed = 0;
    for (const auto &f : frames) {
        unsubmitted += !submitted.contains(f.sample);
        undisplayed += !seen.contains(f.sample);
    }
    std::ostringstream out;
    out << std::setprecision(17) << "{\"schema\":1,\"status\":" << json_string(status)
        << ",\"timing_scope\":\"CAMetalDrawable presentedTime; distinct source frames; "
           "nearest-rank p95\""
        << ",\"frame_scheduling\":" << json_string(o.completion_wakeup ? "completion" : "timer")
        << ",\"device\":" << json_string(device) << ",\"os\":" << json_string(os)
        << ",\"mutations\":" << (o.estimator == FrameEstimator::area_replay ? o.mutation_steps : 0)
        << ",\"area_path_domain\":" << json_string(frame_area_path_domain_name(o.area_path_domain))
        << ",\"spatial_mode\":" << json_string(frame_spatial_mode_name(o.spatial_mode))
        << ",\"history_cap_policy\":"
        << json_string(frame_history_cap_policy_name(o.history_cap_policy))
        << ",\"history_cap\":" << o.enhanced_history_cap
        << ",\"area_stencil\":" << json_string(frame_area_stencil_name(o.area_stencil))
        << ",\"contribution_replay\":"
        << json_string(frame_contribution_replay_name(o.contribution_replay))
        << ",\"mutation_policy\":" << json_string(frame_mutation_policy_name(o.mutation_policy))
        << ",\"spectral_transport_requested\":"
        << json_string(o.spectral_transport == SpectralTransportSelection::automatic ? "auto"
                       : o.spectral_transport == SpectralTransportSelection::hero    ? "hero"
                       : o.spectral_transport == SpectralTransportSelection::packet  ? "packet"
                                                                                     : "scalar")
        << ",\"spectral_transport_effective\":" << json_string(spectral)
        << ",\"hero_wavelengths_effective\":" << json_string(hero) << ",\"variance_policy\":"
        << json_string(o.variance_policy == SpatialVariancePolicy::paired   ? "paired"
                       : o.variance_policy == SpatialVariancePolicy::center ? "center"
                                                                            : "conservative")
        << ",\"near_plane\":" << o.near_plane << ",\"exposure_ev\":" << o.exposure_ev
        << ",\"scene\":" << json_string(o.demo ? "demo" : o.scene.string())
        << ",\"workload\":" << json_string(o.workload) << ",\"seed\":" << o.seed
        << ",\"internal_width\":" << o.width << ",\"internal_height\":" << o.height
        << ",\"output_width\":" << o.output_width << ",\"output_height\":" << o.output_height
        << ",\"reconstruction\":"
        << json_string(o.reconstruction == ReconstructionMode::metalfx    ? "metalfx"
                       : o.reconstruction == ReconstructionMode::temporal ? "temporal"
                                                                          : "raw-rgb")
        << ",\"estimator\":"
        << json_string(o.estimator == FrameEstimator::independent ? "independent" : "area-replay")
        << ",\"area_temporal_reprojection_requested\":"
        << json_string(o.enhanced_temporal_reprojection ? "dual" : "off")
        << ",\"motion_period_frames\":" << o.motion_period
        << ",\"motion_distance\":" << o.motion_distance
        << ",\"rotation_radians\":" << o.rotation_radians << ",\"requested_frames\":" << o.frames
        << ",\"startup_frames\":" << o.warmup << ",\"elapsed_seconds\":" << elapsed
        << ",\"completed_frames\":" << frames.size()
        << ",\"drawable_submissions\":" << drawables.size()
        << ",\"distinct_displayed_frames\":" << displayed.size()
        << ",\"missing_callbacks\":" << missing << ",\"zero_time_callbacks\":" << dropped
        << ",\"repeated_source_callbacks\":" << repeats
        << ",\"unsubmitted_sources\":" << unsubmitted << ",\"undisplayed_sources\":" << undisplayed
        << ",\"measured_displayed_frames\":" << measured_displayed
        << ",\"measured_displayed_span_seconds\":" << measured_last - measured_first
        << ",\"startup_presentation_intervals\":";
    write_interval_summary(out, startup);
    out << ",\"measured_presentation_intervals\":";
    write_interval_summary(out, measured);
    out << ",\"phases\":{";
    bool first = true;
    for (const auto &[phase, values] : phases) {
        if (!first)
            out << ',';
        first = false;
        out << json_string(phase) << ':';
        write_interval_summary(out, values);
    }
    out << "},\"pipeline_frames\":[";
    first = true;
    for (const auto &f : frames) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"sample\":" << f.sample << ",\"phase\":" << json_string(f.phase)
            << ",\"startup\":" << (f.startup ? "true" : "false")
            << ",\"completed_seconds\":" << f.completed_time
            << ",\"pipeline_seconds\":" << f.pipeline_seconds
            << ",\"output_width\":" << f.output_width << ",\"output_height\":" << f.output_height
            << ",\"mode\":"
            << json_string(f.mode == ReconstructionMode::metalfx    ? "metalfx"
                           : f.mode == ReconstructionMode::temporal ? "temporal"
                                                                    : "raw-rgb")
            << ",\"epoch\":" << f.epoch << ",\"camera_medium\":" << f.camera_medium;
        const auto vector = [&](std::string_view name, const auto &v) {
            out << ',' << json_string(name) << ":[" << v[0] << ',' << v[1] << ',' << v[2] << ']';
        };
        const auto gpu_duration = [&](std::string_view name, std::optional<double> seconds) {
            out << ',' << json_string(name) << ':';
            if (seconds && std::isfinite(*seconds) && *seconds >= 0)
                out << *seconds;
            else
                out << "null";
        };
        gpu_duration("raw_gpu_seconds", f.effective.gpu_time_valid
                                            ? std::optional{f.effective.gpu_seconds}
                                            : std::nullopt);
        gpu_duration("reconstruction_gpu_seconds", f.reconstruction_gpu_seconds);
        vector("camera_origin", f.camera_origin);
        vector("camera_forward", f.camera_forward);
        vector("camera_up", f.camera_up);
        out << ",\"effective_area_stencil\":"
            << json_string(frame_area_stencil_name(f.effective.area_stencil))
            << ",\"effective_mutation_policy\":"
            << json_string(frame_mutation_policy_name(f.effective.mutation_policy))
            << ",\"effective_path_domain\":"
            << json_string(frame_area_path_domain_name(f.effective.area_path_domain))
            << ",\"effective_history_cap\":" << f.effective.enhanced_history_cap
            << ",\"enhanced_temporal_reprojection\":"
            << (f.effective.enhanced_temporal_reprojection ? "true" : "false")
            << ",\"enhanced_temporal_reprojection_used\":"
            << (f.effective.enhanced_temporal_reprojection_used ? "true" : "false")
            << ",\"enhanced_motion_routes\":[" << f.effective.enhanced_motion_routes[0] << ','
            << f.effective.enhanced_motion_routes[1] << ',' << f.effective.enhanced_motion_routes[2]
            << ',' << f.effective.enhanced_motion_routes[3] << ']'
            << ",\"history_used\":" << (f.effective.history_used ? "true" : "false")
            << ",\"reconstruction_history_reset\":" << (f.reconstruction_reset ? "true" : "false")
            << '}';
    }
    out << "],\"drawables\":[";
    first = true;
    for (const auto &d : drawables) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"sample\":" << d.sample << ",\"submitted_seconds\":" << d.submitted_time
            << ",\"callback\":" << (d.callback ? "true" : "false") << ",\"presented_seconds\":";
        if (d.callback && d.presented_time > 0 && std::isfinite(d.presented_time))
            out << d.presented_time;
        else
            out << "null";
        out << ",\"drawable_width\":" << d.width << ",\"drawable_height\":" << d.height << '}';
    }
    out << "]}\n";
    return out.str();
}
} // namespace rypath::viewer
