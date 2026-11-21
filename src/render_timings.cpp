// SPDX-License-Identifier: GPL-3.0-only
#include "render_timings.hpp"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <locale>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace rypath {
namespace {
constexpr std::array names{
    "command",          "preflight_overhead", "pbrt_check",         "pbrt_translate",
    "document_cleanup", "post_translation",   "render_job",         "identity",
    "resume",           "driver_overhead",    "driver_validation",  "film_initialize",
    "preparation",      "pipeline",           "initial_checkpoint", "checkpoint_callback",
    "render_loop",      "driver_sample",      "dispatch_setup",     "dispatch_wait",
    "dispatch_decode",  "film_stage",         "film_add",           "film_commit",
    "checkpoint_copy",  "checkpoint_write",   "progress",           "output_metadata",
    "primary_output",   "raw_film_copy",      "raw_output",         "destruction"};
static_assert(names.size() == static_cast<std::size_t>(RenderTimingPhase::count));
std::string json_quote(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else
            result += static_cast<char>(c);
    }
    return result + '"';
}
std::string io_error(std::string_view operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}
bool write_all(int descriptor, std::string_view text) {
    while (!text.empty()) {
        const auto count = ::write(descriptor, text.data(), text.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        text.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}
} // namespace
std::uint64_t RenderTimings::steady_nanoseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
RenderTimings::RenderTimings(Clock clock)
    : clock_(clock ? clock : steady_nanoseconds), start_(clock_()) {}
std::size_t RenderTimings::begin(RenderTimingPhase phase) noexcept {
    if (finished_ || depth_ == maximum_depth || phase >= RenderTimingPhase::count) {
        error_ = "invalid timing scope begin";
        return maximum_depth;
    }
    const auto token = depth_++;
    stack_[token] = {phase, clock_(), 0};
    return token;
}
void RenderTimings::segment(std::uint64_t now) noexcept {
    auto &frame = stack_[depth_ - 1];
    if (now < frame.start || now - frame.start < frame.children) {
        error_ = "nonmonotonic timing scope";
        return;
    }
    const auto elapsed = now - frame.start;
    auto &phase = phases_[static_cast<std::size_t>(frame.phase)];
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (phase.inclusive_ns > maximum - elapsed ||
        phase.exclusive_ns > maximum - (elapsed - frame.children) || phase.count == maximum ||
        (depth_ > 1 && stack_[depth_ - 2].children > maximum - elapsed)) {
        error_ = "timing accumulator overflow";
        return;
    }
    phase.inclusive_ns += elapsed;
    phase.exclusive_ns += elapsed - frame.children;
    ++phase.count;
    if (depth_ > 1)
        stack_[depth_ - 2].children += elapsed;
}
void RenderTimings::end(std::size_t token) noexcept {
    if (token == maximum_depth)
        return;
    if (!depth_ || token != depth_ - 1) {
        error_ = "timing scopes closed out of order";
        return;
    }
    segment(clock_());
    --depth_;
    if (destruction_requested_ && depth_ &&
        stack_[depth_ - 1].phase != RenderTimingPhase::destruction)
        transition(RenderTimingPhase::destruction);
}
void RenderTimings::transition(RenderTimingPhase phase) noexcept {
    if (!depth_ || phase >= RenderTimingPhase::count) {
        error_ = "invalid timing phase transition";
        return;
    }
    const auto now = clock_();
    segment(now);
    stack_[depth_ - 1] = {phase, now, 0};
}
void RenderTimings::begin_destruction() noexcept {
    destruction_requested_ = true;
    if (depth_)
        transition(RenderTimingPhase::destruction);
}
void RenderTimings::finish() noexcept {
    if (finished_)
        return;
    const auto now = clock_();
    if (depth_ || now < start_)
        error_ = "unfinished timing scope";
    elapsed_ = now >= start_ ? now - start_ : 0;
    finished_ = true;
}
void RenderTimings::gpu_command(bool completed, double start, double end) noexcept {
    ++gpu_commands_;
    const double nanoseconds = (end - start) * 1e9;
    if (!completed || !std::isfinite(start) || !std::isfinite(end) || start <= 0 || end < start ||
        !std::isfinite(nanoseconds) || nanoseconds < 0 ||
        nanoseconds >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        ++gpu_invalid_;
        return;
    }
    const auto duration = static_cast<std::uint64_t>(std::llround(nanoseconds));
    if (gpu_ns_ > std::numeric_limits<std::uint64_t>::max() - duration) {
        error_ = "GPU timing accumulator overflow";
        ++gpu_invalid_;
        return;
    }
    gpu_ns_ += duration;
    ++gpu_valid_;
}
void RenderTimings::failure(std::string diagnostic) { diagnostic_ = std::move(diagnostic); }
void RenderTimings::provenance(std::string name, std::string value) {
    provenance_.emplace_back(std::move(name), std::move(value));
}
std::string RenderTimings::json(int exit_code, std::string_view build) const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const int report_exit = exit_code == 0 && (!valid() || !finished_) ? 1 : exit_code;
    std::uint64_t accounted = 0;
    for (const auto &phase : phases_)
        accounted += phase.exclusive_ns;
    out << "{\"schema\":1,\"status\":"
        << json_quote(cancelled_         ? "cancelled"
                      : report_exit == 0 ? "complete"
                                         : "failed")
        << ",\"exit_code\":" << report_exit << ",\"render_exit_code\":" << exit_code
        << ",\"collector_valid\":" << (valid() && finished_ ? "true" : "false")
        << ",\"collector_error\":" << json_quote(error_ ? error_ : "")
        << ",\"diagnostic\":" << json_quote(diagnostic_) << ",\"build\":" << json_quote(build)
        << ",\"elapsed_ns\":" << elapsed_ << ",\"exclusive_accounted_ns\":" << accounted
        << ",\"unscoped_ns\":" << (elapsed_ >= accounted ? elapsed_ - accounted : 0)
        << ",\"report_publication_included\":false,\"host_phases\":{";
    for (std::size_t i = 0; i < phases_.size(); ++i) {
        if (i)
            out << ',';
        const auto &p = phases_[i];
        out << json_quote(names[i]) << ":{\"exclusive_ns\":" << p.exclusive_ns
            << ",\"inclusive_ns\":" << p.inclusive_ns << ",\"count\":" << p.count << '}';
    }
    out << "},\"provenance\":{";
    for (std::size_t i = 0; i < provenance_.size(); ++i) {
        if (i)
            out << ',';
        out << json_quote(provenance_[i].first) << ':' << json_quote(provenance_[i].second);
    }
    out << "},\"GPU\":{\"scope\":\"render sample command buffers only\",\"command_count\":"
        << gpu_commands_ << ",\"valid_intervals\":" << gpu_valid_
        << ",\"invalid_or_missing_intervals\":" << gpu_invalid_
        << ",\"completed_interval_ns\":" << gpu_ns_
        << ",\"overlaps_host_wait\":true,\"complete_coverage\":"
        << (gpu_commands_ != 0 && gpu_commands_ == gpu_valid_ ? "true" : "false") << "}}\n";
    return out.str();
}
RenderTimingsOutput::~RenderTimingsOutput() {
    if (descriptor_ >= 0)
        ::close(descriptor_);
    if (pending_descriptor_ >= 0)
        ::close(pending_descriptor_);
}
std::string RenderTimingsOutput::reserve(const std::filesystem::path &path,
                                         std::span<const std::filesystem::path> protected_paths) {
    try {
        if (descriptor_ >= 0 || path.empty() || path.native().find('\0') != std::string::npos)
            return "invalid or repeated timings output path";
        path_ = path;
        pending_ = path;
        pending_ += ".pending";
        for (const auto &candidate : {path_, pending_}) {
            const auto canonical =
                std::filesystem::weakly_canonical(std::filesystem::absolute(candidate));
            for (const auto &protected_path : protected_paths)
                if (!protected_path.empty() &&
                    canonical == std::filesystem::weakly_canonical(
                                     std::filesystem::absolute(protected_path)))
                    return "timings output must not alias scene, image or checkpoint paths";
            if (std::filesystem::exists(candidate) || std::filesystem::is_symlink(candidate))
                return "timings outputs must be fresh (including the pending path)";
        }
        descriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor_ < 0)
            return io_error("could not reserve timings output");
        if (!write_all(descriptor_, "{\"schema\":1,\"status\":\"reserved\"}\n"))
            return io_error("could not write initial timings reservation");
        pending_descriptor_ = ::open(pending_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (pending_descriptor_ < 0)
            return io_error("could not reserve pending timings output");
        return {};
    } catch (const std::filesystem::filesystem_error &error) {
        return std::string("timings output validation failed: ") + error.what();
    }
}
std::string RenderTimingsOutput::publish(const RenderTimings &timings, int exit_code,
                                         std::string_view build) {
    if (!reserved())
        return "timings output was not safely reserved";
    if (!write_all(pending_descriptor_, timings.json(exit_code, build)) ||
        ::fsync(pending_descriptor_) != 0)
        return io_error("could not write timings report (initial reservation retained)");
    struct stat reserved {
    }, current{}, pending_reserved{}, pending_current{};
    if (::fstat(descriptor_, &reserved) != 0 || ::lstat(path_.c_str(), &current) != 0 ||
        reserved.st_dev != current.st_dev || reserved.st_ino != current.st_ino ||
        ::fstat(pending_descriptor_, &pending_reserved) != 0 ||
        ::lstat(pending_.c_str(), &pending_current) != 0 ||
        pending_reserved.st_dev != pending_current.st_dev ||
        pending_reserved.st_ino != pending_current.st_ino)
        return "timings output changed after reservation; report not published";
    if (::rename(pending_.c_str(), path_.c_str()) != 0)
        return io_error("could not publish timings report (initial reservation retained)");
    ::close(pending_descriptor_);
    pending_descriptor_ = -1;
    ::close(descriptor_);
    descriptor_ = -1;
    return {};
}
} // namespace rypath
