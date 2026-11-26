// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <metal_raytracing>
#include <metal_stdlib>

#include "../src/geometry_wire.h"
#include "analytic.h"
#include "bilinear.h"
#include "triangle_candidate.h"
#include "triangle_traversal_bounds.h"

// Undefined in existing probes/custom compensated kernels: preserve that contract.
constant bool rypath_native_intersections [[function_constant(21)]];
inline bool native_intersections_enabled() {
    return is_function_constant_defined(rypath_native_intersections) ? rypath_native_intersections
                                                                     : false;
}

using MixedIntersector =
    metal::raytracing::intersector<metal::raytracing::triangle_data, metal::raytracing::instancing,
                                   metal::raytracing::world_space_data>;
using MixedIntersectionTable =
    metal::raytracing::intersection_function_table<metal::raytracing::triangle_data,
                                                   metal::raytracing::instancing,
                                                   metal::raytracing::world_space_data>;
using MixedHit = metal::raytracing::intersection_result<metal::raytracing::triangle_data,
                                                        metal::raytracing::instancing,
                                                        metal::raytracing::world_space_data>;

inline metal::float3 patch_corner(const device rypath::geometry_wire::Patch &patch, uint corner) {
    return metal::float3(patch.corners[corner][0], patch.corners[corner][1],
                         patch.corners[corner][2]);
}

inline BilinearIntersection intersect_patch(const device rypath::geometry_wire::Patch &patch,
                                            metal::float3 origin, metal::float3 direction,
                                            float t_min, float t_max) {
    return intersect_bilinear(patch_corner(patch, 0), patch_corner(patch, 1),
                              patch_corner(patch, 2), patch_corner(patch, 3), origin, direction,
                              t_min, t_max);
}

// Payload values are written only for accepted candidates. Retain the nearest
// patch and a deterministic identity at exact ties, independently of Metal's
// choice among co-nearest primitives. Triangle hits never consume this payload.
struct PatchPayload {
    float t;
    uint instance;
    uint primitive;
    metal::float2 uv;
    uint primitive_kind;
};

inline PatchPayload empty_patch_payload() {
    return {INFINITY, 0xffffffffu, 0xffffffffu, {0.0f, 0.0f}, rypath::geometry_wire::miss};
}

struct SurfaceHit {
    metal::raytracing::intersection_type type;
    uint instance_id;
    uint geometry_id;
    uint primitive_id;
    float distance;
    metal::float2 coordinates;
    bool valid;
    uint primitive_kind = 0;
};

struct MaskedAlphaResult {
    bool accepted;
    bool valid;
};

namespace rypath_intersection_detail {
using namespace metal;
using namespace metal::raytracing;
using Query = intersection_query<triangle_data, instancing>;
inline SurfaceHit invalid_hit() { return {intersection_type::none, 0, 0, 0, 0, {0, 0}, false}; }
struct AcceptAll {
    MaskedAlphaResult operator()(uint, uint, uint, intersection_type, float2, float) {
        return {true, true};
    }
};
template <typename Alpha> struct PatchPredicate {
    thread Alpha &alpha;
    uint instance, geometry, primitive;
    bool valid;
    bool operator()(float2 uv, float t) {
        if (!valid)
            return false;
        auto result = alpha(instance, geometry, primitive, intersection_type::bounding_box, uv, t);
        valid = result.valid;
        return valid && result.accepted;
    }
};
struct CurrentTriangleReuse {
    bool matched, valid;
};
// A reset changes only traversal_max. The original world ray/bounds, primitive
// data and instance transform remain immutable throughout this trace call.
// Reuse only the already accepted current-best triangle; alpha stays observable.
template <typename Alpha>
inline CurrentTriangleReuse reuse_current_triangle(const thread SurfaceHit &best,
                                                   const thread triangle_extended::Pair &distance,
                                                   uint instance, uint geometry, uint primitive,
                                                   thread Alpha &alpha) {
    if (best.type != intersection_type::triangle ||
        best.primitive_kind != rypath::geometry_wire::triangle || instance != best.instance_id ||
        geometry != best.geometry_id || primitive != best.primitive_id)
        return {false, true};
    const auto accepted = alpha(instance, geometry, primitive, intersection_type::triangle,
                                best.coordinates, best.distance);
    if (!accepted.valid)
        return {true, false};
    // Match the original ordering: rejected alpha does not reach the distance
    // validity check. Accepted alpha must still validate the retained full pair.
    if (accepted.accepted && !triangle_extended::valid(distance))
        return {true, false};
    // Rejection does not revoke a previously accepted best. Acceptance cannot
    // replace the identical full distance and IDs, so neither case needs a cap.
    return {true, true};
}
inline bool earlier(uint instance, uint geometry, uint primitive, const thread SurfaceHit &best) {
    return instance < best.instance_id ||
           (instance == best.instance_id &&
            (geometry < best.geometry_id ||
             (geometry == best.geometry_id && primitive < best.primitive_id)));
}
} // namespace rypath_intersection_detail

// Internal packet-only preaccepted opaque candidate. Public/generic callers
// retain nullptr; no alpha callback acceptance is cached for arbitrary Alpha.
struct TraceSurfaceSeed {
    bool available = false;
    SurfaceHit surface{};
    triangle_extended::Pair distance{};
    bool has_cap = false;
    float cap = INFINITY;
};

// Compensated mode never commits native rounded hit distances: a conservative
// world-box exit cap may restart traversal while retaining the precise closest
// hit. Native mode commits accepted hardware/procedural binary32 distances.
// Candidate predicates always use the original ray bounds.
template <typename Alpha>
inline SurfaceHit trace_validated_surface(metal::raytracing::ray ray,
                                          metal::raytracing::instance_acceleration_structure scene,
                                          uint mask, thread Alpha &alpha, bool any_hit = false,
                                          float initial_traversal_max = INFINITY,
                                          const thread TraceSurfaceSeed *initial_seed = nullptr) {
    using namespace metal;
    using namespace metal::raytracing;
    using namespace rypath_intersection_detail;
    if (!pc_normal(ray.min_distance) || ray.min_distance < 0 || isnan(ray.max_distance) ||
        ray.max_distance < ray.min_distance ||
        (isfinite(ray.max_distance) && !pc_normal(ray.max_distance)))
        return invalid_hit();
    for (uint axis = 0; axis < 3; ++axis)
        if (!pc_normal(ray.origin[axis]) || !pc_normal(ray.direction[axis]))
            return invalid_hit();
    if (all(ray.direction == 0))
        return invalid_hit();
    intersection_params parameters;
    parameters.force_opacity(forced_opacity::non_opaque);
    parameters.accept_any_intersection(false);
    parameters.set_triangle_cull_mode(triangle_cull_mode::none);
    float starting_max = min(ray.max_distance, initial_traversal_max);
    if (isnan(initial_traversal_max) || starting_max < ray.min_distance)
        return invalid_hit();
    if ((any_hit || native_intersections_enabled()) && initial_seed != nullptr)
        return invalid_hit();
    SurfaceHit best{
        intersection_type::none, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0, {0, 0}, true};
    triangle_extended::Pair distance{};
    if (initial_seed != nullptr && initial_seed->available) {
        const auto seed = initial_seed->surface;
        if (!seed.valid || seed.type != intersection_type::triangle ||
            seed.primitive_kind != rypath::geometry_wire::triangle ||
            seed.instance_id == 0xffffffffu || seed.geometry_id != 0 ||
            seed.primitive_id == 0xffffffffu || !isfinite(seed.distance) ||
            !all(isfinite(seed.coordinates)) || !triangle_extended::valid(initial_seed->distance))
            return invalid_hit();
        const auto lower = triangle_extended::compare(
            initial_seed->distance,
            triangle_extended::pair_from_bits(as_type<uint>(ray.min_distance)));
        if (!lower.valid || lower.order < 0)
            return invalid_hit();
        if (isfinite(ray.max_distance)) {
            const auto upper = triangle_extended::compare(
                initial_seed->distance,
                triangle_extended::pair_from_bits(as_type<uint>(ray.max_distance)));
            if (!upper.valid || upper.order > 0)
                return invalid_hit();
        }
        // Store the complete pair before constructing the query. It survives
        // both an unavailable cap and an empty native procedural offer stream.
        best = seed;
        distance = initial_seed->distance;
        if (initial_seed->has_cap && pc_normal(initial_seed->cap) &&
            initial_seed->cap > ray.min_distance && initial_seed->cap < starting_max) {
            const auto exterior = triangle_extended::compare(
                triangle_extended::pair_from_bits(as_type<uint>(initial_seed->cap)), distance);
            if (exterior.valid && exterior.order > 0)
                starting_max = initial_seed->cap;
        }
    }
    auto starting_ray = ray;
    starting_ray.max_distance = starting_max;
    Query query(starting_ray, scene, mask, parameters);
    float traversal_max = starting_max;
    while (query.next()) {
        auto kind = query.get_candidate_intersection_type();
        auto surface_kind = kind;
        uint instance = query.get_candidate_instance_id(),
             geometry = query.get_candidate_geometry_id(),
             primitive = query.get_candidate_primitive_id();
        BilinearNumber t(INFINITY);
        triangle_extended::Pair candidate_distance{};
        const device rypath::geometry_wire::Triangle *triangle_record = nullptr;
        float2 uv(0);
        uint primitive_kind = 0;
        bool found = false;
        if (kind == intersection_type::triangle) {
            if (native_intersections_enabled()) {
                // The hardware predicate and binary32 result define this profile.
                // Do not validate or replace it with the compensated predicate.
                auto record = static_cast<const device rypath::geometry_wire::NativeTriangleTag *>(
                    query.get_candidate_primitive_data());
                if (!record || (record->kind != rypath::geometry_wire::triangle &&
                                record->kind != rypath::geometry_wire::miss)) {
                    query.abort();
                    return invalid_hit();
                }
                if (record->kind == rypath::geometry_wire::miss)
                    continue;
                const float native_t = query.get_candidate_triangle_distance();
                uv = query.get_candidate_triangle_barycentric_coord();
                if (!isfinite(native_t) || native_t < ray.min_distance ||
                    native_t > ray.max_distance || !all(isfinite(uv))) {
                    query.abort();
                    return invalid_hit();
                }
                const auto accepted = alpha(instance, geometry, primitive, kind, uv, native_t);
                if (!accepted.valid) {
                    query.abort();
                    return invalid_hit();
                }
                if (!accepted.accepted)
                    continue;
                t = BilinearNumber(native_t);
                candidate_distance = triangle_extended::pair_from_bits(as_type<uint>(native_t));
                primitive_kind = rypath::geometry_wire::triangle;
                found = true;
            } else {
                auto record = static_cast<const device rypath::geometry_wire::Triangle *>(
                    query.get_candidate_primitive_data());
                if (!record) {
                    query.abort();
                    return invalid_hit();
                }
                const auto reused =
                    reuse_current_triangle(best, distance, instance, geometry, primitive, alpha);
                if (reused.matched) {
                    if (!reused.valid) {
                        query.abort();
                        return invalid_hit();
                    }
                    continue;
                }
                auto hit = precise_triangle_candidate(
                    *record, query.get_candidate_object_to_world_transform(), ray.origin,
                    ray.direction, ray.min_distance, ray.max_distance);
                if (!hit.valid) {
                    query.abort();
                    return invalid_hit();
                }
                if (!hit.hit)
                    continue;
                auto accepted = alpha(instance, geometry, primitive, kind, hit.uv, hit.t.high);
                if (!accepted.valid) {
                    query.abort();
                    return invalid_hit();
                }
                if (!accepted.accepted)
                    continue;
                t = hit.t;
                candidate_distance = hit.distance;
                triangle_record = record;
                uv = hit.uv;
                primitive_kind = rypath::geometry_wire::triangle;
                found = true;
            }
        } else if (kind == intersection_type::bounding_box) {
            auto record = static_cast<const device rypath::geometry_wire::ProceduralRecord *>(
                query.get_candidate_primitive_data());
            if (!record) {
                query.abort();
                return invalid_hit();
            }
            if (record->kind == rypath::geometry_wire::miss)
                continue;
            if (record->kind == rypath::geometry_wire::triangle) {
                const auto reused =
                    reuse_current_triangle(best, distance, instance, geometry, primitive, alpha);
                if (reused.matched) {
                    if (!reused.valid) {
                        query.abort();
                        return invalid_hit();
                    }
                    continue;
                }
                auto hit = precise_triangle_candidate(
                    record->triangle, query.get_candidate_object_to_world_transform(), ray.origin,
                    ray.direction, ray.min_distance, ray.max_distance);
                if (!hit.valid) {
                    query.abort();
                    return invalid_hit();
                }
                if (!hit.hit)
                    continue;
                auto accepted = alpha(instance, geometry, primitive, intersection_type::triangle,
                                      hit.uv, hit.t.high);
                if (!accepted.valid) {
                    query.abort();
                    return invalid_hit();
                }
                if (!accepted.accepted)
                    continue;
                t = hit.t;
                candidate_distance = hit.distance;
                triangle_record = &record->triangle;
                uv = hit.uv;
                primitive_kind = rypath::geometry_wire::triangle;
                surface_kind = intersection_type::triangle;
                found = true;
            } else {
                PatchPredicate<Alpha> accept{alpha, instance, geometry, primitive, true};
                AnalyticIntersection hit{false, INFINITY, {0, 0}};
                if (record->kind == rypath::geometry_wire::bilinear_patch) {
                    auto p = intersect_bilinear_if(
                        patch_corner(record->patch, 0), patch_corner(record->patch, 1),
                        patch_corner(record->patch, 2), patch_corner(record->patch, 3),
                        query.get_candidate_ray_origin(), query.get_candidate_ray_direction(),
                        ray.min_distance, ray.max_distance, accept);
                    hit = {p.hit, p.t, p.uv};
                } else if (record->kind == rypath::geometry_wire::analytic) {
                    hit = intersect_analytic_if(record->analytic, query.get_candidate_ray_origin(),
                                                query.get_candidate_ray_direction(),
                                                ray.min_distance, ray.max_distance, accept);
                } else {
                    query.abort();
                    return invalid_hit();
                }
                if (!accept.valid) {
                    query.abort();
                    return invalid_hit();
                }
                if (hit.hit) {
                    t = BilinearNumber(hit.t);
                    candidate_distance = triangle_extended::pair_from_bits(as_type<uint>(hit.t));
                    uv = hit.uv;
                    primitive_kind = record->kind;
                    found = true;
                }
            }
        } else {
            query.abort();
            return invalid_hit();
        }
        if (found) {
            if (!triangle_extended::valid(candidate_distance)) {
                query.abort();
                return invalid_hit();
            }
            if (any_hit) {
                query.abort();
                return {surface_kind, instance, geometry, primitive,
                        t.high,       uv,       true,     primitive_kind};
            }
            bool replace = best.type == intersection_type::none;
            if (!replace) {
                const auto compared = triangle_extended::compare(candidate_distance, distance);
                if (!compared.valid) {
                    query.abort();
                    return invalid_hit();
                }
                replace = compared.order < 0 ||
                          (compared.order == 0 && earlier(instance, geometry, primitive, best));
            }
            if (replace) {
                distance = candidate_distance;
                best = {surface_kind, instance, geometry, primitive,
                        t.high,       uv,       true,     primitive_kind};
                if (native_intersections_enabled()) {
                    // Hardware distances also bound later procedural/native traversal.
                    // Predicates retain the caller's original finite segment endpoints.
                    if (kind == intersection_type::triangle)
                        query.commit_triangle_intersection();
                    else
                        query.commit_bounding_box_intersection(t.high);
                } else if (triangle_record != nullptr) {
                    const auto cap = triangle_pruning_cap(
                        *triangle_record, query.get_candidate_object_to_world_transform(),
                        ray.origin, ray.direction, distance, ray.min_distance, traversal_max);
                    if (cap.finite) {
                        traversal_max = as_type<float>(cap.bits);
                        auto traversal_ray = ray;
                        traversal_ray.max_distance = traversal_max;
                        query.reset(traversal_ray, scene, mask, parameters);
                        // No candidate getter is used again until next() succeeds.
                    }
                }
            }
        }
    }
    return best;
}

inline SurfaceHit trace_surface(thread MixedIntersector &trace, metal::raytracing::ray ray,
                                metal::raytracing::instance_acceleration_structure scene, uint mask,
                                MixedIntersectionTable table) {
    (void)trace;
    (void)table;
    rypath_intersection_detail::AcceptAll alpha;
    return trace_validated_surface(ray, scene, mask, alpha);
}
inline SurfaceHit trace_shadow(thread MixedIntersector &trace, metal::raytracing::ray ray,
                               metal::raytracing::instance_acceleration_structure scene, uint mask,
                               MixedIntersectionTable table) {
    (void)trace;
    (void)table;
    rypath_intersection_detail::AcceptAll alpha;
    return trace_validated_surface(ray, scene, mask, alpha, true);
}

#include "native_triangle_hint.h"
