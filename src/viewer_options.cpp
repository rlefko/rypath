// SPDX-License-Identifier: GPL-3.0-only
#include "viewer_options.hpp"
#include <charconv>
#include <cmath>
#include <locale>
#include <set>
#include <sstream>

namespace rypath::viewer {
namespace {
template <class T> bool integer(std::string_view value, T &output) {
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
bool real(std::string_view text, double &output) {
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> output;
    return stream && stream.eof() && std::isfinite(output);
}
} // namespace
const char *usage() {
    return "Usage: rypath-viewer (--scene SCENE.pbrt | --demo) [options]\n"
           "  --width 1920 --height 1080  fixed internal resolution\n"
           "  --mode temporal|metalfx|raw  (default: temporal, native resolution)\n"
           "  --reconstruction temporal|metalfx|raw-rgb  alternate option spelling\n"
           "  --estimator independent|area-replay  (default: independent)\n"
           "  --spectral-transport auto|scalar|packet|hero --hero-wavelengths uniform|visible\n"
           "  --area-mutation-policy auto|legacy|spectral-refresh --area-stencil "
           "auto|adjacent|reciprocal16\n"
           "  --mutations 0|1|2|4  (default: 1 for Area; independent uses zero)\n"
           "  --frames N  exit after N completed pipeline frames; omitted means unlimited\n"
           "  --seed 1 --exposure 0 --near-plane .001 --capture-dir DIR (default: "
           "Pictures/rypath)\n"
           "  WASD/QE move; Shift moves faster; drag mouse/arrows rotate; Home resets\n"
           "  R toggles raw/selected denoiser; +/- changes display EV; Cmd+S saves fresh EXRs\n"
           "  Esc closes; fullscreen is available from the View menu\n"
           "One fresh path/pixel/frame; area replay adds corrected evaluations.\n";
}
ReconstructionMode preferred_denoiser(ReconstructionMode selected) {
    return selected == ReconstructionMode::raw_rgb ? ReconstructionMode::temporal : selected;
}
ReconstructionMode toggle_comparison(ReconstructionMode current, ReconstructionMode preferred) {
    return current == ReconstructionMode::raw_rgb ? preferred_denoiser(preferred)
                                                  : ReconstructionMode::raw_rgb;
}
std::string parse_options(int argc, char **argv, Options &o) {
    bool mode_seen = false;
    std::set<std::string_view> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (!seen.insert(key).second)
            return "duplicate option: " + std::string(key);
        if (key == "--help") {
            o.help = true;
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
        else if (key == "--capture-dir") {
            if (value.empty())
                return "capture directory must not be empty";
            o.capture_directory = value;
        } else if (key == "--width") {
            if (!integer(value, o.width) || !o.width)
                return "invalid width";
        } else if (key == "--height") {
            if (!integer(value, o.height) || !o.height)
                return "invalid height";
        } else if (key == "--frames") {
            if (!integer(value, o.frames) || !o.frames)
                return "frames must be positive";
        } else if (key == "--seed") {
            if (!integer(value, o.seed))
                return "invalid seed";
        } else if (key == "--spectral-transport") {
            const auto parsed = parse_spectral_transport(value);
            if (!parsed)
                return "spectral transport must be auto, scalar, packet, or hero";
            o.spectral_transport = *parsed;
        } else if (key == "--hero-wavelengths") {
            o.hero_wavelengths = parse_hero_wavelength_sampling(value);
            if (!o.hero_wavelengths)
                return "hero wavelengths must be uniform or visible";
        } else if (key == "--area-mutation-policy") {
            if (value == "auto")
                o.mutation_policy = FrameMutationPolicy::automatic;
            else if (value == "legacy")
                o.mutation_policy = FrameMutationPolicy::legacy;
            else if (value == "spectral-refresh")
                o.mutation_policy = FrameMutationPolicy::spectral_refresh;
            else
                return "area mutation policy must be auto, legacy, or spectral-refresh";
        } else if (key == "--area-stencil") {
            if (value == "auto")
                o.area_stencil = FrameAreaStencil::automatic;
            else if (value == "adjacent")
                o.area_stencil = FrameAreaStencil::adjacent;
            else if (value == "reciprocal16")
                o.area_stencil = FrameAreaStencil::reciprocal16;
            else
                return "area stencil must be auto, adjacent, or reciprocal16";
        } else if (key == "--mutations") {
            if (!integer(value, o.mutation_steps) ||
                (o.mutation_steps != 0 && o.mutation_steps != 1 && o.mutation_steps != 2 &&
                 o.mutation_steps != 4))
                return "mutations must be 0, 1, 2, or 4";
        } else if (key == "--mode" || key == "--reconstruction") {
            if (mode_seen)
                return "choose reconstruction mode only once";
            mode_seen = true;
            if (value == "metalfx")
                o.reconstruction = ReconstructionMode::metalfx;
            else if (value == "temporal")
                o.reconstruction = ReconstructionMode::temporal;
            else if ((key == "--mode" && value == "raw") ||
                     (key == "--reconstruction" && value == "raw-rgb"))
                o.reconstruction = ReconstructionMode::raw_rgb;
            else
                return key == "--mode" ? "mode must be temporal, metalfx, or raw"
                                       : "reconstruction must be temporal, metalfx, or raw-rgb";
        } else if (key == "--estimator") {
            if (value == "area-replay")
                o.estimator = FrameEstimator::area_replay;
            else if (value == "independent")
                o.estimator = FrameEstimator::independent;
            else
                return "estimator must be area-replay or independent";
        } else if (key == "--exposure") {
            if (!real(value, o.exposure_ev) || std::abs(o.exposure_ev) > 24)
                return "exposure EV must be finite and in [-24,24]";
        } else if (key == "--near-plane") {
            if (!real(value, o.near_plane) || o.near_plane <= 0 ||
                !std::isnormal(float(o.near_plane)))
                return "near plane must pack to a positive normal float";
        } else
            return "unknown option: " + std::string(key);
    }
    if (!o.help && o.demo == !o.scene.empty())
        return "choose exactly one of --scene and --demo";
    if (o.width > reconstruction_dimension_limit || o.height > reconstruction_dimension_limit ||
        std::uint64_t(o.width) * o.height > UINT32_MAX)
        return "resolution exceeds reconstruction limits";
    return {};
}
} // namespace rypath::viewer
