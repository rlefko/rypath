// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#define RYPATH_MOTION_THREAD thread
#else
#include <cmath>
#define RYPATH_MOTION_THREAD
#endif

// Static resident geometry, camera-only donor heuristic. This is not a physical
// acceptance test or a path shift. Callers query immutable geometry at requested
// pixels and replay U,j in the resulting fixed source/destination domain pair.
namespace rypath_enhanced_motion {
using Word = unsigned;
#if defined(__METAL_VERSION__)
using Wide = ulong;
inline bool finite(float x) { return metal::isfinite(x); }
inline float root(float x) { return metal::sqrt(x); }
inline float floor_value(float x) { return metal::floor(x); }
#else
using Wide = unsigned long long;
inline bool finite(float x) { return std::isfinite(x); }
inline float root(float x) { return std::sqrt(x); }
inline float floor_value(float x) { return std::floor(x); }
#endif
struct Vec2 {
    float x = 0, y = 0;
};
struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline float absolute(float x) { return x < 0 ? -x : x; }
inline float maximum(float a, float b) { return a > b ? a : b; }
inline bool finite(Vec3 p) { return finite(p.x) && finite(p.y) && finite(p.z); }
inline Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 subtract(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float dot(Vec3 a, Vec3 b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
inline bool unit(Vec3 v, RYPATH_MOTION_THREAD Vec3 &n) {
    if (!finite(v))
        return false;
    const float scale = maximum(absolute(v.x), maximum(absolute(v.y), absolute(v.z)));
    if (!(scale > 0))
        return false;
    v = {v.x / scale, v.y / scale, v.z / scale};
    const float length = root(dot(v, v));
    n = {v.x / length, v.y / length, v.z / length};
    return finite(n);
}
struct Camera {
    // Column-major, column-vector convention of reconstruction::Camera.
    float world_to_view[16]{}, view_to_clip[16]{};
    // common_origin - this camera's MATRIX origin, formed in host double and
    // checked before packing. Every Surface.position uses that SAME common origin.
    Vec3 common_to_matrix_origin{};
    Word width = 0, height = 0;
};
inline bool dimensions(Word width, Word height) {
    // Binary32 represents every i+.5 pixel center through this axis bound.
    // This does not bound matrix/projection arithmetic error near image edges.
    return width && height && width <= (1u << 23) && height <= (1u << 23) &&
           Wide(width) * height <= Wide(0xffffffffu);
}
enum class ProjectionStatus : Word { valid, invalid_input, behind_camera, before_near_plane };
struct Projection {
    Vec2 pixel{};
    float depth = 0;
    ProjectionStatus status = ProjectionStatus::invalid_input;
};
inline Projection project(RYPATH_MOTION_THREAD const Camera &camera, Vec3 common_position) {
    Projection out;
    if (!dimensions(camera.width, camera.height) || !finite(common_position) ||
        !finite(camera.common_to_matrix_origin))
        return out;
    const auto p = add(common_position, camera.common_to_matrix_origin);
    if (!finite(p))
        return out;
    const float point[4]{p.x, p.y, p.z, 1};
    float view[4]{}, clip[4]{};
    for (Word row = 0; row < 4; ++row) {
        for (Word col = 0; col < 4; ++col) {
            if (!finite(camera.world_to_view[col * 4 + row]) ||
                !finite(camera.view_to_clip[col * 4 + row]))
                return out;
            view[row] += camera.world_to_view[col * 4 + row] * point[col];
        }
        if (!finite(view[row]))
            return out;
    }
    for (Word row = 0; row < 4; ++row) {
        for (Word col = 0; col < 4; ++col)
            clip[row] += camera.view_to_clip[col * 4 + row] * view[col];
        if (!finite(clip[row]))
            return out;
    }
    if (!(view[2] > 0) || !(clip[3] > 0)) {
        out.status = ProjectionStatus::behind_camera;
        return out;
    }
    out.pixel = {(clip[0] / clip[3] + 1) * .5f * camera.width,
                 (1 - clip[1] / clip[3]) * .5f * camera.height};
    out.depth = clip[2] / clip[3];
    if (!finite(out.pixel.x) || !finite(out.pixel.y) || !finite(out.depth) || !(out.depth > 0))
        return out;
    out.status = out.depth > 1 ? ProjectionStatus::before_near_plane : ProjectionStatus::valid;
    return out;
}
inline bool point_index(Vec2 pixel, Word width, Word height, RYPATH_MOTION_THREAD Word &index) {
    if (!dimensions(width, height) || !finite(pixel.x) || !finite(pixel.y) || pixel.x < 0 ||
        pixel.y < 0 || !(pixel.x < float(width)) || !(pixel.y < float(height)))
        return false;
    const Word x = Word(floor_value(pixel.x)), y = Word(floor_value(pixel.y));
    if (x >= width || y >= height)
        return false;
    index = y * width + x;
    return true;
}
inline Vec2 alternate(Vec2 old_blocker, Vec2 current_point, Vec2 current_blocker) {
    // The discrete old blocker's own old anchor, not the continuous projection
    // of X. Replacing O with X collapses the intended foreground parallax.
    return {old_blocker.x + (current_point.x - current_blocker.x),
            old_blocker.y + (current_point.y - current_blocker.y)};
}
enum class Geometry : Word { finite_surface, sky, unsupported, error };
enum : Word { absent_group = 0xffffffffu };
struct Surface {
    Vec3 common_position{}, geometric_normal{};
    // Positive local world-space size of one pixel, explicitly supplied by the
    // geometry producer. Never compare positions from different camera origins.
    float pixel_footprint = 0;
    Word instance = 0, material = 0, group = absent_group;
    Geometry geometry = Geometry::unsupported;
};
struct Policy {
    // Required explicit caller policy, without numerical/model defaults. The
    // initial tests predeclare .99 cosine, .5 pixel, 32*FLOAT epsilon separation.
    float minimum_normal_cosine, maximum_plane_footprints, relative_depth_separation;
    // No group is inferred from an instance/primitive. Enable only if BOTH
    // snapshots carry a documented stable geometry group with identical meaning.
    Word require_group;
};
inline bool valid_policy(Policy p) {
    return finite(p.minimum_normal_cosine) && p.minimum_normal_cosine >= 0 &&
           p.minimum_normal_cosine <= 1 && finite(p.maximum_plane_footprints) &&
           p.maximum_plane_footprints >= 0 && finite(p.relative_depth_separation) &&
           p.relative_depth_separation >= 0 && p.relative_depth_separation < 1 &&
           p.require_group <= 1;
}
inline bool valid_surface(Surface s) {
    Vec3 normal;
    return s.geometry == Geometry::finite_surface && finite(s.common_position) &&
           finite(s.pixel_footprint) && s.pixel_footprint > 0 && unit(s.geometric_normal, normal);
}
inline bool compatible(Surface current, Surface old, Policy policy) {
    if (!valid_policy(policy) || !valid_surface(current) || !valid_surface(old) ||
        current.instance != old.instance || current.material != old.material ||
        (policy.require_group && (current.group == absent_group || old.group == absent_group ||
                                  current.group != old.group)))
        return false;
    Vec3 a, b;
    unit(current.geometric_normal, a);
    unit(old.geometric_normal, b);
    if (dot(a, b) < policy.minimum_normal_cosine)
        return false;
    const auto delta = subtract(old.common_position, current.common_position);
    const float tolerance =
        policy.maximum_plane_footprints * maximum(current.pixel_footprint, old.pixel_footprint);
    const float da = absolute(dot(delta, a)), db = absolute(dot(delta, b));
    return finite(tolerance) && finite(da) && finite(db) && da <= tolerance && db <= tolerance;
}
enum class Classification : Word { cold, ordinary, dual, same_pixel_fallback };
enum class Status : Word { complete, needs_dual_candidate, invalid_input };
struct Decision {
    // No absent-domain sentinel while history is valid. Fallback always owns d.
    Word source_pixel = 0;
    Classification classification = Classification::same_pixel_fallback;
    Status status = Status::complete;
    Vec2 ordinary_raster{}, alternate_raster{};
};
inline bool foreground(float blocker_depth, float predicted_depth, Policy p) {
    return valid_policy(p) && finite(blocker_depth) && finite(predicted_depth) &&
           blocker_depth > 0 && blocker_depth <= 1 && predicted_depth > 0 && predicted_depth <= 1 &&
           blocker_depth - predicted_depth >
               p.relative_depth_separation * maximum(blocker_depth, predicted_depth);
}
// Query sequence: project X into previous camera; point-sample k; pass old[k]
// here. Only needs_dual_candidate permits a second query at source_pixel.
inline Decision begin(Word destination, Word history_valid,
                      RYPATH_MOTION_THREAD const Camera &current_camera,
                      RYPATH_MOTION_THREAD const Camera &previous_camera, Surface current,
                      Surface old_at_k, Policy policy) {
    Decision out;
    out.source_pixel = destination;
    if (!valid_policy(policy) || !dimensions(current_camera.width, current_camera.height) ||
        current_camera.width != previous_camera.width ||
        current_camera.height != previous_camera.height ||
        Wide(destination) >= Wide(current_camera.width) * current_camera.height ||
        history_valid > 1) {
        out.status = Status::invalid_input;
        return out;
    }
    if (!history_valid) {
        out.classification = Classification::cold;
        return out;
    }
    if (current.geometry == Geometry::error || old_at_k.geometry == Geometry::error) {
        out.status = Status::invalid_input;
        return out;
    }
    if (!valid_surface(current))
        return out;
    const auto x = project(current_camera, current.common_position);
    const auto y0 = project(previous_camera, current.common_position);
    if (x.status == ProjectionStatus::invalid_input ||
        y0.status == ProjectionStatus::invalid_input) {
        out.status = Status::invalid_input;
        return out;
    }
    Word k = 0, current_pixel = 0;
    if (x.status != ProjectionStatus::valid || y0.status != ProjectionStatus::valid ||
        !point_index(x.pixel, current_camera.width, current_camera.height, current_pixel) ||
        current_pixel != destination ||
        !point_index(y0.pixel, previous_camera.width, previous_camera.height, k))
        return out;
    out.ordinary_raster = y0.pixel;
    if (!valid_surface(old_at_k))
        return out;
    const auto y = project(previous_camera, old_at_k.common_position);
    if (y.status == ProjectionStatus::invalid_input) {
        out.status = Status::invalid_input;
        return out;
    }
    Word blocker_pixel = 0;
    if (y.status != ProjectionStatus::valid ||
        !point_index(y.pixel, previous_camera.width, previous_camera.height, blocker_pixel) ||
        blocker_pixel != k)
        return out;
    if (compatible(current, old_at_k, policy)) {
        out.classification = Classification::ordinary;
        out.source_pixel = k;
        return out;
    }
    if (!foreground(y.depth, y0.depth, policy))
        return out;
    const auto z = project(current_camera, old_at_k.common_position);
    if (z.status != ProjectionStatus::valid) {
        if (z.status == ProjectionStatus::invalid_input)
            out.status = Status::invalid_input;
        return out;
    }
    out.alternate_raster = alternate(y.pixel, x.pixel, z.pixel);
    Word q = 0;
    if (!point_index(out.alternate_raster, previous_camera.width, previous_camera.height, q))
        return out;
    out.source_pixel = q;
    out.status = Status::needs_dual_candidate;
    return out;
}
// Caller supplies the geometry at exactly request.source_pixel. Candidate
// projection checks prevent accidentally supplying a different old pixel.
inline Decision finish(Decision request, Word destination,
                       RYPATH_MOTION_THREAD const Camera &previous_camera, Surface current,
                       Surface old_at_q, Policy policy) {
    if (request.status != Status::needs_dual_candidate)
        return request;
    Decision out = request;
    out.status = Status::complete;
    out.source_pixel = destination;
    if (!valid_policy(policy) || !dimensions(previous_camera.width, previous_camera.height) ||
        Wide(destination) >= Wide(previous_camera.width) * previous_camera.height ||
        current.geometry == Geometry::error || old_at_q.geometry == Geometry::error) {
        out.status = Status::invalid_input;
        return out;
    }
    if (!compatible(current, old_at_q, policy))
        return out;
    const auto q = project(previous_camera, old_at_q.common_position);
    if (q.status == ProjectionStatus::invalid_input) {
        out.status = Status::invalid_input;
        return out;
    }
    Word pixel = 0;
    if (q.status != ProjectionStatus::valid ||
        !point_index(q.pixel, previous_camera.width, previous_camera.height, pixel) ||
        pixel != request.source_pixel)
        return out;
    out.classification = Classification::dual;
    out.source_pixel = request.source_pixel;
    return out;
}
} // namespace rypath_enhanced_motion
#undef RYPATH_MOTION_THREAD
