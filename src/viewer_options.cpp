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
           "  --reconstruction metalfx|raw-rgb  (default: metalfx, native resolution)\n"
           "  --estimator area-replay|independent  (default: area-replay)\n"
           "  --mutations 0|1|2|4  (default: 1; independent uses zero)\n"
           "  --frames N  exit after N completed pipeline frames; omitted means unlimited\n"
           "  --seed 1 --exposure 0 --near-plane .001 --capture-dir captures\n"
           "  WASD/QE move; Shift moves faster; drag mouse/arrows rotate; Home resets\n"
           "  R toggles raw/MetalFX; +/- changes display EV; Cmd+S saves fresh EXRs\n"
           "  Esc closes; fullscreen is available from the View menu\n"
           "One fresh path/pixel/frame; area replay adds corrected evaluations.\n";
}
std::string parse_options(int argc, char **argv, Options &o) {
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
        } else if (key == "--mutations") {
            if (!integer(value, o.mutation_steps) ||
                (o.mutation_steps != 0 && o.mutation_steps != 1 && o.mutation_steps != 2 &&
                 o.mutation_steps != 4))
                return "mutations must be 0, 1, 2, or 4";
        } else if (key == "--reconstruction") {
            if (value == "metalfx")
                o.reconstruction = ReconstructionMode::metalfx;
            else if (value == "raw-rgb")
                o.reconstruction = ReconstructionMode::raw_rgb;
            else
                return "reconstruction must be metalfx or raw-rgb";
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
