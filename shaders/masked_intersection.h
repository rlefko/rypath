// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "intersection.h"

struct MaskedAlphaResult {
    bool accepted;
    bool valid;
};

namespace rypath_masked_detail {
using namespace metal;
using namespace metal::raytracing;

using Query = intersection_query<triangle_data, instancing>;

inline SurfaceHit invalid_hit() { return {intersection_type::none, 0, 0, 0, 0, {0, 0}, false}; }

struct AcceptedPatch {
    uint instance;
    uint geometry;
    uint primitive;
    float t;
    float2 uv;
    uint primitive_kind = 0;

    bool matches(const thread Query &query) const {
        return query.get_committed_intersection_type() == intersection_type::bounding_box &&
               instance == query.get_committed_instance_id() &&
               geometry == query.get_committed_geometry_id() &&
               primitive == query.get_committed_primitive_id() &&
               t == query.get_committed_distance();
    }
};

template <typename Alpha> struct PatchPredicate {
    thread Alpha &alpha;
    uint instance;
    uint geometry;
    uint primitive;
    bool valid;

    bool operator()(float2 uv, float t) {
        if (!valid) {
            return false;
        }
        const auto result =
            alpha(instance, geometry, primitive, intersection_type::bounding_box, uv, t);
        valid = result.valid;
        return valid && result.accepted;
    }
};

} // namespace rypath_masked_detail

// Only triangles marked nonopaque are surfaced as manual query candidates.
// Bounding boxes always require a patch solve and supply object-space ray data.
template <typename Alpha>
inline SurfaceHit trace_masked_surface(metal::raytracing::ray ray,
                                       metal::raytracing::instance_acceleration_structure scene,
                                       uint mask, thread Alpha &alpha, bool any_hit = false) {
    using namespace metal::raytracing;
    using namespace rypath_masked_detail;
    intersection_params parameters;
    parameters.accept_any_intersection(any_hit);
    Query query;
    query.reset(ray, scene, mask, parameters);
    AcceptedPatch accepted_patch{0xffffffffu, 0xffffffffu, 0xffffffffu, INFINITY, {0, 0}};
    while (query.next()) {
        const auto kind = query.get_candidate_intersection_type();
        const uint instance = query.get_candidate_instance_id();
        const uint geometry = query.get_candidate_geometry_id();
        const uint primitive = query.get_candidate_primitive_id();
        if (kind == intersection_type::triangle) {
            const auto result = alpha(instance, geometry, primitive, kind,
                                      query.get_candidate_triangle_barycentric_coord(),
                                      query.get_candidate_triangle_distance());
            if (!result.valid) {
                query.abort();
                return invalid_hit();
            }
            if (result.accepted) {
                query.commit_triangle_intersection();
            }
            continue;
        }
        if (kind != intersection_type::bounding_box) {
            query.abort();
            return invalid_hit();
        }
        const auto record = static_cast<const device rypath::geometry_wire::ProceduralRecord *>(
            query.get_candidate_primitive_data());
        if (!record) {
            query.abort();
            return invalid_hit();
        }
        const float t_max = query.get_committed_intersection_type() == intersection_type::none
                                ? ray.max_distance
                                : query.get_committed_distance();
        PatchPredicate<Alpha> accept{alpha, instance, geometry, primitive, true};
        AnalyticIntersection hit{false, INFINITY, {0, 0}};
        if (record->kind == rypath::geometry_wire::bilinear_patch) {
            const device auto &patch = record->patch;
            const auto found = intersect_bilinear_if(
                patch_corner(patch, 0), patch_corner(patch, 1), patch_corner(patch, 2),
                patch_corner(patch, 3), query.get_candidate_ray_origin(),
                query.get_candidate_ray_direction(), ray.min_distance, t_max, accept);
            hit = {found.hit, found.t, found.uv};
        } else if (record->kind == rypath::geometry_wire::analytic) {
            hit = intersect_analytic_if(record->analytic, query.get_candidate_ray_origin(),
                                        query.get_candidate_ray_direction(), ray.min_distance,
                                        t_max, accept);
        } else {
            query.abort();
            return invalid_hit();
        }
        if (!accept.valid) {
            query.abort();
            return invalid_hit();
        }
        if (hit.hit) {
            query.commit_bounding_box_intersection(hit.t);
            const AcceptedPatch candidate{instance, geometry, primitive,
                                          hit.t,    hit.uv,   record->kind};
            // Retain the UV from the exact committed candidate, including ties
            // with other primitives or multiple roots rounded to the same t.
            if (candidate.matches(query)) {
                accepted_patch = candidate;
            }
        }
    }

    switch (query.get_committed_intersection_type()) {
    case intersection_type::triangle:
        return {intersection_type::triangle,
                query.get_committed_instance_id(),
                query.get_committed_geometry_id(),
                query.get_committed_primitive_id(),
                query.get_committed_distance(),
                query.get_committed_triangle_barycentric_coord(),
                true,
                rypath::geometry_wire::triangle};
    case intersection_type::bounding_box:
        return {intersection_type::bounding_box,
                accepted_patch.instance,
                accepted_patch.geometry,
                accepted_patch.primitive,
                accepted_patch.t,
                accepted_patch.uv,
                accepted_patch.matches(query),
                accepted_patch.primitive_kind};
    case intersection_type::none:
        return {intersection_type::none, 0, 0, 0, 0, {0, 0}, true};
    default:
        return invalid_hit();
    }
}
