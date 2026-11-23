// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rypath {

enum class RenderTimingPhase : std::size_t {
    command,
    preflight_overhead,
    pbrt_check,
    pbrt_translate,
    document_cleanup,
    post_translation,
    render_job,
    identity,
    resume,
    driver_overhead,
    driver_validation,
    film_initialize,
    preparation,
    pipeline,
    initial_checkpoint,
    checkpoint_callback,
    render_loop,
    driver_sample,
    dispatch_setup,
    dispatch_wait,
    dispatch_decode,
    film_stage,
    film_add,
    film_commit,
    checkpoint_copy,
    checkpoint_write,
    progress,
    output_metadata,
    primary_output,
    raw_film_copy,
    raw_output,
    destruction,
    count
};

// Host-only opt-in observations. These never enter scene identity, checkpoint
// payloads, RenderRequest or shader parameters. Inclusive GPU intervals overlap
// host wait and are reported separately from exclusive host phases.
class RenderTimings {
  public:
    using Clock = std::uint64_t (*)();
    struct Phase {
        std::uint64_t exclusive_ns = 0, inclusive_ns = 0, count = 0;
    };
    explicit RenderTimings(Clock clock = steady_nanoseconds);
    std::size_t begin(RenderTimingPhase phase) noexcept;
    void end(std::size_t token) noexcept;
    void transition(RenderTimingPhase phase) noexcept;
    void begin_destruction() noexcept;
    void finish() noexcept;
    void gpu_command(bool completed, double start, double end) noexcept;
    void failure(std::string diagnostic);
    void provenance(std::string name, std::string value);
    void cancelled() noexcept { cancelled_ = true; }
    const auto &phases() const noexcept { return phases_; }
    bool valid() const noexcept { return error_ == nullptr; }
    std::uint64_t gpu_valid_count() const noexcept { return gpu_valid_; }
    std::uint64_t gpu_invalid_count() const noexcept { return gpu_invalid_; }
    std::uint64_t gpu_nanoseconds() const noexcept { return gpu_ns_; }
    std::string json(int exit_code, std::string_view build) const;
    static std::uint64_t steady_nanoseconds() noexcept;

  private:
    struct Frame {
        RenderTimingPhase phase{};
        std::uint64_t start = 0, children = 0;
    };
    static constexpr std::size_t maximum_depth = 32;
    Clock clock_;
    std::array<Phase, static_cast<std::size_t>(RenderTimingPhase::count)> phases_{};
    std::array<Frame, maximum_depth> stack_{};
    std::size_t depth_ = 0;
    std::uint64_t start_ = 0, elapsed_ = 0;
    std::uint64_t gpu_commands_ = 0, gpu_valid_ = 0, gpu_invalid_ = 0, gpu_ns_ = 0;
    const char *error_ = nullptr;
    bool finished_ = false, cancelled_ = false, destruction_requested_ = false;
    std::string diagnostic_;
    std::vector<std::pair<std::string, std::string>> provenance_;
    void segment(std::uint64_t now) noexcept;
};

class RenderTimingScope {
  public:
    RenderTimingScope(RenderTimings *timings, RenderTimingPhase phase) noexcept
        : timings_(timings), token_(timings ? timings->begin(phase) : 0) {}
    ~RenderTimingScope() { close(); }
    RenderTimingScope(const RenderTimingScope &) = delete;
    RenderTimingScope &operator=(const RenderTimingScope &) = delete;
    void close() noexcept {
        if (timings_) {
            timings_->end(token_);
            timings_ = nullptr;
        }
    }

  private:
    RenderTimings *timings_;
    std::size_t token_;
};

template <typename Function>
decltype(auto) render_timed(RenderTimings *timings, RenderTimingPhase phase, Function &&function) {
    if (!timings)
        return std::forward<Function>(function)();
    RenderTimingScope scope(timings, phase);
    return std::forward<Function>(function)();
}

// Reserve fresh report and sibling pending paths only after checking all known
// scene/output aliases. Failure leaves its initial reservation/partial report;
// it never overwrites a caller's preexisting file or masks a render failure.
class RenderTimingsOutput {
  public:
    ~RenderTimingsOutput();
    RenderTimingsOutput() = default;
    RenderTimingsOutput(const RenderTimingsOutput &) = delete;
    RenderTimingsOutput &operator=(const RenderTimingsOutput &) = delete;
    std::string reserve(const std::filesystem::path &path,
                        std::span<const std::filesystem::path> protected_paths);
    std::string publish(const RenderTimings &timings, int exit_code, std::string_view build);
    bool reserved() const noexcept { return descriptor_ >= 0 && pending_descriptor_ >= 0; }

  private:
    std::filesystem::path path_, pending_;
    int descriptor_ = -1, pending_descriptor_ = -1;
};

} // namespace rypath
