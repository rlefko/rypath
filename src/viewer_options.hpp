// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "reconstruction.hpp"
#include "resource_limits.hpp"

namespace rypath::viewer {
struct Options {
    bool completion_wakeup = false;
    std::filesystem::path scene, capture_directory,
        measurement_json; // Empty resolves to Pictures/rypath in the app.
    bool demo = false, help = false;
    std::uint32_t width = 1920, height = 1080, mutation_steps = 1;
    std::uint32_t output_width = 0, output_height = 0;
    std::uint64_t warmup = 0, motion_period = 0;
    std::string workload = "interactive";
    double motion_distance = .1, rotation_radians = .1;
    std::uint64_t frames = 0, seed = 1; // Zero frames means interactive/unlimited.
    SpectralTransportSelection spectral_transport = SpectralTransportSelection::automatic;
    std::optional<HeroWavelengthSampling> hero_wavelengths;
    FrameMutationPolicy mutation_policy = FrameMutationPolicy::automatic;
    FrameAreaStencil area_stencil = FrameAreaStencil::automatic;
    FrameAreaPathDomain area_path_domain = FrameAreaPathDomain::summed;
    FrameContributionReplay contribution_replay = FrameContributionReplay::full;
    bool enhanced_temporal_reprojection = false;
    FrameSpatialMode spatial_mode = FrameSpatialMode::all;
    std::uint32_t enhanced_history_cap = FrameConfig{}.enhanced_history_cap;
    FrameHistoryCapPolicy history_cap_policy = FrameHistoryCapPolicy::fixed;
    ResourceProfile resource_profile = ResourceProfile::standard;
    SpatialVariancePolicy variance_policy = SpatialVariancePolicy::paired;
    FrameEstimator estimator = FrameEstimator::independent;
    ReconstructionMode reconstruction = ReconstructionMode::temporal;
    double exposure_ev = 0, near_plane = .001;
};
// Parsing never opens scenes, creates capture directories, or initializes Metal.
std::string parse_options(int argc, char **argv, Options &);
const char *usage();
// The native viewer uses these same configurations; CPU tests validate plumbing.
FrameConfig frame_configuration(const Options &);
ReconstructionConfig reconstruction_configuration(const Options &, ReconstructionMode);

// R compares raw output with the selected denoiser; an initially raw viewer
// uses signed temporal reconstruction as its explicit comparison preference.
ReconstructionMode preferred_denoiser(ReconstructionMode selected);
ReconstructionMode toggle_comparison(ReconstructionMode current, ReconstructionMode preferred);
} // namespace rypath::viewer
