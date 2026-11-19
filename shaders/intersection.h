// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <metal_raytracing>
#include <metal_stdlib>

#include "../src/geometry_wire.h"
#include "analytic.h"
#include "bilinear.h"
#include "triangle_candidate.h"

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
inline bool earlier(uint instance, uint geometry, uint primitive, const thread SurfaceHit &best) {
    return instance < best.instance_id ||
           (instance == best.instance_id &&
            (geometry < best.geometry_id ||
             (geometry == best.geometry_id && primitive < best.primitive_id)));
}
} // namespace rypath_intersection_detail

// Never commit: native rounded distances must not prune a mathematically nearer
// candidate or a valid surface behind a native false positive. This validates
// offered candidates; native traversal completeness is still a bounded premise.
template <typename Alpha>
inline SurfaceHit trace_validated_surface(metal::raytracing::ray ray,
                                          metal::raytracing::instance_acceleration_structure scene,
                                          uint mask, thread Alpha &alpha, bool any_hit = false) {
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
    Query query(ray, scene, mask, parameters);
    SurfaceHit best{
        intersection_type::none, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0, {0, 0}, true};
    BilinearNumber distance(INFINITY);
    while (query.next()) {
        auto kind = query.get_candidate_intersection_type();
        uint instance = query.get_candidate_instance_id(),
             geometry = query.get_candidate_geometry_id(),
             primitive = query.get_candidate_primitive_id();
        BilinearNumber t(INFINITY);
        float2 uv(0);
        uint primitive_kind = 0;
        bool found = false;
        if (kind == intersection_type::triangle) {
            auto record = static_cast<const device rypath::geometry_wire::Triangle *>(
                query.get_candidate_primitive_data());
            if (!record) {
                query.abort();
                return invalid_hit();
            }
            auto hit = precise_triangle_candidate(
                *record, query.get_candidate_object_to_world_transform(), ray.origin, ray.direction,
                ray.min_distance, ray.max_distance);
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
            uv = hit.uv;
            primitive_kind = rypath::geometry_wire::triangle;
            found = true;
        } else if (kind == intersection_type::bounding_box) {
            auto record = static_cast<const device rypath::geometry_wire::ProceduralRecord *>(
                query.get_candidate_primitive_data());
            if (!record) {
                query.abort();
                return invalid_hit();
            }
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
                                            query.get_candidate_ray_direction(), ray.min_distance,
                                            ray.max_distance, accept);
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
                uv = hit.uv;
                primitive_kind = record->kind;
                found = true;
            }
        } else {
            query.abort();
            return invalid_hit();
        }
        if (found) {
            if (any_hit) {
                query.abort();
                return {kind, instance, geometry, primitive, t.high, uv, true, primitive_kind};
            }
            bool replace = best.type == intersection_type::none;
            if (!replace) {
                bool valid = true;
                auto difference = pc_sub(t, distance, valid);
                if (!valid) {
                    query.abort();
                    return invalid_hit();
                }
                int sign = precise_triangle_sign(difference);
                replace = sign < 0 || (sign == 0 && earlier(instance, geometry, primitive, best));
            }
            if (replace) {
                distance = t;
                best = {kind, instance, geometry, primitive, t.high, uv, true, primitive_kind};
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
