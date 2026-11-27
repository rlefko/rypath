// SPDX-License-Identifier: GPL-3.0-only
#include "frame_qualification_identity.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace rypath;
unsigned checks = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value)
        throw std::runtime_error(label);
}
// Handwritten accounting controls: 2x3 pixels, cold five leaves, one mutation.
FrameInfo enhanced() {
    FrameInfo f;
    f.width = 2;
    f.height = 3;
    f.status = FrameStatus::complete;
    f.completed_samples = 6;
    f.area_serial = 17;
    f.area_path_domain = FrameAreaPathDomain::enhanced;
    f.contribution_replay = FrameContributionReplay::prefix;
    f.canonical_transport_version = 4;
    f.fresh_discovery_version = 1;
    f.contribution_depth = 6;
    f.enhanced_suffix_cache = true;
    f.mutation_policy = FrameMutationPolicy::legacy;
    f.area_stencil = FrameAreaStencil::adjacent;
    f.area_stencil_offsets = {-1, 0, 0, -1};
    f.fresh_candidates = 6;
    f.completed_evaluations = 26;
    f.analytic_null_evaluations = 10;
    f.logical_evaluations = 36;
    f.mutation_attempts = 6;
    return f;
}
FrameConfig config() {
    FrameConfig c;
    c.width = 2;
    c.height = 3;
    c.seed = 0x123456789abcdef0ull;
    c.estimator = FrameEstimator::area_replay;
    c.area_path_domain = FrameAreaPathDomain::enhanced;
    c.contribution_replay = FrameContributionReplay::prefix;
    return c;
}
void controls() {
    auto c = config();
    auto f = enhanced();
    check(valid_frame_qualification_identity(c, f, 6), "enhanced cold positive");
    {
        auto full = c;
        auto actual = f;
        full.contribution_replay = actual.contribution_replay = FrameContributionReplay::full;
        actual.enhanced_suffix_cache = false;
        check(valid_frame_qualification_identity(full, actual, 6),
              "full replay makes requested suffix ineffective");
        full.mutation_policy = actual.mutation_policy = FrameMutationPolicy::spectral_refresh;
        full.area_stencil = actual.area_stencil = FrameAreaStencil::reciprocal16;
        const auto d = rypath_area_stencil::draw(17, 0, std::uint32_t(full.seed),
                                                 std::uint32_t(full.seed >> 32));
        actual.area_stencil_offsets = {d.first.x, d.first.y, d.second.x, d.second.y};
        check(valid_frame_qualification_identity(full, actual, 6),
              "explicit refresh and reciprocal stencil positive");
        actual.area_stencil_offsets[3] += 1;
        check(!valid_frame_qualification_identity(full, actual, 6), "wrong reciprocal offsets");
    }

    const auto reject = [&](auto edit, const char *label) {
        auto bad = f;
        edit(bad);
        std::string diagnostic;
        check(!valid_frame_qualification_identity(c, bad, 6, &diagnostic) && !diagnostic.empty(),
              label);
    };
    reject([](auto &x) { x.area_path_domain = FrameAreaPathDomain::summed; }, "wrong domain");
    reject([](auto &x) { x.contribution_replay = FrameContributionReplay::full; }, "wrong replay");
    reject([](auto &x) { x.canonical_transport_version = 0; }, "wrong canonical law");
    reject([](auto &x) { x.fresh_discovery_version = 0; }, "wrong discovery law");
    reject([](auto &x) { x.contribution_depth = 5; }, "wrong depth");
    reject([](auto &x) { x.enhanced_suffix_cache = false; }, "ignored requested suffix");
    reject([](auto &x) { x.status = FrameStatus::failed; }, "failed status");
    reject([](auto &x) { x.width = 3; }, "wrong dimensions");
    reject([](auto &x) { x.completed_samples = 5; }, "missing samples");
    reject([](auto &x) { x.compacted_null_evaluations = 1; }, "disabled compaction count");
    reject([](auto &x) { x.analytic_null_evaluations = 9; }, "wrong null accounting");
    reject([](auto &x) { x.mutation_policy = FrameMutationPolicy::spectral_refresh; },
           "wrong policy");
    reject([](auto &x) { x.area_stencil = FrameAreaStencil::reciprocal16; }, "wrong stencil");
    reject([](auto &x) { x.area_stencil_offsets[0] = 0; }, "wrong adjacent offsets");
    reject([](auto &x) { x.enhanced_geometry_reused = 6; }, "inactive geometry");
    reject([](auto &x) { x.paired_atlas.sha256 = "stray"; }, "inactive atlas");
    c.compact_enhanced_pairs = true;
    check(!valid_frame_qualification_identity(c, f, 6), "ignored requested compaction");
    f.enhanced_pair_compaction = true;
    f.compacted_null_evaluations = 10;
    check(valid_frame_qualification_identity(c, f, 6), "compaction positive");
    reject([](auto &x) { x.compacted_null_evaluations = 9; }, "partial compacted null count");
    reject([](auto &x) { x.compacted_null_evaluations = 11; }, "excess compacted null count");
    c.enhanced_temporal_reprojection = true;
    check(!valid_frame_qualification_identity(c, f, 6), "ignored requested motion");
    f.enhanced_temporal_reprojection_requested = true;
    f.enhanced_temporal_reprojection = true;
    f.enhanced_geometry_generated = 6;
    f.enhanced_motion_routes[0] = 6;
    check(valid_frame_qualification_identity(c, f, 6), "cold motion positive");
    reject(
        [](auto &x) {
            x.enhanced_geometry_generated = 0;
            x.enhanced_geometry_reused = 6;
            x.enhanced_motion_routes = {};
        },
        "cold geometry cannot be reused");
    reject([](auto &x) { x.enhanced_motion_routes[0] = 5; }, "missing motion route");
    reject([](auto &x) { x.enhanced_temporal_reprojection_requested = false; },
           "wrong motion request");
    f.history_used = true;
    f.logical_evaluations = 48;
    f.completed_evaluations = 38;
    f.enhanced_motion_routes = {0, 2, 1, 3};
    f.enhanced_temporal_reprojection_used = true;
    check(valid_frame_qualification_identity(c, f, 6), "mapped warm motion positive");
    reject([](auto &x) { x.temporal_evaluation_cache_used = true; }, "mapped cache incompatible");
    f.enhanced_geometry_generated = 0;
    f.enhanced_geometry_reused = 6;
    f.enhanced_motion_routes = {};
    f.enhanced_temporal_reprojection_used = false;
    f.temporal_evaluation_cache_used = true;
    f.cached_evaluations = 12;
    f.completed_evaluations = 26;
    check(valid_frame_qualification_identity(c, f, 6), "identity motion cache positive");
    c.cache_temporal_evaluations = false;
    check(!valid_frame_qualification_identity(c, f, 6), "ignored cache-off");
}
void legacy_and_independent() {
    auto c = config();
    auto f = enhanced();
    c.area_path_domain = FrameAreaPathDomain::summed;
    c.contribution_replay = FrameContributionReplay::full;
    f.area_path_domain = FrameAreaPathDomain::summed;
    f.contribution_replay = FrameContributionReplay::full;
    f.canonical_transport_version = f.fresh_discovery_version = f.contribution_depth = 0;
    f.enhanced_suffix_cache = false;
    f.completed_evaluations = 36;
    f.analytic_null_evaluations = 0;
    check(valid_frame_qualification_identity(c, f, 6), "summed positive");
    f.analytic_null_evaluations = 1;
    check(!valid_frame_qualification_identity(c, f, 6), "legacy null forbidden");
    f.analytic_null_evaluations = 0;
    c.area_path_domain = f.area_path_domain = FrameAreaPathDomain::contribution;
    c.contribution_replay = f.contribution_replay = FrameContributionReplay::prefix;
    f.contribution_depth = 6;
    check(valid_frame_qualification_identity(c, f, 6), "contribution prefix positive");
    c = FrameConfig{};
    c.width = 2;
    c.height = 3;
    c.mutation_policy = FrameMutationPolicy::spectral_refresh;
    c.area_stencil = FrameAreaStencil::paired_gaussian16;
    c.mutation_steps = 4;
    f = FrameInfo{};
    f.width = 2;
    f.height = 3;
    f.status = FrameStatus::complete;
    f.completed_samples = f.fresh_candidates = f.completed_evaluations = f.logical_evaluations = 6;
    check(valid_frame_qualification_identity(c, f, 6),
          "independent ignores Area-only policy/stencil");
    f.analytic_null_evaluations = 1;
    check(!valid_frame_qualification_identity(c, f, 6), "independent null forbidden");
    f.analytic_null_evaluations = 0;
    f.enhanced_suffix_misses = 1;
    check(!valid_frame_qualification_identity(c, f, 6), "disabled suffix telemetry");
}
void atlas() {
    namespace p = rypath_area_pair_atlas;
    auto c = config();
    auto f = enhanced();
    c.area_stencil = f.area_stencil = FrameAreaStencil::paired_gaussian16;
    f.area_stencil_offsets = {};
    auto &a = f.paired_atlas;
    a.version = 1;
    a.entry_count = 199152;
    a.asset_bytes = 398304;
    a.iterations = 128;
    a.sigma = 16;
    a.seed = {0x50414952, 0x41544c31};
    a.widths = {254, 230, 210, 194};
    a.sha256 = std::string(64, 'a');
    // Shared deterministic pose function supplies the valid control; independent
    // atlas/Philox tests cover its algorithm. These controls test identity rejection.
    const auto poses = p::frame_config(17, 0, std::uint32_t(c.seed), std::uint32_t(c.seed >> 32));
    for (unsigned k = 0; k < 4; ++k) {
        const auto pose = poses.config.poses[k];
        a.poses[k] = {pose.transform, pose.offset_x, pose.offset_y};
    }
    check(valid_frame_qualification_identity(c, f, 6), "paired identity positive");
    const auto reject = [&](auto edit, const char *label) {
        auto bad = f;
        edit(bad);
        check(!valid_frame_qualification_identity(c, bad, 6), label);
    };
    reject([](auto &x) { x.paired_atlas.version = 0; }, "ignored paired atlas");
    reject([](auto &x) { --x.paired_atlas.entry_count; }, "wrong atlas count");
    reject([](auto &x) { --x.paired_atlas.asset_bytes; }, "wrong atlas bytes");
    reject([](auto &x) { --x.paired_atlas.iterations; }, "wrong atlas iterations");
    reject([](auto &x) { x.paired_atlas.sigma = 15; }, "wrong sigma");
    reject([](auto &x) { ++x.paired_atlas.seed[0]; }, "wrong atlas seed");
    reject([](auto &x) { --x.paired_atlas.widths[3]; }, "wrong fourth width");
    reject([](auto &x) { x.paired_atlas.sha256[2] = 'z'; }, "malformed digest");
    reject([](auto &x) { x.paired_atlas.poses[2][0] ^= 1; }, "wrong paired transform");
    reject([](auto &x) { ++x.paired_atlas.poses[1][1]; }, "wrong paired offset");
    reject([](auto &x) { ++x.area_serial; }, "stale paired serial");
    check(valid_frame_qualification_identity(c, f, 6, nullptr, std::string(64, 'a')),
          "stable asset digest");
    check(!valid_frame_qualification_identity(c, f, 6, nullptr, std::string(64, 'b')),
          "changed asset digest");
}
} // namespace
int main() {
    try {
        controls();
        legacy_and_independent();
        atlas();
        std::cout << checks << " qualification identity checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
