// SPDX-License-Identifier: GPL-3.0-only
#include "render_job.hpp"
#include "render_timings.hpp"

#include "render_identity.hpp"
#include <cmath>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/utsname.h>
#include <system_error>

namespace rypath {
namespace {
std::atomic<int> requested_signal{0};
static_assert(std::atomic<int>::is_always_lock_free);

extern "C" void request_cancel(int signal) {
    int expected = 0;
    requested_signal.compare_exchange_strong(expected, signal, std::memory_order_relaxed);
}

class SignalScope {
  public:
    SignalScope() {
        requested_signal.store(0, std::memory_order_relaxed);
        struct sigaction action {};
        action.sa_handler = request_cancel;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (sigaction(SIGINT, &action, &interrupt_) != 0)
            throw std::system_error(errno, std::generic_category(), "install SIGINT handler");
        if (sigaction(SIGTERM, &action, &terminate_) != 0) {
            const auto error = errno;
            sigaction(SIGINT, &interrupt_, nullptr);
            throw std::system_error(error, std::generic_category(), "install SIGTERM handler");
        }
    }
    ~SignalScope() {
        sigaction(SIGINT, &interrupt_, nullptr);
        sigaction(SIGTERM, &terminate_, nullptr);
    }
    SignalScope(const SignalScope &) = delete;
    SignalScope &operator=(const SignalScope &) = delete;

  private:
    struct sigaction interrupt_ {
    }, terminate_{};
};

bool aliases(const std::filesystem::path &first, const std::filesystem::path &second) {
    if (first.empty() || second.empty())
        return false;
    // Missing leaf paths still compare through any existing symlinked parent.
    if (std::filesystem::weakly_canonical(std::filesystem::absolute(first)) ==
        std::filesystem::weakly_canonical(std::filesystem::absolute(second)))
        return true;
    return std::filesystem::exists(first) && std::filesystem::exists(second) &&
           std::filesystem::equivalent(first, second);
}

std::string operating_system() {
    struct utsname info {};
    if (uname(&info) != 0)
        throw std::system_error(errno, std::generic_category(), "read OS provenance");
    return std::string{info.sysname} + " " + info.release + " " + info.version + " " + info.machine;
}
} // namespace

std::string validate_checkpoint_paths(const CheckpointOptions &options,
                                      const std::filesystem::path &image,
                                      std::span<const std::filesystem::path> inputs) {
    if (options.interval_seconds == 0)
        return "--checkpoint-interval must be a positive unsigned integer";
    if (options.interval_supplied && options.output.empty() && options.resume.empty())
        return "--checkpoint-interval requires --checkpoint or --resume";
    for (const auto &path : {options.output, options.resume}) {
        if (aliases(path, image))
            return "checkpoint and image output paths must not alias";
        for (const auto &input : inputs)
            if (aliases(path, input))
                return "checkpoint paths must not alias scene inputs";
    }
    return {};
}

RenderJobResult run_render_job(const RenderScene &scene, const RenderSettings &settings,
                               const CheckpointOptions &options, const std::filesystem::path &image,
                               std::span<const std::filesystem::path> inputs, std::string build,
                               const std::function<void(std::uint64_t)> &progress) {
    return run_render_job(scene, settings, options, image, inputs, std::move(build), progress,
                          nullptr);
}
RenderJobResult run_render_job(const RenderScene &scene, const RenderSettings &settings,
                               const CheckpointOptions &options, const std::filesystem::path &image,
                               std::span<const std::filesystem::path> inputs, std::string build,
                               const std::function<void(std::uint64_t)> &progress,
                               RenderTimings *timings) {
    RenderJobResult job;
    job.render.diagnostic = validate_checkpoint_paths(options, image, inputs);
    if (!job.render.diagnostic.empty())
        return job;
    const SignalScope signals;
    const auto destination = options.output.empty() ? options.resume : options.output;
    const bool enabled = !destination.empty();
    RenderCheckpoint checkpoint;
    checkpoint.settings = settings;
    if (enabled) {
        checkpoint.identity = render_timed(timings, RenderTimingPhase::identity,
                                           [&] { return render_identity(scene, settings); });
        job.identity = digest_hex(checkpoint.identity);
    }
    if (!options.resume.empty()) {
        RenderTimingScope resume_timing(timings, RenderTimingPhase::resume);
        auto loaded = read_checkpoint(options.resume);
        if (!loaded.value) {
            job.render.diagnostic = loaded.diagnostic;
            return job;
        }
        if (loaded.value->identity != checkpoint.identity ||
            loaded.value->settings.width != settings.width ||
            loaded.value->settings.height != settings.height ||
            loaded.value->settings.samples != settings.samples ||
            loaded.value->settings.sampler.kind != settings.sampler.kind ||
            loaded.value->settings.sampler.randomization != settings.sampler.randomization ||
            (settings.sampler.kind != SamplerKind::independent
                 ? sobol_signed_seed(loaded.value->settings.seed) !=
                       sobol_signed_seed(settings.seed)
                 : loaded.value->settings.seed != settings.seed) ||
            loaded.value->film.model != scene.model ||
            loaded.value->film.packet != (scene.spectral_transport == SpectralTransport::packet) ||
            (loaded.value->film.packet &&
             (loaded.value->film.filter != scene.filter ||
              loaded.value->film.max_component_value != scene.max_component_value ||
              (std::isfinite(scene.max_component_value) &&
               loaded.value->film.exposure != scene.exposure)))) {
            job.render.diagnostic =
                "checkpoint does not match the scene, sampler, or fixed sample target";
            return job;
        }
        checkpoint = std::move(*loaded.value);
        checkpoint.settings = settings;
        // Finite-clamp packet films contain sensor-exposed compatibility planes.
        // Their exposure is part of identity and must not change on continuation.
        if (!checkpoint.film.packet || !std::isfinite(checkpoint.film.max_component_value))
            checkpoint.film.exposure = scene.exposure;
        std::cout << "Resuming samples per pixel: " << checkpoint.film.pixels.front().count << '/'
                  << settings.samples << '\n';
    }
    const auto start = checkpoint.film.pixels.empty() ? 0 : checkpoint.film.pixels.front().count;
    const auto os = enabled ? operating_system() : std::string{};
    auto last_save = std::chrono::steady_clock::now();
    bool saved = false;
    bool session_segment = false;
    std::uint64_t saved_count = 0;
    RenderControl control;
    control.initial_film = options.resume.empty() ? nullptr : &checkpoint.film;
    control.cancelled = [] { return requested_signal.load(std::memory_order_relaxed) != 0; };
    control.progress = progress;
    control.timings = timings;
    const auto save = [&](const Film &film) -> std::string {
        // Keep the persistent snapshot separate from the driver's current Film.
        render_timed(timings, RenderTimingPhase::checkpoint_copy, [&] { checkpoint.film = film; });
        const auto diagnostic = render_timed(timings, RenderTimingPhase::checkpoint_write, [&] {
            return write_checkpoint(checkpoint, destination);
        });
        if (diagnostic.empty()) {
            saved = true;
            saved_count = film.pixels.front().count;
            last_save = std::chrono::steady_clock::now();
            std::cout << "Checkpoint saved: " << destination.string() << " (" << saved_count << '/'
                      << settings.samples << " samples per pixel)\n"
                      << std::flush;
        }
        return diagnostic;
    };
    if (enabled) {
        control.committed = [&](const Film &film, const MetalDeviceInfo &device) -> std::string {
            const auto count = film.pixels.front().count;
            if (count > start) {
                if (!session_segment) {
                    checkpoint.segments.push_back(
                        {start, count, build, device.name, device.architecture, os});
                    session_segment = true;
                } else {
                    checkpoint.segments.back().end = count;
                }
            }
            if (!saved || count == settings.samples ||
                requested_signal.load(std::memory_order_relaxed) != 0 ||
                std::chrono::steady_clock::now() - last_save >=
                    std::chrono::seconds{options.interval_seconds})
                return save(film);
            return {};
        };
    }
    job.render = render_progressive(scene, settings, control);
    // A signal can arrive after the last callback and before the next batch poll.
    if (enabled && job.render.completion == RenderCompletion::cancelled &&
        (!saved || saved_count != job.render.film.pixels.front().count)) {
        job.render.diagnostic = save(job.render.film);
        if (!job.render.diagnostic.empty()) {
            job.render.status = GeometryStatus::failure;
            job.render.completion = RenderCompletion::failed;
        }
    }
    job.signal = requested_signal.load(std::memory_order_relaxed);
    job.segments = std::move(checkpoint.segments);
    return job;
}

std::string checkpoint_provenance(std::span<const CheckpointSegment> segments) {
    std::ostringstream text;
    for (const auto &segment : segments)
        text << '[' << segment.begin << ',' << segment.end
             << ") build=" << std::quoted(segment.build)
             << " device=" << std::quoted(segment.device)
             << " architecture=" << std::quoted(segment.architecture)
             << " os=" << std::quoted(segment.os) << '\n';
    return text.str();
}
} // namespace rypath
