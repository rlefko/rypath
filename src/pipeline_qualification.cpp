// SPDX-License-Identifier: GPL-3.0-only
#include "area_evaluation_context.hpp"
#include "area_wire.h"
#include "frame_qualification_identity.hpp"
#include "frame_session.hpp"
#include "image_output.hpp"
#include "pbrt_render.hpp"
#include "reconstruction.hpp"
#include "render_identity.hpp"
#include "resource_limits.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef RYPATH_BUILD_DESCRIPTION
#define RYPATH_BUILD_DESCRIPTION "unknown"
#endif
namespace {
using namespace rypath;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using RGB = std::array<double, 3>;
using Image = std::vector<RGB>;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
std::string quote(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += char(c);
    }
    return out + '"';
}
std::ostringstream json_stream() {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(17);
    return stream;
}
std::string optional_number(std::optional<double> value) {
    if (!value || !std::isfinite(*value))
        return "null";
    auto stream = json_stream();
    stream << *value;
    return stream.str();
}
struct Options {
    std::optional<HeroWavelengthSampling> hero_wavelengths;
    FrameMutationPolicy mutation_policy = FrameMutationPolicy::automatic;
    FrameAreaStencil area_stencil = FrameAreaStencil::automatic;
    FrameAreaPathDomain area_path_domain = FrameAreaPathDomain::summed;
    FrameContributionReplay contribution_replay = FrameContributionReplay::full;
    ResourceProfile resource_profile = ResourceProfile::standard;
    SpatialVariancePolicy variance_policy = SpatialVariancePolicy::paired;
    bool suffix_cache = true, compact_pairs = false, temporal_reprojection = false;
    std::uint32_t enhanced_history_cap = 20;
    bool compare_reconstruction = false;
    FrameSpatialMode spatial_mode = FrameSpatialMode::all;
    FrameAreaReuse area_reuse = FrameAreaReuse::full;
    std::vector<int> arms{-1, 0, 1, 2, 4};
    SpectralTransportSelection spectral_transport = SpectralTransportSelection::automatic;
    fs::path scene, output_dir;
    std::uint32_t width = 320, height = 180, frames = 32, seeds = 8;
    std::uint32_t reference_spp = 4096, reference_batches = 8;
    ReconstructionMode reconstruction = ReconstructionMode::temporal;
    FrameAreaOutput area_output = FrameAreaOutput::conditional_mean;
    std::optional<float> motion_offset;
};
const char *area_output_name(FrameAreaOutput output) {
    return output == FrameAreaOutput::selected ? "selected" : "conditional-mean";
}
const char *variance_name(SpatialVariancePolicy policy) {
    return policy == SpatialVariancePolicy::paired   ? "paired"
           : policy == SpatialVariancePolicy::center ? "center"
                                                     : "conservative";
}
const char *target_name(const Options &o) {
    return o.area_path_domain == FrameAreaPathDomain::enhanced ? "quarter-l1-hybrid-scaled-zero-v4"
                                                               : "quarter-l1-v1";
}
const char *source_mis_name(const Options &o) {
    if (o.area_reuse == FrameAreaReuse::fresh)
        return "fresh-local-selection-v1";
    return o.area_path_domain == FrameAreaPathDomain::enhanced
               ? "enhanced-hybrid-confidence-scaled-mis-v4"
               : "source-target-mis-v1";
}
std::string atlas_json(const FrameAreaAtlasInfo &atlas) {
    if (!atlas.version)
        return "null";
    auto out = json_stream();
    out << "{\"version\":" << atlas.version << ",\"entry_count\":" << atlas.entry_count
        << ",\"asset_bytes\":" << atlas.asset_bytes << ",\"sigma\":" << atlas.sigma
        << ",\"iterations\":" << atlas.iterations << ",\"sha256\":" << quote(atlas.sha256)
        << ",\"seed\":[" << atlas.seed[0] << ',' << atlas.seed[1] << "],\"widths\":[";
    for (unsigned i = 0; i < atlas.widths.size(); ++i)
        out << (i ? "," : "") << atlas.widths[i];
    out << "],\"poses\":[";
    for (unsigned i = 0; i < atlas.poses.size(); ++i)
        out << (i ? "," : "") << '[' << atlas.poses[i][0] << ',' << atlas.poses[i][1] << ','
            << atlas.poses[i][2] << ']';
    out << "]}";
    return out.str();
}
const char *reconstruction_name(ReconstructionMode mode) {
    return mode == ReconstructionMode::temporal ? "temporal" : "metalfx";
}
const char *reconstructed_kind(ReconstructionMode mode) {
    return mode == ReconstructionMode::temporal ? "temporal-denoised-linear-rgb"
                                                : "metalfx-denoised-linear-rgb";
}
const char *usage =
    "Usage: rypath-pipeline-qualification --scene SCENE.pbrt --output-dir FRESH\n"
    "  --spectral-transport auto|scalar|packet|hero (default: auto)\n"
    "  --hero-wavelengths uniform|visible (hero only; default: visible)\n"
    "  --area-mutation-policy auto|legacy|spectral-refresh --area-stencil "
    "auto|adjacent|reciprocal16|paired-gaussian16\n"
    "  [--area-path-domain summed|contribution|enhanced] [--area-contribution-replay full|prefix]\n"
    "  [--area-reuse full|fresh (fresh requires --mutation-arms 0)]\n"
    "  [--area-spatial-mode all|stochastic-pairs|stochastic-shared (default: all)]\n"
    "  [--area-suffix-cache on|off] [--area-pair-compaction on|off]\n"
    "  [--area-temporal-reprojection off|dual] [--mutation-arms 0|0,1|0,1,2,4]\n"
    "  [--resource-profile standard|large] [--variance-policy paired|center|conservative]\n"
    "  [--width 320] [--height 180] [--frames 32] [--seeds 8]\n"
    "  [--reconstruction temporal|metalfx (default: temporal)]\n"
    "  [--area-history-cap N (Enhanced fixed temporal confidence, 1 through 20)]\n"
    "  [--compare-reconstruction on|off (same-frame signed filter ablations and native MetalFX)]\n"
    "  [--area-output selected|conditional-mean (Area arms only; default: conditional-mean)]\n"
    "  [--reference-spp 4096] [--reference-batches 8] [--motion-offset WORLD_X]\n"
    "Reference SPP must divide equally across at least two independent batches.\n"
    "Explicit native-resolution reconstruction; optional four-phase translation; one fresh "
    "sample/frame; readbacks excluded "
    "from frame timing.\n";
Options parse(int argc, char **argv) {
    Options o;
    std::set<std::string_view> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        require(seen.insert(key).second, "Duplicate qualification option");
        require(i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--"),
                "Missing qualification option value");
        const std::string_view value = argv[++i];
        if (key == "--area-path-domain") {
            require(value == "summed" || value == "contribution" || value == "enhanced",
                    "Area path domain must be summed, contribution, or enhanced");
            o.area_path_domain = value == "summed"         ? FrameAreaPathDomain::summed
                                 : value == "contribution" ? FrameAreaPathDomain::contribution
                                                           : FrameAreaPathDomain::enhanced;
        } else if (key == "--area-contribution-replay") {
            require(value == "full" || value == "prefix",
                    "Contribution replay must be full or prefix");
            o.contribution_replay =
                value == "full" ? FrameContributionReplay::full : FrameContributionReplay::prefix;
        } else if (key == "--area-suffix-cache" || key == "--area-pair-compaction") {
            require(value == "on" || value == "off", "Area execution switches must be on or off");
            (key == "--area-suffix-cache" ? o.suffix_cache : o.compact_pairs) = value == "on";
        } else if (key == "--area-reuse") {
            require(value == "full" || value == "fresh", "Area reuse must be full or fresh");
            o.area_reuse = value == "fresh" ? FrameAreaReuse::fresh : FrameAreaReuse::full;
        } else if (key == "--area-spatial-mode") {
            require(value == "all" || value == "stochastic-pairs" || value == "stochastic-shared",
                    "Area spatial mode must be all, stochastic-pairs, or stochastic-shared");
            o.spatial_mode = value == "all"                ? FrameSpatialMode::all
                             : value == "stochastic-pairs" ? FrameSpatialMode::independent
                                                           : FrameSpatialMode::shared;
        } else if (key == "--area-temporal-reprojection") {
            require(value == "off" || value == "dual", "Temporal reprojection must be off or dual");
            o.temporal_reprojection = value == "dual";
        } else if (key == "--mutation-arms") {
            if (value == "0")
                o.arms = {-1, 0};
            else if (value == "0,1")
                o.arms = {-1, 0, 1};
            else
                require(value == "0,1,2,4", "Mutation arms must be 0, 0,1 or 0,1,2,4");
        } else if (key == "--compare-reconstruction") {
            require(value == "on" || value == "off", "Reconstruction comparison must be on or off");
            o.compare_reconstruction = value == "on";
        } else if (key == "--resource-profile") {
            const auto profile = parse_resource_profile(value);
            require(profile.has_value(), "Resource profile must be standard or large");
            o.resource_profile = *profile;
        } else if (key == "--variance-policy") {
            require(value == "paired" || value == "center" || value == "conservative",
                    "Variance policy must be paired, center, or conservative");
            o.variance_policy = value == "paired"   ? SpatialVariancePolicy::paired
                                : value == "center" ? SpatialVariancePolicy::center
                                                    : SpatialVariancePolicy::conservative;
        } else if (key == "--hero-wavelengths") {
            o.hero_wavelengths = parse_hero_wavelength_sampling(value);
            if (!o.hero_wavelengths)
                throw std::runtime_error("Hero wavelengths must be uniform or visible");
        } else if (key == "--area-mutation-policy") {
            if (value == "auto")
                o.mutation_policy = FrameMutationPolicy::automatic;
            else if (value == "legacy")
                o.mutation_policy = FrameMutationPolicy::legacy;
            else if (value == "spectral-refresh")
                o.mutation_policy = FrameMutationPolicy::spectral_refresh;
            else
                throw std::runtime_error(
                    "Area mutation policy must be auto, legacy, or spectral-refresh");
        } else if (key == "--area-stencil") {
            if (value == "auto")
                o.area_stencil = FrameAreaStencil::automatic;
            else if (value == "adjacent")
                o.area_stencil = FrameAreaStencil::adjacent;
            else if (value == "reciprocal16")
                o.area_stencil = FrameAreaStencil::reciprocal16;
            else if (value == "paired-gaussian16")
                o.area_stencil = FrameAreaStencil::paired_gaussian16;
            else
                throw std::runtime_error(
                    "Area stencil must be auto, adjacent, reciprocal16, or paired-gaussian16");
        } else if (key == "--spectral-transport") {
            const auto selection = parse_spectral_transport(value);
            if (!selection)
                throw std::runtime_error(
                    "Spectral transport must be auto, scalar, packet, or hero");
            o.spectral_transport = *selection;
        } else if (key == "--scene")
            o.scene = value;
        else if (key == "--output-dir")
            o.output_dir = value;
        else if (key == "--reconstruction") {
            require(value == "temporal" || value == "metalfx",
                    "Reconstruction must be temporal or metalfx");
            o.reconstruction =
                value == "temporal" ? ReconstructionMode::temporal : ReconstructionMode::metalfx;
        } else if (key == "--area-output") {
            require(value == "selected" || value == "conditional-mean",
                    "Area output must be selected or conditional-mean");
            o.area_output =
                value == "selected" ? FrameAreaOutput::selected : FrameAreaOutput::conditional_mean;
        } else if (key == "--motion-offset") {
            float offset = 0;
            std::istringstream parsed{std::string(value)};
            parsed.imbue(std::locale::classic());
            parsed >> std::noskipws >> offset;
            require(!parsed.fail() && parsed.eof() && std::isfinite(offset) && offset != 0,
                    "Motion offset must be a finite nonzero float");
            o.motion_offset = offset;
        } else {
            std::uint32_t number = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
            require(result.ec == std::errc{} && result.ptr == value.data() + value.size() && number,
                    "Qualification counts/dimensions must be positive uint32 values");
            if (key == "--area-history-cap") {
                require(number <= 20, "Area history cap must be 1 through 20");
                o.enhanced_history_cap = number;
            } else if (key == "--width")
                o.width = number;
            else if (key == "--height")
                o.height = number;
            else if (key == "--frames")
                o.frames = number;
            else if (key == "--seeds")
                o.seeds = number;
            else if (key == "--reference-spp")
                o.reference_spp = number;
            else if (key == "--reference-batches")
                o.reference_batches = number;
            else
                throw std::runtime_error("Unknown qualification option: " + std::string(key));
        }
    }
    require(!o.scene.empty() && !o.output_dir.empty(), "--scene and --output-dir are required");
    require(o.area_reuse != FrameAreaReuse::fresh ||
                (o.area_path_domain == FrameAreaPathDomain::enhanced &&
                 o.arms == std::vector<int>{-1, 0} && o.spatial_mode == FrameSpatialMode::all &&
                 !o.compact_pairs && !o.temporal_reprojection),
            "Fresh extraction requires Enhanced, mutation-arms 0, full spatial mode and no "
            "reprojection/compaction");
    require(!rypath_stochastic_spatial::enabled(o.spatial_mode) ||
                (o.area_path_domain == FrameAreaPathDomain::enhanced &&
                 o.area_stencil == FrameAreaStencil::paired_gaussian16),
            "Stochastic spatial pairs require Enhanced paired-gaussian16");
    require(o.contribution_replay == FrameContributionReplay::full ||
                frame_area_contribution_family(o.area_path_domain),
            "Prefix replay requires contribution or enhanced path domain");
    require((!o.compact_pairs && !o.temporal_reprojection) ||
                o.area_path_domain == FrameAreaPathDomain::enhanced,
            "Pair compaction and dual motion require enhanced path domain");
    require(!seen.contains("--variance-policy") || o.reconstruction == ReconstructionMode::temporal,
            "Variance policy requires temporal reconstruction");
    require(!seen.contains("--area-history-cap") ||
                o.area_path_domain == FrameAreaPathDomain::enhanced,
            "Area history cap requires Enhanced path domain");
    require(!o.compare_reconstruction || o.reconstruction == ReconstructionMode::temporal,
            "Same-frame comparison requires temporal as the primary reconstruction");
    require(o.width <= reconstruction_dimension_limit && o.height <= reconstruction_dimension_limit,
            "Qualification dimensions exceed 16384");
    require(o.frames <= 1000000 && o.seeds <= 4096 && o.reference_spp <= 100000000 &&
                o.reference_batches >= 2 && o.reference_batches <= 4096 &&
                o.reference_batches <= o.reference_spp &&
                o.reference_spp % o.reference_batches == 0,
            "Counts exceed qualification limits or reference SPP does not divide into >=2 batches");
    require(!o.motion_offset || (o.frames >= 8 && o.frames % 4 == 0),
            "Motion requires at least eight frames divisible by four");
    // Bound this driver's simultaneously retained CPU means, variance, and captures.
    require(std::uint64_t(o.width) * o.height * (o.motion_offset ? 320u : 192u) <= (1ull << 30),
            "Qualification CPU image workspace exceeds 1 GiB");
    return o;
}
std::uint64_t evaluation_seed(std::uint32_t i) { return 0x1000000000000000ull + i + 1; }
std::uint64_t reference_seed(std::uint32_t i, bool end = false) {
    return (end ? 0x3000000000000000ull : 0x2000000000000000ull) + i + 1;
}

class Output {
    fs::path directory_;
    int directory_fd_ = -1, frames_fd_ = -1, seeds_fd_ = -1;
    static void bytes(int fd, std::string_view data) {
        while (!data.empty()) {
            const auto count = ::write(fd, data.data(), data.size());
            if (count < 0 && errno == EINTR)
                continue;
            require(count > 0, "Qualification output write failed");
            data.remove_prefix(std::size_t(count));
        }
    }

  public:
    explicit Output(const fs::path &directory) : directory_(fs::absolute(directory)) {
        require(!fs::exists(directory_) && !fs::is_symlink(directory_),
                "Qualification output directory must be fresh");
        require(fs::create_directory(directory_),
                "Cannot create fresh output directory (parent must exist)");
        directory_fd_ = ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        require(directory_fd_ >= 0, "Cannot retain qualification output directory");
    }
    ~Output() {
        if (seeds_fd_ >= 0)
            ::close(seeds_fd_);
        if (frames_fd_ >= 0)
            ::close(frames_fd_);
        if (directory_fd_ >= 0)
            ::close(directory_fd_);
    }
    void text(const std::string &name, std::string_view data) {
        const auto pending = name + ".pending";
        const int fd = ::openat(directory_fd_, pending.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        require(fd >= 0, "Qualification destination is not fresh");
        try {
            bytes(fd, data);
            require(::fsync(fd) == 0, "Qualification output fsync failed");
        } catch (...) {
            ::close(fd);
            throw;
        }
        require(::close(fd) == 0, "Qualification output close failed");
        require(::renameat(directory_fd_, pending.c_str(), directory_fd_, name.c_str()) == 0,
                "Qualification output publication failed");
    }
    void frame(std::string_view line) {
        if (frames_fd_ < 0)
            frames_fd_ = ::openat(directory_fd_, "frames.jsonl", O_WRONLY | O_CREAT | O_EXCL, 0600);
        require(frames_fd_ >= 0, "Cannot create fresh frame records");
        bytes(frames_fd_, line);
        require(::fsync(frames_fd_) == 0, "Frame record fsync failed");
    }
    void seed(std::string_view line) {
        if (seeds_fd_ < 0)
            seeds_fd_ =
                ::openat(directory_fd_, "seed-aggregates.jsonl", O_WRONLY | O_CREAT | O_EXCL, 0600);
        require(seeds_fd_ >= 0, "Cannot create fresh seed aggregates");
        bytes(seeds_fd_, line);
        require(::fsync(seeds_fd_) == 0, "Seed record fsync failed");
    }
    void exr(const std::string &name, const Image &image, const Options &o,
             std::span<const std::pair<std::string, std::string>> metadata) {
        require(image.size() == std::uint64_t(o.width) * o.height, "EXR image extent mismatch");
        std::vector<float> rgb;
        rgb.reserve(image.size() * 3);
        for (const auto &pixel : image)
            for (const double value : pixel) {
                require(std::isfinite(value) &&
                            std::abs(value) <= std::numeric_limits<float>::max(),
                        "Qualification FLOAT EXR cannot represent a value");
                rgb.push_back(float(value));
            }
        const auto pending = name + ".pending";
        const int fd = ::openat(directory_fd_, pending.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        require(fd >= 0, "Cannot create fresh EXR destination");
        try {
            const auto diagnostic = encode_exr(
                {o.width, o.height, rgb, ExrPrecision::float32, metadata},
                [&](std::uint64_t offset, std::span<const std::uint8_t> data) {
                    const auto maximum = std::uint64_t(std::numeric_limits<off_t>::max());
                    if (offset > maximum || data.size() > maximum - offset)
                        return false;
                    while (!data.empty()) {
                        const auto count = ::pwrite(fd, data.data(), data.size(), off_t(offset));
                        if (count < 0 && errno == EINTR)
                            continue;
                        if (count <= 0)
                            return false;
                        offset += std::uint64_t(count);
                        data = data.subspan(std::size_t(count));
                    }
                    return true;
                },
                {.max_dimension = std::max(o.width, o.height),
                 .retained_bytes = rgb.size() * sizeof(float)});
            require(diagnostic.empty(), diagnostic);
            require(::fsync(fd) == 0, "Qualification EXR fsync failed");
        } catch (...) {
            ::close(fd);
            throw;
        }
        require(::close(fd) == 0 &&
                    ::renameat(directory_fd_, pending.c_str(), directory_fd_, name.c_str()) == 0,
                "Qualification EXR publication failed");
    }
};

void set_parameter(pbrt::Descriptor &d, std::string name, std::optional<std::int32_t> value) {
    // Preserve the preflight-selected last matching parameters while indices move.
    std::vector<pbrt::Parameter> effective;
    for (const auto index : d.effective_authored)
        effective.push_back(d.authored.parameters[index]);
    d.authored.parameters = std::move(effective);
    std::erase_if(d.authored.parameters, [&](const auto &p) { return p.name == name; });
    std::erase_if(d.inherited, [&](const auto &p) { return p.parameter->name == name; });
    if (value) {
        pbrt::Parameter p;
        p.type = "integer";
        p.name = name;
        pbrt::Value v;
        v.kind = pbrt::ValueKind::integer;
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
std::vector<std::string> overrides(pbrt::Result &checked, const Options &o) {
    std::vector<std::string> provenance;
    for (const auto &[name, type] :
         {std::pair{"Sampler", "independent"}, std::pair{"Film", "rgb"}}) {
        if (std::ranges::none_of(checked.scene->options,
                                 [&](const auto &d) { return d.authored.name == name; })) {
            pbrt::Descriptor descriptor;
            descriptor.authored.name = name;
            pbrt::Value value;
            value.kind = pbrt::ValueKind::string;
            value.text = type;
            descriptor.authored.arguments.push_back(value);
            checked.scene->options.push_back(std::move(descriptor));
            provenance.push_back(std::string("implicit ") + name +
                                 " overridden for pipeline qualification");
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

struct Scene {
    RenderScene value;
    std::vector<std::string> provenance;
    std::vector<fs::path> dependencies;
};
Scene load_scene(const Options &o) {
    const auto limits = resource_limits(o.resource_profile).pbrt;
    auto checked = pbrt::check(o.scene, limits);
    require(checked.exit_code() == 0 && checked.scene.has_value(), pbrt::report(checked));
    Scene result;
    result.dependencies.push_back(o.scene);
    for (const auto &dependency : checked.document.dependencies)
        result.dependencies.push_back(dependency.resolved_path);
    result.provenance = overrides(checked, o);
    auto translated = pbrt::translate(checked, {o.scene.string()}, limits, o.spectral_transport);
    for (const auto &diagnostic : translated.diagnostics)
        std::cerr << pbrt::format_diagnostic(diagnostic);
    if (translated.exit_code() != 0)
        throw std::runtime_error("Qualification PBRT translation failed");
    auto &translated_scene = translated.scene;
    if (!translated_scene.has_value())
        throw std::runtime_error("Qualification PBRT translation failed");
    result.value = std::move(*translated_scene);
    result.provenance.insert(result.provenance.end(), translated.provenance.begin(),
                             translated.provenance.end());
    auto &scene = result.value;
    if (o.motion_offset)
        require(std::ranges::none_of(
                    scene.instance_media,
                    [](const auto &medium) { return medium.inside != medium.outside; }),
                "Motion cannot classify distinct medium interfaces");

    scene.integrator = scene.volume_transport ? IntegratorKind::volpath : IntegratorKind::path;
    scene.max_component_value = std::numeric_limits<double>::infinity();
    std::string spectral_provenance;
    const auto spectral_error =
        select_spectral_transport(scene, o.spectral_transport, &spectral_provenance);
    require(spectral_error.empty(), spectral_error);
    if (o.hero_wavelengths && scene.spectral_transport != SpectralTransport::hero)
        throw std::runtime_error("--hero-wavelengths requires effective hero transport");
    if (o.hero_wavelengths)
        scene.hero_wavelengths = *o.hero_wavelengths;
    if (frame_area_contribution_family(o.area_path_domain))
        require(scene.model == RenderModel::pbrt &&
                    scene.spectral_transport == SpectralTransport::hero && scene.max_depth <= 65,
                "Contribution domains require PBRT hero transport and finite authored depth 0..65");
    result.provenance.push_back(spectral_provenance);
    result.provenance.push_back("all arms and reference use native wavefront, independent tape, "
                                "fixed footprint_samples=1, material_seed=0, unexposed output");
    return result;
}
FrameCamera camera_pose(const RenderScene &scene, float offset) {
    FrameCamera pose{.camera = scene.camera,
                     .camera_medium = scene.camera_medium,
                     .raster_camera = scene.raster_camera,
                     .perspective_lens = scene.perspective_lens,
                     .texture_camera = scene.texture_camera};
    const auto translated_x = [offset](float x) { return double(x) + offset; };
    const auto active_translate = [&](GeometryPoint &point, bool require_movement) {
        const double x = translated_x(point.x);
        require(std::isfinite(x) && std::abs(x) <= std::numeric_limits<float>::max(),
                "Translated active camera exceeds float range");
        const float translated = float(x);
        require(!require_movement || offset == 0 || translated != point.x,
                "Motion offset is below active camera float resolution");
        point.x = translated;
    };
    if (pose.raster_camera) {
        active_translate(pose.raster_camera->origin, true);
        // These compatibility fields do not generate raster rays. Keep finite
        // representable translations without imposing their resolution on the active origin.
        for (auto *point : {&pose.camera.origin, &pose.camera.target}) {
            const double x = translated_x(point->x);
            if (std::isfinite(x) && std::abs(x) <= std::numeric_limits<float>::max())
                point->x = float(x);
        }
    } else {
        active_translate(pose.camera.origin, true);
        active_translate(pose.camera.target, false);
        const double dx = double(pose.camera.target.x) - pose.camera.origin.x;
        const double dy = double(pose.camera.target.y) - pose.camera.origin.y;
        const double dz = double(pose.camera.target.z) - pose.camera.origin.z;
        require(std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
                    (dx != 0 || dy != 0 || dz != 0),
                "Translated demo camera direction is nonfinite or collapsed");
    }
    // TextureCamera contains only two 3x3 linear matrices; translation preserves both.
    return pose;
}
void write_camera(std::ostream &out, const FrameCamera &camera) {
    const auto point = [&](std::string_view name, const GeometryPoint &value) {
        out << ',' << quote(name) << ":[" << value.x << ',' << value.y << ',' << value.z << ']';
    };
    out << ",\"camera_kind\":" << quote(camera.raster_camera ? "raster" : "demo");
    if (camera.raster_camera) {
        point("camera_origin", camera.raster_camera->origin);
        point("camera_corner", camera.raster_camera->corner);
        point("camera_dx", camera.raster_camera->dx);
        point("camera_dy", camera.raster_camera->dy);
    } else {
        point("camera_origin", camera.camera.origin);
        point("camera_target", camera.camera.target);
        point("camera_up", camera.camera.up);
        out << ",\"camera_vertical_fov_degrees\":" << camera.camera.vertical_fov_degrees;
    }
}
struct Pose {
    std::string phase = "static";
    float offset = 0;
    std::uint64_t epoch = 0;
    bool metric_valid = true, end_reference = false;
};
Pose pose_at(const Options &o, std::uint32_t sample) {
    if (!o.motion_offset)
        return {};
    const auto q = o.frames / 4;
    if (sample < q)
        return {"hold-start", 0, 0, true, false};
    if (sample < 2 * q)
        return {"translate", float(double(*o.motion_offset) * (sample - q + 1) / q), 0, false,
                false};
    if (sample < 3 * q)
        return {"hold-end", *o.motion_offset, 0, true, true};
    return {"cut-start", 0, 1, true, false};
}
std::string identity(const RenderScene &scene, const Options &o, std::uint64_t seed) {
    RenderSettings settings;
    settings.width = o.width;
    settings.height = o.height;
    settings.samples = 1;
    settings.seed = seed;
    settings.material_seed = 0;
    settings.sampler.kind = SamplerKind::independent;
    return digest_hex(render_identity(scene, settings));
}
FrameConfig configuration(const Options &o, std::uint64_t seed) {
    FrameConfig config;
    config.width = o.width;
    config.height = o.height;
    config.seed = seed;
    config.material_seed = 0;
    config.footprint_samples = 1;
    config.intersection_profile = IntersectionProfile::native;
    config.execution = FrameExecution::wavefront;
    config.estimator = FrameEstimator::independent;
    config.mutation_steps = 0;
    config.target_scale = 1;
    return config;
}
std::unique_ptr<FrameSession> session(const RenderScene &scene, const FrameConfig &config,
                                      MetalDeviceInfo &device) {
    auto built = build_frame_session(scene, config);
    require(built.status == GeometryStatus::success && built.session, built.diagnostic);
    require(!device.registry_id || device.registry_id == built.device.registry_id,
            "Qualification GPU device changed across sessions");
    device = built.device;
    return std::move(built.session);
}
std::unique_ptr<Frame> frame(FrameSession &session, std::uint64_t sample, std::uint64_t pixels,
                             std::optional<FrameCamera> camera = {}, std::uint64_t epoch = 0) {
    auto submitted = session.submit({.sample_index = sample, .epoch = epoch, .camera = camera});
    require(submitted.status == GeometryStatus::success && submitted.frame && !submitted.busy,
            submitted.diagnostic);
    const auto info = submitted.frame->wait();
    require(info.status == FrameStatus::complete && info.completed_samples == pixels &&
                info.sample_index == sample && info.epoch == epoch,
            info.diagnostic.empty() ? "Raw frame identity/count mismatch" : info.diagnostic);
    return std::move(submitted.frame);
}
RGB xyz_rgb(const RGB &xyz, RenderModel model) {
    if (model == RenderModel::demo)
        return linear_rgb(xyz);
    const auto [x, y, z] = xyz;
    return {3.240444291419 * x - 1.537133830545 * y - .498529890988 * z,
            -.969266752560 * x + 1.876012242980 * y + .041556048487 * z,
            .055643519672 * x - .204026238798 * y + 1.057226873771 * z};
}
Image raw_rgb(const FrameCapture &capture, RenderModel model, std::uint64_t pixels) {
    require(capture.status == GeometryStatus::success && capture.raw_xyz.size() == pixels,
            capture.diagnostic);
    Image result;
    result.reserve(capture.raw_xyz.size());
    for (const auto &xyz : capture.raw_xyz) {
        for (const auto value : xyz)
            require(std::isfinite(value), "Nonfinite raw XYZ capture");
        result.push_back(xyz_rgb({xyz[0], xyz[1], xyz[2]}, model));
    }
    return result;
}
Image reconstructed_rgb(const ReconstructionCapture &capture, std::uint64_t pixels) {
    require(capture.status == GeometryStatus::success && capture.linear_rgb.size() == pixels,
            capture.diagnostic);
    Image result;
    result.reserve(capture.linear_rgb.size());
    for (const auto &rgb : capture.linear_rgb) {
        for (const auto value : rgb)
            require(std::isfinite(value), "Nonfinite reconstructed RGB capture");
        result.push_back({rgb[0], rgb[1], rgb[2]});
    }
    return result;
}
using Metadata = std::vector<std::pair<std::string, std::string>>;
Metadata metadata(const Options &o, const std::string &source, std::string kind, std::string seed,
                  std::string arm, std::uint64_t sample = 0) {
    return {{"rypath.qual.kind", std::move(kind)},
            {"rypath.qual.seed", std::move(seed)},
            {"rypath.qual.arm", std::move(arm)},
            {"rypath.frame.sample", std::to_string(sample)},
            {"rypath.frame.epoch", "0"},
            {"rypath.frame.material_seed", "0"},
            {"rypath.frame.intersections", "native"},
            {"rypath.frame.footprint_samples", "1"},
            {"rypath.frame.source_identity", source},
            {"rypath.frame.source", o.scene.string()},
            {"rypath.frame.build", RYPATH_BUILD_DESCRIPTION},
            {"rypath.qual.reconstruction", reconstruction_name(o.reconstruction)},
            {"rypath.output.exposure", "1"},
            {"rypath.output.width", std::to_string(o.width)},
            {"rypath.output.height", std::to_string(o.height)}};
}
struct Reference {
    Image mean, variance_of_mean;
    double noise_floor_mse = 0;
};
Reference reference(const Scene &scene, const Options &o, Output &output, MetalDeviceInfo &device,
                    bool end = false) {
    require(!end || o.motion_offset.has_value(), "End reference requires motion");
    const float offset = end ? o.motion_offset.value_or(0) : 0;
    const std::string prefix = o.motion_offset ? (end ? "end-" : "start-") : "";
    const auto camera =
        o.motion_offset ? std::optional(camera_pose(scene.value, offset)) : std::nullopt;
    const auto pixels = std::uint64_t(o.width) * o.height;
    Reference result{Image(pixels), Image(pixels)};
    const auto count = o.reference_spp / o.reference_batches;
    for (std::uint32_t batch = 0; batch < o.reference_batches; ++batch) {
        const auto seed = reference_seed(batch, end);
        auto renderer = session(scene.value, configuration(o, seed), device);
        Image sum(pixels);
        for (std::uint32_t sample = 0; sample < count; ++sample) {
            auto raw = frame(*renderer, sample, pixels, camera);
            const auto info = raw->poll();
            require(info.completed_evaluations == pixels && info.fresh_candidates == pixels,
                    "Independent reference evaluation count mismatch");
            const auto image = raw->capture();
            require(image.status == GeometryStatus::success && image.raw_xyz.size() == pixels,
                    image.diagnostic);
            for (std::size_t p = 0; p < pixels; ++p)
                for (unsigned c = 0; c < 3; ++c) {
                    require(std::isfinite(image.raw_xyz[p][c]), "Nonfinite reference sample");
                    sum[p][c] += image.raw_xyz[p][c];
                    require(std::isfinite(sum[p][c]), "Reference double accumulation overflow");
                }
        }
        Image batch_mean(pixels);
        for (std::size_t p = 0; p < pixels; ++p) {
            RGB xyz{};
            for (unsigned c = 0; c < 3; ++c)
                xyz[c] = sum[p][c] / count;
            batch_mean[p] = xyz_rgb(xyz, scene.value.model);
            for (unsigned c = 0; c < 3; ++c) {
                const auto delta = batch_mean[p][c] - result.mean[p][c];
                result.mean[p][c] += delta / (batch + 1);
                result.variance_of_mean[p][c] += delta * (batch_mean[p][c] - result.mean[p][c]);
            }
        }
        auto attributes =
            metadata(o, identity(scene.value, o, seed), "independent-reference-batch-mean",
                     std::to_string(seed), "independent", count - 1);
        attributes.emplace_back("rypath.reference.spp", std::to_string(count));
        attributes.emplace_back("rypath.camera.offset_x", optional_number(offset));
        output.exr(prefix + "reference-batch-" + std::to_string(batch) + ".exr", batch_mean, o,
                   attributes);
    }
    for (auto &pixel : result.variance_of_mean)
        for (auto &value : pixel) {
            value /= double(o.reference_batches - 1) * o.reference_batches;
            require(std::isfinite(value) && value >= 0,
                    "Invalid reference batch variance of the mean");
            result.noise_floor_mse += value / (3 * double(pixels));
        }
    auto attributes =
        metadata(o, identity(scene.value, o, 1), "independent-reference-aggregate-mean",
                 "disjoint-reference-seed-batches", "independent");
    attributes.emplace_back("rypath.reference.spp", std::to_string(o.reference_spp));
    attributes.emplace_back("rypath.reference.batches", std::to_string(o.reference_batches));
    attributes.emplace_back("rypath.camera.offset_x", optional_number(offset));
    output.exr(prefix + "reference.exr", result.mean, o, attributes);
    attributes.front().second = "physical-rgb-reference-variance-of-mean";
    output.exr(prefix + "reference-variance-of-mean.exr", result.variance_of_mean, o, attributes);
    return result;
}
struct Metric {
    double mse = 0;
    std::optional<double> nrmse;
    RGB signed_channel_mean{};
};
Metric metric(const Image &image, const Image &reference, bool presentation_clamp = false) {
    require(!image.empty() && image.size() == reference.size(), "Metric image extent mismatch");
    Metric result;
    double normalization = 0;
    for (std::size_t p = 0; p < image.size(); ++p)
        for (unsigned c = 0; c < 3; ++c) {
            require(std::isfinite(image[p][c]) && std::isfinite(reference[p][c]),
                    "Nonfinite metric input");
            // Presentation is a separate, explicitly clipped comparison. Never
            // mutate signed raw/reference images or reconstructed captures.
            const auto value =
                presentation_clamp ? std::clamp(image[p][c], 0., 65504.) : image[p][c];
            const auto difference = value - reference[p][c];
            result.mse += difference * difference;
            normalization += reference[p][c] * reference[p][c];
            result.signed_channel_mean[c] += difference / double(image.size());
        }
    result.mse /= 3 * double(image.size());
    normalization /= 3 * double(image.size());
    require(std::isfinite(result.mse) && std::isfinite(normalization), "Metric reduction overflow");
    if (normalization > 0)
        result.nrmse = std::sqrt(result.mse / normalization);
    return result;
}
void rgb_json(std::ostream &out, const RGB &rgb) {
    out << '[' << rgb[0] << ',' << rgb[1] << ',' << rgb[2] << ']';
}
void metric_json(std::ostream &out, const Metric &metric) {
    out << "{\"mse\":" << metric.mse << ",\"nrmse\":" << optional_number(metric.nrmse)
        << ",\"signed_channel_mean\":";
    rgb_json(out, metric.signed_channel_mean);
    out << '}';
}

void write_counters(std::ostream &out, const ReconstructionCounters &c) {
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

struct SeedAggregate {
    std::string arm;
    std::string reconstruction;
    std::uint64_t seed = 0;
    std::uint32_t warm_frames = 0, metric_frames = 0, valid_gpu_frames = 0;
    double raw_mse = 0, reconstructed_mse = 0, presentation_mse = 0, wall = 0, gpu = 0;
    double session_startup = 0, reconstruction_startup = 0;
    RGB raw_bias{}, reconstructed_bias{};
    std::string json() const {
        auto out = json_stream();
        out << "{\"arm\":" << quote(arm) << ",\"seed\":" << seed
            << ",\"reconstruction\":" << quote(reconstruction)
            << ",\"valid_metric_frames\":" << metric_frames << ",\"warm_frames\":" << warm_frames
            << ",\"raw_mse_mean\":" << raw_mse / metric_frames
            << ",\"reconstructed_physical_mse_mean\":" << reconstructed_mse / metric_frames
            << ",\"reconstructed_presentation_mse_mean\":" << presentation_mse / metric_frames
            << ",\"pipeline_wall_mean_seconds\":" << wall / warm_frames
            << ",\"valid_pipeline_gpu_frames\":" << valid_gpu_frames
            << ",\"pipeline_gpu_mean_seconds\":"
            << optional_number(valid_gpu_frames ? std::optional(gpu / valid_gpu_frames)
                                                : std::nullopt)
            << ",\"session_startup_seconds\":" << session_startup
            << ",\"reconstruction_startup_seconds\":" << reconstruction_startup
            << ",\"raw_signed_channel_mean\":";
        RGB bias{};
        for (unsigned c = 0; c < 3; ++c)
            bias[c] = raw_bias[c] / metric_frames;
        rgb_json(out, bias);
        out << ",\"reconstructed_signed_channel_mean\":";
        for (unsigned c = 0; c < 3; ++c)
            bias[c] = reconstructed_bias[c] / metric_frames;
        rgb_json(out, bias);
        out << "}\n";
        return out.str();
    }
};
std::optional<double> gpu_time(double value, bool valid) {
    return valid && std::isfinite(value) && value >= 0 ? std::optional(value) : std::nullopt;
}
// Each variant owns a history stream for the entire seed/arm. The primary result
// already covers history 64 with five spatial passes. Submit each remaining
// variant the exact completed raw Frame before the transport session advances.
struct ComparisonFilter {
    const char *name;
    ReconstructionConfig config;
    std::unique_ptr<ReconstructionStream> stream{};
};
void compare_filters(std::ostream &line, std::span<ComparisonFilter> filters, const Frame &raw,
                     const FrameInfo &source_frame, const Image *truth, const Options &o,
                     Output &output, std::string_view arm, std::string_view source,
                     std::uint32_t seed_index, std::uint32_t sample) {
    line << ",\"comparison_filters\":{";
    for (auto &filter : filters) {
        if (!filter.stream) {
            auto built = build_reconstruction_stream(raw, filter.config);
            require(built.status == GeometryStatus::success && built.stream, built.diagnostic);
            filter.stream = std::move(built.stream);
        }
        auto submitted = filter.stream->submit(raw);
        require(submitted.status == GeometryStatus::success && submitted.frame && !submitted.busy,
                submitted.diagnostic);
        const auto info = submitted.frame->wait();
        const auto pixels = std::uint64_t(o.width) * o.height;
        require(info.status == ReconstructionStatus::complete && info.mode == filter.config.mode &&
                    info.width == o.width && info.height == o.height &&
                    info.source.epoch == source_frame.epoch &&
                    info.source.sample_index == source_frame.sample_index &&
                    info.counters.completed == pixels && !info.counters.failure,
                info.diagnostic.empty() ? "Comparison filter frame incomplete" : info.diagnostic);
        auto image = reconstructed_rgb(submitted.frame->capture(), pixels);
        line << (&filter == filters.data() ? "" : ",") << quote(filter.name)
             << ":{\"history_reset\":" << (info.history_reset ? "true" : "false")
             << ",\"gpu_seconds\":"
             << optional_number(gpu_time(info.gpu_seconds, info.gpu_time_valid))
             << ",\"physical\":";
        if (truth)
            metric_json(line, metric(image, *truth));
        else
            line << "null";
        line << '}';
        if (sample + 1 == o.frames) {
            auto attrs =
                metadata(o, std::string(source), reconstructed_kind(info.mode),
                         std::to_string(evaluation_seed(seed_index)), std::string(arm), sample);
            for (auto &[key, value] : attrs) {
                if (key == "rypath.qual.reconstruction")
                    value = reconstruction_name(info.mode);
                if (key == "rypath.frame.epoch")
                    value = std::to_string(info.source.epoch);
            }
            attrs.emplace_back("rypath.qual.filter", filter.name);
            output.exr(std::string(arm) + "-seed-" + std::to_string(seed_index) + "-final-" +
                           filter.name + ".exr",
                       image, o, attrs);
        }
    }
    line << '}';
}
SeedAggregate evaluate(const Scene &scene, const Options &o, const Reference &reference,
                       const Image &presentation_reference,
                       const std::optional<Reference> &end_reference,
                       const Image &end_presentation_reference, std::uint32_t seed_index,
                       Output &output, int mutations, MetalDeviceInfo &device) {
    SeedAggregate aggregate;
    aggregate.arm = mutations < 0 ? "independent" : "area-" + std::to_string(mutations);
    aggregate.reconstruction = reconstruction_name(o.reconstruction);
    aggregate.seed = evaluation_seed(seed_index);
    const auto source_id = identity(scene.value, o, aggregate.seed);
    const auto startup = Clock::now();
    auto config = configuration(o, aggregate.seed);
    config.estimator = mutations < 0 ? FrameEstimator::independent : FrameEstimator::area_replay;
    config.mutation_steps = mutations < 0 ? 0 : unsigned(mutations);
    config.mutation_policy = o.mutation_policy;
    config.area_stencil = o.area_stencil;
    config.area_output = o.area_output;
    if (mutations >= 0) {
        config.area_path_domain = o.area_path_domain;
        config.contribution_replay = o.contribution_replay;
        config.enhanced_history_cap = o.enhanced_history_cap;
        config.enhanced_suffix_cache = o.suffix_cache;
        config.compact_enhanced_pairs = o.compact_pairs;
        config.spatial_mode = o.spatial_mode;
        config.area_reuse = o.area_reuse;
        config.enhanced_temporal_reprojection = o.temporal_reprojection;
    }
    auto renderer = session(scene.value, config, device);
    aggregate.session_startup = elapsed(startup);
    std::string atlas_digest; // Freeze the first validated immutable asset identity.
    std::unique_ptr<ReconstructionStream> reconstruction;
    std::array<ComparisonFilter, 4> comparison{
        {{"temporal-h1-s0",
          {.mode = ReconstructionMode::temporal, .history_limit = 1, .spatial_iterations = 0}},
         {"temporal-h1-s5",
          {.mode = ReconstructionMode::temporal, .history_limit = 1, .spatial_iterations = 5}},
         {"temporal-h64-s0",
          {.mode = ReconstructionMode::temporal, .history_limit = 64, .spatial_iterations = 0}},
         {"metalfx-native", {.mode = ReconstructionMode::metalfx}}}};
    for (auto &filter : comparison)
        if (filter.config.mode == ReconstructionMode::temporal)
            filter.config.spatial_variance_policy = o.variance_policy;
    const auto pixels = std::uint64_t(o.width) * o.height;
    const unsigned excluded = o.frames > 4 ? 4 : 0;
    for (std::uint32_t sample = 0; sample < o.frames; ++sample) {
        const auto start = Clock::now();
        double wall = 0;
        std::optional<FrameInfo> raw_info;
        std::optional<ReconstructionInfo> reconstruction_info;
        const auto pose = pose_at(o, sample);
        try {
            const auto camera = o.motion_offset
                                    ? std::optional(camera_pose(scene.value, pose.offset))
                                    : std::nullopt;
            auto raw = frame(*renderer, sample, pixels, camera, pose.epoch);
            raw_info = raw->poll();
            const auto raw_wall = elapsed(start);
            std::string identity_diagnostic;
            const bool identity_valid = valid_frame_qualification_identity(
                config, *raw_info, scene.value.max_depth, &identity_diagnostic, atlas_digest);
            require(identity_valid, identity_diagnostic);
            if (raw_info->paired_atlas.version && atlas_digest.empty())
                atlas_digest = raw_info->paired_atlas.sha256;
            const bool creation = !reconstruction;
            if (creation) {
                const auto creation_start = Clock::now();
                auto built = build_reconstruction_stream(
                    *raw, {.mode = o.reconstruction, .spatial_variance_policy = o.variance_policy});
                aggregate.reconstruction_startup = elapsed(creation_start);
                require(built.status == GeometryStatus::success && built.stream, built.diagnostic);
                reconstruction = std::move(built.stream);
            }
            auto submitted = reconstruction->submit(*raw);
            require(submitted.status == GeometryStatus::success && submitted.frame &&
                        !submitted.busy,
                    submitted.diagnostic);
            reconstruction_info = submitted.frame->wait();
            wall = elapsed(start); // Freeze before either capture, metric calculation, or file I/O.
            require(reconstruction_info->status == ReconstructionStatus::complete &&
                        reconstruction_info->mode == o.reconstruction &&
                        reconstruction_info->width == o.width &&
                        reconstruction_info->height == o.height &&
                        reconstruction_info->counters.completed == pixels &&
                        !reconstruction_info->counters.failure,
                    reconstruction_info->diagnostic.empty() ? "Reconstruction count/status mismatch"
                                                            : reconstruction_info->diagnostic);
            const auto raw_gpu = gpu_time(raw_info->gpu_seconds, raw_info->gpu_time_valid);
            const auto reconstructed_gpu =
                gpu_time(reconstruction_info->gpu_seconds, reconstruction_info->gpu_time_valid);
            std::optional<double> pipeline_gpu;
            if (!creation && raw_gpu && reconstructed_gpu &&
                std::isfinite(*raw_gpu + *reconstructed_gpu))
                pipeline_gpu = *raw_gpu + *reconstructed_gpu;
            const auto raw_image = raw_rgb(raw->capture(), scene.value.model, pixels);
            const auto denoised = reconstructed_rgb(submitted.frame->capture(), pixels);
            std::optional<Metric> physical, reconstructed_physical, presentation;
            if (pose.metric_valid) {
                const auto &truth = pose.end_reference ? end_reference->mean : reference.mean;
                const auto &display_truth =
                    pose.end_reference ? end_presentation_reference : presentation_reference;
                physical = metric(raw_image, truth);
                reconstructed_physical = metric(denoised, truth);
                presentation = metric(denoised, display_truth, true);
            }
            auto line = json_stream();
            line << "{\"complete\":true,\"source_identity\":" << quote(source_id)
                 << ",\"seed\":" << aggregate.seed << ",\"arm\":" << quote(aggregate.arm)
                 << ",\"reconstruction\":" << quote(reconstruction_name(o.reconstruction))
                 << ",\"reconstructed_output_kind\":" << quote(reconstructed_kind(o.reconstruction))
                 << ",\"sample_index\":" << sample << ",\"phase\":" << quote(pose.phase)
                 << ",\"camera_offset_x\":" << pose.offset;
            write_camera(line, camera ? *camera : camera_pose(scene.value, 0));
            line << ",\"quality_reference_anchor\":"
                 << (pose.metric_valid ? quote(pose.end_reference ? "end" : "start") : "null")
                 << ",\"epoch\":" << pose.epoch
                 << ",\"warm\":" << (sample >= excluded ? "true" : "false")
                 << ",\"pipeline_wall_seconds\":" << wall << ",\"raw_wall_seconds\":" << raw_wall
                 << ",\"raw_gpu_seconds\":" << optional_number(raw_gpu)
                 << ",\"reconstruction_gpu_seconds\":" << optional_number(reconstructed_gpu)
                 << ",\"pipeline_gpu_seconds\":" << optional_number(pipeline_gpu)
                 << ",\"startup_gpu_probe_unmeasured\":" << (creation ? "true" : "false")
                 << ",\"completed_samples\":" << raw_info->completed_samples
                 << ",\"fresh_candidates\":" << raw_info->fresh_candidates
                 << ",\"completed_evaluations\":" << raw_info->completed_evaluations
                 << ",\"cached_evaluations\":" << raw_info->cached_evaluations
                 << ",\"analytic_null_evaluations\":" << raw_info->analytic_null_evaluations
                 << ",\"compacted_null_evaluations\":" << raw_info->compacted_null_evaluations
                 << ",\"enhanced_pair_compaction\":"
                 << (raw_info->enhanced_pair_compaction ? "true" : "false")
                 << ",\"requested_area_reuse\":" << quote(frame_area_reuse_name(o.area_reuse))
                 << ",\"area_reuse\":" << quote(frame_area_reuse_name(raw_info->area_reuse))
                 << ",\"requested_area_spatial_mode\":"
                 << quote(frame_spatial_mode_name(o.spatial_mode)) << ",\"area_spatial_mode\":"
                 << quote(frame_spatial_mode_name(raw_info->spatial_mode))
                 << ",\"stochastic_forward_atlas\":" << raw_info->stochastic_forward_atlas
                 << ",\"stochastic_canonical_atlas\":" << raw_info->stochastic_canonical_atlas
                 << ",\"enhanced_suffix_cache\":"
                 << (raw_info->enhanced_suffix_cache ? "true" : "false")
                 << ",\"enhanced_temporal_reprojection_enabled\":"
                 << (raw_info->enhanced_temporal_reprojection ? "true" : "false")
                 << ",\"enhanced_temporal_reprojection_used\":"
                 << (raw_info->enhanced_temporal_reprojection_used ? "true" : "false")
                 << ",\"area_path_domain\":"
                 << quote(mutations < 0 ? "not-applicable"
                                        : frame_area_path_domain_name(raw_info->area_path_domain))
                 << ",\"area_contribution_replay\":"
                 << quote(mutations < 0
                              ? "not-applicable"
                              : frame_contribution_replay_name(raw_info->contribution_replay))
                 << ",\"canonical_transport_version\":" << raw_info->canonical_transport_version
                 << ",\"fresh_discovery_version\":" << raw_info->fresh_discovery_version
                 << ",\"paired_atlas\":" << atlas_json(raw_info->paired_atlas)
                 << ",\"logical_evaluations\":" << raw_info->logical_evaluations
                 << ",\"temporal_evaluation_cache_used\":"
                 << (raw_info->temporal_evaluation_cache_used ? "true" : "false")
                 << ",\"mutation_attempts\":" << raw_info->mutation_attempts
                 << ",\"mutation_accepts\":" << raw_info->mutation_accepts
                 << ",\"raw_commands\":" << raw_info->command_count
                 << ",\"reconstruction_commands\":" << reconstruction_info->command_count
                 << ",\"area_contract\":" << (mutations < 0 ? 0 : area_wire::contract)
                 << ",\"area_output\":"
                 << quote(mutations < 0 ? "not-applicable" : area_output_name(o.area_output))
                 << ",\"area_target\":" << quote(mutations < 0 ? "none" : target_name(o))
                 << ",\"area_source_mis\":" << quote(mutations < 0 ? "none" : source_mis_name(o))
                 << ",\"area_temporal_reuse\":"
                 << frame_temporal_reuse_cap(config.estimator, config.area_reuse,
                                             config.enhanced_history_cap)

                 << ",\"requested_area_mutation_policy\":"
                 << quote(frame_mutation_policy_name(o.mutation_policy))
                 << ",\"requested_area_stencil\":" << quote(frame_area_stencil_name(o.area_stencil))
                 << ",\"area_mutation_policy\":"
                 << quote(mutations < 0 ? "not-applicable"
                                        : frame_mutation_policy_name(raw_info->mutation_policy))
                 << ",\"area_stencil\":"
                 << quote(mutations < 0 ? "not-applicable"
                                        : frame_area_stencil_name(raw_info->area_stencil))
                 << ",\"area_stencil_offsets\":[" << raw_info->area_stencil_offsets[0] << ','
                 << raw_info->area_stencil_offsets[1] << ',' << raw_info->area_stencil_offsets[2]
                 << ',' << raw_info->area_stencil_offsets[3] << ']'
                 << ",\"area_serial\":" << raw_info->area_serial
                 << ",\"history_generation\":" << raw_info->history_generation
                 << ",\"history_used\":" << (raw_info->history_used ? "true" : "false")
                 << ",\"temporal_candidates\":" << raw_info->temporal_candidates
                 << ",\"spatial_candidates\":" << raw_info->spatial_candidates
                 << ",\"reconstruction_reset\":"
                 << (reconstruction_info->history_reset ? "true" : "false")
                 << ",\"temporal_history_accepted\":"
                 << (o.reconstruction == ReconstructionMode::temporal
                         ? std::to_string(reconstruction_info->history_accepted)
                         : "null")
                 << ",\"temporal_history_rejected\":"
                 << (o.reconstruction == ReconstructionMode::temporal
                         ? std::to_string(reconstruction_info->history_rejected)
                         : "null")
                 << ",\"defocus_approximation\":"
                 << (reconstruction_info->defocus_approximation ? "true" : "false")
                 << ",\"guide_counters\":";
            write_counters(line, reconstruction_info->counters);
            line << ",\"raw_physical\":";
            if (physical)
                metric_json(line, *physical);
            else
                line << "null";
            line << ",\"reconstructed_physical\":";
            if (reconstructed_physical)
                metric_json(line, *reconstructed_physical);
            else
                line << "null";
            line << ",\"reconstructed_presentation\":";
            if (presentation)
                metric_json(line, *presentation);
            else
                line << "null";
            if (o.compare_reconstruction) {
                const Image *truth =
                    pose.metric_valid ? &(pose.end_reference ? end_reference->mean : reference.mean)
                                      : nullptr;
                compare_filters(line, comparison, *raw, *raw_info, truth, o, output, aggregate.arm,
                                source_id, seed_index, sample);
            }
            line << "}\n";
            if (sample >= excluded) {
                ++aggregate.warm_frames;
                if (physical) {
                    ++aggregate.metric_frames;
                    aggregate.raw_mse += physical->mse;
                    aggregate.reconstructed_mse += reconstructed_physical->mse;
                    aggregate.presentation_mse += presentation->mse;
                }
                aggregate.wall += wall;
                if (pipeline_gpu) {
                    ++aggregate.valid_gpu_frames;
                    aggregate.gpu += *pipeline_gpu;
                }
                if (physical)
                    for (unsigned c = 0; c < 3; ++c) {
                        aggregate.raw_bias[c] += physical->signed_channel_mean[c];
                        aggregate.reconstructed_bias[c] +=
                            reconstructed_physical->signed_channel_mean[c];
                    }
            }
            std::vector<std::pair<const char *, std::uint32_t>> endpoints{{"cold", 0},
                                                                          {"final", o.frames - 1}};
            if (o.motion_offset) {
                endpoints.emplace_back("end-hold-first", o.frames / 2);
                endpoints.emplace_back("end-hold-last", 3 * (o.frames / 4) - 1);
            }
            for (const auto &[kind, endpoint] : endpoints)
                if (sample == endpoint) {
                    const auto stem =
                        aggregate.arm + "-seed-" + std::to_string(seed_index) + "-" + kind;
                    auto attributes =
                        metadata(o, source_id, "physical-raw-linear-rgb",
                                 std::to_string(aggregate.seed), aggregate.arm, sample);
                    attributes.emplace_back("rypath.camera.offset_x", optional_number(pose.offset));
                    attributes.emplace_back("rypath.motion.phase", pose.phase);
                    for (auto &[key, value] : attributes)
                        if (key == "rypath.frame.epoch")
                            value = std::to_string(pose.epoch);
                    attributes.emplace_back(
                        "rypath.area.contract",
                        std::to_string(mutations < 0 ? 0 : area_wire::contract));
                    attributes.emplace_back("rypath.area.output",
                                            mutations < 0 ? "not-applicable"
                                                          : area_output_name(o.area_output));
                    attributes.emplace_back("rypath.area.target",
                                            mutations < 0 ? "none" : target_name(o));
                    attributes.emplace_back("rypath.area.source_mis",
                                            mutations < 0 ? "none" : source_mis_name(o));
                    attributes.emplace_back(
                        "rypath.area.path_domain",
                        mutations < 0 ? "not-applicable"
                                      : frame_area_path_domain_name(raw_info->area_path_domain));
                    attributes.emplace_back("rypath.area.contribution_replay",
                                            mutations < 0 ? "not-applicable"
                                                          : frame_contribution_replay_name(
                                                                raw_info->contribution_replay));
                    attributes.emplace_back("rypath.area.canonical_transport_version",
                                            std::to_string(raw_info->canonical_transport_version));
                    attributes.emplace_back("rypath.area.fresh_discovery_version",
                                            std::to_string(raw_info->fresh_discovery_version));
                    attributes.emplace_back("rypath.area.reuse",
                                            frame_area_reuse_name(raw_info->area_reuse));
                    attributes.emplace_back("rypath.area.requested_spatial_mode",
                                            frame_spatial_mode_name(o.spatial_mode));
                    attributes.emplace_back("rypath.area.spatial_mode",
                                            frame_spatial_mode_name(raw_info->spatial_mode));
                    attributes.emplace_back("rypath.area.stochastic_forward_atlas",
                                            std::to_string(raw_info->stochastic_forward_atlas));
                    attributes.emplace_back("rypath.area.stochastic_canonical_atlas",
                                            std::to_string(raw_info->stochastic_canonical_atlas));
                    attributes.emplace_back("rypath.area.paired_atlas",
                                            atlas_json(raw_info->paired_atlas));
                    attributes.emplace_back("rypath.area.suffix_cache_enabled",
                                            raw_info->enhanced_suffix_cache ? "true" : "false");
                    attributes.emplace_back("rypath.area.pair_compaction_enabled",
                                            raw_info->enhanced_pair_compaction ? "true" : "false");
                    attributes.emplace_back("rypath.area.temporal_reprojection_enabled",
                                            raw_info->enhanced_temporal_reprojection ? "true"
                                                                                     : "false");
                    attributes.emplace_back(
                        "rypath.area.temporal_reprojection_used",
                        raw_info->enhanced_temporal_reprojection_used ? "true" : "false");
                    attributes.emplace_back("rypath.recon.variance_policy",
                                            variance_name(o.variance_policy));
                    attributes.emplace_back("rypath.frame.resource_profile",
                                            std::string(resource_profile_name(o.resource_profile)));
                    attributes.emplace_back(
                        "rypath.area.temporal_reuse",
                        std::to_string(frame_temporal_reuse_cap(config.estimator, config.area_reuse,
                                                                config.enhanced_history_cap)));
                    attributes.emplace_back("rypath.area.mutation_steps",
                                            std::to_string(mutations < 0 ? 0 : mutations));
                    attributes.emplace_back("rypath.hero.requested",
                                            o.hero_wavelengths
                                                ? hero_wavelength_sampling_name(*o.hero_wavelengths)
                                                : "default");
                    attributes.emplace_back(
                        "rypath.hero.effective",
                        scene.value.spectral_transport == SpectralTransport::hero
                            ? hero_wavelength_sampling_name(scene.value.hero_wavelengths)
                            : "not-applicable");
                    attributes.emplace_back("rypath.area.requested_mutation",
                                            frame_mutation_policy_name(o.mutation_policy));
                    attributes.emplace_back("rypath.area.requested_stencil",
                                            frame_area_stencil_name(o.area_stencil));
                    attributes.emplace_back(
                        "rypath.area.mutation_policy",
                        mutations < 0 ? "not-applicable"
                                      : frame_mutation_policy_name(raw_info->mutation_policy));
                    attributes.emplace_back("rypath.area.stencil",
                                            mutations < 0
                                                ? "not-applicable"
                                                : frame_area_stencil_name(raw_info->area_stencil));
                    attributes.emplace_back(
                        "rypath.area.stencil_offsets",
                        std::to_string(raw_info->area_stencil_offsets[0]) + "," +
                            std::to_string(raw_info->area_stencil_offsets[1]) + "," +
                            std::to_string(raw_info->area_stencil_offsets[2]) + "," +
                            std::to_string(raw_info->area_stencil_offsets[3]));
                    output.exr(stem + "-raw.exr", raw_image, o, attributes);
                    attributes.front().second = reconstructed_kind(o.reconstruction);
                    output.exr(stem + "-" + reconstruction_name(o.reconstruction) + "-denoised.exr",
                               denoised, o, attributes);
                }
            output.frame(line.str());
        } catch (const std::exception &error) {
            auto line = json_stream();
            line << "{\"complete\":false,\"seed\":" << aggregate.seed
                 << ",\"arm\":" << quote(aggregate.arm) << ",\"sample_index\":" << sample
                 << ",\"reconstruction\":" << quote(reconstruction_name(o.reconstruction))
                 << ",\"phase\":" << quote(pose.phase) << ",\"epoch\":" << pose.epoch
                 << ",\"camera_offset_x\":" << pose.offset
                 << ",\"pipeline_wall_seconds\":" << (wall != 0 ? wall : elapsed(start))
                 << ",\"diagnostic\":" << quote(error.what()) << ",\"guide_counters\":";
            write_counters(line, reconstruction_info ? reconstruction_info->counters
                                                     : ReconstructionCounters{});
            line << "}\n";
            output.frame(line.str());
            throw;
        }
    }
    require(aggregate.metric_frames > 0, "No eligible warm anchor metrics");
    require(aggregate.warm_frames == o.frames - excluded, "Warm-frame aggregation count mismatch");
    output.seed(aggregate.json());
    return aggregate;
}
struct Statistic {
    std::uint32_t count = 0;
    double mean = 0, m2 = 0;
    void add(double value) {
        ++count;
        const auto delta = value - mean;
        mean += delta / count;
        m2 += delta * (value - mean);
    }
    std::string json() const {
        auto out = json_stream();
        out << "{\"independent_seed_count\":" << count
            << ",\"mean\":" << (count ? optional_number(mean) : "null")
            << ",\"standard_error_across_seed_means\":"
            << optional_number(count > 1 ? std::optional(std::sqrt(m2 / (count - 1) / count))
                                         : std::nullopt)
            << '}';
        return out.str();
    }
};
std::string settings_json(const Options &o) {
    auto out = json_stream();
    out << "{\"scene\":" << quote(o.scene.string()) << ",\"width\":" << o.width
        << ",\"height\":" << o.height << ",\"frames\":" << o.frames << ",\"seeds\":" << o.seeds
        << ",\"area_contract\":" << area_wire::contract
        << ",\"area_output\":" << quote(area_output_name(o.area_output))
        << ",\"area_target\":" << quote(target_name(o))
        << ",\"area_source_mis\":" << quote(source_mis_name(o))
        << ",\"requested_area_history_cap\":" << o.enhanced_history_cap
        << ",\"area_temporal_reuse\":"
        << frame_temporal_reuse_cap(FrameEstimator::area_replay, o.area_reuse,
                                    o.enhanced_history_cap)
        << ",\"requested_area_path_domain\":"
        << quote(frame_area_path_domain_name(o.area_path_domain))
        << ",\"requested_area_contribution_replay\":"
        << quote(frame_contribution_replay_name(o.contribution_replay))
        << ",\"requested_area_suffix_cache\":" << (o.suffix_cache ? "true" : "false")
        << ",\"requested_area_pair_compaction\":" << (o.compact_pairs ? "true" : "false")
        << ",\"requested_area_temporal_reprojection\":"
        << quote(o.temporal_reprojection ? "dual" : "off")
        << ",\"requested_area_reuse\":" << quote(frame_area_reuse_name(o.area_reuse))
        << ",\"requested_area_spatial_mode\":" << quote(frame_spatial_mode_name(o.spatial_mode))
        << ",\"requested_area_stencil\":" << quote(frame_area_stencil_name(o.area_stencil))
        << ",\"requested_area_mutation_policy\":"
        << quote(frame_mutation_policy_name(o.mutation_policy))
        << ",\"resource_profile\":" << quote(resource_profile_name(o.resource_profile))
        << ",\"variance_policy\":" << quote(variance_name(o.variance_policy))
        << ",\"mutation_arms\":"
        << quote(o.arms.size() == 2   ? "0"
                 : o.arms.size() == 3 ? "0,1"
                                      : "0,1,2,4")
        << ",\"reference_spp\":" << o.reference_spp
        << ",\"reference_batches\":" << o.reference_batches
        << ",\"reference_spp_per_batch\":" << o.reference_spp / o.reference_batches
        << ",\"warm_frames_excluded\":" << (o.frames > 4 ? 4 : 0)
        << ",\"intersection_profile\":\"native\",\"execution\":\"wavefront\",\"footprint_samples\":"
           "1,\"material_seed\":0"
        << ",\"sampler\":\"independent\",\"target_scale\":1,\"reconstruction\":"
        << quote(reconstruction_name(o.reconstruction))
        << ",\"same_frame_reconstruction_comparison\":"
        << (o.compare_reconstruction ? "true" : "false")
        << ",\"reconstruction_resolution\":\"native\",\"reconstructed_output_kind\":"
        << quote(reconstructed_kind(o.reconstruction)) << ",\"reconstructed_storage\":"
        << quote(o.reconstruction == ReconstructionMode::temporal ? "signed-rgba32-float"
                                                                  : "metalfx-rgba16-float")
        << ",\"reconstruction_color_policy\":"
        << quote(o.reconstruction == ReconstructionMode::temporal
                     ? "signed RGB; no presentation clamp"
                     : "negative/high RGB clamped to [0,65504] before MetalFX")
        << ",\"reconstruction_substitution\":false,\"exposure\":1"
        << ",\"motion_offset\":" << optional_number(o.motion_offset) << ",\"metric_scope\":"
        << quote(o.motion_offset
                     ? "anchor-only mixed-pose warm-frame means; translation metrics null"
                     : "static warm-frame means")
        << ",\"motion_phase_schedule\":\"equal quarters: hold-start, translate fractions 1/q..1, "
           "hold-end, cut-start epoch 1; all translation metrics null\""
        << ",\"motion_limitations\":\"translation only; fixed orientation/lens; no distinct medium "
           "interfaces\""
        << ",\"reference_end_seeds\":[";
    if (o.motion_offset)
        for (unsigned i = 0; i < o.reference_batches; ++i) {
            if (i)
                out << ',';
            out << reference_seed(i, true);
        }
    out << "],\"reference_seeds\":[";
    for (unsigned i = 0; i < o.reference_batches; ++i) {
        if (i)
            out << ',';
        out << reference_seed(i);
    }
    out << "],\"evaluation_seeds\":[";
    for (unsigned i = 0; i < o.seeds; ++i) {
        if (i)
            out << ',';
        out << evaluation_seed(i);
    }
    out << "],\"arms\":[";
    for (std::size_t i = 0; i < o.arms.size(); ++i) {
        if (i)
            out << ',';
        out << quote(o.arms[i] < 0 ? "independent" : "area-" + std::to_string(o.arms[i]));
    }
    out << "]}";
    return out.str();
}
std::string summary_json(const Options &o, const std::vector<SeedAggregate> &aggregates,
                         const std::optional<Reference> &reference,
                         const std::optional<Reference> &end_reference,
                         const MetalDeviceInfo &device, const std::string &source_id, double wall,
                         std::string_view diagnostic, bool complete) {
    auto out = json_stream();
    out << "{\"schema\":2,\"complete\":" << (complete ? "true" : "false")
        << ",\"quality_pass_claimed\":false,\"scope\":\"static/translation anchor measurement; not "
           "a "
           "quality acceptance gate\""
        << ",\"diagnostic\":" << quote(diagnostic) << ",\"settings\":" << settings_json(o)
        << ",\"source_identity\":" << quote(source_id)
        << ",\"build\":" << quote(RYPATH_BUILD_DESCRIPTION) << ",\"device\":" << quote(device.name)
        << ",\"registry_id\":" << device.registry_id
        << ",\"total_wall_seconds_including_readbacks\":" << wall
        << ",\"completed_seed_arm_runs\":" << aggregates.size()
        << ",\"reference_complete\":" << (reference ? "true" : "false")
        << ",\"reference_noise_floor_physical_rgb_mse\":"
        << (reference ? optional_number(reference->noise_floor_mse) : "null")
        << ",\"end_reference_complete\":" << (end_reference ? "true" : "false")
        << ",\"end_reference_noise_floor_physical_rgb_mse\":"
        << (end_reference ? optional_number(end_reference->noise_floor_mse) : "null")
        << ",\"reference_uncertainty_method\":\"per-pixel independent batch-mean sample variance "
           "divided by batch count; retained variance-of-mean EXR\""
        << ",\"physical_metric\":\"signed unexposed RGB against independent signed RGB reference; "
           "no presentation clamp\""
        << ",\"presentation_reference\":\"clamp aggregate physical RGB reference to [0,65504]; its "
           "nonlinear uncertainty is not estimated\""
        << ",\"presentation_metric\":\"separately clamp reconstructed RGB and aggregate physical "
           "RGB reference to [0,65504]; nonlinear reference uncertainty is not estimated\""
        << ",\"history_telemetry\":\"temporal accepted/rejected counts only; null for MetalFX, "
           "whose zero counters do not report matches\""
        << ",\"statistical_unit\":\"independent seed/session; correlated warm frames averaged "
           "inside seed first\""
        << ",\"seed_standard_errors\":\"descriptive and conditional on the shared finite-SPP "
           "reference, not simultaneous confidence bounds\""
        << ",\"timing\":\"raw start through reconstruction completion before captures; first-frame "
           "reconstruction creation included in wall; startup GPU probe not separately timed, so "
           "its pipeline GPU is null\""
        << ",\"tone_map_mse\":null,\"arms\":[";
    bool first = true;
    for (const auto mutations : o.arms) {
        const auto arm =
            mutations < 0 ? std::string("independent") : "area-" + std::to_string(mutations);
        Statistic raw, denoised, presentation, latency;
        std::uint64_t metric_frames = 0, timed_frames = 0;
        std::array<Statistic, 3> raw_bias, denoised_bias;
        for (const auto &seed : aggregates)
            if (seed.arm == arm) {
                metric_frames += seed.metric_frames;
                timed_frames += seed.warm_frames;
                raw.add(seed.raw_mse / seed.metric_frames);
                denoised.add(seed.reconstructed_mse / seed.metric_frames);
                presentation.add(seed.presentation_mse / seed.metric_frames);
                latency.add(seed.wall / seed.warm_frames);
                for (unsigned c = 0; c < 3; ++c) {
                    raw_bias[c].add(seed.raw_bias[c] / seed.metric_frames);
                    denoised_bias[c].add(seed.reconstructed_bias[c] / seed.metric_frames);
                }
            }
        if (!first)
            out << ',';
        first = false;
        out << "{\"arm\":" << quote(arm) << ",\"valid_metric_frames\":" << metric_frames
            << ",\"timed_warm_frames\":" << timed_frames << ",\"raw_mse\":" << raw.json()
            << ",\"reconstructed_physical_mse\":" << denoised.json()
            << ",\"reconstructed_presentation_mse\":" << presentation.json()
            << ",\"pipeline_wall_seconds\":" << latency.json() << ",\"raw_signed_channel_means\":[";
        for (unsigned c = 0; c < 3; ++c) {
            if (c)
                out << ',';
            out << raw_bias[c].json();
        }
        out << "],\"reconstructed_signed_channel_means\":[";
        for (unsigned c = 0; c < 3; ++c) {
            if (c)
                out << ',';
            out << denoised_bias[c].json();
        }
        out << "]}";
    }
    out << "]}\n";
    return out.str();
}
int run(const Options &o) {
    const auto start = Clock::now();
    std::unique_ptr<Output> output;
    std::optional<Reference> reference_image, end_reference;
    std::vector<SeedAggregate> aggregates;
    MetalDeviceInfo device;
    std::string source_id;
    try {
        output = std::make_unique<Output>(o.output_dir);
        const auto scene = load_scene(o);
        if (o.motion_offset) {
            // Validate both anchors and the smallest translation before any GPU work.
            (void)camera_pose(scene.value, 0);
            (void)camera_pose(scene.value, *o.motion_offset);
            const auto quarter_frames = o.frames / 4;
            (void)camera_pose(scene.value, float(double(*o.motion_offset) / quarter_frames));
        }
        source_id = identity(scene.value, o, 1);
        auto manifest = json_stream();
        manifest << "{\"schema\":2,\"complete\":false,\"status\":\"running; summary.json "
                    "determines final completion\",\"settings\":"
                 << settings_json(o) << ",\"source_identity\":" << quote(source_id)
                 << ",\"build\":" << quote(RYPATH_BUILD_DESCRIPTION) << ",\"sensor_model\":"
                 << quote(scene.value.model == RenderModel::pbrt ? "pbrt" : "demo")

                 << ",\"requested_hero_wavelengths\":"
                 << quote(o.hero_wavelengths ? hero_wavelength_sampling_name(*o.hero_wavelengths)
                                             : "default")
                 << ",\"hero_wavelengths\":"
                 << quote(scene.value.spectral_transport == SpectralTransport::hero
                              ? hero_wavelength_sampling_name(scene.value.hero_wavelengths)
                              : "not-applicable")
                 << ",\"spectral_transport\":"
                 << quote(spectral_transport_name(scene.value.spectral_transport))
                 << ",\"max_depth\":" << scene.value.max_depth
                 << ",\"raw_unclamped\":true,\"provenance\":[";
        for (std::size_t i = 0; i < scene.provenance.size(); ++i) {
            if (i)
                manifest << ',';
            manifest << quote(scene.provenance[i]);
        }
        manifest << "],\"dependencies\":[";
        for (std::size_t i = 0; i < scene.dependencies.size(); ++i) {
            if (i)
                manifest << ',';
            manifest << quote(scene.dependencies[i].string());
        }
        manifest << "]}\n";
        output->text("manifest.json", manifest.str());
        reference_image = reference(scene, o, *output, device);
        if (o.motion_offset)
            end_reference = reference(scene, o, *output, device, true);
        Image end_presentation = end_reference ? end_reference->mean : Image{};
        for (auto &pixel : end_presentation)
            for (auto &value : pixel)
                value = std::clamp(value, 0., 65504.);
        Image presentation = reference_image->mean;
        for (auto &pixel : presentation)
            for (auto &value : pixel)
                value = std::clamp(value, 0., 65504.);
        // Rotate arm execution order across seeds to expose rather than freeze an order effect.
        const auto &arms = o.arms;
        for (unsigned seed = 0; seed < o.seeds; ++seed)
            for (unsigned i = 0; i < arms.size(); ++i)
                aggregates.push_back(evaluate(scene, o, *reference_image, presentation,
                                              end_reference, end_presentation, seed, *output,
                                              arms[(i + seed) % arms.size()], device));
        require(aggregates.size() == std::uint64_t(o.seeds) * arms.size(),
                "Completed seed-arm count mismatch");
        output->text("summary.json", summary_json(o, aggregates, reference_image, end_reference,
                                                  device, source_id, elapsed(start), {}, true));
        std::cout << "Pipeline measurements complete; inspect reference uncertainty and "
                     "seed aggregates\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Pipeline qualification: " << error.what() << '\n';
        if (output) {
            try {
                output->text("summary.json",
                             summary_json(o, aggregates, reference_image, end_reference, device,
                                          source_id, elapsed(start), error.what(), false));
            } catch (const std::exception &publication) {
                std::cerr << "Failure-summary publication: " << publication.what() << '\n';
            }
        }
        return 1;
    }
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage;
        return 0;
    }
    try {
        return run(parse(argc, argv));
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n' << usage;
        return 1;
    }
}
