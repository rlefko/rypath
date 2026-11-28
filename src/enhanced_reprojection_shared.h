// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"
#if !defined(__METAL_VERSION__)
#include <bit>
#endif

// Camera-only dual-motion domain selection (Enhanced 2026 section 6.4;
// Zeng et al. 2021 section 4.3/Eq.6). All projections come from deterministic,
// non-jittered geometry guides, independent of physical U,j and reservoir draws.
// No camera inversion, geometry fetch, light/path evaluation or sample copying
// occurs here. The caller supplies geometry compatibility and depth tolerance.
namespace rypath_enhanced_reprojection {
using Word = rypath_replay::Word;
using Wide = rypath_replay::Wide;
enum : Word { invalid_pixel = 0xffffffffu, maximum_dimension = 1u << 23 };
enum class Status : Word { ok, invalid_config, invalid_input };
enum class Route : Word { same_pixel, ordinary, dual };
enum class Reason : Word {
    none,
    no_history,
    projection_unavailable,
    unsupported_geometry,
    offscreen,
    owner_mismatch,
    foreground_anchor_mismatch,
    depth_mismatch,
    geometry_mismatch,
    dual_geometry_rejected
};
struct Config {
    Word width = 0, height = 0, history_valid = 0;
    // Absolute tolerance in infinite-far reversed depth, not world distance.
    // Zero or positive normal finite. No denoiser threshold is implicit here.
    float depth_tolerance = 0;
};
struct Projection {
    float x = 0, y = 0, depth = 0;
    Word valid = 0;
};
struct Lookup {
    Word pixel = invalid_pixel;
    Word supported = 0;
    Reason reason = Reason::none;
    Status status = Status::ok;
};
struct Proposal {
    Word owner = invalid_pixel, pixel = invalid_pixel;
    Route route = Route::same_pixel;
    Reason reason = Reason::none;
    Status status = Status::ok;
};
struct Donor {
    Word pixel = invalid_pixel;
    // A same-pixel fallback still admits the old domain when history exists.
    // Cold history is structurally absent, never a null reservoir realization.
    Word present = 0, supported = 0;
    Route route = Route::same_pixel;
    Reason reason = Reason::none;
    Status status = Status::ok;
};
inline Word bits(float value) {
#if defined(__METAL_VERSION__)
    return metal::as_type<Word>(value);
#else
    return std::bit_cast<Word>(value);
#endif
}
inline bool finite(float value) { return (bits(value) & 0x7f800000u) != 0x7f800000u; }
inline bool valid_config(Config c, Word owner) {
    const Word tolerance = bits(c.depth_tolerance);
    const bool valid_tolerance =
        (tolerance & 0x7fffffffu) == 0 ||
        (!(tolerance & 0x80000000u) && (tolerance & 0x7f800000u) != 0 && finite(c.depth_tolerance));
    // Binary32 represents every admitted pixel CENTER i+.5 exactly. Wide
    // multiplication validates the full Word pixel domain before flattening.
    const Wide pixels = Wide(c.width) * c.height;
    return c.width > 0 && c.height > 0 && c.width <= maximum_dimension &&
           c.height <= maximum_dimension && pixels <= Wide(invalid_pixel) && Wide(owner) < pixels &&
           c.history_valid <= 1 && valid_tolerance;
}
inline Reason projection_reason(Projection p) {
    if (!p.valid || !finite(p.x) || !finite(p.y) || !finite(p.depth))
        return Reason::projection_unavailable;
    // Sky(depth0), behind-camera and before-near geometry are not finite donors.
    return p.depth > 0 && p.depth <= 1 ? Reason::none : Reason::unsupported_geometry;
}
inline Lookup point_sample(Config c, Projection p) {
    // Internal arithmetic helper: config and the 0/1 projection flag have
    // already been checked by ordinary_lookup/propose.
    const auto reason = projection_reason(p);
    if (reason != Reason::none)
        return {invalid_pixel, 0, reason, Status::ok};
    if (!(p.x >= 0 && p.y >= 0 && p.x < float(c.width) && p.y < float(c.height)))
        return {invalid_pixel, 0, Reason::offscreen, Status::ok};
    // Checked nonnegative finite conversion truncates exactly as floor. Never
    // round-to-nearest, bilinearly mix depths, clamp, or convert before bounds.
    return {Word(p.y) * c.width + Word(p.x), 1, Reason::none, Status::ok};
}
inline Lookup ordinary_lookup(Config c, Word owner, Projection current_in_current,
                              Projection current_in_previous) {
    if (!valid_config(c, owner))
        return {invalid_pixel, 0, Reason::none, Status::invalid_config};
    if (!c.history_valid)
        return {invalid_pixel, 0, Reason::no_history, Status::ok};
    if (current_in_current.valid > 1 || current_in_previous.valid > 1)
        return {invalid_pixel, 0, Reason::none, Status::invalid_input};
    const auto current = point_sample(c, current_in_current);
    if (!current.supported)
        return current;
    // A sanity check on the declared non-jittered guide, not an alignment clamp
    // or an assertion that a floating projection is bit-identical to the center.
    if (current.pixel != owner)
        return {invalid_pixel, 0, Reason::owner_mismatch, Status::ok};
    return point_sample(c, current_in_previous);
}
inline Proposal fallback(Config c, Word owner, Reason reason, Status status = Status::ok) {
    return {owner, status == Status::ok && c.history_valid ? owner : invalid_pixel,
            Route::same_pixel, reason, status};
}
// First call ordinary_lookup to obtain k, then fetch the OLD visible foreground
// O at k. Pass both projections of O here. If this produces a dual candidate,
// fetch its old background geometry and pass that compatibility to resolve().
inline Proposal propose(Config c, Word owner, Projection current_in_current,
                        Projection current_in_previous, Projection foreground_in_previous,
                        Projection foreground_in_current, Word ordinary_geometry_compatible) {
    const auto lookup = ordinary_lookup(c, owner, current_in_current, current_in_previous);
    if (lookup.status != Status::ok || !lookup.supported)
        return fallback(c, owner, lookup.reason, lookup.status);
    if (ordinary_geometry_compatible > 1 || foreground_in_previous.valid > 1)
        return fallback(c, owner, Reason::none, Status::invalid_input);
    const auto foreground = point_sample(c, foreground_in_previous);
    if (!foreground.supported)
        return fallback(c, owner, foreground.reason);
    if (foreground.pixel != lookup.pixel)
        return fallback(c, owner, Reason::foreground_anchor_mismatch);
    const float separation = foreground_in_previous.depth - current_in_previous.depth;
    const bool occluded = separation > c.depth_tolerance;
    if (!occluded) {
        const float distance = separation < 0 ? -separation : separation;
        if (distance > c.depth_tolerance)
            return fallback(c, owner, Reason::depth_mismatch);
        if (!ordinary_geometry_compatible)
            return fallback(c, owner, Reason::geometry_mismatch);
        return {owner, lookup.pixel, Route::ordinary, Reason::none, Status::ok};
    }
    if (foreground_in_current.valid > 1)
        return fallback(c, owner, Reason::none, Status::invalid_input);
    const auto reason = projection_reason(foreground_in_current);
    if (reason != Reason::none)
        return fallback(c, owner, reason);
    // O may project OFFSCREEN in the current view. Its finite z is still needed
    // for the extrapolation; only the resulting old donor must lie in bounds.
    const float x = float(owner % c.width) + .5f;
    const float y = float(owner / c.width) + .5f;
    // Declared binary32 reference: (project_old(O)+current_center)-project_now(O).
    const Projection alternate{(foreground_in_previous.x + x) - foreground_in_current.x,
                               (foreground_in_previous.y + y) - foreground_in_current.y,
                               current_in_previous.depth, 1};
    const auto candidate = point_sample(c, alternate);
    if (!candidate.supported)
        return fallback(c, owner, candidate.reason);
    return {owner, candidate.pixel, Route::dual, Reason::none, Status::ok};
}
inline Donor resolve(Config c, Word owner, Proposal proposal, Word dual_geometry_compatible) {
    if (!valid_config(c, owner))
        return {invalid_pixel, 0, 0, Route::same_pixel, Reason::none, Status::invalid_config};
    if (proposal.status != Status::ok)
        return {invalid_pixel, 0, 0, Route::same_pixel, proposal.reason, proposal.status};
    if (proposal.owner != owner || Word(proposal.route) > Word(Route::dual) ||
        (proposal.route == Route::same_pixel
             ? proposal.pixel != (c.history_valid ? owner : invalid_pixel)
             : !c.history_valid || Wide(proposal.pixel) >= Wide(c.width) * c.height))
        return {invalid_pixel, 0, 0, Route::same_pixel, Reason::none, Status::invalid_input};
    if (proposal.route == Route::dual) {
        if (dual_geometry_compatible > 1)
            return {invalid_pixel, 0, 0, Route::same_pixel, Reason::none, Status::invalid_input};
        if (!dual_geometry_compatible)
            return {owner, 1, 0, Route::same_pixel, Reason::dual_geometry_rejected, Status::ok};
    }
    return {proposal.pixel, c.history_valid, Word(proposal.route != Route::same_pixel),
            proposal.route, proposal.reason, Status::ok};
}
} // namespace rypath_enhanced_reprojection
