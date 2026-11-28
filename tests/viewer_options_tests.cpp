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
void spectral_and_area_policies() {
    const auto o =
        accepted({"--demo", "--spectral-transport", "hero", "--hero-wavelengths", "uniform",
                  "--area-mutation-policy", "spectral-refresh", "--area-stencil", "reciprocal16"});
    require(o.spectral_transport == SpectralTransportSelection::hero &&
                o.hero_wavelengths == HeroWavelengthSampling::uniform &&
                o.mutation_policy == FrameMutationPolicy::spectral_refresh &&
                o.area_stencil == FrameAreaStencil::reciprocal16,
            "Spectral and Area policy selections preserved");
    const auto defaults = accepted({"--demo"});
    require(!defaults.hero_wavelengths &&
                defaults.mutation_policy == FrameMutationPolicy::automatic &&
                defaults.area_stencil == FrameAreaStencil::automatic,
            "Spectral and Area defaults remain explicit");
    rejected({"--demo", "--hero-wavelengths", "invalid"}, "Invalid wavelength sampler");
    rejected({"--demo", "--area-mutation-policy", "invalid"}, "Invalid mutation policy");
    rejected({"--demo", "--area-stencil", "invalid"}, "Invalid area stencil");
}
void enhanced_configuration() {
    const auto o = accepted({"--scene",
                             "fixture.pbrt",
                             "--estimator",
                             "area-replay",
                             "--spectral-transport",
                             "hero",
                             "--hero-wavelengths",
                             "visible",
                             "--area-path-domain",
                             "enhanced",
                             "--area-contribution-replay",
                             "prefix",
                             "--area-stencil",
                             "paired-gaussian16",
                             "--area-spatial-mode",
                             "stochastic-shared",
                             "--area-history-cap",
                             "3",
                             "--mutations",
                             "0",
                             "--resource-profile",
                             "large",
                             "--variance-policy",
                             "conservative"});
    const auto c = viewer::frame_configuration(o);
    require(c.area_path_domain == FrameAreaPathDomain::enhanced &&
                c.contribution_replay == FrameContributionReplay::prefix &&
                c.area_stencil == FrameAreaStencil::paired_gaussian16 &&
                c.spatial_mode == FrameSpatialMode::shared && c.enhanced_history_cap == 3 &&
                c.mutation_steps == 0 && c.width == 1920 && c.height == 1080 && c.seed == 1 &&
                c.footprint_samples == 1 && c.execution == FrameExecution::wavefront &&
                c.intersection_profile == IntersectionProfile::native,
            "Actual viewer FrameConfig preserves Enhanced shared0 and scheduling defaults");
    require(o.resource_profile == ResourceProfile::large, "Large scene parsing profile preserved");
    const auto r = viewer::reconstruction_configuration(o, ReconstructionMode::temporal);
    require(r.mode == ReconstructionMode::temporal &&
                r.spatial_variance_policy == SpatialVariancePolicy::conservative &&
                r.near_plane == o.near_plane && r.cache_surface_guides,
            "Actual viewer reconstruction preserves conservative variance");
    for (const auto mode : {"all", "stochastic-pairs", "stochastic-shared"}) {
        const auto chosen = accepted({"--scene", "fixture.pbrt", "--estimator", "area-replay",
                                      "--area-path-domain", "enhanced", "--area-stencil",
                                      "paired-gaussian16", "--area-spatial-mode", mode});
        require(std::string(frame_spatial_mode_name(
                    viewer::frame_configuration(chosen).spatial_mode)) == mode,
                "All spatial modes reach native viewer configuration");
    }
    const auto defaults = viewer::frame_configuration(accepted({"--demo"}));
    require(defaults.area_path_domain == FrameAreaPathDomain::summed &&
                defaults.contribution_replay == FrameContributionReplay::full &&
                defaults.spatial_mode == FrameSpatialMode::all &&
                defaults.enhanced_history_cap == 20 && defaults.mutation_steps == 0 &&
                defaults.history_cap_policy == FrameHistoryCapPolicy::fixed,
            "New opt-ins preserve independent and full-replay defaults");
    for (const auto cap : {"0", "21", "-1", "4294967296", "1x"})
        rejected({"--scene", "fixture.pbrt", "--estimator", "area-replay", "--area-path-domain",
                  "enhanced", "--area-history-cap", cap},
                 "Invalid Enhanced history cap");
    for (const auto cap : {"1", "20"}) {
        const auto bounded =
            accepted({"--scene", "fixture.pbrt", "--estimator", "area-replay", "--area-path-domain",
                      "enhanced", "--area-history-cap", cap});
        require(viewer::frame_configuration(bounded).enhanced_history_cap == std::stoul(cap),
                "Both fixed-cap endpoints reach native configuration");
    }
    rejected({"--demo", "--area-spatial-mode", "invalid"}, "Unknown spatial policy");
    rejected({"--demo", "--area-path-domain", "invalid"}, "Unknown path domain");
    rejected({"--demo", "--area-contribution-replay", "invalid"}, "Unknown replay policy");
    rejected({"--demo", "--area-history-cap", "1"}, "Cap needs Enhanced");
    const auto adaptive =
        accepted({"--scene", "fixture.pbrt", "--estimator", "area-replay", "--area-path-domain",
                  "enhanced", "--area-history-cap-policy", "duplication"});
    require(viewer::frame_configuration(adaptive).history_cap_policy ==
                FrameHistoryCapPolicy::duplication,
            "Opt-in duplication policy reaches the viewer session");
    rejected({"--demo", "--area-history-cap-policy", "duplication"}, "Policy needs Enhanced");
    rejected({"--scene", "fixture.pbrt", "--estimator", "area-replay", "--area-path-domain",
              "enhanced", "--area-history-cap-policy", "automatic"},
             "Unsupported cap policy");
    rejected({"--scene", "fixture.pbrt", "--area-path-domain", "enhanced"},
             "Path domain needs Area estimator");
    rejected({"--demo", "--estimator", "area-replay", "--area-path-domain", "enhanced"},
             "Enhanced needs PBRT");
    rejected({"--scene", "fixture.pbrt", "--estimator", "area-replay", "--area-path-domain",
              "enhanced", "--area-spatial-mode", "stochastic-shared"},
             "Shared needs paired atlas");
    rejected({"--demo", "--area-contribution-replay", "prefix"},
             "Prefix needs contribution domain");
    rejected({"--demo", "--resource-profile", "invalid"}, "Unknown resource budget");
    rejected({"--demo", "--variance-policy", "invalid"}, "Unknown variance policy");
    rejected({"--demo", "--mode", "metalfx", "--variance-policy", "conservative"},
             "Variance option needs temporal mode");
}
void defaults_and_modes() {
    const auto defaults = accepted({"--demo"});
    require(defaults.demo && defaults.scene.empty() && !defaults.help, "Demo selection defaults");
    require(defaults.width == 1920 && defaults.height == 1080,
            "Viewer defaults to fixed native 1080p internal resolution");
    require(
        defaults.estimator == FrameEstimator::independent && defaults.mutation_steps == 1 &&
            defaults.reconstruction == ReconstructionMode::temporal,
        "Viewer defaults to independent/temporal while retaining one mutation as Area preference");
    require(defaults.frames == 0 && defaults.seed == 1 && defaults.exposure_ev == 0 &&
                defaults.near_plane == .001 && defaults.capture_directory.empty(),
            "Default frame limit, seed, presentation, and capture path");
    const auto area_default = accepted({"--demo", "--estimator", "area-replay"});
    require(area_default.estimator == FrameEstimator::area_replay &&
                area_default.mutation_steps == 1 &&
                area_default.reconstruction == ReconstructionMode::temporal,
            "Explicit Area opt-in retains its default one mutation");
    for (const auto steps : {0u, 1u, 2u, 4u}) {
        const auto area = accepted(
            {"--demo", "--estimator", "area-replay", "--mutations", std::to_string(steps)});
        require(area.estimator == FrameEstimator::area_replay && area.mutation_steps == steps,
                "Supported Area mutation count preserved");
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
    rejected({"--demo", "--reconstruction", "none"}, "unknown reconstruction mode");
    for (const auto value : {"3", "5", "-1", "4294967296", "1x"})
        rejected({"--demo", "--mutations", value}, "unsupported or malformed mutation count");
}
void denoiser_selection_and_comparison() {
    const std::array<std::pair<const char *, ReconstructionMode>, 3> modes{
        {{"temporal", ReconstructionMode::temporal},
         {"metalfx", ReconstructionMode::metalfx},
         {"raw", ReconstructionMode::raw_rgb}}};
    for (const auto &[name, mode] : modes) {
        const auto selected = accepted({"--demo", "--mode", name});
        require(selected.reconstruction == mode, "Explicit mode preserved");
        const auto preferred = viewer::preferred_denoiser(mode);
        require(
            preferred ==
                (mode == ReconstructionMode::raw_rgb ? ReconstructionMode::temporal : mode),
            "Initially raw comparison explicitly selects temporal; denoiser selections persist");
        const auto once = viewer::toggle_comparison(mode, preferred);
        require(once ==
                    (mode == ReconstructionMode::raw_rgb ? preferred : ReconstructionMode::raw_rgb),
                "R compares raw with the selected denoiser");
        require(viewer::toggle_comparison(once, preferred) == mode,
                "Repeated R restores the selected mode");
    }
    require(accepted({"--demo", "--reconstruction", "temporal"}).reconstruction ==
                ReconstructionMode::temporal,
            "Existing reconstruction option accepts temporal");
    rejected({"--demo", "--mode", "none"}, "Unknown mode");
    rejected({"--demo", "--mode", "raw-rgb"}, "Mode spelling uses raw");
    rejected({"--demo", "--mode", "temporal", "--reconstruction", "metalfx"},
             "Aliases cannot select conflicting modes");
    rejected({"--demo", "--reconstruction", "raw-rgb", "--mode", "temporal"},
             "Alias conflict independent of order");
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
                usage.find("--frames") != std::string::npos &&
                usage.find("--mode temporal|metalfx|raw") != std::string::npos &&
                usage.find("raw/selected denoiser") != std::string::npos,
            "Help describes source selection and the finite frame limit");
    require(usage.find("default: temporal, native resolution") != std::string::npos &&
                usage.find("default: independent") != std::string::npos &&
                usage.find("default: 1 for Area") != std::string::npos,
            "Help distinguishes viewer defaults from opt-in Area mutation preference");
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
        spectral_and_area_policies();
        enhanced_configuration();
        defaults_and_modes();
        denoiser_selection_and_comparison();
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
