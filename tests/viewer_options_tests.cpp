// SPDX-License-Identifier: GPL-3.0-only
#include "viewer_options.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Parsed {
    viewer::Options options;
    std::string diagnostic;
};
Parsed parse(std::initializer_list<std::string> arguments) {
    std::vector<std::string> storage{"rypath-viewer"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv;
    for (auto &argument : storage)
        argv.push_back(argument.data());
    Parsed result;
    result.diagnostic =
        viewer::parse_options(static_cast<int>(argv.size()), argv.data(), result.options);
    return result;
}
viewer::Options accepted(std::initializer_list<std::string> arguments) {
    auto result = parse(arguments);
    require(result.diagnostic.empty(), "Expected accepted options: " + result.diagnostic);
    return result.options;
}
void rejected(std::initializer_list<std::string> arguments, const std::string &reason) {
    require(!parse(arguments).diagnostic.empty(), "Invalid options accepted: " + reason);
}
void defaults_and_modes() {
    const auto defaults = accepted({"--demo"});
    require(defaults.demo && defaults.scene.empty() && !defaults.help, "Demo selection defaults");
    require(defaults.width == 1920 && defaults.height == 1080,
            "Viewer defaults to fixed native 1080p internal resolution");
    require(defaults.estimator == FrameEstimator::area_replay && defaults.mutation_steps == 1 &&
                defaults.reconstruction == ReconstructionMode::metalfx,
            "Viewer defaults to Area replay with one mutation and MetalFX");
    require(defaults.frames == 0 && defaults.seed == 1 && defaults.exposure_ev == 0 &&
                defaults.near_plane == .001 && defaults.capture_directory.empty(),
            "Default frame limit, seed, presentation, and capture path");
    for (const auto steps : {0u, 1u, 2u, 4u}) {
        const auto area = accepted({"--demo", "--mutations", std::to_string(steps)});
        require(area.mutation_steps == steps, "Supported mutation count preserved");
    }
    const auto independent = accepted({"--demo", "--estimator", "independent", "--mutations", "4",
                                       "--reconstruction", "raw-rgb"});
    require(
        independent.estimator == FrameEstimator::independent &&
            independent.reconstruction == ReconstructionMode::raw_rgb,
        "Independent/raw-RGB options are accepted; effective mutation policy belongs to rendering");
    rejected({}, "scene selection is required");
    rejected({"--scene", ""}, "empty scene is not a source");
    rejected({"--demo", "--scene", "scene.pbrt"}, "scene/demo are mutually exclusive");
    rejected({"--scene", "scene.pbrt", "--demo"}, "source XOR is independent of option order");
    rejected({"--demo", "--estimator", "bdpt"}, "unknown estimator");
    rejected({"--demo", "--reconstruction", "none"},
             "viewer reconstruction has two supported modes");
    for (const auto value : {"3", "5", "-1", "4294967296", "1x"})
        rejected({"--demo", "--mutations", value}, "unsupported or malformed mutation count");
}
void integer_boundaries() {
    const auto maximum = accepted({"--demo", "--width", "16384", "--height", "16384"});
    require(maximum.width == reconstruction_dimension_limit &&
                maximum.height == reconstruction_dimension_limit,
            "Supported dimension endpoints are accepted without allocating an image");
    const auto minimum =
        accepted({"--demo", "--width", "1", "--height", "1", "--frames", "1", "--seed", "0"});
    require(minimum.width == 1 && minimum.height == 1 && minimum.frames == 1 && minimum.seed == 0,
            "Positive dimension/frame minimum and zero seed");
    for (const auto key : {"--width", "--height"})
        for (const auto value : {"0", "-1", "16385", "4294967296", "12x"})
            rejected({"--demo", key, value}, "invalid or oversized dimension");
    const auto wide =
        accepted({"--demo", "--frames", "18446744073709551615", "--seed", "18446744073709551615"});
    require(wide.frames == std::numeric_limits<std::uint64_t>::max() &&
                wide.seed == std::numeric_limits<std::uint64_t>::max(),
            "Full uint64 frame/seed domain is preserved by parsing");
    for (const auto value : {"0", "-1", "18446744073709551616", "2.5", "8x"})
        rejected({"--demo", "--frames", value},
                 "explicit frame limits must be positive uint64 values");
    rejected({"--demo", "--seed", "-1"}, "negative unsigned seed");
    rejected({"--demo", "--seed", "18446744073709551616"}, "seed overflow");
}
void real_boundaries() {
    for (const auto value : {"-24", "0", "24"})
        (void)accepted({"--demo", "--exposure", value});
    for (const auto value : {"-24.01", "24.01", "nan", "inf", "1e309", "1x"})
        rejected({"--demo", "--exposure", value},
                 "exposure must be finite and within its EV range");
    const auto normal_minimum = accepted({"--demo", "--near-plane", "1.17549435e-38"});
    require(std::isnormal(static_cast<float>(normal_minimum.near_plane)),
            "The smallest normal float near plane survives double parsing");
    (void)accepted({"--demo", "--near-plane", "3.4028234e38"});
    for (const auto value : {"0", "-0.01", "1e-45", "1e-100", "1e100", "nan", "inf", "0.1x"})
        rejected({"--demo", "--near-plane", value},
                 "near plane must pack to a positive normal float");
}
void syntax_and_help() {
    const auto help = accepted({"--help"});
    require(help.help, "Help does not require a scene");
    const std::string usage = viewer::usage();
    require(usage.find("--scene") != std::string::npos &&
                usage.find("--demo") != std::string::npos &&
                usage.find("--frames") != std::string::npos,
            "Help describes source selection and the finite frame limit");
    rejected({"--demo", "--demo"}, "duplicate flag");
    rejected({"--demo", "--width", "1", "--width", "2"}, "duplicate valued option");
    rejected({"--demo", "--not-an-option", "1"}, "unknown option");
    rejected({"--demo", "--frames"}, "missing final value");
    rejected({"--width", "--demo"}, "another option cannot supply a missing value");
    rejected({"--demo", "--capture-dir", ""}, "empty capture directory");
}
void paths_are_only_parsed() {
    const auto root = fs::temp_directory_path() /
                      ("rypath-viewer-options-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(fs::create_directory(root), "Create isolated path-test directory");
    struct Cleanup {
        fs::path root;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(root, error);
        }
    } cleanup{root};
    const auto scene = root / "missing scene.pbrt";
    const auto captures = root / "not-created" / "capture folder";
    const auto options = accepted({"--scene", scene.string(), "--capture-dir", captures.string()});
    require(options.scene == scene && options.capture_directory == captures && !options.demo,
            "Scene and capture paths preserve argv strings including spaces");
    require(!fs::exists(scene) && !fs::exists(captures.parent_path()) && fs::is_empty(root),
            "Parsing must not open the missing scene or create capture directories/files");
}
} // namespace
int main() {
    try {
        defaults_and_modes();
        integer_boundaries();
        real_boundaries();
        syntax_and_help();
        paths_are_only_parsed();
        std::cout << "Viewer option parsing checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Viewer option parsing: " << error.what() << '\n';
        return 1;
    }
}
