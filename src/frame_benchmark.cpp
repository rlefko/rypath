// SPDX-License-Identifier: GPL-3.0-only
#include "area_wire.h"
#include "frame_benchmark_motion.hpp"
#include "frame_session.hpp"
#include "frame_statistics.hpp"
#include "gpu_capture.hpp"
#include "image_output.hpp"
#include "mutation_shared.h"
#include "pbrt_render.hpp"
#include "reconstruction.hpp"
#include "render_identity.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef RYPATH_BUILD_DESCRIPTION
#define RYPATH_BUILD_DESCRIPTION "unknown"
#endif

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
std::string quote(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += char(c);
        } else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else
            result += char(c);
    }
    return result + '"';
}
template <typename T> bool integer(std::string_view value, T &result) {
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
struct Options {
    fs::path scene, output, capture, preview, reconstructed_capture;
    bool demo = false, profile_stages = false;
    std::optional<float> motion_offset;
    std::optional<std::uint64_t> capture_frame;
    fs::path capture_path;
    std::optional<rypath::ReconstructionMode> reconstruction;
    bool guide_cache = false;
    std::uint32_t output_width = 0, output_height = 0;
    rypath::FrameExecution execution = rypath::FrameConfig{}.execution;
    rypath::FrameEstimator estimator = rypath::FrameEstimator::independent;
    float target_scale = 1.f;
    std::uint32_t mutation_steps = 1;
    std::uint32_t capacity = rypath::FrameConfig{}.capacity;
    std::uint32_t quantum = rypath::FrameConfig{}.quantum;
    std::uint32_t group_width = rypath::FrameConfig{}.group_width;
    std::uint32_t packet_events = 1;
    rypath::FramePacketLayout packet_layout = rypath::FramePacketLayout::packed;
    std::uint32_t width = 1920, height = 1080;
    std::uint64_t frames = 120, warmup = 8, seed = 1;
    std::int32_t material_seed = 0;
    rypath::IntersectionProfile intersection_profile = rypath::IntersectionProfile::native;
    rypath::ShadowTraversal shadow_traversal = rypath::ShadowTraversal::query;
    rypath::AccelerationBuildPolicy as_build = rypath::AccelerationBuildPolicy::default_build;
};
const char *shadow_name(rypath::ShadowTraversal mode) {
    return mode == rypath::ShadowTraversal::opaque_auto ? "opaque-auto" : "query";
}
const char *packet_layout_name(rypath::FramePacketLayout layout) {
    return layout == rypath::FramePacketLayout::hot_cold      ? "hot-cold"
           : layout == rypath::FramePacketLayout::split_trace ? "split-trace"
                                                              : "packed";
}
const char *profile_name(rypath::IntersectionProfile profile) {
    return profile == rypath::IntersectionProfile::native ? "native" : "compensated";
}
const char *execution_name(rypath::FrameExecution execution) {
    return execution == rypath::FrameExecution::wavefront ? "wavefront" : "megakernel";
}
const char *estimator_name(rypath::FrameEstimator estimator) {
    return estimator == rypath::FrameEstimator::area_replay ? "area-replay" : "independent";
}
const char *reconstruction_name(const std::optional<rypath::ReconstructionMode> &mode) {
    if (!mode)
        return "none";
    return *mode == rypath::ReconstructionMode::metalfx    ? "metalfx"
           : *mode == rypath::ReconstructionMode::temporal ? "temporal"
                                                           : "raw-rgb";
}
std::string precise_float(float value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(std::numeric_limits<float>::max_digits10);
    out << value;
    return out.str();
}
const char *usage =
    "Usage: rypath-frame-benchmark (--scene SCENE.pbrt | --demo) --output REPORT.json\n"
    "  [--width 1920] [--height 1080] [--frames 120] [--warmup 8]\n"
    "  [--as-build default|fast-blas|fast-tlas|fast-all (default: default)]\n"
    "  [--seed 1] [--material-seed 0] [--intersections native|compensated (default: native)]\n"
    "  [--shadow-traversal query|opaque-auto (default: query)]\n"
    "  [--execution wavefront|megakernel (default: wavefront)]\n"
    "  [--estimator independent|area-replay (default: independent)] [--target-scale 1]\n"
    "  [--mutations 0|1|2|4 (default: 1)]\n"
    "  [--packet-events 1|2|4 (default: 1)] [--packet-layout packed|hot-cold|split-trace]\n"
    "  [--capacity 65536] [--quantum 8] [--group-width 32]\n"
    "  [--motion-offset WORLD_X (measured quarters: hold A, translate, hold B, cut A)]\n"
    "  [--profile-stages] [--capture-frame N --capture-path FRAME.gputrace]\n"
    "  [--capture FLOAT.exr] [--preview IMAGE.png]\n"
    "  [--reconstruction none|raw-rgb|metalfx|temporal (default: none)]\n"
    "  [--guide-cache on|off (default: on for temporal/metalfx, off otherwise)]\n"
    "  [--output-width WIDTH] [--output-height HEIGHT] [--reconstructed-capture FLOAT.exr]\n"
    "Fresh output paths required. One fresh path/pixel/frame; Area reuse adds complete replays.\n";
std::string parse(int argc, char **argv, Options &o) {
    std::set<std::string_view> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (!seen.insert(key).second)
            return "duplicate option: " + std::string(key);
        if (key == "--profile-stages") {
            o.profile_stages = true;
            continue;
        }
        if (key == "--demo") {
            o.demo = true;
            continue;
        }
        if (i + 1 == argc || std::string_view(argv[i + 1]).starts_with("--"))
            return "missing value for " + std::string(key);
        const std::string_view value = argv[++i];
        if (key == "--scene")
            o.scene = value;
        else if (key == "--output")
            o.output = value;
        else if (key == "--capture-frame") {
            std::uint64_t frame = 0;
            if (!integer(value, frame))
                return "invalid capture frame";
            o.capture_frame = frame;
        } else if (key == "--capture-path")
            o.capture_path = value;
        else if (key == "--capture")
            o.capture = value;
        else if (key == "--preview")
            o.preview = value;
        else if (key == "--reconstructed-capture")
            o.reconstructed_capture = value;
        else if (key == "--output-width") {
            if (!integer(value, o.output_width) || !o.output_width)
                return "invalid output width";
        } else if (key == "--output-height") {
            if (!integer(value, o.output_height) || !o.output_height)
                return "invalid output height";
        } else if (key == "--guide-cache") {
            if (value != "on" && value != "off")
                return "guide cache must be on or off";
            o.guide_cache = value == "on";
        } else if (key == "--reconstruction") {
            if (value == "none")
                o.reconstruction.reset();
            else if (value == "raw-rgb")
                o.reconstruction = rypath::ReconstructionMode::raw_rgb;
            else if (value == "metalfx")
                o.reconstruction = rypath::ReconstructionMode::metalfx;
            else if (value == "temporal")
                o.reconstruction = rypath::ReconstructionMode::temporal;
            else
                return "invalid reconstruction mode: " + std::string(value);
        } else if (key == "--estimator") {
            if (value == "independent")
                o.estimator = rypath::FrameEstimator::independent;
            else if (value == "area-replay")
                o.estimator = rypath::FrameEstimator::area_replay;
            else
                return "invalid estimator: " + std::string(value);
        } else if (key == "--mutations") {
            if (!integer(value, o.mutation_steps) ||
                !rypath_mutation::valid_step_count(o.mutation_steps))
                return "mutations must be 0, 1, 2, or 4";
        } else if (key == "--motion-offset") {
            float offset = 0;
            std::istringstream input{std::string(value)};
            input.imbue(std::locale::classic());
            input >> offset;
            if (!input || !input.eof() || !std::isfinite(offset) || offset == 0)
                return "motion offset must be finite and nonzero";
            o.motion_offset = offset;
        } else if (key == "--target-scale") {
            std::istringstream input{std::string(value)};
            input.imbue(std::locale::classic());
            input >> o.target_scale;
            if (!input || !input.eof() || !std::isnormal(o.target_scale) || o.target_scale <= 0)
                return "target scale must be positive, finite and normal";
        } else if (key == "--execution") {
            if (value == "wavefront")
                o.execution = rypath::FrameExecution::wavefront;
            else if (value == "megakernel")
                o.execution = rypath::FrameExecution::megakernel;
            else
                return "invalid execution mode: " + std::string(value);
        } else if (key == "--capacity") {
            if (!integer(value, o.capacity) || !o.capacity || o.capacity > 65536)
                return "capacity must be in [1,65536]";
        } else if (key == "--quantum") {
            if (!integer(value, o.quantum) || !o.quantum || o.quantum > 1024)
                return "quantum must be in [1,1024]";
        } else if (key == "--packet-layout") {
            if (value == "packed")
                o.packet_layout = rypath::FramePacketLayout::packed;
            else if (value == "hot-cold")
                o.packet_layout = rypath::FramePacketLayout::hot_cold;
            else if (value == "split-trace")
                o.packet_layout = rypath::FramePacketLayout::split_trace;
            else
                return "invalid packet layout";
        } else if (key == "--packet-events") {
            if (!integer(value, o.packet_events) ||
                (o.packet_events != 1 && o.packet_events != 2 && o.packet_events != 4))
                return "packet events must be 1, 2, or 4";
        } else if (key == "--group-width") {
            if (!integer(value, o.group_width) || !o.group_width || o.group_width > 256)
                return "group width must be in [1,256]";
        } else if (key == "--shadow-traversal") {
            if (value == "query")
                o.shadow_traversal = rypath::ShadowTraversal::query;
            else if (value == "opaque-auto")
                o.shadow_traversal = rypath::ShadowTraversal::opaque_auto;
            else
                return "invalid shadow traversal: " + std::string(value);
        } else if (key == "--as-build") {
            const auto policy = rypath::parse_acceleration_build_policy(value);
            if (!policy)
                return "invalid acceleration build policy: " + std::string(value);
            o.as_build = *policy;
        } else if (key == "--intersections") {
            if (value == "native")
                o.intersection_profile = rypath::IntersectionProfile::native;
            else if (value == "compensated")
                o.intersection_profile = rypath::IntersectionProfile::compensated;
            else
                return "invalid intersections profile: " + std::string(value);
        } else if (key == "--width") {
            if (!integer(value, o.width) || !o.width)
                return "invalid width";
        } else if (key == "--height") {
            if (!integer(value, o.height) || !o.height)
                return "invalid height";
        } else if (key == "--frames") {
            if (!integer(value, o.frames) || !o.frames)
                return "invalid frames";
        } else if (key == "--warmup") {
            if (!integer(value, o.warmup))
                return "invalid warmup";
        } else if (key == "--seed") {
            if (!integer(value, o.seed))
                return "invalid seed";
        } else if (key == "--material-seed") {
            if (!integer(value, o.material_seed))
                return "invalid material seed";
        } else
            return "unknown option: " + std::string(key);
    }
    if (!seen.contains("--guide-cache"))
        o.guide_cache = o.reconstruction == rypath::ReconstructionMode::temporal ||
                        o.reconstruction == rypath::ReconstructionMode::metalfx;
    if (o.guide_cache &&
        (!o.reconstruction || o.reconstruction == rypath::ReconstructionMode::raw_rgb))
        return "guide cache requires temporal or metalfx reconstruction";
    if (o.as_build != rypath::AccelerationBuildPolicy::default_build &&
        o.intersection_profile != rypath::IntersectionProfile::native)
        return "fast acceleration builds require native intersections";
    if (!o.reconstruction && (seen.contains("--output-width") || seen.contains("--output-height") ||
                              !o.reconstructed_capture.empty()))
        return "output dimensions and reconstructed capture require reconstruction";
    if (!o.output_width)
        o.output_width = o.width;
    if (!o.output_height)
        o.output_height = o.height;
    if (o.reconstruction && (o.width > rypath::reconstruction_dimension_limit ||
                             o.height > rypath::reconstruction_dimension_limit ||
                             o.output_width > rypath::reconstruction_dimension_limit ||
                             o.output_height > rypath::reconstruction_dimension_limit))
        return "reconstruction dimensions exceed 16384";
    if (o.reconstruction == rypath::ReconstructionMode::raw_rgb &&
        (o.output_width != o.width || o.output_height != o.height))
        return "raw-rgb output dimensions must equal input dimensions";
    if (o.reconstruction == rypath::ReconstructionMode::temporal &&
        (o.output_width != o.width || o.output_height != o.height))
        return "temporal output dimensions must equal input dimensions";
    if (o.reconstruction == rypath::ReconstructionMode::metalfx &&
        (o.output_width < o.width || o.output_height < o.height))
        return "metalfx does not support downscaling";
    if (o.capture_frame.has_value() != !o.capture_path.empty())
        return "--capture-frame and --capture-path require each other";
    if (o.capture_frame && *o.capture_frame >= o.warmup + o.frames)
        return "capture frame must be less than warmup + frames (zero-based absolute frame)";
    if (o.motion_offset && (o.frames < 8 || o.frames % 4 != 0))
        return "motion requires at least eight measured frames divisible by four";
    if (!o.capture_path.empty() && o.capture_path.extension() != ".gputrace")
        return "--capture-path requires .gputrace";
    if ((o.packet_events != 1 || o.packet_layout != rypath::FramePacketLayout::packed) &&
        o.execution != rypath::FrameExecution::wavefront)
        return "packet scheduling controls require wavefront execution";
    if (o.packet_layout == rypath::FramePacketLayout::split_trace && o.packet_events != 1)
        return "split packet trace requires --packet-events 1";
    if (!o.reconstructed_capture.empty() && o.reconstructed_capture.extension() != ".exr")
        return "--reconstructed-capture requires .exr";
    if (o.estimator == rypath::FrameEstimator::area_replay &&
        o.execution != rypath::FrameExecution::wavefront)
        return "area-replay requires wavefront execution";
    if (o.demo == !o.scene.empty())
        return "choose exactly one of --scene and --demo";
    if (o.output.empty())
        return "--output is required";
    if (!o.capture.empty() && o.capture.extension() != ".exr")
        return "--capture requires .exr";
    if (!o.preview.empty() && o.preview.extension() != ".png")
        return "--preview requires .png";
    if (o.width > INT32_MAX || o.height > INT32_MAX ||
        std::uint64_t(o.width) * o.height > UINT32_MAX)
        return "resolution exceeds pixel indexing limit";
    if (o.warmup > UINT64_MAX - o.frames)
        return "frame sample count overflow";
    return {};
}
// Keep a descriptor for each reservation; verify inode ownership before publication.
class Output {
    fs::path path_, pending_;
    int descriptor_ = -1, pending_descriptor_ = -1;
    static bool same(int fd, const fs::path &path) {
        struct stat a {
        }, b{};
        return ::fstat(fd, &a) == 0 && ::lstat(path.c_str(), &b) == 0 && a.st_dev == b.st_dev &&
               a.st_ino == b.st_ino;
    }

  public:
    ~Output() {
        if (descriptor_ >= 0)
            ::close(descriptor_);
        if (pending_descriptor_ >= 0)
            ::close(pending_descriptor_);
    }
    void reserve(const fs::path &path) {
        path_ = path;
        pending_ = path;
        pending_ += ".pending";
        descriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor_ < 0)
            throw std::runtime_error("could not reserve fresh output: " + path.string());
        constexpr std::string_view initial = "{\"schema\":1,\"status\":\"reserved\"}\n";
        if (::write(descriptor_, initial.data(), initial.size()) !=
            static_cast<ssize_t>(initial.size()))
            throw std::runtime_error("could not write output reservation");
        pending_descriptor_ = ::open(pending_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (pending_descriptor_ < 0)
            throw std::runtime_error("could not reserve pending output: " + pending_.string());
    }
    void publish(std::string_view data) {
        while (!data.empty()) {
            const auto n = ::write(pending_descriptor_, data.data(), data.size());
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("report write failed");
            data.remove_prefix(std::size_t(n));
        }
        if (::fsync(pending_descriptor_) || !same(descriptor_, path_) ||
            !same(pending_descriptor_, pending_) || ::rename(pending_.c_str(), path_.c_str()))
            throw std::runtime_error("report publication failed");
    }
    void image(const rypath::Film &film, bool exr,
               std::span<const std::pair<std::string, std::string>> metadata = {}) {
        if (!same(descriptor_, path_) || !same(pending_descriptor_, pending_))
            throw std::runtime_error("capture reservation changed");
        const auto error =
            exr ? rypath::write_exr(film, pending_, rypath::ExrPrecision::float32, metadata)
                : rypath::write_png(film, pending_);
        if (!error.empty())
            throw std::runtime_error(error);
        if (!same(descriptor_, path_) || ::rename(pending_.c_str(), path_.c_str()))
            throw std::runtime_error("capture publication failed");
    }
    void linear_rgb(const rypath::ReconstructionCapture &image,
                    std::span<const std::pair<std::string, std::string>> metadata) {
        if (image.status != rypath::GeometryStatus::success ||
            image.linear_rgb.size() != std::uint64_t(image.width) * image.height)
            throw std::runtime_error("incomplete reconstructed RGB capture");
        if (!same(descriptor_, path_) || !same(pending_descriptor_, pending_))
            throw std::runtime_error("reconstructed capture reservation changed");
        std::vector<float> rgb;
        rgb.reserve(image.linear_rgb.size() * 3);
        for (const auto &pixel : image.linear_rgb)
            rgb.insert(rgb.end(), pixel.begin(), pixel.end());
        if (::ftruncate(pending_descriptor_, 0))
            throw std::runtime_error("could not truncate reconstructed capture reservation");
        std::uint64_t end = 0;
        bool write_failed = false;
        const auto error = rypath::encode_exr(
            {image.width, image.height, rgb, rypath::ExrPrecision::float32, metadata},
            [&](std::uint64_t offset, std::span<const std::uint8_t> bytes) {
                const auto maximum = std::uint64_t(std::numeric_limits<off_t>::max());
                if (offset > maximum || bytes.size() > maximum - offset) {
                    write_failed = true;
                    return false;
                }
                while (!bytes.empty()) {
                    const auto requested =
                        std::min<std::size_t>(bytes.size(), std::numeric_limits<ssize_t>::max());
                    const auto written =
                        ::pwrite(pending_descriptor_, bytes.data(), requested, off_t(offset));
                    if (written < 0 && errno == EINTR)
                        continue;
                    if (written <= 0) {
                        write_failed = true;
                        return false;
                    }
                    offset += std::uint64_t(written);
                    bytes = bytes.subspan(std::size_t(written));
                }
                end = std::max(end, offset);
                return true;
            },
            {.max_dimension = std::max(image.width, image.height),
             .retained_bytes = rgb.size() * sizeof(float)});
        if (write_failed || !error.empty())
            throw std::runtime_error(write_failed ? "reconstructed EXR positioned write failed"
                                                  : error);
        if (::ftruncate(pending_descriptor_, off_t(end)) || ::fsync(pending_descriptor_) ||
            !same(descriptor_, path_) || !same(pending_descriptor_, pending_) ||
            ::rename(pending_.c_str(), path_.c_str()))
            throw std::runtime_error("reconstructed capture publication failed");
    }
};
void validate_paths(const Options &o, const std::vector<fs::path> &inputs) {
    std::set<fs::path> outputs;
    for (const auto &path :
         {o.output, o.capture, o.preview, o.reconstructed_capture, o.capture_path}) {
        if (path.empty())
            continue;
        for (auto candidate : {path, fs::path(path.string() + ".pending")}) {
            const auto canonical = fs::weakly_canonical(fs::absolute(candidate));
            if (!outputs.insert(canonical).second)
                throw std::runtime_error("outputs alias each other");
            for (const auto &input : inputs)
                if (canonical == fs::weakly_canonical(fs::absolute(input)))
                    throw std::runtime_error("output aliases scene input dependency");
            if (fs::exists(candidate) || fs::is_symlink(candidate))
                throw std::runtime_error("outputs must be fresh: " + candidate.string());
        }
    }
}
void set_parameter(rypath::pbrt::Descriptor &d, std::string name,
                   std::optional<std::int32_t> value) {
    // Preserve the preflight-selected last matching parameters while indices move.
    std::vector<rypath::pbrt::Parameter> effective;
    for (const auto index : d.effective_authored)
        effective.push_back(d.authored.parameters[index]);
    d.authored.parameters = std::move(effective);
    std::erase_if(d.authored.parameters, [&](const auto &p) { return p.name == name; });
    std::erase_if(d.inherited, [&](const auto &p) { return p.parameter->name == name; });
    if (value) {
        rypath::pbrt::Parameter p;
        p.type = "integer";
        p.name = name;
        rypath::pbrt::Value v;
        v.kind = rypath::pbrt::ValueKind::integer;
        v.integer = *value;
        v.number = *value;
        v.text = std::to_string(*value);
        p.values.push_back(v);
        d.authored.parameters.push_back(p);
    }
    d.effective_authored.clear();
    for (std::size_t i = 0; i < d.authored.parameters.size(); ++i)
        d.effective_authored.push_back(i);
}
// Overrides apply only to this benchmark's checked copy, before rendering translation.
std::vector<std::string> overrides(rypath::pbrt::Result &checked, const Options &o) {
    std::vector<std::string> provenance;
    for (const auto &[name, type] :
         {std::pair{"Sampler", "independent"}, std::pair{"Film", "rgb"}}) {
        if (std::ranges::none_of(checked.scene->options,
                                 [&](const auto &d) { return d.authored.name == name; })) {
            rypath::pbrt::Descriptor descriptor;
            descriptor.authored.name = name;
            rypath::pbrt::Value value;
            value.kind = rypath::pbrt::ValueKind::string;
            value.text = type;
            descriptor.authored.arguments.push_back(value);
            checked.scene->options.push_back(std::move(descriptor));
            provenance.push_back(std::string("implicit ") + name +
                                 " overridden for frame benchmark");
        }
    }
    for (auto &d : checked.scene->options) {
        if (d.authored.name == "Film" || d.authored.name == "Sampler" ||
            d.authored.name == "Integrator") {
            const auto retain = [&](const auto &p, std::string_view source) {
                std::string text =
                    std::string(source) + " " + d.authored.name + " " + p.type + " " + p.name;
                for (const auto &v : p.values)
                    text += " " + v.text;
                provenance.push_back(text);
            };
            for (const auto &p : d.authored.parameters)
                retain(p, "authored");
            for (const auto &p : d.inherited)
                retain(*p.parameter, "inherited");
        }
        if (d.authored.name == "Integrator") {
            provenance.push_back("authored Integrator " + d.authored.arguments.front().text);
            if (d.authored.arguments.front().text == "bdpt")
                d.authored.arguments.front().text = "path";
        } else if (d.authored.name == "Sampler") {
            provenance.push_back("authored Sampler " + d.authored.arguments.front().text);
            d.authored.arguments.front().text = "independent";
            d.authored.parameters.clear();
            d.inherited.clear();
            d.effective_authored.clear();
            set_parameter(d, "pixelsamples", 1);
        } else if (d.authored.name == "Film") {
            set_parameter(d, "xresolution", std::int32_t(o.width));
            set_parameter(d, "yresolution", std::int32_t(o.height));
            set_parameter(d, "maxcomponentvalue", {});
        }
    }
    return provenance;
}
struct Observation : rypath::FrameStatisticsSample {
    rypath::frame_benchmark::MotionPose pose;
    std::optional<rypath::FrameCamera> camera;
    std::uint64_t sample_index = 0, completed_samples = 0, camera_upload_bytes = 0,
                  command_count = 0, area_serial = 0, history_generation = 0,
                  completed_evaluations = 0, fresh_candidates = 0, mutation_attempts = 0,
                  mutation_accepts = 0;
    std::uint32_t temporal_candidates = 0, spatial_candidates = 0;
    bool history_used = false;
    rypath::GPUStageDiagnostics gpu_stages;
    rypath::FrameStatisticsSample pipeline;
    std::optional<rypath::ReconstructionInfo> reconstruction;
    double reconstruction_wall_seconds = 0;
    bool reconstruction_started = false;
    std::string diagnostic;
};
void write_stages(std::ostream &out, const rypath::GPUStageDiagnostics &d) {
    out << "{\"requested\":" << (d.requested ? "true" : "false")
        << ",\"complete\":" << (d.complete ? "true" : "false") << ",\"status\":" << quote(d.status)
        << ",\"sampling_mode\":" << quote(d.sampling_mode)
        << ",\"structural_overhead\":" << (d.structural_overhead ? "true" : "false")
        << ",\"intervals_may_overlap\":true,\"stages\":[";
    bool first = true;
    for (const auto &s : d.stages) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"name\":" << quote(s.name) << ",\"dispatches\":" << s.dispatches
            << ",\"seconds\":";
        if (s.valid)
            out << s.seconds;
        else
            out << "null";
        out << '}';
    }
    out << "]}";
}
void write_camera(std::ostream &out, const std::optional<rypath::FrameCamera> &camera) {
    if (!camera) {
        out << "null";
        return;
    }
    const auto point = [&](std::string_view name, const rypath::GeometryPoint &value) {
        out << ',' << quote(name) << ":[" << value.x << ',' << value.y << ',' << value.z << ']';
    };
    out << "{\"kind\":" << quote(camera->raster_camera ? "raster" : "demo");
    if (camera->raster_camera) {
        point("origin", camera->raster_camera->origin);
        point("corner", camera->raster_camera->corner);
        point("dx", camera->raster_camera->dx);
        point("dy", camera->raster_camera->dy);
    } else {
        point("origin", camera->camera.origin);
        point("target", camera->camera.target);
        point("up", camera->camera.up);
        out << ",\"vertical_fov_degrees\":" << camera->camera.vertical_fov_degrees;
    }
    out << '}';
}
void write_counters(std::ostream &out, const rypath::ReconstructionCounters &c) {
    const std::array<std::pair<const char *, std::uint32_t>, 16> fields{
        {{"failure", c.failure},
         {"completed", c.completed},
         {"color_negative", c.color_negative},
         {"color_high", c.color_high},
         {"albedo_clamped", c.albedo_clamped},
         {"roughness_clamped", c.roughness_clamped},
         {"mix_fallback", c.mix_fallback},
         {"measured_fallback", c.measured_fallback},
         {"fourier_fallback", c.fourier_fallback},
         {"layered_approximation", c.layered_approximation},
         {"transmission_approximation", c.transmission_approximation},
         {"normal_approximation", c.normal_approximation},
         {"passthrough", c.passthrough},
         {"media_approximation", c.media_approximation},
         {"invalid_previous", c.invalid_previous},
         {"background", c.background}}};
    out << '{';
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i)
            out << ',';
        out << quote(fields[i].first) << ':' << fields[i].second;
    }
    out << '}';
}
struct Report {
    Options options;
    rypath::AccelerationBuildInfo acceleration_build;
    std::string diagnostic, device, architecture, integrator, os, source_identity;
    std::uint64_t registry_id = 0, warmup_completed = 0, frames_completed = 0;
    double startup_seconds = 0, warmup_seconds = 0, reconstruction_startup_seconds = 0;
    std::uint32_t max_depth = 0;
    bool packet_controls_effective = false;
    std::vector<std::string> provenance;
    std::vector<fs::path> dependencies;
    std::vector<Observation> observations;
    std::optional<rypath::ShadowTraversalSelection> shadow_traversal;
    std::string json(int code) const {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out.precision(17);
        out << "{\"schema\":1,\"benchmark\":\"serial-resident-frame-v1\",\"status\":"
            << quote(code == 0   ? "complete"
                     : code == 2 ? "unavailable"
                                 : "failed")
            << ",\"exit_code\":" << code << ",\"diagnostic\":" << quote(diagnostic)
            << ",\"source_identity\":" << quote(source_identity)
            << ",\"build\":" << quote(RYPATH_BUILD_DESCRIPTION) << ",\"os\":" << quote(os)
            << ",\"scene\":" << quote(options.demo ? "demo" : options.scene.string())
            << ",\"motion_offset\":"
            << (options.motion_offset ? precise_float(*options.motion_offset) : "null")
            << ",\"motion_scope\":\"timing only; translation with fixed orientation/lens; "
               "distinct medium interfaces unsupported; source_identity identifies base pose\""
            << ",\"device\":{\"name\":" << quote(device)
            << ",\"architecture\":" << quote(architecture) << ",\"registry_id\":" << registry_id
            << "},\"settings\":{\"width\":" << options.width << ",\"height\":" << options.height
            << ",\"reconstruction\":" << quote(reconstruction_name(options.reconstruction))
            << ",\"guide_cache\":" << (options.guide_cache ? "true" : "false")
            << ",\"internal_width\":" << options.width << ",\"internal_height\":" << options.height
            << ",\"output_width\":" << options.output_width
            << ",\"output_height\":" << options.output_height
            << ",\"output_pixels_per_input_pixel\":"
            << double(std::uint64_t(options.output_width) * options.output_height) /
                   double(std::uint64_t(options.width) * options.height)
            << ",\"seed\":" << options.seed << ",\"material_seed\":" << options.material_seed
            << ",\"execution\":" << quote(execution_name(options.execution))
            << ",\"estimator\":" << quote(estimator_name(options.estimator))
            << ",\"area_contract\":"
            << (options.estimator == rypath::FrameEstimator::area_replay
                    ? rypath::area_wire::contract
                    : 0)
            << ",\"replay_contract\":"
            << (options.estimator == rypath::FrameEstimator::area_replay ? rypath_replay::contract
                                                                         : 0)
            << ",\"area_target\":\"bounded-l1-contrast-0.1-v1\""
            << ",\"mutation_steps\":" << options.mutation_steps << ",\"effective_mutation_steps\":"
            << (options.estimator == rypath::FrameEstimator::area_replay ? options.mutation_steps
                                                                         : 0)
            << ",\"target_scale\":" << options.target_scale
            << ",\"fresh_paths_per_pixel_per_frame\":1"
            << ",\"capacity\":" << options.capacity << ",\"effective_capacity\":"
            << (options.execution == rypath::FrameExecution::wavefront
                    ? std::to_string(std::min<std::uint64_t>(
                          options.capacity, std::uint64_t(options.width) * options.height))
                    : "null")
            << ",\"packet_layout\":" << quote(packet_layout_name(options.packet_layout))
            << ",\"packet_events\":" << options.packet_events << ",\"effective_packet_events\":"
            << (packet_controls_effective ? std::to_string(options.packet_events) : "null")
            << ",\"effective_packet_layout\":"
            << (packet_controls_effective ? quote(packet_layout_name(options.packet_layout))
                                          : "null")
            << ",\"quantum\":" << options.quantum << ",\"group_width\":" << options.group_width
            << ",\"profile_stages\":" << (options.profile_stages ? "true" : "false")
            << ",\"capture_path\":" << quote(options.capture_path.string())
            << ",\"capture_frame\":";
        if (options.capture_frame)
            out << *options.capture_frame;
        else
            out << "null";
        out << ",\"intersection_profile\":" << quote(profile_name(options.intersection_profile))
            << ",\"shadow_traversal_requested\":" << quote(shadow_name(options.shadow_traversal))
            << ",\"shadow_traversal_effective\":"
            << (shadow_traversal ? quote(shadow_name(shadow_traversal->effective)) : "null")
            << ",\"opaque_shadow_eligible\":"
            << (shadow_traversal ? (shadow_traversal->eligible ? "true" : "false") : "null")
            << ",\"opaque_shadow_eligibility_reason\":"
            << (shadow_traversal ? quote(shadow_traversal->reason) : "null")
            << ",\"shadow_traversal_fallback_reason\":"
            << quote(shadow_traversal &&
                             options.shadow_traversal == rypath::ShadowTraversal::opaque_auto &&
                             !shadow_traversal->eligible
                         ? shadow_traversal->reason
                         : "")
            << ",\"as_build\":" << quote(rypath::acceleration_build_policy_name(options.as_build))
            << ",\"integrator\":" << quote(integrator) << ",\"max_depth\":" << max_depth
            << ",\"sampler\":\"independent\",\"samples_per_pixel_per_frame\":1,\"footprint_"
               "samples\":1,"
               "\"raw_unclamped\":true,\"submission\":\"serial-submit-wait\"},\"requested_frames\":"
            << options.frames << ",\"requested_warmup\":" << options.warmup
            << ",\"completed_frames\":" << frames_completed
            << ",\"completed_warmup\":" << warmup_completed
            << ",\"shadow_traversal_frames\":{\"opaque_auto\":"
            << (shadow_traversal &&
                        shadow_traversal->effective == rypath::ShadowTraversal::opaque_auto
                    ? frames_completed
                    : 0)
            << ",\"query\":"
            << (shadow_traversal && shadow_traversal->effective == rypath::ShadowTraversal::query
                    ? frames_completed
                    : 0)
            << ",\"excludes_warmup\":true}"
            << ",\"consumed_sample_indices\":" << observations.size()
            << ",\"acceleration_build\":{\"requested\":"
            << quote(rypath::acceleration_build_policy_name(acceleration_build.requested))
            << ",\"effective\":"
            << (acceleration_build.effective
                    ? quote(rypath::acceleration_build_policy_name(*acceleration_build.effective))
                    : "null")
            << ",\"supported\":"
            << (acceleration_build.supported ? (*acceleration_build.supported ? "true" : "false")
                                             : "null")
            << ",\"diagnostic\":" << quote(acceleration_build.diagnostic)
            << ",\"build_cpu_wall_seconds\":" << acceleration_build.build_cpu_wall_seconds
            << ",\"blas_bytes\":" << acceleration_build.blas_bytes
            << ",\"tlas_bytes\":" << acceleration_build.tlas_bytes << ",\"retained_as_bytes\":"
            << acceleration_build.blas_bytes + acceleration_build.tlas_bytes
            << ",\"blas_count\":" << acceleration_build.blas_count
            << ",\"tlas_count\":" << acceleration_build.tlas_count
            << ",\"fast_blas_count\":" << acceleration_build.fast_blas_count
            << ",\"fast_tlas_count\":" << acceleration_build.fast_tlas_count
            << ",\"device_allocated_bytes\":"
            << (acceleration_build.device_allocated_bytes
                    ? std::to_string(*acceleration_build.device_allocated_bytes)
                    : "null")
            << "}"
            << ",\"startup_seconds\":" << startup_seconds
            << ",\"warmup_seconds\":" << warmup_seconds
            << ",\"reconstruction_startup_seconds\":" << reconstruction_startup_seconds
            << ",\"reconstruction_startup_in_first_frame_wall\":true"
            << ",\"frame_completion_fields\":\"complete is raw; pipeline_complete includes "
               "reconstruction\""
            << ",\"pipeline_GPU_scope\":\"raw plus reconstruction commands; CPU gaps excluded; "
               "creation frame omitted because startup GPU probe is not separately timed\""
            << ",\"capture_in_frame_timing\":false,\"GPU_time_source\":\"command-buffer-"
               "timestamps\","
               "\"overrides\":[\"authored resolution -> CLI resolution\","
               "\"authored sampler -> independent\",\"authored sample budget -> fixed 1 spp "
               "footprint\","
               "\"authored seed/material seed -> CLI seeds\",\"BDPT -> path; volpath preserved\","
               "\"finite authored film clamp -> infinity\",\"authored film outputs -> optional CLI "
               "capture\"],"
               "\"provenance\":[";
        for (std::size_t i = 0; i < provenance.size(); ++i) {
            if (i)
                out << ',';
            out << quote(provenance[i]);
        }
        out << "],\"dependencies\":[";
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            if (i)
                out << ',';
            out << quote(dependencies[i].string());
        }
        out << "],\"frames\":[";
        const auto statistics = rypath::summarize_frames(observations);
        std::vector<rypath::FrameStatisticsSample> pipeline_samples;
        double pipeline_wall = 0;
        std::uint64_t pipeline_count = 0;
        for (const auto &sample : observations) {
            pipeline_samples.push_back(sample.pipeline);
            if (!sample.warmup) {
                pipeline_wall += sample.pipeline.cpu_seconds;
                pipeline_count += sample.pipeline.complete;
            }
        }
        const auto pipeline_statistics = rypath::summarize_frames(pipeline_samples);
        for (std::size_t i = 0; i < observations.size(); ++i) {
            const auto &f = observations[i];
            if (i)
                out << ',';
            out << "{\"sample_index\":" << f.sample_index << ",\"epoch\":" << f.pose.epoch
                << ",\"phase\":" << quote(f.pose.phase) << ",\"motion_offset\":" << f.pose.offset
                << ",\"camera\":";
            write_camera(out, f.camera);
            out << ",\"warmup\":" << (f.warmup ? "true" : "false")
                << ",\"complete\":" << (f.complete ? "true" : "false")
                << ",\"completed_samples\":" << f.completed_samples
                << ",\"camera_upload_bytes\":" << f.camera_upload_bytes
                << ",\"command_count\":" << f.command_count << ",\"area_serial\":" << f.area_serial
                << ",\"history_used\":" << (f.history_used ? "true" : "false")
                << ",\"history_generation\":" << f.history_generation
                << ",\"temporal_candidates\":" << f.temporal_candidates
                << ",\"spatial_candidates\":" << f.spatial_candidates
                << ",\"mutation_attempts\":" << f.mutation_attempts
                << ",\"mutation_accepts\":" << f.mutation_accepts
                << ",\"completed_evaluations\":" << f.completed_evaluations
                << ",\"fresh_candidates\":" << f.fresh_candidates << ",\"gpu_stages\":";
            write_stages(out, f.gpu_stages);
            out << ",\"cpu_wall_seconds\":" << f.cpu_seconds << ",\"gpu_seconds\":";
            if (f.gpu_seconds)
                out << *f.gpu_seconds;
            else
                out << "null";
            out << ",\"pipeline_complete\":" << (f.pipeline.complete ? "true" : "false")
                << ",\"pipeline_cpu_wall_seconds\":" << f.pipeline.cpu_seconds
                << ",\"pipeline_gpu_seconds\":";
            if (f.pipeline.gpu_seconds)
                out << *f.pipeline.gpu_seconds;
            else
                out << "null";
            out << ",\"reconstruction\":";
            if (!options.reconstruction)
                out << "null";
            else {
                out << "{\"mode\":" << quote(reconstruction_name(options.reconstruction))
                    << ",\"attempted\":" << (f.reconstruction_started ? "true" : "false")
                    << ",\"complete\":"
                    << (f.reconstruction &&
                                f.reconstruction->status == rypath::ReconstructionStatus::complete
                            ? "true"
                            : "false")
                    << ",\"cpu_wall_seconds\":" << f.reconstruction_wall_seconds
                    << ",\"gpu_stages\":";
                if (f.reconstruction)
                    write_stages(out, f.reconstruction->gpu_stages);
                else
                    out << "null";
                out << ",\"gpu_seconds\":";
                if (f.reconstruction && f.reconstruction->gpu_time_valid)
                    out << f.reconstruction->gpu_seconds;
                else
                    out << "null";
                out << ",\"command_count\":"
                    << (f.reconstruction ? f.reconstruction->command_count : 0)
                    << ",\"history_reset\":"
                    << (f.reconstruction ? (f.reconstruction->history_reset ? "true" : "false")
                                         : "null")
                    << ",\"defocus_approximation\":"
                    << (f.reconstruction && f.reconstruction->defocus_approximation ? "true"
                                                                                    : "false")
                    << ",\"guide_cache_hit\":"
                    << (f.reconstruction && f.reconstruction->guide_cache_hit ? "true" : "false")
                    << ",\"generated_surface_pixels\":"
                    << (f.reconstruction ? f.reconstruction->generated_surface_pixels : 0)
                    << ",\"width\":" << options.output_width
                    << ",\"height\":" << options.output_height << ",\"history_accepted\":"
                    << (f.reconstruction ? f.reconstruction->history_accepted : 0)
                    << ",\"history_rejected\":"
                    << (f.reconstruction ? f.reconstruction->history_rejected : 0)
                    << ",\"counters\":";
                write_counters(out, f.reconstruction ? f.reconstruction->counters
                                                     : rypath::ReconstructionCounters{});
                out << ",\"diagnostic\":"
                    << quote(f.reconstruction ? f.reconstruction->diagnostic : f.diagnostic) << '}';
            }
            out << ",\"diagnostic\":" << quote(f.diagnostic) << '}';
        }
        out << "],\"summary\":{\"percentile_method\":\"nearest-rank\",\"excludes_warmup_and_failed_"
               "frames\":true,";
        const auto summary = [&](std::string_view name, const rypath::FrameMetricSummary &metric) {
            out << quote(name) << ":{\"count\":" << metric.count << ",\"median_seconds\":";
            if (metric.median_seconds)
                out << *metric.median_seconds;
            else
                out << "null";
            out << ",\"p95_seconds\":";
            if (metric.p95_seconds)
                out << *metric.p95_seconds;
            else
                out << "null";
            out << '}';
        };
        summary("cpu_wall", statistics.cpu);
        out << ',';
        summary("GPU", statistics.gpu);
        out << ",\"missing_GPU_timestamps\":" << statistics.missing_gpu_timestamps
            << "},\"pipeline_summary\":{"
               "\"percentile_method\":\"nearest-rank\",\"excludes_warmup_and_failed_frames\":true,";
        summary("cpu_wall", pipeline_statistics.cpu);
        out << ',';
        summary("GPU", pipeline_statistics.gpu);
        out << ",\"missing_GPU_timestamps\":" << pipeline_statistics.missing_gpu_timestamps
            << ",\"completed_frames\":" << pipeline_count
            << ",\"measured_wall_seconds\":" << pipeline_wall << ",\"frames_per_second\":";
        if (pipeline_wall > 0)
            out << double(pipeline_count) / pipeline_wall;
        else
            out << "null";
        out << "}}\n";
        return out.str();
    }
};
int run(const Options &options) {
    const auto start = Clock::now();
    Report report;
    report.options = options;
    report.acceleration_build.requested = options.as_build;
    report.provenance.push_back(std::string("reconstruction mode ") +
                                reconstruction_name(options.reconstruction));
    report.provenance.push_back("transport resolution " + std::to_string(options.width) + "x" +
                                std::to_string(options.height) + "; reconstruction output " +
                                std::to_string(options.output_width) + "x" +
                                std::to_string(options.output_height));
    if (options.reconstruction)
        report.provenance.push_back("reconstructed output is separate unexposed linear RGB "
                                    "presentation data; raw XYZ is retained");
    struct utsname system {};
    if (::uname(&system) == 0)
        report.os = std::string(system.sysname) + " " + system.release + " " + system.machine;
    Output output, capture, preview, reconstructed_capture;
    bool reserved = false;
    int code = 1;
    try {
        std::optional<rypath::pbrt::Result> checked;
        if (!options.demo) {
            checked = rypath::pbrt::check(options.scene);
            report.dependencies.push_back(options.scene);
            for (const auto &dependency : checked->document.dependencies)
                report.dependencies.push_back(dependency.resolved_path);
        }
        validate_paths(options, report.dependencies);
        output.reserve(options.output);
        reserved = true;
        // Retain useful machine-readable state even if interrupted during startup.
        // The final report replaces only our own reserved inode.
        if (!options.capture.empty())
            capture.reserve(options.capture);
        if (!options.preview.empty())
            preview.reserve(options.preview);
        if (!options.reconstructed_capture.empty())
            reconstructed_capture.reserve(options.reconstructed_capture);
        rypath::RenderScene scene;
        if (checked) {
            if (checked->exit_code() != 0) {
                std::cerr << rypath::pbrt::report(*checked);
                throw std::runtime_error("PBRT preflight failed; rendering not attempted");
            }
            bool authored_packet_policy = false;
            for (const auto &descriptor : checked->scene->options) {
                if (descriptor.authored.name == "Integrator" &&
                    descriptor.authored.arguments.front().text == "bdpt")
                    authored_packet_policy = true;
                if (descriptor.authored.name == "Film") {
                    const auto *clamp =
                        descriptor.effective_parameter("maxcomponentvalue").parameter;
                    if (clamp && !clamp->values.empty() &&
                        std::isfinite(clamp->values.front().number))
                        authored_packet_policy = true;
                }
            }
            const auto benchmark_overrides = overrides(*checked, options);
            report.provenance.insert(report.provenance.end(), benchmark_overrides.begin(),
                                     benchmark_overrides.end());
            auto translated = rypath::pbrt::translate(*checked, {options.scene.string()});
            for (const auto &diagnostic : translated.diagnostics)
                std::cerr << rypath::pbrt::format_diagnostic(diagnostic);
            report.provenance.insert(report.provenance.end(), translated.provenance.begin(),
                                     translated.provenance.end());
            if (translated.exit_code() || !translated.scene)
                throw std::runtime_error("PBRT translation failed; rendering not attempted");
            scene = std::move(*translated.scene);
            if (authored_packet_policy) {
                const auto active_medium = [&](std::uint32_t id) {
                    if (id == rypath::no_medium)
                        return false;
                    const auto &medium = scene.media.at(id);
                    const auto nonzero = [](float value) { return value != 0; };
                    return std::ranges::any_of(medium.sigma_a, nonzero) ||
                           std::ranges::any_of(medium.sigma_s, nonzero) ||
                           std::ranges::any_of(medium.emission, nonzero);
                };
                bool active = active_medium(scene.camera_medium);
                for (const auto &medium : scene.instance_media)
                    active =
                        active || active_medium(medium.inside) || active_medium(medium.outside);
                for (const auto &light : checked->scene->lights)
                    for (std::size_t i = 0; i < checked->scene->media.size(); ++i)
                        if (checked->scene->media[i].authored.arguments.front().text ==
                            light.outside_medium)
                            active = active || active_medium(static_cast<std::uint32_t>(i));
                if (!active && scene.max_depth <= 64) {
                    scene.spectral_transport = rypath::SpectralTransport::packet;
                    scene.camera_medium = rypath::no_medium;
                    scene.instance_media.clear();
                    report.provenance.push_back("authored BDPT/finite clamp packet wavelength "
                                                "policy preserved for vacuum live PT");
                } else
                    report.provenance.push_back(
                        active ? "authored packet wavelength policy overridden: active media "
                                 "require scalar transport"
                               : "authored packet wavelength policy overridden: maxdepth above 64 "
                                 "requires scalar transport");
            }
        } else
            scene = rypath::make_demo_scene();
        scene.integrator =
            scene.volume_transport ? rypath::IntegratorKind::volpath : rypath::IntegratorKind::path;
        scene.max_component_value = std::numeric_limits<double>::infinity();
        if (options.motion_offset) {
            if (!rypath::frame_benchmark::motion_medium_supported(scene))
                throw std::runtime_error("Motion cannot classify distinct medium interfaces");
            (void)rypath::frame_benchmark::translated_camera(scene, *options.motion_offset);
            const auto quarter = options.frames / 4;
            (void)rypath::frame_benchmark::translated_camera(
                scene, float(double(*options.motion_offset) / double(quarter)));
        }
        report.integrator = scene.volume_transport ? "volpath" : "path";
        report.max_depth = scene.max_depth;
        report.provenance.push_back("effective exposure " + std::to_string(scene.exposure));
        report.provenance.push_back(
            "effective spectral transport " +
            std::string(scene.spectral_transport == rypath::SpectralTransport::packet ? "packet"
                                                                                      : "scalar"));
        report.provenance.push_back("effective light sampler " +
                                    std::to_string(static_cast<int>(scene.light_sampler)));
        report.provenance.push_back("effective pixel filter " +
                                    std::to_string(static_cast<int>(scene.filter.kind)) +
                                    " radius " + std::to_string(scene.filter.x_radius) + " " +
                                    std::to_string(scene.filter.y_radius));
        rypath::RenderSettings identity_settings;
        identity_settings.width = options.width;
        identity_settings.height = options.height;
        identity_settings.seed = options.seed;
        identity_settings.material_seed = options.material_seed;
        identity_settings.samples = 1;
        identity_settings.sampler.kind = rypath::SamplerKind::independent;
        report.source_identity =
            rypath::digest_hex(rypath::render_identity(scene, identity_settings));
        rypath::FrameConfig config;
        config.width = options.width;
        config.height = options.height;
        config.seed = options.seed;
        config.material_seed = options.material_seed;
        config.footprint_samples = 1;
        config.profile_stages = options.profile_stages;
        config.intersection_profile = options.intersection_profile;
        config.shadow_traversal = options.shadow_traversal;
        config.acceleration_build_policy = options.as_build;
        config.execution = options.execution;
        config.estimator = options.estimator;
        config.target_scale = options.target_scale;
        config.mutation_steps = options.mutation_steps;
        config.capacity = options.capacity;
        config.quantum = options.quantum;
        config.group_width = options.group_width;
        config.packet_events = options.packet_events;
        config.packet_layout = options.packet_layout;
        auto built = rypath::build_frame_session(scene, config);
        report.packet_controls_effective =
            built.status == rypath::GeometryStatus::success && built.session &&
            scene.spectral_transport == rypath::SpectralTransport::packet &&
            options.execution == rypath::FrameExecution::wavefront;
        report.acceleration_build = built.acceleration_build;
        report.startup_seconds = seconds(start);
        report.device = built.device.name;
        report.architecture = built.device.architecture;
        report.registry_id = built.device.registry_id;
        if (built.status != rypath::GeometryStatus::success || !built.session) {
            code = built.status == rypath::GeometryStatus::not_built ||
                           built.status == rypath::GeometryStatus::no_device ||
                           built.status == rypath::GeometryStatus::no_ray_tracing
                       ? 2
                       : 1;
            report.diagnostic =
                built.diagnostic.empty() ? "frame session unavailable" : built.diagnostic;
        } else {
            report.shadow_traversal = built.shadow_traversal;
            const auto warmup_start = Clock::now();
            std::unique_ptr<rypath::Frame> last;
            std::unique_ptr<rypath::ReconstructionStream> reconstruction;
            std::unique_ptr<rypath::ReconstructedFrame> last_reconstructed;
            for (std::uint64_t sample = 0; sample < options.warmup + options.frames; ++sample) {
                const bool warmup = sample < options.warmup;
                if (sample == options.warmup)
                    report.warmup_seconds = seconds(warmup_start);
                const auto frame_start = Clock::now();
                std::unique_ptr<rypath::GPUCapture> gpu_capture;
                const bool capture_this_frame =
                    options.capture_frame && *options.capture_frame == sample;
                if (capture_this_frame) {
                    gpu_capture = std::make_unique<rypath::GPUCapture>();
                    const auto error = gpu_capture->start(options.capture_path);
                    if (!error.empty())
                        throw std::runtime_error(error);
                }
                // Retain one previous success for capture; the other session slot remains free.
                Observation observation;
                observation.pose = rypath::frame_benchmark::motion_pose(
                    sample, options.warmup, options.frames, options.motion_offset);
                if (options.motion_offset)
                    observation.camera =
                        rypath::frame_benchmark::translated_camera(scene, observation.pose.offset);
                auto submitted =
                    built.session->submit({sample, observation.pose.epoch, observation.camera});
                observation.sample_index = sample;
                observation.warmup = warmup;
                observation.pipeline.warmup = warmup;
                if (submitted.status != rypath::GeometryStatus::success || !submitted.frame) {
                    observation.cpu_seconds = seconds(frame_start);
                    observation.diagnostic = submitted.diagnostic;
                } else {
                    const auto info = submitted.frame->wait();
                    observation.cpu_seconds = seconds(frame_start);
                    observation.complete =
                        info.status == rypath::FrameStatus::complete &&
                        info.completed_samples == std::uint64_t(options.width) * options.height;
                    observation.completed_samples = info.completed_samples;
                    observation.gpu_stages = info.gpu_stages;
                    observation.camera_upload_bytes = info.camera_upload_bytes;
                    observation.command_count = info.command_count;
                    observation.area_serial = info.area_serial;
                    observation.history_used = info.history_used;
                    observation.history_generation = info.history_generation;
                    observation.temporal_candidates = info.temporal_candidates;
                    observation.spatial_candidates = info.spatial_candidates;
                    observation.completed_evaluations = info.completed_evaluations;
                    observation.mutation_attempts = info.mutation_attempts;
                    observation.mutation_accepts = info.mutation_accepts;
                    observation.fresh_candidates = info.fresh_candidates;
                    observation.diagnostic = info.diagnostic;
                    if (info.gpu_time_valid && std::isfinite(info.gpu_seconds) &&
                        info.gpu_seconds >= 0)
                        observation.gpu_seconds = info.gpu_seconds;
                    if (observation.complete && options.reconstruction) {
                        const auto reconstruction_start = Clock::now();
                        observation.reconstruction_started = true;
                        bool created = false;
                        try {
                            if (!reconstruction) {
                                created = true;
                                const auto creation_start = Clock::now();
                                auto stream = rypath::build_reconstruction_stream(
                                    *submitted.frame,
                                    {.profile_stages = options.profile_stages,
                                     .mode = *options.reconstruction,
                                     .output_width = options.output_width,
                                     .output_height = options.output_height,
                                     .cache_surface_guides = options.guide_cache});
                                report.reconstruction_startup_seconds = seconds(creation_start);
                                if (stream.status != rypath::GeometryStatus::success ||
                                    !stream.stream)
                                    observation.diagnostic =
                                        stream.diagnostic.empty()
                                            ? "reconstruction stream unavailable"
                                            : stream.diagnostic;
                                else
                                    reconstruction = std::move(stream.stream);
                            }
                            if (reconstruction) {
                                auto reconstructed = reconstruction->submit(*submitted.frame);
                                if (reconstructed.status != rypath::GeometryStatus::success ||
                                    !reconstructed.frame)
                                    observation.diagnostic =
                                        reconstructed.diagnostic.empty()
                                            ? "reconstruction submission failed"
                                            : reconstructed.diagnostic;
                                else {
                                    observation.reconstruction = reconstructed.frame->wait();
                                    if (observation.reconstruction->status !=
                                        rypath::ReconstructionStatus::complete)
                                        observation.diagnostic =
                                            observation.reconstruction->diagnostic.empty()
                                                ? "reconstruction failed"
                                                : observation.reconstruction->diagnostic;
                                    else
                                        last_reconstructed = std::move(reconstructed.frame);
                                }
                            }
                        } catch (const std::exception &error) {
                            observation.diagnostic = error.what();
                        }
                        observation.reconstruction_wall_seconds = seconds(reconstruction_start);
                        if (created && report.reconstruction_startup_seconds == 0)
                            report.reconstruction_startup_seconds =
                                observation.reconstruction_wall_seconds;
                        observation.pipeline.complete = observation.reconstruction &&
                                                        observation.reconstruction->status ==
                                                            rypath::ReconstructionStatus::complete;
                        if (observation.pipeline.complete && !created && observation.gpu_seconds &&
                            observation.reconstruction->gpu_time_valid &&
                            std::isfinite(observation.reconstruction->gpu_seconds) &&
                            observation.reconstruction->gpu_seconds >= 0) {
                            const auto sum =
                                *observation.gpu_seconds + observation.reconstruction->gpu_seconds;
                            if (std::isfinite(sum))
                                observation.pipeline.gpu_seconds = sum;
                        }
                    }
                    if (observation.complete)
                        last = std::move(submitted.frame);
                }
                if (!options.reconstruction) {
                    observation.pipeline.complete = observation.complete;
                    observation.pipeline.gpu_seconds = observation.gpu_seconds;
                    observation.pipeline.cpu_seconds = observation.cpu_seconds;
                } else
                    observation.pipeline.cpu_seconds = seconds(frame_start);
                if (gpu_capture)
                    gpu_capture->stop();
                report.observations.push_back(observation);
                if (!observation.pipeline.complete) {
                    report.diagnostic = observation.diagnostic.empty()
                                            ? "frame incomplete or sample count mismatch"
                                            : observation.diagnostic;
                    break;
                }
                if (warmup)
                    ++report.warmup_completed;
                else
                    ++report.frames_completed;
            }
            if (report.warmup_completed < options.warmup)
                report.warmup_seconds = seconds(warmup_start);
            code = report.frames_completed == options.frames &&
                           report.warmup_completed == options.warmup
                       ? 0
                       : 1;
            if (last && (!options.capture.empty() || !options.preview.empty())) {
                const auto image = last->capture();
                if (image.status != rypath::GeometryStatus::success ||
                    image.raw_xyz.size() != std::uint64_t(options.width) * options.height)
                    throw std::runtime_error("frame capture failed: " + image.diagnostic);
                auto film = rypath::make_film(options.width, options.height, scene.model, 1);
                for (std::size_t i = 0; i < image.raw_xyz.size(); ++i) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        if (!std::isfinite(image.raw_xyz[i][c]))
                            throw std::runtime_error("nonfinite captured XYZ");
                        film.pixels[i].sum[c] = image.raw_xyz[i][c];
                    }
                    film.pixels[i].count = 1;
                }
                const auto frame_info = last->poll();
                const auto captured_pose = rypath::frame_benchmark::motion_pose(
                    frame_info.sample_index, options.warmup, options.frames, options.motion_offset);
                const std::vector<std::pair<std::string, std::string>> metadata{
                    {"rypath.frame.sample_index", std::to_string(frame_info.sample_index)},
                    {"rypath.frame.epoch", std::to_string(frame_info.epoch)},
                    {"rypath.frame.motion_phase", std::string(captured_pose.phase)},
                    {"rypath.frame.motion_offset", precise_float(captured_pose.offset)},
                    {"rypath.frame.seed", std::to_string(options.seed)},
                    {"rypath.frame.material_seed", std::to_string(options.material_seed)},
                    {"rypath.frame.footprint_samples", "1"},
                    {"rypath.frame.execution", execution_name(options.execution)},
                    {"rypath.frame.estimator", estimator_name(options.estimator)},
                    {"rypath.area.contract",
                     std::to_string(options.estimator == rypath::FrameEstimator::area_replay
                                        ? rypath::area_wire::contract
                                        : 0)},
                    {"rypath.area.target_scale", precise_float(options.target_scale)},
                    {"rypath.area.mutation_steps",
                     std::to_string(options.estimator == rypath::FrameEstimator::area_replay
                                        ? options.mutation_steps
                                        : 0)},
                    {"rypath.area.mutation_attempts", std::to_string(frame_info.mutation_attempts)},
                    {"rypath.area.mutation_accepts", std::to_string(frame_info.mutation_accepts)},
                    {"rypath.area.target", "bounded-l1-contrast-0.1-v1"},
                    {"rypath.area.serial", std::to_string(frame_info.area_serial)},
                    {"rypath.area.history_used", frame_info.history_used ? "true" : "false"},
                    {"rypath.area.generation", std::to_string(frame_info.history_generation)},
                    {"rypath.area.temporal_count", std::to_string(frame_info.temporal_candidates)},
                    {"rypath.area.spatial_count", std::to_string(frame_info.spatial_candidates)},
                    {"rypath.frame.evaluations", std::to_string(frame_info.completed_evaluations)},
                    {"rypath.frame.fresh_candidates", std::to_string(frame_info.fresh_candidates)},
                    {"rypath.frame.capacity", std::to_string(options.capacity)},
                    {"rypath.frame.effective_capacity",
                     options.execution == rypath::FrameExecution::wavefront
                         ? std::to_string(std::min<std::uint64_t>(
                               options.capacity, std::uint64_t(options.width) * options.height))
                         : "not-applicable"},
                    {"rypath.frame.quantum", std::to_string(options.quantum)},
                    {"rypath.frame.group_width", std::to_string(options.group_width)},
                    {"rypath.frame.packet_events", std::to_string(options.packet_events)},
                    {"rypath.frame.packet_layout", packet_layout_name(options.packet_layout)},
                    {"rypath.frame.command_count", std::to_string(frame_info.command_count)},
                    {"rypath.frame.integrator", report.integrator},
                    {"rypath.frame.intersections", profile_name(options.intersection_profile)},
                    {"rypath.frame.as_build",
                     std::string(rypath::acceleration_build_policy_name(options.as_build))},
                    {"rypath.frame.spectral_mode",
                     scene.spectral_transport == rypath::SpectralTransport::packet ? "packet"
                                                                                   : "scalar"},
                    {"rypath.frame.source_identity", report.source_identity},
                    {"rypath.frame.source", options.demo ? "demo" : options.scene.string()},
                    {"rypath.frame.build", RYPATH_BUILD_DESCRIPTION},
                    {"rypath.frame.output", "raw-unexposed-unclamped"}};
                if (!options.capture.empty())
                    capture.image(film, true, metadata);
                if (!options.preview.empty()) {
                    film.exposure = scene.exposure;
                    preview.image(film, false);
                }
            }
            if (last_reconstructed && !options.reconstructed_capture.empty()) {
                const auto info = last_reconstructed->poll();
                const auto captured_pose =
                    rypath::frame_benchmark::motion_pose(info.source.sample_index, options.warmup,
                                                         options.frames, options.motion_offset);
                const auto image = last_reconstructed->capture();
                if (image.status != rypath::GeometryStatus::success)
                    throw std::runtime_error("reconstructed capture failed: " + image.diagnostic);
                const std::vector<std::pair<std::string, std::string>> metadata{
                    {"rypath.output.kind",
                     *options.reconstruction == rypath::ReconstructionMode::metalfx
                         ? "metalfx-denoised-linear-rgb"
                     : *options.reconstruction == rypath::ReconstructionMode::temporal
                         ? "temporal-denoised-linear-rgb"
                         : "raw-rgb-color-conversion"},
                    {"rypath.recon.mode", reconstruction_name(options.reconstruction)},
                    {"rypath.output.space", "linear-srgb"},
                    {"rypath.recon.exposure", "1"},
                    {"rypath.recon.reset", info.history_reset ? "true" : "false"},
                    {"rypath.recon.defocus", info.defocus_approximation ? "true" : "false"},
                    {"rypath.frame.sample", std::to_string(info.source.sample_index)},
                    {"rypath.frame.epoch", std::to_string(info.source.epoch)},
                    {"rypath.frame.motion_phase", std::string(captured_pose.phase)},
                    {"rypath.frame.motion_offset", precise_float(captured_pose.offset)},
                    {"rypath.frame.seed", std::to_string(options.seed)},
                    {"rypath.frame.material_seed", std::to_string(options.material_seed)},
                    {"rypath.frame.estimator", estimator_name(options.estimator)},
                    {"rypath.area.mutations",
                     std::to_string(options.estimator == rypath::FrameEstimator::area_replay
                                        ? options.mutation_steps
                                        : 0)},
                    {"rypath.area.serial", std::to_string(info.source.area_serial)},
                    {"rypath.internal.width", std::to_string(options.width)},
                    {"rypath.internal.height", std::to_string(options.height)},
                    {"rypath.output.width", std::to_string(image.width)},
                    {"rypath.output.height", std::to_string(image.height)},
                    {"rypath.frame.source_identity", report.source_identity},
                    {"rypath.frame.source", options.demo ? "demo" : options.scene.string()},
                    {"rypath.frame.build", RYPATH_BUILD_DESCRIPTION},
                    {"rypath.frame.intersections", profile_name(options.intersection_profile)},
                    {"rypath.frame.as_build",
                     std::string(rypath::acceleration_build_policy_name(options.as_build))},
                    {"rypath.recon.color_negative", std::to_string(info.counters.color_negative)},
                    {"rypath.recon.color_high", std::to_string(info.counters.color_high)}};
                reconstructed_capture.linear_rgb(image, metadata);
            }
        }
    } catch (const std::exception &error) {
        code = 1;
        report.diagnostic = error.what();
        if (report.startup_seconds == 0)
            report.startup_seconds = seconds(start);
    }
    if (!report.diagnostic.empty())
        std::cerr << "frame benchmark: " << report.diagnostic << '\n';
    if (reserved) {
        try {
            output.publish(report.json(code));
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    return code;
}
} // namespace
int main(int argc, char **argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage;
        return 0;
    }
    Options options;
    if (const auto error = parse(argc, argv, options); !error.empty()) {
        std::cerr << error << '\n' << usage;
        return 1;
    }
    return run(options);
} catch (const std::exception &error) {
    std::cerr << "frame benchmark: " << error.what() << '\n';
    return 1;
}
