// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "reconstruction.hpp"

namespace rypath::viewer {
struct Options {
    std::filesystem::path scene, capture_directory; // Empty resolves to Pictures/rypath in the app.
    bool demo = false, help = false;
    std::uint32_t width = 1920, height = 1080, mutation_steps = 1;
    std::uint64_t frames = 0, seed = 1; // Zero frames means interactive/unlimited.
    FrameEstimator estimator = FrameEstimator::area_replay;
    ReconstructionMode reconstruction = ReconstructionMode::metalfx;
    double exposure_ev = 0, near_plane = .001;
};
// Parsing never opens scenes, creates capture directories, or initializes Metal.
std::string parse_options(int argc, char **argv, Options &);
const char *usage();
} // namespace rypath::viewer
