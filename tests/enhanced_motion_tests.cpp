// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_motion_shared.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace motion = rypath_enhanced_motion;
using D3 = std::array<double, 3>;
unsigned checks = 0;
void need(bool ok, const char *why) {
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
void close(double actual, double expected, double tolerance, const char *why) {
    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, why);
}
// Predeclared heuristic policy, not physical transport acceptance. Footprints
// below are supplied analytically from the corresponding pinhole plane/raster.
constexpr motion::Policy policy{.99f, .5f, 32 * std::numeric_limits<float>::epsilon(), 0};
struct OracleCamera {
    D3 origin{}, right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, 1};
    unsigned width = 64, height = 48;
    double sx = 1, sy = 1, ox = 0, oy = 0, near = .01;
};
double dot(D3 a, D3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 sub(D3 a, D3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
motion::Vec3 pack(D3 v) { return {float(v[0]), float(v[1]), float(v[2])}; }
// Independent double camera oracle: analytic orthonormal basis/pinhole ratios,
// not multiplication by the tested packed matrices or production project().
std::array<double, 3> oracle(const OracleCamera &camera, D3 world) {
    const auto relative = sub(world, camera.origin);
    const auto z = dot(relative, camera.forward);
    return {(camera.sx * dot(relative, camera.right) / z + camera.ox + 1) * .5 * camera.width,
            (1 - camera.sy * dot(relative, camera.up) / z - camera.oy) * .5 * camera.height,
            camera.near / z};
}
motion::Camera packed(const OracleCamera &camera, D3 common_origin = {}, D3 matrix_origin = {}) {
    motion::Camera out;
    out.width = camera.width;
    out.height = camera.height;
    out.common_to_matrix_origin = pack(sub(common_origin, matrix_origin));
    const std::array<D3, 3> rows{camera.right, camera.up, camera.forward};
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned col = 0; col < 3; ++col)
            out.world_to_view[col * 4 + row] = float(rows[row][col]);
        out.world_to_view[12 + row] = float(-dot(rows[row], sub(camera.origin, matrix_origin)));
    }
    out.world_to_view[15] = 1;
    out.view_to_clip[0] = float(camera.sx);
    out.view_to_clip[5] = float(camera.sy);
    out.view_to_clip[8] = float(camera.ox);
    out.view_to_clip[9] = float(camera.oy);
    out.view_to_clip[11] = 1;
    out.view_to_clip[14] = float(camera.near);
    return out;
}
void projection_oracle() {
    for (double angle : {0., .37, -.61}) {
        OracleCamera camera;
        camera.origin = {1.25, -.75, 2};
        camera.right = {std::cos(angle), 0, -std::sin(angle)};
        camera.forward = {std::sin(angle), 0, std::cos(angle)};
        camera.sx = 1.7;
        camera.sy = .8;
        camera.ox = .13;
        camera.oy = -.07;
        const auto gpu = packed(camera, {4, -2, 1}, {3, -1, 2});
        for (D3 point : {D3{1, 2, 8}, D3{-2, -1, 12}, D3{5, 0, 20}}) {
            const auto expected = oracle(camera, point);
            const auto actual = motion::project(gpu, pack(sub(point, {4, -2, 1})));
            need(actual.status == motion::ProjectionStatus::valid,
                 "Rotated translated projection valid");
            close(actual.pixel.x, expected[0], 2e-5, "Independent rotated X projection");
            close(actual.pixel.y, expected[1], 2e-5, "Independent rotated Y projection");
            close(actual.depth, expected[2], 2e-9, "Independent reversed depth");
        }
    }
    // Retain small local geometry at a huge world origin. Differences are formed
    // in double before FLOAT packing, never by subtracting absolute FLOAT points.
    const D3 common{1e12, -1e12, 3e12};
    OracleCamera far;
    far.origin = {common[0] + 2, common[1] - 3, common[2] + 1};
    const D3 matrix{common[0] - 4, common[1] + 8, common[2] - 2};
    const D3 point{common[0] + 3, common[1] - 2, common[2] + 9};
    const auto a = motion::project(packed(far, common, matrix), pack(sub(point, common)));
    const auto expected = oracle(far, point);
    need(a.status == motion::ProjectionStatus::valid, "Large rebased origin projection valid");
    close(a.pixel.x, expected[0], 1e-6, "Large-origin X uses double-packed deltas");
    close(a.pixel.y, expected[1], 1e-6, "Large-origin Y uses double-packed deltas");
    close(a.depth, expected[2], 1e-9, "Large-origin reversed depth");
    OracleCamera base;
    auto camera = packed(base);
    need(motion::project(camera, {0, 0, 0}).status == motion::ProjectionStatus::behind_camera,
         "Zero view depth rejected");
    need(motion::project(camera, {0, 0, -1}).status == motion::ProjectionStatus::behind_camera,
         "Behind-camera geometry rejected");
    need(motion::project(camera, {0, 0, .005f}).status ==
             motion::ProjectionStatus::before_near_plane,
         "Before-near geometry rejected without clamp");
    need(motion::project(camera, {0, 0, .01f}).status == motion::ProjectionStatus::valid,
         "Near plane itself admitted");
    camera.view_to_clip[14] = .1f;
    need(motion::project(camera, {0, 0, .05f}).status ==
             motion::ProjectionStatus::before_near_plane,
         "Changed guide near plane changes only projection eligibility");
    camera.world_to_view[0] = std::numeric_limits<float>::quiet_NaN();
    need(motion::project(camera, {0, 0, 8}).status == motion::ProjectionStatus::invalid_input,
         "Nonfinite matrix rejected");
    need(motion::project(packed(base), {0, std::numeric_limits<float>::infinity(), 8}).status ==
             motion::ProjectionStatus::invalid_input,
         "Nonfinite position rejected");
}
void point_sampling() {
    unsigned pixel = 999;
    need(motion::point_index({.5f, .5f}, 64, 48, pixel) && pixel == 0,
         "Pixel center floors to first pixel");
    need(motion::point_index({1, 1}, 64, 48, pixel) && pixel == 65,
         "Integer boundary floors toward next pixel");
    need(motion::point_index({63.999f, 47.999f}, 64, 48, pixel) && pixel == 3071,
         "Last interior point admitted");
    for (motion::Vec2 p : {motion::Vec2{-0.001f, 1},
                           {64, 1},
                           {1, 48},
                           {1, -1},
                           {std::numeric_limits<float>::infinity(), 0},
                           {0, std::numeric_limits<float>::quiet_NaN()}})
        need(!motion::point_index(p, 64, 48, pixel), "No edge clamp or nonfinite cast");
    need(!motion::point_index({0, 0}, 0, 48, pixel), "Empty raster rejected");
    need(!motion::point_index({0, 0}, 0xffffffffu, 2, pixel), "Raster product overflow rejected");
}
motion::Surface surface(motion::Vec3 p, unsigned instance = 7) {
    return {.common_position = p,
            .geometric_normal = {0, 0, -1},
            .pixel_footprint = .25f,
            .instance = instance,
            .material = 3,
            .geometry = motion::Geometry::finite_surface};
}
struct Fixture {
    motion::Camera current, previous;
    motion::Surface x, blocker, candidate;
    unsigned destination = 24 * 64 + 32;
};
Fixture translation() {
    OracleCamera old, now;
    now.origin = {1, 0, 0};
    // X's old projection is36.2, while sampled O's old anchor is36.5. Mixing
    // those anchors shifts alternate48.2 to47.9 and selects a DIFFERENT pixel.
    return {packed(now), packed(old), surface({1.05f, -1.f / 6, 8}),
            surface({.28125f, -1.f / 24, 2}, 4), surface({4.125f, -1.f / 6, 8})};
}
void dual_translation() {
    auto f = translation();
    const auto x = motion::project(f.current, f.x.common_position);
    const auto y0 = motion::project(f.previous, f.x.common_position);
    const auto y = motion::project(f.previous, f.blocker.common_position);
    const auto z = motion::project(f.current, f.blocker.common_position);
    close(y0.pixel.x - x.pixel.x, 32. / 8, 1e-5, "Ordinary background parallax f*DeltaC/Zbg");
    close(y.pixel.x - z.pixel.x, 32. / 2, 1e-5, "Foreground blocker parallax f*DeltaC/Zfg");
    auto request = motion::begin(f.destination, 1, f.current, f.previous, f.x, f.blocker, policy);
    need(request.status == motion::Status::needs_dual_candidate &&
             request.source_pixel == 24 * 64 + 48,
         "Dual request point-samples alternate donor");
    close(request.alternate_raster.x, 48.2, 1e-5, "Discrete blocker anchor retained");
    auto result = motion::finish(request, f.destination, f.previous, f.x, f.candidate, policy);
    need(result.status == motion::Status::complete &&
             result.classification == motion::Classification::dual &&
             result.source_pixel == 24 * 64 + 48,
         "Compatible background candidate selects dual donor");
    unsigned wrong = 0;
    need(motion::point_index(motion::alternate(y0.pixel, x.pixel, z.pixel), 64, 48, wrong) &&
             wrong != result.source_pixel,
         "Wrong continuous old anchor is a failing negative control");
    need(motion::point_index(motion::alternate(y0.pixel, x.pixel, x.pixel), 64, 48, wrong) &&
             wrong != result.source_pixel,
         "Replacing old blocker by X fails parallax oracle");
    // Independent vertical translation has opposite raster sign (Y points down).
    OracleCamera old, now;
    now.origin = {0, .5, 0};
    const motion::Vec3 background{0, .5f, 8}, blocker{0, .125f, 2};
    const auto cx = motion::project(packed(now), background);
    const auto py = motion::project(packed(old), blocker);
    const auto cz = motion::project(packed(now), blocker);
    const auto alt = motion::alternate(py.pixel, cx.pixel, cz.pixel);
    close(alt.y - cx.pixel.y, -24 * .5 / 2, 1e-6, "Vertical foreground parallax has y-down sign");
    // Pure and combined rotations use independent double projections for every
    // term, including the foreground point rather than the background substitute.
    for (double angle : {.15, -.23}) {
        now.origin = {1, .5, .25};
        now.right = {std::cos(angle), 0, -std::sin(angle)};
        now.forward = {std::sin(angle), 0, std::cos(angle)};
        const D3 X{1, .5, 8}, O{.25, .125, 2};
        const auto xp = oracle(now, X), yp = oracle(old, O), zp = oracle(now, O);
        const auto got = motion::alternate(motion::project(packed(old), pack(O)).pixel,
                                           motion::project(packed(now), pack(X)).pixel,
                                           motion::project(packed(now), pack(O)).pixel);
        close(got.x, yp[0] + xp[0] - zp[0], 2e-5, "Rotated alternate X double oracle");
        close(got.y, yp[1] + xp[1] - zp[1], 2e-5, "Rotated alternate Y double oracle");
    }
}
void compatibility_and_fallback() {
    auto f = translation();
    auto flat = f.x;
    flat.common_position = {1.125f, -1.f / 6, 8}; // center of old pixel36
    auto ordinary = motion::begin(f.destination, 1, f.current, f.previous, f.x, flat, policy);
    need(ordinary.classification == motion::Classification::ordinary &&
             ordinary.source_pixel == 24 * 64 + 36,
         "Ordinary reprojection accepts local matching plane");
    const auto unchanged_camera = f.previous;
    auto static_x = surface({.125f, -1.f / 6, 8});
    ordinary = motion::begin(f.destination, 1, unchanged_camera, unchanged_camera, static_x,
                             static_x, policy);
    need(ordinary.classification == motion::Classification::ordinary &&
             ordinary.source_pixel == f.destination,
         "Static camera yields exact self ordinary donor");
    auto close_plane = f.x;
    close_plane.common_position.z += .125f;
    need(motion::compatible(f.x, close_plane, policy), "Half-footprint plane boundary admitted");
    close_plane.common_position.z += .001f;
    need(!motion::compatible(f.x, close_plane, policy),
         "Beyond half-footprint plane boundary rejected");
    auto concave = f.candidate;
    concave.common_position.z += 1;
    need(!motion::compatible(f.x, concave, policy), "Same instance concave remote plane rejected");
    auto normal = f.candidate;
    normal.geometric_normal = {0, 0, 100};
    need(!motion::compatible(f.x, normal, policy),
         "Opposite geometric normal rejected regardless of magnitude");
    normal.geometric_normal = {0, 0, -100};
    need(motion::compatible(f.x, normal, policy), "Normal comparison is normalized");
    normal.geometric_normal = {.2f, 0, -1};
    need(!motion::compatible(f.x, normal, policy),
         "Declared cosine threshold rejects tilted plane");
    auto grouped = policy;
    grouped.require_group = 1;
    need(!motion::compatible(f.x, f.candidate, grouped),
         "Missing optional groups are not claimed compatible");
    auto a = f.x, b = f.candidate;
    a.group = b.group = 11;
    need(motion::compatible(a, b, grouped), "Explicit equal groups accepted");
    b.group = 12;
    need(!motion::compatible(a, b, grouped), "Explicit unequal groups rejected");
    b = f.candidate;
    b.material++;
    need(!motion::compatible(a, b, policy), "Material mismatch rejected");
    b = f.candidate;
    b.instance++;
    need(!motion::compatible(a, b, policy), "Instance mismatch rejected");
    auto request = motion::begin(f.destination, 1, f.current, f.previous, f.x, f.blocker, policy);
    for (auto candidate : {concave, b, motion::Surface{}}) {
        const auto result =
            motion::finish(request, f.destination, f.previous, f.x, candidate, policy);
        need(result.status == motion::Status::complete && result.source_pixel == f.destination &&
                 result.classification == motion::Classification::same_pixel_fallback,
             "Unusable dual geometry retains labeled same-pixel domain");
    }
    b = f.candidate;
    b.common_position.x += .25f;
    need(motion::finish(request, f.destination, f.previous, f.x, b, policy).classification ==
             motion::Classification::same_pixel_fallback,
         "Wrong queried pixel cannot impersonate candidate");
    for (auto kind : {motion::Geometry::sky, motion::Geometry::unsupported}) {
        auto old = f.blocker;
        old.geometry = kind;
        auto result = motion::begin(f.destination, 1, f.current, f.previous, f.x, old, policy);
        need(result.classification == motion::Classification::same_pixel_fallback &&
                 result.source_pixel == f.destination,
             "Sky/unsupported old geometry cannot supply finite blocker O");
        auto current = f.x;
        current.geometry = kind;
        result = motion::begin(f.destination, 1, f.current, f.previous, current, f.blocker, policy);
        need(result.classification == motion::Classification::same_pixel_fallback,
             "Sky/unsupported current geometry uses explicit fallback");
    }
    auto cold = motion::begin(f.destination, 0, f.current, f.previous, {}, {}, policy);
    need(cold.classification == motion::Classification::cold && cold.source_pixel == f.destination,
         "Cold history is separate from valid fallback");
    auto error = f.blocker;
    error.geometry = motion::Geometry::error;
    need(motion::begin(f.destination, 1, f.current, f.previous, f.x, error, policy).status ==
             motion::Status::invalid_input,
         "Claimed geometry error is not correspondence success");
    need(!motion::foreground(.5f, .5f, policy), "Equal depth is not foreground");
    need(!motion::foreground(.5f + std::numeric_limits<float>::epsilon(), .5f, policy),
         "Depth ambiguity within predeclared precision margin rejected");
    need(motion::foreground(.5f, .125f, policy), "Strict finite foreground separation admitted");
    // Thin old blocker remains finite and changes candidate geometry only; no
    // filtered depth or fabricated continuous blocker is requested by the API.
    f.blocker.pixel_footprint = .0001f;
    need(motion::begin(f.destination, 1, f.current, f.previous, f.x, f.blocker, policy).status ==
             motion::Status::needs_dual_candidate,
         "Thin finite blocker uses its discrete own anchor");
    f.current = packed(OracleCamera{.origin = {5, 0, 0}});
    f.x = surface({5.05f, -1.f / 6, 8});
    f.blocker = surface({1.28125f, -1.f / 24, 2}, 4);
    auto edge = motion::begin(f.destination, 1, f.current, f.previous, f.x, f.blocker, policy);
    need(edge.source_pixel == f.destination &&
             edge.classification == motion::Classification::same_pixel_fallback &&
             edge.status == motion::Status::complete,
         "Offscreen alternate is fallback, never edge clamp");
    auto bad_policy = policy;
    bad_policy.maximum_plane_footprints = -1;
    need(
        motion::begin(f.destination, 1, f.current, f.previous, f.x, f.blocker, bad_policy).status ==
            motion::Status::invalid_input,
        "Invalid explicit policy diagnosed");
}
void additional_contract_edges() {
    auto f = translation();
    const auto request =
        motion::begin(f.destination, 1, f.current, f.previous, f.x, f.blocker, policy);
    auto bad = f.candidate;
    bad.pixel_footprint = 0;
    need(!motion::compatible(f.x, bad, policy),
         "Zero footprint cannot silently choose scene tolerance");
    bad.pixel_footprint = std::numeric_limits<float>::infinity();
    need(!motion::compatible(f.x, bad, policy), "Infinite footprint rejected");
    bad = f.candidate;
    bad.geometric_normal = {};
    need(!motion::compatible(f.x, bad, policy), "Zero normal rejected");
    bad.geometric_normal.x = std::numeric_limits<float>::quiet_NaN();
    need(!motion::compatible(f.x, bad, policy), "Nonfinite normal rejected");
    bad = f.candidate;
    bad.geometry = motion::Geometry::sky;
    auto result = motion::finish(request, f.destination, f.previous, f.x, bad, policy);
    need(result.source_pixel == f.destination &&
             result.classification == motion::Classification::same_pixel_fallback,
         "Sky candidate never masquerades as finite dual background");
    bad.geometry = motion::Geometry::error;
    need(motion::finish(request, f.destination, f.previous, f.x, bad, policy).status ==
             motion::Status::invalid_input,
         "Candidate protocol error explicitly diagnosed");
    auto mismatched = f.previous;
    mismatched.width++;
    need(motion::begin(f.destination, 1, f.current, mismatched, f.x, f.blocker, policy).status ==
             motion::Status::invalid_input,
         "Mismatched camera dimensions diagnosed");
    need(motion::begin(64 * 48, 1, f.current, f.previous, f.x, f.blocker, policy).status ==
             motion::Status::invalid_input,
         "Invalid destination cannot become fallback pixel");
    need(motion::begin(f.destination, 2, f.current, f.previous, f.x, f.blocker, policy).status ==
             motion::Status::invalid_input,
         "History-valid flag is structural boolean");
    auto near = f.current;
    near.view_to_clip[14] = 16;
    result = motion::begin(f.destination, 1, near, f.previous, f.x, f.blocker, policy);
    need(result.source_pixel == f.destination &&
             result.classification == motion::Classification::same_pixel_fallback,
         "Guide near rejection preserves the same-pixel transport domain");
    auto offscreen = f.previous;
    offscreen.view_to_clip[8] = 4;
    result = motion::begin(f.destination, 1, f.current, offscreen, f.x, f.blocker, policy);
    need(result.source_pixel == f.destination &&
             result.classification == motion::Classification::same_pixel_fallback,
         "Offscreen ordinary projection falls back without sampling an edge");
    // Plane positions must share one coordinate space, while camera deltas
    // convert that space into each matrix origin. A common rebase preserves q.
    const motion::Vec3 delta{4096, -2048, 512};
    f.x.common_position = motion::add(f.x.common_position, delta);
    f.blocker.common_position = motion::add(f.blocker.common_position, delta);
    f.candidate.common_position = motion::add(f.candidate.common_position, delta);
    f.current.common_to_matrix_origin = motion::subtract(f.current.common_to_matrix_origin, delta);
    f.previous.common_to_matrix_origin =
        motion::subtract(f.previous.common_to_matrix_origin, delta);
    const auto rebased =
        motion::begin(f.destination, 1, f.current, f.previous, f.x, f.blocker, policy);
    result = motion::finish(rebased, f.destination, f.previous, f.x, f.candidate, policy);
    need(result.classification == motion::Classification::dual &&
             result.source_pixel == request.source_pixel,
         "Explicit common-space rebase preserves donor and local plane checks");
    // Pure nontrivial camera rotation, with no translation, checked separately
    // from the earlier combined-motion oracle.
    OracleCamera old, now;
    constexpr double angle = .43;
    now.right = {std::cos(angle), 0, -std::sin(angle)};
    now.forward = {std::sin(angle), 0, std::cos(angle)};
    const D3 X{.5, .25, 8}, O{.125, .0625, 2};
    const auto xp = oracle(now, X), yp = oracle(old, O), zp = oracle(now, O);
    const auto got = motion::alternate(motion::project(packed(old), pack(O)).pixel,
                                       motion::project(packed(now), pack(X)).pixel,
                                       motion::project(packed(now), pack(O)).pixel);
    close(got.x, yp[0] + xp[0] - zp[0], 2e-5, "Pure rotation alternate X double oracle");
    close(got.y, yp[1] + xp[1] - zp[1], 2e-5, "Pure rotation alternate Y double oracle");
    auto invalid = policy;
    invalid.relative_depth_separation = -1;
    need(!motion::foreground(.5f, .125f, invalid), "Invalid foreground policy rejected");
}
void projection_error_propagation() {
    const auto f = translation();
    const auto request =
        motion::begin(f.destination, 1, f.current, f.previous, f.x, f.blocker, policy);
    need(request.status == motion::Status::needs_dual_candidate,
         "Error controls start from real dual request");
    const auto invalid = [&](motion::Decision result, const char *why) {
        need(result.status == motion::Status::invalid_input &&
                 result.source_pixel == f.destination &&
                 result.classification == motion::Classification::same_pixel_fallback,
             why);
    };
    for (bool current : {true, false}) {
        auto a = f.current, b = f.previous;
        (current ? a : b).world_to_view[0] = std::numeric_limits<float>::quiet_NaN();
        invalid(motion::begin(f.destination, 1, a, b, f.x, f.blocker, policy),
                "Begin propagates malformed current/previous camera projection");
        a = f.current;
        b = f.previous;
        (current ? a : b).world_to_view[10] = std::numeric_limits<float>::max();
        invalid(motion::begin(f.destination, 1, a, b, f.x, f.blocker, policy),
                "Begin propagates finite overflow projecting X in either camera");
    }
    // X's previous projection remains finite, but reading the old blocker's
    // finite geometry overflows that previous projection before any index cast.
    auto previous = f.previous;
    previous.world_to_view[10] = 2;
    auto blocker = f.blocker;
    blocker.common_position.z = std::numeric_limits<float>::max();
    invalid(motion::begin(f.destination, 1, f.current, previous, f.x, blocker, policy),
            "Begin propagates overflow at old blocker anchor y");
    // Both previous projections fit the same pixel and O is foreground. A narrow
    // current camera overflows only O's clip X when computing z. All coefficients
    // here are finite normal FLOATs; this is not a NaN-camera shortcut.
    auto current = packed(OracleCamera{});
    previous = current;
    current.view_to_clip[0] = 256;
    previous.view_to_clip[0] = 2e-38f;
    const auto x = surface({0, 0, 8});
    blocker = surface({std::numeric_limits<float>::max() / 128, 0, 4}, 4);
    const auto py = motion::project(previous, blocker.common_position);
    need(py.status == motion::ProjectionStatus::valid &&
             motion::foreground(py.depth, motion::project(previous, x.common_position).depth,
                                policy),
         "Current-blocker overflow control has a valid previous foreground projection");
    invalid(motion::begin(f.destination, 1, current, previous, x, blocker, policy),
            "Begin propagates finite overflow at current blocker projection z");
    previous = f.previous;
    previous.view_to_clip[0] = std::numeric_limits<float>::quiet_NaN();
    invalid(motion::finish(request, f.destination, previous, f.x, f.candidate, policy),
            "Finish propagates malformed previous camera projection");
    previous = f.previous;
    previous.world_to_view[0] = std::numeric_limits<float>::max();
    invalid(motion::finish(request, f.destination, previous, f.x, f.candidate, policy),
            "Finish propagates finite candidate projection overflow");
    auto error = f.x;
    error.geometry = motion::Geometry::error;
    invalid(motion::finish(request, f.destination, f.previous, error, f.candidate, policy),
            "Finish diagnoses current geometry error consistently with begin");
    for (unsigned kind : {0u, 1u, 2u}) {
        previous = f.previous;
        if (kind == 0)
            previous.world_to_view[14] = -16;
        if (kind == 1)
            previous.view_to_clip[14] = 16;
        if (kind == 2)
            previous.view_to_clip[8] = 4;
        const auto result =
            motion::finish(request, f.destination, previous, f.x, f.candidate, policy);
        need(result.status == motion::Status::complete && result.source_pixel == f.destination &&
                 result.classification == motion::Classification::same_pixel_fallback,
             "Finite behind/near/offscreen finish remains an expected geometry fallback");
    }
    current = f.current;
    current.world_to_view[14] = -16;
    const auto behind =
        motion::begin(f.destination, 1, current, f.previous, f.x, f.blocker, policy);
    need(behind.status == motion::Status::complete && behind.source_pixel == f.destination &&
             behind.classification == motion::Classification::same_pixel_fallback,
         "Finite behind-camera begin remains expected fallback");
}
} // namespace
int main() {
    try {
        projection_oracle();
        point_sampling();
        dual_translation();
        compatibility_and_fallback();
        additional_contract_edges();
        projection_error_propagation();
        std::cout << "Enhanced camera-only motion passed " << checks << " CPU checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Enhanced camera-only motion: " << error.what() << '\n';
        return 1;
    }
}
