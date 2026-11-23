// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"
#include "frame_statistics.hpp"
#include "image_output.hpp"
#include "pbrt_render.hpp"
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
    fs::path scene, output, capture, preview;
    bool demo = false;
    rypath::FrameExecution execution = rypath::FrameConfig{}.execution;
    std::uint32_t capacity = rypath::FrameConfig{}.capacity;
    std::uint32_t quantum = rypath::FrameConfig{}.quantum;
    std::uint32_t group_width = rypath::FrameConfig{}.group_width;
    std::uint32_t width = 1920, height = 1080;
    std::uint64_t frames = 120, warmup = 8, seed = 1;
    std::int32_t material_seed = 0;
    rypath::IntersectionProfile intersection_profile = rypath::IntersectionProfile::native;
};
const char *profile_name(rypath::IntersectionProfile profile) {
    return profile == rypath::IntersectionProfile::native ? "native" : "compensated";
}
const char *execution_name(rypath::FrameExecution execution) {
    return execution == rypath::FrameExecution::wavefront ? "wavefront" : "megakernel";
}
const char *usage =
    "Usage: rypath-frame-benchmark (--scene SCENE.pbrt | --demo) --output REPORT.json\n"
    "  [--width 1920] [--height 1080] [--frames 120] [--warmup 8]\n"
    "  [--seed 1] [--material-seed 0] [--intersections native|compensated (default: native)]\n"
    "  [--execution wavefront|megakernel (default: wavefront)]\n"
    "  [--capacity 65536] [--quantum 8] [--group-width 32]\n"
    "  [--capture FLOAT.exr] [--preview IMAGE.png]\n"
    "Fresh output paths required. Serial resident-scene, one new sample/pixel/frame baseline.\n";
std::string parse(int argc, char **argv, Options &o) {
    std::set<std::string_view> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (!seen.insert(key).second)
            return "duplicate option: " + std::string(key);
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
        else if (key == "--capture")
            o.capture = value;
        else if (key == "--preview")
            o.preview = value;
        else if (key == "--execution") {
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
        } else if (key == "--group-width") {
            if (!integer(value, o.group_width) || !o.group_width || o.group_width > 256)
                return "group width must be in [1,256]";
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
};
void validate_paths(const Options &o, const std::vector<fs::path> &inputs) {
    std::set<fs::path> outputs;
    for (const auto &path : {o.output, o.capture, o.preview}) {
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
    std::uint64_t sample_index = 0, completed_samples = 0, camera_upload_bytes = 0,
                  command_count = 0;
    std::string diagnostic;
};
struct Report {
    Options options;
    std::string diagnostic, device, architecture, integrator, os, source_identity;
    std::uint64_t registry_id = 0, warmup_completed = 0, frames_completed = 0;
    double startup_seconds = 0, warmup_seconds = 0;
    std::uint32_t max_depth = 0;
    std::vector<std::string> provenance;
    std::vector<fs::path> dependencies;
    std::vector<Observation> observations;
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
            << ",\"device\":{\"name\":" << quote(device)
            << ",\"architecture\":" << quote(architecture) << ",\"registry_id\":" << registry_id
            << "},\"settings\":{\"width\":" << options.width << ",\"height\":" << options.height
            << ",\"seed\":" << options.seed << ",\"material_seed\":" << options.material_seed
            << ",\"execution\":" << quote(execution_name(options.execution))
            << ",\"capacity\":" << options.capacity << ",\"effective_capacity\":"
            << (options.execution == rypath::FrameExecution::wavefront
                    ? std::to_string(std::min<std::uint64_t>(
                          options.capacity, std::uint64_t(options.width) * options.height))
                    : "null")
            << ",\"quantum\":" << options.quantum << ",\"group_width\":" << options.group_width
            << ",\"intersection_profile\":" << quote(profile_name(options.intersection_profile))
            << ",\"integrator\":" << quote(integrator) << ",\"max_depth\":" << max_depth
            << ",\"sampler\":\"independent\",\"samples_per_pixel_per_frame\":1,\"footprint_"
               "samples\":1,"
               "\"raw_unclamped\":true,\"submission\":\"serial-submit-wait\"},\"requested_frames\":"
            << options.frames << ",\"requested_warmup\":" << options.warmup
            << ",\"completed_frames\":" << frames_completed
            << ",\"completed_warmup\":" << warmup_completed
            << ",\"consumed_sample_indices\":" << observations.size()
            << ",\"startup_seconds\":" << startup_seconds
            << ",\"warmup_seconds\":" << warmup_seconds
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
        for (std::size_t i = 0; i < observations.size(); ++i) {
            const auto &f = observations[i];
            if (i)
                out << ',';
            out << "{\"sample_index\":" << f.sample_index
                << ",\"warmup\":" << (f.warmup ? "true" : "false")
                << ",\"complete\":" << (f.complete ? "true" : "false")
                << ",\"completed_samples\":" << f.completed_samples
                << ",\"camera_upload_bytes\":" << f.camera_upload_bytes
                << ",\"command_count\":" << f.command_count
                << ",\"cpu_wall_seconds\":" << f.cpu_seconds << ",\"gpu_seconds\":";
            if (f.gpu_seconds)
                out << *f.gpu_seconds;
            else
                out << "null";
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
        out << ",\"missing_GPU_timestamps\":" << statistics.missing_gpu_timestamps << "}}\n";
        return out.str();
    }
};
int run(const Options &options) {
    const auto start = Clock::now();
    Report report;
    report.options = options;
    struct utsname system {};
    if (::uname(&system) == 0)
        report.os = std::string(system.sysname) + " " + system.release + " " + system.machine;
    Output output, capture, preview;
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
            report.provenance = overrides(*checked, options);
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
        config.intersection_profile = options.intersection_profile;
        config.execution = options.execution;
        config.capacity = options.capacity;
        config.quantum = options.quantum;
        config.group_width = options.group_width;
        auto built = rypath::build_frame_session(scene, config);
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
            const auto warmup_start = Clock::now();
            std::unique_ptr<rypath::Frame> last;
            for (std::uint64_t sample = 0; sample < options.warmup + options.frames; ++sample) {
                const bool warmup = sample < options.warmup;
                if (sample == options.warmup)
                    report.warmup_seconds = seconds(warmup_start);
                const auto frame_start = Clock::now();
                // Retain one previous success for capture; the other session slot remains free.
                auto submitted = built.session->submit({sample, 0, {}});
                Observation observation;
                observation.sample_index = sample;
                observation.warmup = warmup;
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
                    observation.camera_upload_bytes = info.camera_upload_bytes;
                    observation.command_count = info.command_count;
                    observation.diagnostic = info.diagnostic;
                    if (info.gpu_time_valid && std::isfinite(info.gpu_seconds) &&
                        info.gpu_seconds >= 0)
                        observation.gpu_seconds = info.gpu_seconds;
                    if (observation.complete)
                        last = std::move(submitted.frame);
                }
                report.observations.push_back(observation);
                if (!observation.complete) {
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
                const std::vector<std::pair<std::string, std::string>> metadata{
                    {"rypath.frame.sample_index", std::to_string(frame_info.sample_index)},
                    {"rypath.frame.epoch", std::to_string(frame_info.epoch)},
                    {"rypath.frame.seed", std::to_string(options.seed)},
                    {"rypath.frame.material_seed", std::to_string(options.material_seed)},
                    {"rypath.frame.footprint_samples", "1"},
                    {"rypath.frame.execution", execution_name(options.execution)},
                    {"rypath.frame.capacity", std::to_string(options.capacity)},
                    {"rypath.frame.effective_capacity",
                     options.execution == rypath::FrameExecution::wavefront
                         ? std::to_string(std::min<std::uint64_t>(
                               options.capacity, std::uint64_t(options.width) * options.height))
                         : "not-applicable"},
                    {"rypath.frame.quantum", std::to_string(options.quantum)},
                    {"rypath.frame.group_width", std::to_string(options.group_width)},
                    {"rypath.frame.command_count", std::to_string(frame_info.command_count)},
                    {"rypath.frame.integrator", report.integrator},
                    {"rypath.frame.intersections", profile_name(options.intersection_profile)},
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
