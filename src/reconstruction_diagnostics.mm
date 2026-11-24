// SPDX-License-Identifier: GPL-3.0-only
#include "area_wire.h"
#include "frame_benchmark_motion.hpp"
#include "metal_frame_access.hpp"
#include "metal_reconstruction_access.hpp"
#include "pbrt_render.hpp"
#include "render_identity.hpp"
#include "resource_limits.hpp"
#include "temporal_denoiser.hpp"
#include "temporal_denoiser_wire.h"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#ifndef RYPATH_BUILD_DESCRIPTION
#define RYPATH_BUILD_DESCRIPTION "unavailable"
#endif

namespace {
using namespace rypath;
namespace fs = std::filesystem;
constexpr std::uint64_t artifact_limit = 8ull << 30;
constexpr std::uint64_t json_limit = 8ull << 20;
void require(bool ok, std::string_view message) {
    if (!ok)
        throw std::runtime_error(std::string(message));
}
NSString *ns(std::string_view text) {
    return [[NSString alloc] initWithBytes:text.data()
                                    length:text.size()
                                  encoding:NSUTF8StringEncoding];
}
std::string string(NSString *text) {
    require(text && [text isKindOfClass:NSString.class], "Expected JSON string");
    const char *utf8 = text.UTF8String;
    require(utf8 != nullptr, "JSON string is not valid UTF8");
    std::string value(utf8, [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    require(value.find('\0') == std::string::npos, "Embedded NUL in JSON string");
    return value;
}
std::uint64_t number(std::string_view text) {
    std::uint64_t n = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), n);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "Invalid integer");
    return n;
}
NSDictionary *object(id value) {
    require([value isKindOfClass:NSDictionary.class], "Expected JSON object");
    return value;
}
NSArray *array(id value) {
    require([value isKindOfClass:NSArray.class], "Expected JSON array");
    return value;
}
std::uint64_t integer(id value) {
    require([value isKindOfClass:NSNumber.class] &&
                CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID(),
            "Expected non-boolean JSON unsigned integer");
    return number(string([(NSNumber *)value stringValue]));
}
bool boolean(id value) {
    require([value isKindOfClass:NSNumber.class] &&
                CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID(),
            "Expected JSON boolean");
    return [(NSNumber *)value boolValue];
}
float finite_float(const std::string &value) {
    std::istringstream input(value);
    float x = 0;
    input >> std::noskipws >> x;
    require(input.eof() && !input.fail() && std::isfinite(x), "Invalid finite float");
    return x;
}
SpatialVariancePolicy variance_policy(std::string_view value) {
    if (value == "paired")
        return SpatialVariancePolicy::paired;
    if (value == "center")
        return SpatialVariancePolicy::center;
    if (value == "conservative")
        return SpatialVariancePolicy::conservative;
    throw std::runtime_error("Variance policy must be paired, center, or conservative");
}
std::string_view variance_policy_name(SpatialVariancePolicy policy) {
    switch (policy) {
    case SpatialVariancePolicy::paired:
        return "paired";
    case SpatialVariancePolicy::center:
        return "center";
    case SpatialVariancePolicy::conservative:
        return "conservative";
    }
    throw std::runtime_error("Unknown spatial variance policy");
}
bool on_off(std::string_view value) {
    require(value == "on" || value == "off", "Boolean options require on or off");
    return value == "on";
}
NSDictionary *signed_configuration(SpatialVariancePolicy policy, bool prefilter, bool strict,
                                   std::uint32_t spatial) {
    return @{
        @"variance_policy" : ns(variance_policy_name(policy)),
        @"variance_prefilter" : @(prefilter),
        @"strict_spatial_segments" : @(strict),
        @"spatial_iterations" : @(spatial),
        @"history_limit" : @64,
        @"temporal_wire_version" : @(temporal_denoiser_wire::version)
    };
}
NSData *read_file(const fs::path &path, std::uint64_t maximum,
                  std::optional<std::uint64_t> exact = {}) {
    require(fs::is_regular_file(fs::symlink_status(path)), "Input must be a regular file");
    const auto size = fs::file_size(path);
    require(size <= maximum && (!exact || size == *exact),
            "Input file size exceeds limit or differs from manifest");
    // Allocate/read the checked size only, even if the file changes after stat.
    NSMutableData *data = [NSMutableData dataWithLength:static_cast<NSUInteger>(size)];
    require(data != nil, "Input allocation failed");
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot open input file");
    input.read(static_cast<char *>(data.mutableBytes), static_cast<std::streamsize>(size));
    require(std::uint64_t(input.gcount()) == size && input.peek() == std::char_traits<char>::eof(),
            "Input file changed or read failed");
    return data;
}
void help() {
    std::cout
        << "Usage: rypath-reconstruction-diagnostics --scene FILE --output-dir DIR --artifact-root "
           "DIR [options]\n"
           "  --width N --height N --frames N --seed N --estimator independent|area1|area2|area4\n"
           "  --area-output selected|conditional-mean (Area fanout only; default: "
           "conditional-mean)\n"
           "  --capture-mode final|replay --capture-frames N,N --roi-file FILE\n"
           "  --motion-offset X | --reference-samples 1024|4096 --seed N [--reference-offset X]\n"
           "  --replay-list FILE [--replay-spatial 0..5] --resource-profile NAME\n"
           "  --variance-policy paired|center|conservative --variance-prefilter on|off\n"
           "  --strict-spatial-segments on|off --save-online-mean on|off\n"
           "Native diagnostic captures only; output directory must be fresh, artifact root capped "
           "at 8 GiB.\n";
}
struct Options {
    fs::path scene, output_dir, artifact_root, replay_list, roi_file;
    std::uint32_t width = 1920, height = 1080, frames = 128, reference_samples = 0;
    std::uint64_t seed = 51001;
    ResourceProfile resource_profile = ResourceProfile::standard;
    std::optional<float> motion_offset, reference_offset;
    FrameEstimator estimator = FrameEstimator::independent;
    FrameAreaOutput area_output = FrameAreaOutput::conditional_mean;
    std::uint32_t mutation_steps = 0;
    std::set<std::uint64_t> captures;
    std::uint32_t replay_spatial = 5;
    std::string capture_mode = "final";
    SpatialVariancePolicy spatial_variance_policy = SpatialVariancePolicy::paired;
    bool variance_prefilter = false, strict_spatial_segments = false, save_online_mean = false;
    bool variance_policy_explicit = false, variance_prefilter_explicit = false,
         strict_spatial_segments_explicit = false, replay_spatial_explicit = false;
};
std::string_view area_output_name(FrameAreaOutput output) {
    return output == FrameAreaOutput::selected ? "selected" : "conditional-mean";
}
std::string_view effective_area_output(const Options &o) {
    return o.estimator == FrameEstimator::area_replay ? area_output_name(o.area_output)
                                                      : "not-applicable";
}
Options parse(int argc, char **argv) {
    Options o;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        require(seen.insert(key).second && i + 1 < argc, "Duplicate option or missing value");
        std::string value = argv[++i];
        if (key == "--scene")
            o.scene = value;
        else if (key == "--output-dir")
            o.output_dir = value;
        else if (key == "--artifact-root")
            o.artifact_root = value;
        else if (key == "--replay-list")
            o.replay_list = value;
        else if (key == "--roi-file")
            o.roi_file = value;
        else if (key == "--variance-policy") {
            o.spatial_variance_policy = variance_policy(value);
            o.variance_policy_explicit = true;
        } else if (key == "--variance-prefilter") {
            o.variance_prefilter = on_off(value);
            o.variance_prefilter_explicit = true;
        } else if (key == "--strict-spatial-segments") {
            o.strict_spatial_segments = on_off(value);
            o.strict_spatial_segments_explicit = true;
        } else if (key == "--save-online-mean")
            o.save_online_mean = on_off(value);
        else if (key == "--capture-mode") {
            require(value == "final" || value == "replay", "Capture mode must be final or replay");
            o.capture_mode = value;
        } else if (key == "--resource-profile") {
            auto p = parse_resource_profile(value);
            require(p.has_value(), "Invalid resource profile");
            o.resource_profile = *p;
        } else if (key == "--estimator") {
            require(value == "independent" || value == "area1" || value == "area2" ||
                        value == "area4",
                    "Estimator must be independent, area1, area2, or area4");
            o.estimator =
                value == "independent" ? FrameEstimator::independent : FrameEstimator::area_replay;
            o.mutation_steps = value == "independent" ? 0u : unsigned(value.back() - '0');
        } else if (key == "--area-output") {
            require(value == "selected" || value == "conditional-mean",
                    "Area output must be selected or conditional-mean");
            o.area_output =
                value == "selected" ? FrameAreaOutput::selected : FrameAreaOutput::conditional_mean;
        } else if (key == "--motion-offset") {
            const auto x = finite_float(value);
            require(x != 0, "Invalid motion offset");
            o.motion_offset = x;
        } else if (key == "--reference-offset") {
            o.reference_offset = finite_float(value);
        } else if (key == "--capture-frames") {
            require(!value.empty() && value.back() != ',', "Empty capture frame");
            std::istringstream input(value);
            std::string item;
            while (std::getline(input, item, ','))
                require(o.captures.insert(number(item)).second, "Duplicate capture frame");
        } else {
            auto n = number(value);
            if (key == "--seed")
                o.seed = n;
            else {
                require(n <= UINT32_MAX, "Integer exceeds uint32");
                if (key == "--width")
                    o.width = n;
                else if (key == "--height")
                    o.height = n;
                else if (key == "--frames")
                    o.frames = n;
                else if (key == "--reference-samples")
                    o.reference_samples = n;
                else if (key == "--replay-spatial") {
                    o.replay_spatial = n;
                    o.replay_spatial_explicit = true;
                } else
                    throw std::runtime_error("Unknown option: " + key);
            }
        }
    }
    require(!seen.contains("--area-output") ||
                (o.estimator == FrameEstimator::area_replay && o.replay_list.empty()),
            "Area output requires an Area diagnostic fanout run");
    require(!o.scene.empty() && !o.output_dir.empty() && !o.artifact_root.empty(),
            "--scene, --output-dir and --artifact-root required");
    require(o.width && o.height && o.width <= 16384 && o.height <= 16384 && o.frames &&
                o.frames <= 4096,
            "Invalid dimensions/frames");
    require(std::uint64_t(o.width) * o.height * 96 <= (1ull << 30), "CPU workspace exceeds 1 GiB");
    require(o.reference_samples == 0 || o.reference_samples == 1024 || o.reference_samples == 4096,
            "Reference samples must be 1024 or the one allowed extension 4096");
    require(!o.reference_samples ||
                (!o.motion_offset && o.estimator == FrameEstimator::independent),
            "References require static independent sampling");
    require(!o.reference_samples || seen.contains("--seed"),
            "References require an explicit --seed independent of diagnostic seeds");
    require(!o.reference_offset || (o.reference_samples && !o.motion_offset),
            "Reference offset requires a static reference run");
    require(o.replay_list.empty() ||
                (!o.reference_samples && !o.motion_offset && !o.reference_offset),
            "Replay cannot combine reference or motion options");
    require(o.replay_spatial <= 5, "Replay spatial count exceeds five");
    require(!o.variance_prefilter || o.spatial_variance_policy == SpatialVariancePolicy::center,
            "--variance-prefilter on requires --variance-policy center");
    require(!o.save_online_mean ||
                (!o.motion_offset && !o.reference_samples && o.replay_list.empty()),
            "--save-online-mean on requires a static diagnostic fanout run");
    if (o.captures.empty())
        o.captures.insert(o.frames - 1);
    if (o.capture_mode == "replay") {
        require(o.frames <= 16 && !o.reference_samples, "Replay capture limited to sixteen frames");
        for (unsigned i = 0; i < o.frames; ++i)
            o.captures.insert(i);
        require(std::uint64_t(o.width) * o.height * o.frames * 384 <=
                    artifact_limit - (64ull << 20),
                "Replay preflight exceeds 8 GiB");
    }
    require(*o.captures.rbegin() < o.frames, "Capture frame outside sequence");
    if (o.motion_offset)
        (void)frame_benchmark::motion_pose(0, 0, o.frames, o.motion_offset);
    return o;
}
#include "reconstruction_diagnostics_scene.hpp"

class Output {
    fs::path root_, directory_;
    std::map<std::pair<std::string, std::size_t>, std::string> rgb_payloads_;

  public:
    explicit Output(const Options &o) {
        root_ = fs::canonical(o.artifact_root);
        require(!fs::exists(o.output_dir), "Output directory must be fresh");
        require(fs::is_directory(o.output_dir.parent_path()), "Output parent must exist");
        directory_ = fs::canonical(o.output_dir.parent_path()) / o.output_dir.filename();
        auto relative = directory_.lexically_relative(root_);
        require(!relative.empty() && *relative.begin() != "..",
                "Output must be below artifact root");
        require(fs::create_directory(directory_), "Cannot create output directory");
    }
    void budget(std::uint64_t additional) const {
        std::uint64_t total = 0;
        for (const auto &entry : fs::recursive_directory_iterator(root_)) {
            require(!entry.is_symlink(), "Symlink inside artifact root rejected");
            if (entry.is_regular_file()) {
                const auto bytes = entry.file_size();
                require(bytes <= artifact_limit - total, "Artifact root already exceeds 8 GiB");
                total += bytes;
            }
        }
        require(additional <= artifact_limit - total,
                "Write would exceed shared 8 GiB artifact cap");
    }
    std::string bytes(const std::string &name, const void *data, std::size_t size) {
        require(fs::path(name).filename() == name, "Unsafe output name");
        budget(size);
        const auto path = directory_ / name;
        require(!fs::exists(path), "Destination must be fresh");
        std::ofstream out(path, std::ios::binary);
        out.write(static_cast<const char *>(data), size);
        out.close();
        require(bool(out), "Output write failed");
        return digest_hex(sha256({static_cast<const std::uint8_t *>(data), size}));
    }
    struct RGBPayload {
        std::string path, hash;
        bool alias = false;
    };
    RGBPayload rgb_payload(const std::string &name, const void *data, std::size_t size) {
        const auto hash = digest_hex(sha256({static_cast<const std::uint8_t *>(data), size}));
        const auto key = std::pair{hash, size};
        if (const auto existing = rgb_payloads_.find(key); existing != rgb_payloads_.end())
            return {existing->second, hash, true};
        require(bytes(name, data, size) == hash, "RGB payload hash changed during write");
        rgb_payloads_.emplace(key, name);
        return {name, hash, false};
    }
    void json(const std::string &name, id value) {
        NSError *error = nil;
        NSData *data = [NSJSONSerialization
            dataWithJSONObject:value
                       options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                         error:&error];
        require(data != nil,
                error ? string(error.localizedDescription) : "JSON serialization failed");
        bytes(name, data.bytes, data.length);
    }
};
NSDictionary *load_json(const fs::path &path) {
    NSData *data = read_file(path, json_limit);
    NSError *error = nil;
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    require([value isKindOfClass:NSDictionary.class], "JSON root must be an object");
    return object(value);
}
std::size_t pixel_bytes(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatR16Float:
        return 2;
    case MTLPixelFormatR32Float:
    case MTLPixelFormatR32Uint:
    case MTLPixelFormatRG16Float:
        return 4;
    case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA16Float:
        return 8;
    case MTLPixelFormatRGBA32Float:
    case MTLPixelFormatRGBA32Uint:
        return 16;
    default:
        throw std::runtime_error("Unsupported snapshot pixel format");
    }
}
NSDictionary *save_snapshot(Output &out, const MetalReconstructionSnapshot &snapshot,
                            std::string prefix, std::uint64_t frame, const Options &o, bool reset,
                            const ReconstructionConfig &config) {
    require(snapshot.diagnostic.empty(), snapshot.diagnostic);
    NSMutableArray *textures = [NSMutableArray array];
    for (const auto &texture : snapshot.textures) {
        require(texture.bytes.size() ==
                    std::uint64_t(texture.width) * texture.height * pixel_bytes(texture.format),
                "Snapshot size mismatch");
        const bool archive =
            o.capture_mode == "replay" && prefix.starts_with("temporal-") &&
            (texture.name.starts_with("input.") || texture.name == "temporal.mean" ||
             texture.name == "temporal.variance" || texture.name == "temporal.count" ||
             texture.name == "output");
        std::string filename = prefix + "." + texture.name + ".bin";
        auto hash = digest_hex(sha256(
            {reinterpret_cast<const std::uint8_t *>(texture.bytes.data()), texture.bytes.size()}));
        if (archive)
            out.bytes(filename, texture.bytes.data(), texture.bytes.size());
        [textures addObject:@{
            @"name" : ns(texture.name),
            @"path" : archive ? ns(filename) : (id)NSNull.null,
            @"sha256" : ns(hash),
            @"width" : @(texture.width),
            @"height" : @(texture.height),
            @"archived" : @(archive),
            @"metal_pixel_format" : @(texture.format),
            @"byte_length" : @(texture.bytes.size()),
            @"layout" : @"tight-native-little-endian"
        }];
    }
    NSDictionary *metadata = @{
        @"schema" : @2,
        @"frame" : @(frame),
        @"seed" : @(o.seed),
        @"epoch" : @(snapshot.info.source.epoch),
        @"reset_history" : @(reset),
        @"spatial_iterations" : @(config.spatial_iterations),
        @"variance_policy" : ns(variance_policy_name(config.spatial_variance_policy)),
        @"variance_prefilter" : @(config.variance_prefilter),
        @"strict_spatial_segments" : @(config.strict_spatial_segments),
        @"temporal_wire_version" : @(temporal_denoiser_wire::version),
        @"signed_configuration_applicable" : @(config.mode == ReconstructionMode::temporal),
        @"history_limit" : @64,
        @"full_replay_inputs" : @(o.capture_mode == "replay" && prefix.starts_with("temporal-")),
        @"scene" : ns(fs::absolute(o.scene).string()),
        @"textures" : textures,
        @"signed_raw_is_accumulated_spp" : @NO,
        @"source_sample" : @(snapshot.info.source.sample_index),
        @"area_contract" : @(o.estimator == FrameEstimator::area_replay ? area_wire::contract : 0),
        @"area_output" : ns(effective_area_output(o)),
        @"area_target" : o.estimator == FrameEstimator::area_replay ? @"quarter-l1-v1" : @"none",
        @"area_source_mis" : o.estimator == FrameEstimator::area_replay ? @"source-target-mis-v1"
                                                                        : @"none",
        @"area_temporal_reuse" : @(o.estimator == FrameEstimator::area_replay ? 20 : 0),
        @"source_evaluations" : @(snapshot.info.source.completed_evaluations),
        @"history_accepted" : @(snapshot.info.history_accepted),
        @"history_rejected" : @(snapshot.info.history_rejected)
    };
    out.json(prefix + ".json", metadata);
    return metadata;
}
std::unique_ptr<Frame> render(FrameSession &session, std::uint64_t sample, const Options &o,
                              const RenderScene &scene) {
    auto pose = frame_benchmark::motion_pose(sample, 0, o.frames, o.motion_offset);
    FrameRequest request{.sample_index = sample, .epoch = pose.epoch};
    if (o.motion_offset)
        request.camera = frame_benchmark::translated_camera(scene, pose.offset);
    auto submitted = session.submit(request);
    require(submitted.frame != nullptr && submitted.status == GeometryStatus::success,
            submitted.diagnostic);
    auto info = submitted.frame->wait();
    require(info.status == FrameStatus::complete, info.diagnostic);
    const auto pixels = std::uint64_t(o.width) * o.height;
    const auto evaluations = o.estimator == FrameEstimator::area_replay
                                 ? (info.history_used ? 7u : 5u) + o.mutation_steps
                                 : 1u;
    require(info.completed_samples == pixels && info.completed_evaluations == pixels * evaluations,
            "Diagnostic source-MIS path accounting mismatch");
    return std::move(submitted.frame);
}
std::array<float, 3> xyz_rgb(const std::array<float, 3> &v) {
    return {float(3.240444291419 * v[0] - 1.537133830545 * v[1] - .498529890988 * v[2]),
            float(-.969266752560 * v[0] + 1.876012242980 * v[1] + .041556048487 * v[2]),
            float(.055643519672 * v[0] - .204026238798 * v[1] + 1.057226873771 * v[2])};
}
void save_rgb(Output &out, std::string prefix, const std::vector<std::array<float, 3>> &rgb,
              const Options &o, std::string role) {
    for (const auto &pixel : rgb)
        for (float value : pixel)
            require(std::isfinite(value), "Nonfinite RGB metric plane");
    const auto size = rgb.size() * sizeof(rgb[0]);
    const auto payload = out.rgb_payload(prefix + ".bin", rgb.data(), size);
    out.json(
        prefix + ".json", @{
            @"schema" : @1,
            @"path" : ns(payload.path),
            @"sha256" : ns(payload.hash),
            @"byte_length" : @(size),
            @"payload_alias" : @(payload.alias),
            @"payload_alias_of" : payload.alias ? (id)ns(payload.path) : (id)NSNull.null,
            @"width" : @(o.width),
            @"height" : @(o.height),
            @"channels" : @3,
            @"dtype" : @"<f4",
            @"role" : ns(role),
            @"seed" : @(o.seed),
            @"area_contract" :
                @(o.estimator == FrameEstimator::area_replay ? area_wire::contract : 0),
            @"area_output" : ns(effective_area_output(o)),
            @"area_target" : o.estimator == FrameEstimator::area_replay ? @"quarter-l1-v1"
                                                                        : @"none",
            @"area_source_mis" : o.estimator == FrameEstimator::area_replay
                ? @"source-target-mis-v1"
                : @"none",
            @"area_temporal_reuse" : @(o.estimator == FrameEstimator::area_replay ? 20 : 0),
            @"reference_samples" : @(o.reference_samples)
        });
}

void save_mean(Output &out, const MetalReconstructionSnapshot &snapshot, std::string prefix,
               const Options &o) {
    for (const auto &texture : snapshot.textures)
        if (texture.name == "temporal.mean") {
            require(texture.format == MTLPixelFormatRGBA32Float, "Temporal mean must be float32");
            std::vector<std::array<float, 3>> rgb(std::uint64_t(texture.width) * texture.height);
            for (std::size_t p = 0; p < rgb.size(); ++p)
                std::memcpy(rgb[p].data(), texture.bytes.data() + p * 16, 12);
            save_rgb(out, prefix, rgb, o, "signed pre-spatial temporal mean");
            return;
        }
    throw std::runtime_error("Temporal mean snapshot missing");
}

NSDictionary *region_means(const std::vector<std::array<float, 3>> &rgb, NSDictionary *regions,
                           const Options &o) {
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    for (NSString *name in regions) {
        (void)string(name);
        NSArray *box = array(regions[name]);
        require(box.count == 4, "ROI requires four exclusive coordinates");
        const auto x0 = integer(box[0]), y0 = integer(box[1]), x1 = integer(box[2]),
                   y1 = integer(box[3]);
        require(x0 < x1 && y0 < y1 && x1 <= o.width && y1 <= o.height, "ROI outside native image");
        std::array<double, 3> sum{};
        for (unsigned y = y0; y < y1; ++y)
            for (unsigned x = x0; x < x1; ++x)
                for (unsigned c = 0; c < 3; ++c)
                    sum[c] += rgb[std::uint64_t(y) * o.width + x][c];
        for (auto &value : sum)
            value /= std::uint64_t(x1 - x0) * (y1 - y0);
        const double luminance = .2126 * sum[0] + .7152 * sum[1] + .0722 * sum[2];
        result[name] = @{
            @"signed_RGB_mean" : @[ @(sum[0]), @(sum[1]), @(sum[2]) ],
            @"signed_Rec709_Y_mean" : @(luminance),
            @"box" : box
        };
    }
    return result;
}

void fanout(const Options &o, const Scene &scene, FrameSession &session, Output &out) {
    struct Arm {
        std::string name;
        ReconstructionConfig config;
        std::unique_ptr<ReconstructionStream> stream;
    };
    std::array<Arm, 3> arms;
    arms[0].name = "temporal";
    arms[0].config.mode = ReconstructionMode::temporal;
    arms[1].name = "temporal0";
    arms[1].config.mode = ReconstructionMode::temporal;
    arms[1].config.spatial_iterations = 0;
    arms[2].name = "metalfx";
    arms[2].config.mode = ReconstructionMode::metalfx;
    for (auto &arm : arms) {
        arm.config.capture_diagnostics = true;
        if (arm.config.mode == ReconstructionMode::temporal) {
            arm.config.spatial_variance_policy = o.spatial_variance_policy;
            arm.config.variance_prefilter = o.variance_prefilter;
            arm.config.strict_spatial_segments = o.strict_spatial_segments;
        }
    }
    std::vector<std::array<double, 3>> mean(std::uint64_t(o.width) * o.height);
    NSMutableArray *captures = [NSMutableArray array];
    NSMutableArray *series = [NSMutableArray array];
    NSDictionary *regions = o.roi_file.empty()
                                ? @{@"whole" : @[ @0, @0, @(o.width), @(o.height) ]}
                                : load_json(o.roi_file)[@"regions"];
    require([regions isKindOfClass:NSDictionary.class] && regions.count,
            "ROI file requires nonempty regions dictionary");
    for (std::uint64_t frame = 0; frame < o.frames; ++frame)
        @autoreleasepool {
            auto raw =
                render(session, frame, o, scene.value); // Held through all consumers and captures.
            auto capture = raw->capture();
            require(capture.status == GeometryStatus::success, capture.diagnostic);
            std::vector<std::array<float, 3>> raw_rgb(capture.raw_xyz.size());
            for (std::size_t p = 0; p < mean.size(); ++p) {
                auto rgb = xyz_rgb(capture.raw_xyz[p]);
                raw_rgb[p] = rgb;
                for (unsigned c = 0; c < 3; ++c)
                    mean[p][c] += (double(rgb[c]) - mean[p][c]) / (frame + 1);
            }
            for (auto &arm : arms) {
                if (!arm.stream) {
                    auto built = build_reconstruction_stream(*raw, arm.config);
                    require(built.stream != nullptr, built.diagnostic);
                    arm.stream = std::move(built.stream);
                }
                auto submitted = arm.stream->submit(*raw);
                require(submitted.frame != nullptr, submitted.diagnostic);
                auto info = submitted.frame->wait();
                require(info.status == ReconstructionStatus::complete, info.diagnostic);
                auto result = submitted.frame->capture();
                require(result.status == GeometryStatus::success, result.diagnostic);
                auto hash = digest_hex(
                    sha256({reinterpret_cast<const std::uint8_t *>(result.linear_rgb.data()),
                            result.linear_rgb.size() * sizeof(result.linear_rgb[0])}));
                auto raw_hash = digest_hex(
                    sha256({reinterpret_cast<const std::uint8_t *>(capture.raw_xyz.data()),
                            capture.raw_xyz.size() * sizeof(capture.raw_xyz[0])}));
                const auto pose = frame_benchmark::motion_pose(frame, 0, o.frames, o.motion_offset);
                [series addObject:@{
                    @"frame" : @(frame),
                    @"arm" : ns(arm.name),
                    @"phase" : ns(pose.phase),
                    @"offset" : @(pose.offset),
                    @"epoch" : @(pose.epoch),
                    @"reset_history" : @(info.history_reset),
                    @"output_RGB_sha256" : ns(hash),
                    @"area_contract" :
                        @(o.estimator == FrameEstimator::area_replay ? area_wire::contract : 0),
                    @"area_output" : ns(effective_area_output(o)),
                    @"area_target" : o.estimator == FrameEstimator::area_replay ? @"quarter-l1-v1"
                                                                                : @"none",
                    @"area_source_mis" : o.estimator == FrameEstimator::area_replay
                        ? @"source-target-mis-v1"
                        : @"none",
                    @"area_temporal_reuse" : @(o.estimator == FrameEstimator::area_replay ? 20 : 0),
                    @"same_input_raw_XYZ_sha256" : ns(raw_hash),
                    @"regions" : region_means(result.linear_rgb, regions, o),
                    @"raw_regions" : region_means(raw_rgb, regions, o),
                    @"history_accepted" : @(info.history_accepted),
                    @"history_rejected" : @(info.history_rejected),
                    @"signed_configuration_applicable" :
                        @(arm.config.mode == ReconstructionMode::temporal),
                    @"reconstruction_configuration" : signed_configuration(
                        arm.config.spatial_variance_policy, arm.config.variance_prefilter,
                        arm.config.strict_spatial_segments, arm.config.spatial_iterations)
                }];
                if (o.captures.contains(frame)) {
                    auto snapshot =
                        MetalReconstructionStreamProbe::capture_diagnostics(*arm.stream);
                    std::string prefix = arm.name + "-" + std::to_string(frame);
                    save_snapshot(out, snapshot, prefix, frame, o, info.history_reset, arm.config);
                    save_rgb(out, prefix + "-RGB", result.linear_rgb, o,
                             "full native reconstruction output");
                    if (arm.name == "temporal") {
                        save_mean(out, snapshot, prefix + "-mean-RGB", o);
                        save_rgb(out, "raw-" + std::to_string(frame) + "-RGB", raw_rgb, o,
                                 "signed current raw frame, not accumulated spp");
                    }
                    [captures addObject:ns(prefix + ".json")];
                }
            }
        }
    std::vector<std::array<float, 3>> rgb(mean.size());
    for (std::size_t p = 0; p < mean.size(); ++p)
        for (unsigned c = 0; c < 3; ++c)
            rgb[p][c] = mean[p][c];
    if (!o.motion_offset) {
        const auto hash = digest_hex(sha256(
            {reinterpret_cast<const std::uint8_t *>(rgb.data()), rgb.size() * sizeof(rgb[0])}));
        out.json(
            "raw-online-mean-summary.json", @{
                @"schema" : @1,
                @"frames" : @(o.frames),
                @"width" : @(o.width),
                @"height" : @(o.height),
                @"sha256" : ns(hash),
                @"payload_requested" : @(o.save_online_mean),
                @"regions" : region_means(rgb, regions, o),
                @"role" : @"signed online raw mean across static frames"
            });
        if (o.save_online_mean)
            save_rgb(out, "raw-online-mean", rgb, o, "signed raw mean across static frames");
    }
    out.json(
        "series.json", @{
            @"schema" : @1,
            @"records" : series,
            @"regions" : regions,
            @"no_spatial_uncertainty_estimate" : @YES
        });
    out.json(
        "complete.json", @{
            @"complete" : @YES,
            @"captures" : captures,
            @"frames" : @(o.frames),
            @"same_raw_frame_fanout" : @YES
        });
}

void reference(const Options &o, const Scene &scene, FrameSession &session, Output &out) {
    std::vector<std::array<double, 3>> sum(std::uint64_t(o.width) * o.height);
    for (std::uint64_t sample = 0; sample < o.reference_samples; ++sample)
        @autoreleasepool {
            // Static independent tape; sample ordinals may exceed the diagnostic sequence length.
            FrameRequest request{.sample_index = sample};
            if (o.reference_offset && *o.reference_offset != 0)
                request.camera =
                    frame_benchmark::translated_camera(scene.value, *o.reference_offset);
            auto submitted = session.submit(request);
            require(submitted.frame != nullptr, submitted.diagnostic);
            auto info = submitted.frame->wait();
            require(info.status == FrameStatus::complete, info.diagnostic);
            require(info.completed_evaluations == sum.size(), "Reference evaluation mismatch");
            auto capture = submitted.frame->capture();
            require(capture.status == GeometryStatus::success, capture.diagnostic);
            for (std::size_t p = 0; p < sum.size(); ++p)
                for (unsigned c = 0; c < 3; ++c)
                    sum[p][c] += capture.raw_xyz[p][c];
            if ((sample + 1) % 64 == 0 || sample + 1 == o.reference_samples)
                std::cout << "reference samples " << (sample + 1) << '/' << o.reference_samples
                          << std::endl;
        }
    std::vector<std::array<float, 3>> rgb(sum.size());
    for (std::size_t p = 0; p < sum.size(); ++p) {
        std::array<float, 3> xyz{};
        for (unsigned c = 0; c < 3; ++c)
            xyz[c] = sum[p][c] / o.reference_samples;
        rgb[p] = xyz_rgb(xyz);
    }
    save_rgb(out, "reference-mean", rgb, o, "independent signed raw reference mean");
    out.json("complete.json", @{@"complete" : @YES, @"reference_samples" : @(o.reference_samples)});
}

void replay(const Options &o, const Scene &scene, FrameSession &session, Output &out) {
    NSDictionary *list = load_json(o.replay_list);
    NSArray *paths = array(list[@"snapshots"]);
    require(paths.count && paths.count <= 128, "Replay requires 1..128 ordered snapshots");
    const auto first_record = load_json(o.replay_list.parent_path() / string(paths[0]));
    require(integer(first_record[@"schema"]) != 1,
            "Replay schema 1 lacks the spatial_position/spatial_normal/spatial_group guide "
            "contract; recapture with schema 2, without substituting missing guides");
    require(integer(first_record[@"schema"]) == 2, "Unsupported replay schema; require schema 2");
    require(integer(first_record[@"temporal_wire_version"]) == temporal_denoiser_wire::version,
            "Replay temporal wire version differs from the current spatial guide contract");
    auto dummy = render(session, 0, o, scene.value);
    auto acquired = MetalFrameAccess::acquire(*dummy);
    require(acquired.lease != nullptr, acquired.diagnostic);
    auto built = TemporalDenoiser::create(*acquired.lease,
                                          {.width = o.width,
                                           .height = o.height,
                                           .history_limit = 64,
                                           .spatial_iterations = o.replay_spatial,
                                           .variance_prefilter = o.variance_prefilter,
                                           .spatial_variance_policy = o.spatial_variance_policy,
                                           .strict_spatial_segments = o.strict_spatial_segments});
    require(built.denoiser != nullptr, built.diagnostic);
    std::uint64_t ordinal = 0, validated = 0;
    std::optional<std::uint64_t> recorded_spatial, recorded_seed, previous_epoch;
    std::optional<SpatialVariancePolicy> recorded_policy;
    std::optional<bool> recorded_prefilter, recorded_strict;
    const std::set<std::string> input_names{
        "input.color",          "input.depth",           "input.motion",
        "input.diffuse_albedo", "input.specular_albedo", "input.normal",
        "input.previous_depth", "input.identity",        "input.spatial_position",
        "input.spatial_normal", "input.spatial_group"};
    for (id relative : paths)
        @autoreleasepool {
            fs::path path = o.replay_list.parent_path() / string(relative);
            NSDictionary *record = load_json(path);
            require(integer(record[@"schema"]) == 2,
                    "Replay requires schema 2 spatial geometry guides on every frame");
            require(integer(record[@"temporal_wire_version"]) == temporal_denoiser_wire::version,
                    "Replay temporal wire version changed within sequence");
            require(integer(record[@"history_limit"]) == 64, "Replay requires history_limit64");
            require(boolean(record[@"full_replay_inputs"]),
                    "Replay requires complete full-frame quantized inputs captured with "
                    "--capture-mode replay");
            const auto frame = integer(record[@"frame"]);
            const bool reset = boolean(record[@"reset_history"]);
            const auto spatial = integer(record[@"spatial_iterations"]),
                       seed = integer(record[@"seed"]), epoch = integer(record[@"epoch"]);
            const auto policy = variance_policy(string(record[@"variance_policy"]));
            const bool prefilter = boolean(record[@"variance_prefilter"]),
                       strict = boolean(record[@"strict_spatial_segments"]);
            require(!prefilter || policy == SpatialVariancePolicy::center,
                    "Recorded variance prefilter requires center policy");
            require(spatial <= 5, "Invalid recorded spatial count");
            require(frame == ordinal && (ordinal || reset),
                    "Replay must start reset at frame zero and remain contiguous");
            require(!previous_epoch || *previous_epoch == epoch || reset,
                    "Recorded epoch change must reset history");
            require(!recorded_spatial || *recorded_spatial == spatial,
                    "Recorded spatial configuration changed within replay");
            require(!recorded_seed || *recorded_seed == seed,
                    "Recorded seed changed within replay");
            require(!recorded_policy || *recorded_policy == policy,
                    "Recorded variance policy changed within replay");
            require(!recorded_prefilter || *recorded_prefilter == prefilter,
                    "Recorded variance prefilter changed within replay");
            require(!recorded_strict || *recorded_strict == strict,
                    "Recorded strict spatial segments changed within replay");
            require(spatial == o.replay_spatial || o.replay_spatial_explicit,
                    "Changing recorded spatial count requires explicit --replay-spatial ablation");
            require(
                policy == o.spatial_variance_policy || o.variance_policy_explicit,
                "Changing recorded variance policy requires explicit --variance-policy ablation");
            require(prefilter == o.variance_prefilter || o.variance_prefilter_explicit,
                    "Changing recorded prefilter requires explicit --variance-prefilter ablation");
            require(strict == o.strict_spatial_segments || o.strict_spatial_segments_explicit,
                    "Changing recorded strict segments requires explicit --strict-spatial-segments "
                    "ablation");
            recorded_spatial = spatial;
            recorded_seed = seed;
            recorded_policy = policy;
            recorded_prefilter = prefilter;
            recorded_strict = strict;
            previous_epoch = epoch;
            const bool baseline =
                spatial == o.replay_spatial && policy == o.spatial_variance_policy &&
                prefilter == o.variance_prefilter && strict == o.strict_spatial_segments;
            std::map<std::string, id<MTLTexture>> textures;
            NSArray *entries = array(record[@"textures"]);
            require(entries.count <= 128, "Too many replay texture entries");
            std::optional<std::string> expected_output;
            std::uint64_t input_bytes = 0;
            for (id raw_entry : entries) {
                NSDictionary *entry = object(raw_entry);
                const std::string name = string(entry[@"name"]);
                const bool input = input_names.contains(name), output = name == "output";
                if (!input && !output) {
                    require(!name.starts_with("input.") || name == "input.exposure" ||
                                name == "input.roughness",
                            "Unsupported replay input texture");
                    continue;
                }
                require(integer(entry[@"width"]) == o.width &&
                            integer(entry[@"height"]) == o.height,
                        "Replay dimension mismatch");
                const auto format = MTLPixelFormat(integer(entry[@"metal_pixel_format"]));
                const auto bytes = std::uint64_t(o.width) * o.height * pixel_bytes(format);
                require(bytes == integer(entry[@"byte_length"]), "Replay byte count mismatch");
                require(boolean(entry[@"archived"]), "Replay texture must be archived");
                require(string(entry[@"layout"]) == "tight-native-little-endian",
                        "Unsupported replay texture layout");
                const std::string expected_hash = string(entry[@"sha256"]);
                require(expected_hash.size() == 64 && expected_hash.find_first_not_of(
                                                          "0123456789abcdef") == std::string::npos,
                        "Invalid replay SHA256");
                if (input) {
                    require(!textures.contains(name), "Duplicate replay texture");
                    require(bytes <= (1ull << 30) - input_bytes,
                            "Replay input workspace exceeds 1 GiB");
                }
                const fs::path payload = path.parent_path() / string(entry[@"path"]);
                NSData *data = read_file(payload, 1ull << 30, bytes);
                const auto hash = digest_hex(
                    sha256({static_cast<const std::uint8_t *>(data.bytes), data.length}));
                require(hash == expected_hash, "Replay hash mismatch");
                if (output) {
                    require(!expected_output && format == MTLPixelFormatRGBA32Float,
                            "Duplicate or invalid replay output");
                    expected_output = hash;
                    continue;
                }
                input_bytes += bytes;
                auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                     width:o.width
                                                                                    height:o.height
                                                                                 mipmapped:NO];
                descriptor.storageMode = MTLStorageModeShared;
                descriptor.usage = MTLTextureUsageShaderRead;
                id<MTLTexture> texture =
                    [acquired.lease->device() newTextureWithDescriptor:descriptor];
                require(texture != nil, "Replay texture allocation failed");
                [texture replaceRegion:MTLRegionMake2D(0, 0, o.width, o.height)
                           mipmapLevel:0
                             withBytes:data.bytes
                           bytesPerRow:o.width * pixel_bytes(format)];
                require(textures.emplace(name, texture).second, "Duplicate replay texture");
            }
            require(expected_output.has_value(), "Replay baseline output missing");
            auto get = [&](const std::string &name) {
                auto it = textures.find("input." + name);
                require(it != textures.end(), "Missing replay input: " + name);
                return it->second;
            };
            auto descriptor =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                   width:o.width
                                                                  height:o.height
                                                               mipmapped:NO];
            descriptor.storageMode = MTLStorageModeShared;
            descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            id<MTLTexture> output = [acquired.lease->device() newTextureWithDescriptor:descriptor];
            require(output != nil, "Replay output allocation failed");
            TemporalDenoiserInputs inputs{.color = get("color"),
                                          .depth = get("depth"),
                                          .motion = get("motion"),
                                          .diffuse_albedo = get("diffuse_albedo"),
                                          .specular_albedo = get("specular_albedo"),
                                          .normal = get("normal"),
                                          .previous_depth = get("previous_depth"),
                                          .identity = get("identity"),
                                          .spatial_position = get("spatial_position"),
                                          .spatial_normal = get("spatial_normal"),
                                          .spatial_group = get("spatial_group"),
                                          .output = output,
                                          .reset_history = reset};
            require(inputs.color.pixelFormat == MTLPixelFormatRGBA32Float,
                    "Signed replay requires the temporal arm's actual float32 input");
            auto command = [acquired.lease->queue() commandBuffer];
            auto error = built.denoiser->encode(command, inputs);
            require(error.empty(), error);
            [command commit];
            [command waitUntilCompleted];
            error = built.denoiser->complete(command.status == MTLCommandBufferStatusCompleted);
            require(error.empty(), error);
            const auto stats = built.denoiser->stats();
            require(stats.history_accepted == integer(record[@"history_accepted"]) &&
                        stats.history_rejected == integer(record[@"history_rejected"]),
                    "Replay temporal history differs from capture");
            std::vector<std::array<float, 4>> rgba(std::uint64_t(o.width) * o.height);
            [output getBytes:rgba.data()
                 bytesPerRow:o.width * 16
                  fromRegion:MTLRegionMake2D(0, 0, o.width, o.height)
                 mipmapLevel:0];
            if (baseline) {
                const auto actual =
                    digest_hex(sha256({reinterpret_cast<const std::uint8_t *>(rgba.data()),
                                       rgba.size() * sizeof(rgba[0])}));
                require(actual == *expected_output,
                        "Exact baseline replay output hash differs from captured output");
                ++validated;
            }
            std::vector<std::array<float, 3>> rgb(rgba.size());
            for (std::size_t p = 0; p < rgb.size(); ++p)
                std::copy_n(rgba[p].begin(), 3, rgb[p].begin());
            auto replay_options = o;
            replay_options.seed = seed;
            save_rgb(out, "replay-" + std::to_string(ordinal), rgb, replay_options,
                     baseline ? "exact baseline signed temporal replay"
                              : "signed temporal explicit configuration ablation");
            ++ordinal;
        }
    out.json(
        "complete.json", @{
            @"complete" : @YES,
            @"replayed_frames" : @(ordinal),
            @"spatial_iterations" : @(o.replay_spatial),
            @"recorded_spatial_iterations" : @(*recorded_spatial),
            @"recorded_seed" : @(*recorded_seed),
            @"spatial_ablation" : @(*recorded_spatial != o.replay_spatial),
            @"variance_policy_ablation" : @(*recorded_policy != o.spatial_variance_policy),
            @"variance_prefilter_ablation" : @(*recorded_prefilter != o.variance_prefilter),
            @"strict_spatial_segments_ablation" : @(*recorded_strict != o.strict_spatial_segments),
            @"recorded_variance_policy" : ns(variance_policy_name(*recorded_policy)),
            @"recorded_variance_prefilter" : @(*recorded_prefilter),
            @"recorded_strict_spatial_segments" : @(*recorded_strict),
            @"replay_configuration" :
                signed_configuration(o.spatial_variance_policy, o.variance_prefilter,
                                     o.strict_spatial_segments, o.replay_spatial),
            @"exact_baseline_validated_frames" : @(validated),
            @"dummy_transport_frames" : @1
        });
}

} // namespace

int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            if (argc == 2 && std::string_view(argv[1]) == "--help") {
                help();
                return 0;
            }
            require(std::endian::native == std::endian::little,
                    "Diagnostic serialization requires little endian");
            auto options = parse(argc, argv);
            auto scene = load_scene(options);
            require(!options.motion_offset || frame_benchmark::motion_medium_supported(scene.value),
                    "Motion cannot classify medium crossings");
            require(!options.reference_offset || *options.reference_offset == 0 ||
                        frame_benchmark::motion_medium_supported(scene.value),
                    "Reference offset cannot classify medium crossings");
            if (options.reference_offset && *options.reference_offset != 0)
                (void)frame_benchmark::translated_camera(scene.value, *options.reference_offset);
            FrameConfig config;
            config.width = options.width;
            config.height = options.height;
            config.seed = options.seed;
            config.estimator = options.estimator;
            config.area_output = options.area_output;
            config.mutation_steps = options.mutation_steps;
            config.intersection_profile = IntersectionProfile::native;
            config.execution = FrameExecution::wavefront;
            auto built = build_frame_session(scene.value, config);
            require(built.session != nullptr, built.diagnostic);
            Output output(options);
            RenderSettings settings;
            settings.width = options.width;
            settings.height = options.height;
            settings.samples = 1;
            settings.seed = options.seed;
            settings.material_seed = 0;
            settings.sampler.kind = SamplerKind::independent;
            NSMutableArray *provenance = [NSMutableArray array];
            for (const auto &item : scene.provenance)
                [provenance addObject:ns(item)];
            output.json(
                "configuration.json", @{
                    @"scene" : ns(fs::absolute(options.scene).string()),
                    @"seed" : @(options.seed),
                    @"width" : @(options.width),
                    @"height" : @(options.height),
                    @"frames" : @(options.frames),
                    @"estimator" : config.estimator == FrameEstimator::area_replay
                        ? ns("area" + std::to_string(config.mutation_steps))
                        : @"independent",
                    @"area_contract" : @(
                        options.estimator == FrameEstimator::area_replay ? area_wire::contract : 0),
                    @"area_output" : ns(effective_area_output(options)),
                    @"area_target" : options.estimator == FrameEstimator::area_replay
                        ? @"quarter-l1-v1"
                        : @"none",
                    @"area_source_mis" : options.estimator == FrameEstimator::area_replay
                        ? @"source-target-mis-v1"
                        : @"none",
                    @"area_temporal_reuse" :
                        @(options.estimator == FrameEstimator::area_replay ? 20 : 0),
                    @"requested_area_output" : ns(area_output_name(options.area_output)),
                    @"mutation_steps" : @(config.mutation_steps),
                    @"source_identity" : ns(digest_hex(render_identity(scene.value, settings))),
                    @"resource_profile" : ns(resource_profile_name(options.resource_profile)),
                    @"build_description" : ns(RYPATH_BUILD_DESCRIPTION),
                    @"signed_reconstruction_configuration" : signed_configuration(
                        options.spatial_variance_policy, options.variance_prefilter,
                        options.strict_spatial_segments,
                        options.replay_list.empty() ? 5u : options.replay_spatial),
                    @"signed_configuration_applicable" : @(options.reference_samples == 0),
                    @"save_online_mean" : @(options.save_online_mean),
                    @"metric_payload_deduplication" :
                        @"within-case SHA256 and byte length; descriptor path resolves aliases",
                    @"reference_offset" : options.reference_offset
                        ? (id) @(*options.reference_offset)
                        : (id)NSNull.null,
                    @"artifact_limit_bytes" : @(artifact_limit),
                    @"provenance" : provenance,
                    @"performance_claim" : @NO
                });
            if (!options.replay_list.empty())
                replay(options, scene, *built.session, output);
            else if (options.reference_samples)
                reference(options, scene, *built.session, output);
            else
                fanout(options, scene, *built.session, output);
            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
