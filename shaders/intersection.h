// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <metal_raytracing>
#include <metal_stdlib>

#include "../src/geometry_wire.h"
#include "bilinear.h"

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
};

inline PatchPayload empty_patch_payload() {
    return {INFINITY, 0xffffffffu, 0xffffffffu, {0.0f, 0.0f}};
}

struct SurfaceHit {
    metal::raytracing::intersection_type type;
    uint instance_id;
    uint geometry_id;
    uint primitive_id;
    float distance;
    metal::float2 coordinates;
    bool valid;
};

inline SurfaceHit trace_surface(thread MixedIntersector &trace, metal::raytracing::ray ray,
                                metal::raytracing::instance_acceleration_structure scene, uint mask,
                                MixedIntersectionTable table) {
    PatchPayload payload = empty_patch_payload();
    const auto hit = trace.intersect(ray, scene, mask, table, payload);
    if (hit.type == metal::raytracing::intersection_type::bounding_box) {
        // Returned float distances are exactly the values supplied by our
        // callback. All payload fields name that accepted endpoint, including
        // when Metal selected another patch at exactly the same distance.
        return {hit.type,
                payload.instance,
                0,
                payload.primitive,
                hit.distance,
                payload.uv,
                payload.t == hit.distance && payload.instance != 0xffffffffu};
    }
    if (hit.type == metal::raytracing::intersection_type::triangle) {
        return {hit.type,
                hit.instance_id,
                hit.geometry_id,
                hit.primitive_id,
                hit.distance,
                hit.triangle_barycentric_coord,
                true};
    }
    return {hit.type, 0, 0, 0, 0, {0, 0}, hit.type == metal::raytracing::intersection_type::none};
}

inline MixedHit trace_shadow(thread MixedIntersector &trace, metal::raytracing::ray ray,
                             metal::raytracing::instance_acceleration_structure scene, uint mask,
                             MixedIntersectionTable table) {
    PatchPayload payload = empty_patch_payload();
    return trace.intersect(ray, scene, mask, table, payload);
}
