// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "area_evaluation_context.hpp"
#include "area_pair_atlas_shared.h"
#include "area_stencil_shared.h"
#include "frame_session.hpp"
#include "stochastic_spatial_shared.h"
#include <algorithm>
#include <limits>
#include <string>
#include <string_view>

namespace rypath {
// Validate reported execution before attaching requested estimator labels to an
// image. Context equality (and therefore cache use) is not inferable from these
// inputs; a cache hit must be permitted, but is never required.
inline bool valid_frame_qualification_identity(const FrameConfig &c, const FrameInfo &f,
                                               std::uint32_t scene_depth,
                                               std::string *diagnostic = nullptr,
                                               std::string_view expected_atlas_digest = {}) {
    const auto fail = [&](const char *message) {
        if (diagnostic)
            *diagnostic = message;
        return false;
    };
    if (diagnostic)
        diagnostic->clear();
    const auto n = std::uint64_t(c.width) * c.height;
    const bool area = c.estimator == FrameEstimator::area_replay;
    const bool enhanced = area && c.area_path_domain == FrameAreaPathDomain::enhanced;
    const bool wavefront = c.execution == FrameExecution::wavefront;
    const bool adaptive = c.history_cap_policy == FrameHistoryCapPolicy::duplication;
    const auto duplication = adaptive && c.area_duplication == FrameAreaDuplication::off
                                 ? FrameAreaDuplication::initial_seed
                                 : c.area_duplication;
    if ((c.history_cap_policy != FrameHistoryCapPolicy::fixed && !adaptive) ||
        f.history_cap_policy != c.history_cap_policy ||
        (adaptive && (!enhanced || !wavefront || c.area_reuse != FrameAreaReuse::full ||
                      duplication != FrameAreaDuplication::initial_seed)) ||
        (!area && duplication != FrameAreaDuplication::off) || f.duplication.mode != duplication)
        return fail("Qualification history cap policy/duplication identity mismatch");
    const bool suffix = enhanced && c.contribution_replay == FrameContributionReplay::prefix &&
                        c.enhanced_suffix_cache && c.area_reuse == FrameAreaReuse::full;
    const bool compact = enhanced && wavefront && c.compact_enhanced_pairs;
    const bool motion = enhanced && wavefront && c.enhanced_temporal_reprojection;
    const bool paired = area && wavefront && c.area_stencil == FrameAreaStencil::paired_gaussian16;
    const auto selection = rypath_stochastic_spatial::draw(
        static_cast<std::uint32_t>(f.area_serial), static_cast<std::uint32_t>(f.area_serial >> 32),
        static_cast<std::uint32_t>(c.seed), static_cast<std::uint32_t>(c.seed >> 32),
        c.spatial_mode);
    if (!rypath_stochastic_spatial::valid_mode(c.spatial_mode) ||
        f.spatial_mode != c.spatial_mode ||
        f.stochastic_spatial_pairs != rypath_stochastic_spatial::enabled(c.spatial_mode) ||
        (rypath_stochastic_spatial::enabled(c.spatial_mode)
             ? !enhanced || !paired || f.stochastic_forward_atlas != selection.forward ||
                   f.stochastic_canonical_atlas != selection.canonical
             : f.stochastic_forward_atlas != 0 || f.stochastic_canonical_atlas != 0))
        return fail("Qualification stochastic spatial identity mismatch");
    if ((c.area_reuse != FrameAreaReuse::full && c.area_reuse != FrameAreaReuse::fresh) ||
        f.area_reuse != c.area_reuse ||
        (c.area_reuse == FrameAreaReuse::fresh &&
         (!enhanced || c.mutation_steps || c.spatial_mode != FrameSpatialMode::all ||
          c.compact_enhanced_pairs || c.enhanced_temporal_reprojection ||
          c.area_duplication != FrameAreaDuplication::off || f.history_used ||
          f.history_generation || f.temporal_candidates || f.spatial_candidates)))
        return fail("Qualification fresh-only identity/publication mismatch");
    const auto mutations = area ? c.mutation_steps : 0u;
    // Bound arithmetic used by the exact accounting helpers, including mutations.
    if (!n || n > std::numeric_limits<std::uint32_t>::max() ||
        mutations > std::numeric_limits<std::uint32_t>::max() - 7u ||
        n > std::numeric_limits<std::uint64_t>::max() / (7ull + mutations))
        return fail("Qualification dimensions/accounting overflow");
    const auto spatial_candidates = !area || c.area_reuse == FrameAreaReuse::fresh       ? 0u
                                    : rypath_stochastic_spatial::enabled(c.spatial_mode) ? 2u
                                                                                         : 5u;
    if (f.spatial_candidates != spatial_candidates)
        return fail("Qualification spatial reservoir candidate count mismatch");
    if (f.status != FrameStatus::complete || f.width != c.width || f.height != c.height ||
        f.completed_samples != n)
        return fail("Qualification frame completion/dimensions mismatch");
    if (f.area_path_domain != c.area_path_domain ||
        f.contribution_replay != (frame_area_contribution_family(c.area_path_domain)
                                      ? c.contribution_replay
                                      : FrameContributionReplay::full) ||
        f.canonical_transport_version != frame_area_canonical_version(c.area_path_domain) ||
        f.fresh_discovery_version != frame_area_discovery_version(c.area_path_domain) ||
        f.contribution_depth !=
            (frame_area_contribution_family(c.area_path_domain) ? scene_depth : 0u))
        return fail("Qualification path domain/replay/law/depth mismatch");
    if (f.enhanced_history_cap !=
            (enhanced ? frame_temporal_reuse_cap(c.estimator, c.area_reuse, c.enhanced_history_cap)
                      : 0u) ||
        f.enhanced_suffix_cache != suffix || f.enhanced_pair_compaction != compact ||
        f.enhanced_temporal_reprojection_requested != c.enhanced_temporal_reprojection ||
        f.enhanced_temporal_reprojection != motion)
        return fail("Qualification effective execution feature mismatch");
    if ((!suffix &&
         (f.enhanced_suffix_hits != std::array<std::uint64_t, 3>{} || f.enhanced_suffix_misses)) ||
        f.compacted_null_evaluations != (compact ? f.analytic_null_evaluations : 0u))
        return fail("Qualification suffix/compacted-null telemetry mismatch");
    if ((f.temporal_evaluation_cache_used &&
         (!area || !c.cache_temporal_evaluations || !f.history_used ||
          f.enhanced_temporal_reprojection_used)) ||
        (!area && f.history_used))
        return fail("Qualification temporal reuse mismatch");
    const bool accounting =
        !area ? f.completed_evaluations == n && f.cached_evaluations == 0 &&
                    f.analytic_null_evaluations == 0 && f.logical_evaluations == n
        : enhanced ? valid_enhanced_area_evaluation_accounting(
                         n, f.history_used, mutations, f.temporal_evaluation_cache_used,
                         f.completed_evaluations, f.cached_evaluations, f.analytic_null_evaluations,
                         f.logical_evaluations,
                         rypath_stochastic_spatial::enabled(c.spatial_mode)
                             ? rypath_stochastic_spatial::evaluations(
                                   {f.stochastic_forward_atlas, f.stochastic_canonical_atlas})
                             : 4u,
                         c.area_reuse == FrameAreaReuse::fresh)
                   : f.analytic_null_evaluations == 0 &&
                         valid_area_evaluation_accounting(
                             n, f.history_used, mutations, f.temporal_evaluation_cache_used,
                             f.completed_evaluations, f.cached_evaluations, f.logical_evaluations);
    if (!accounting || f.fresh_candidates != n || f.mutation_attempts != n * mutations ||
        f.mutation_accepts > f.mutation_attempts)
        return fail("Qualification evaluation/mutation accounting mismatch");
    std::uint64_t routes = 0;
    for (const auto count : f.enhanced_motion_routes) {
        if (count > n || routes > n - count)
            return fail("Qualification motion route count mismatch");
        routes += count;
    }
    if (motion) {
        if (f.enhanced_geometry_generated > n ||
            f.enhanced_geometry_reused != n - f.enhanced_geometry_generated ||
            (f.enhanced_geometry_generated
                 ? (f.enhanced_geometry_generated != n || routes != n ||
                    f.enhanced_temporal_reprojection_used != f.history_used ||
                    f.enhanced_motion_routes[0] != (f.history_used ? 0 : n))
                 : (!f.history_used || routes != 0 || f.enhanced_temporal_reprojection_used)))
            return fail("Qualification motion geometry identity mismatch");
    } else if (f.enhanced_geometry_generated || f.enhanced_geometry_reused || routes ||
               f.enhanced_temporal_reprojection_used)
        return fail("Qualification inactive motion telemetry mismatch");
    const auto policy = area && c.mutation_policy != FrameMutationPolicy::automatic
                            ? c.mutation_policy
                            : FrameMutationPolicy::legacy;
    const auto stencil = area && c.area_stencil != FrameAreaStencil::automatic
                             ? c.area_stencil
                             : FrameAreaStencil::adjacent;
    if (f.mutation_policy != policy || f.area_stencil != stencil)
        return fail("Qualification mutation policy/stencil mismatch");
    if (paired) {
        namespace p = rypath_area_pair_atlas;
        const auto &a = f.paired_atlas;
        // This checks the reported identity's schema and deterministic poses.
        // A syntactically valid digest is not independent attestation of bytes.
        if (a.version != p::version || a.entry_count != p::total_entries ||
            a.asset_bytes != std::uint64_t(p::total_entries) * sizeof(p::Entry) ||
            a.iterations != p::shuffle_count || a.sigma != 16.f ||
            a.seed != std::array<std::uint32_t, 2>{p::atlas_seed_low, p::atlas_seed_high} ||
            a.sha256.size() != 64 ||
            (!expected_atlas_digest.empty() && a.sha256 != expected_atlas_digest) ||
            !std::all_of(a.sha256.begin(), a.sha256.end(),
                         [](char x) { return (x >= '0' && x <= '9') || (x >= 'a' && x <= 'f'); }) ||
            f.area_stencil_offsets != std::array<std::int32_t, 4>{})
            return fail("Qualification paired atlas identity mismatch");
        const auto poses =
            p::frame_config(std::uint32_t(f.area_serial), std::uint32_t(f.area_serial >> 32),
                            std::uint32_t(c.seed), std::uint32_t(c.seed >> 32));
        if (poses.status != p::Status::ok)
            return fail("Qualification paired pose generation failed");
        for (unsigned k = 0; k < 4; ++k) {
            const auto pose = poses.config.poses[k];
            if (a.widths[k] != p::tile_width(k) ||
                a.poses[k] !=
                    std::array<std::uint32_t, 3>{pose.transform, pose.offset_x, pose.offset_y})
                return fail("Qualification paired atlas width/pose mismatch");
        }
    } else {
        const auto &a = f.paired_atlas;
        if (a.version || a.entry_count || a.asset_bytes || a.iterations || a.sigma != 0 ||
            a.widths != std::array<std::uint32_t, 4>{} ||
            a.seed != std::array<std::uint32_t, 2>{} || !a.sha256.empty() ||
            a.poses != std::array<std::array<std::uint32_t, 3>, 4>{})
            return fail("Qualification inactive atlas identity mismatch");
        const auto d = stencil == FrameAreaStencil::reciprocal16
                           ? rypath_area_stencil::draw(
                                 std::uint32_t(f.area_serial), std::uint32_t(f.area_serial >> 32),
                                 std::uint32_t(c.seed), std::uint32_t(c.seed >> 32))
                           : rypath_area_stencil::Offsets{{-1, 0}, {0, -1}};
        const auto expected =
            area ? std::array<std::int32_t, 4>{d.first.x, d.first.y, d.second.x, d.second.y}
                 : std::array<std::int32_t, 4>{};
        if (f.area_stencil_offsets != expected)
            return fail("Qualification stencil offsets mismatch");
    }
    return true;
}
} // namespace rypath
