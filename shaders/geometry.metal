// SPDX-License-Identifier: GPL-3.0-only

#include <metal_raytracing>
#include <metal_stdlib>

#include "intersection.h"

using namespace metal;
using namespace metal::raytracing;
using rypath::geometry_wire::Closest;
using rypath::geometry_wire::Patch;
using rypath::geometry_wire::Ray;
using rypath::geometry_wire::Word;
namespace wire = rypath::geometry_wire;

struct PatchAcceptance {
    bool accept [[accept_intersection]];
    float distance [[distance]];
};

[[intersection(bounding_box, triangle_data, instancing, world_space_data)]]
PatchAcceptance
rypath_patch_intersection(float3 origin [[origin]], float3 direction [[direction]],
                          float t_min [[min_distance]], float t_max [[max_distance]],
                          const device Patch *patch [[primitive_data]],
                          uint instance [[instance_id]], uint primitive [[primitive_id]],
                          ray_data PatchPayload &payload [[payload]]) {
    const auto hit = intersect_patch(*patch, origin, direction, t_min, t_max);
    if (hit.hit && (hit.t < payload.t ||
                    (hit.t == payload.t &&
                     (instance < payload.instance ||
                      (instance == payload.instance && primitive < payload.primitive))))) {
        payload = {hit.t, instance, primitive, hit.uv};
    }
    return {hit.hit, hit.t};
}

static ray native_ray(const device Ray &input) {
    return ray(float3(input.origin[0], input.origin[1], input.origin[2]),
               float3(input.direction[0], input.direction[1], input.direction[2]), input.t_min,
               input.t_max);
}

kernel void rypath_closest_hits(const device Ray *rays [[buffer(0)]],
                                device Closest *output [[buffer(1)]],
                                constant uint &ray_count [[buffer(2)]],
                                instance_acceleration_structure scene [[buffer(3)]],
                                MixedIntersectionTable intersections [[buffer(4)]],
                                uint index [[thread_position_in_grid]]) {
    if (index >= ray_count) {
        return;
    }

    MixedIntersector trace;
    trace.set_triangle_cull_mode(triangle_cull_mode::none);
    const auto ray = native_ray(rays[index]);
    const auto hit = trace_surface(trace, ray, scene, wire::all_visible, intersections);
    if (hit.type == intersection_type::triangle) {
        output[index] =
            Closest{wire::triangle, hit.instance_id,   hit.geometry_id,  hit.primitive_id,
                    hit.distance,   hit.coordinates.x, hit.coordinates.y};
    } else if (hit.type == intersection_type::bounding_box) {
        output[index] = Closest{hit.valid ? wire::bilinear_patch : wire::invalid_hit,
                                hit.instance_id,
                                hit.geometry_id,
                                hit.primitive_id,
                                hit.distance,
                                hit.coordinates.x,
                                hit.coordinates.y};
    } else if (hit.type == intersection_type::none) {
        output[index] = Closest{};
    } else {
        output[index] = Closest{wire::invalid_hit, 0, 0, 0, 0, 0, 0};
    }
}

kernel void rypath_occlusion(const device Ray *rays [[buffer(0)]],
                             device Word *output [[buffer(1)]],
                             constant uint &ray_count [[buffer(2)]],
                             instance_acceleration_structure scene [[buffer(3)]],
                             MixedIntersectionTable intersections [[buffer(4)]],
                             uint index [[thread_position_in_grid]]) {
    if (index >= ray_count) {
        return;
    }

    MixedIntersector trace;
    trace.set_triangle_cull_mode(triangle_cull_mode::none);
    trace.accept_any_intersection(true);
    const auto hit =
        trace_shadow(trace, native_ray(rays[index]), scene, wire::all_visible, intersections);
    if (hit.type == intersection_type::triangle || hit.type == intersection_type::bounding_box) {
        output[index] = 1u;
    } else {
        output[index] = hit.type == intersection_type::none ? 0u : wire::invalid_hit;
    }
}
