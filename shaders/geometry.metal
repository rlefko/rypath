// SPDX-License-Identifier: GPL-3.0-only

#include <metal_raytracing>
#include <metal_stdlib>

#include "../src/geometry_wire.h"

using namespace metal;
using namespace metal::raytracing;
using rypath::geometry_wire::Closest;
using rypath::geometry_wire::Ray;
using rypath::geometry_wire::Word;

static ray native_ray(const device Ray &input) {
    return ray(float3(input.origin[0], input.origin[1], input.origin[2]),
               float3(input.direction[0], input.direction[1], input.direction[2]), input.t_min,
               input.t_max);
}

kernel void rypath_closest_hits(const device Ray *rays [[buffer(0)]],
                                device Closest *output [[buffer(1)]],
                                constant uint &ray_count [[buffer(2)]],
                                instance_acceleration_structure scene [[buffer(3)]],
                                uint index [[thread_position_in_grid]]) {
    if (index >= ray_count) {
        return;
    }

    intersector<triangle_data, instancing> trace;
    trace.assume_geometry_type(geometry_type::triangle);
    trace.set_triangle_cull_mode(triangle_cull_mode::none);
    const auto hit =
        trace.intersect(native_ray(rays[index]), scene, rypath::geometry_wire::all_visible);
    if (hit.type == intersection_type::triangle) {
        output[index] = Closest{1,
                                hit.instance_id,
                                hit.geometry_id,
                                hit.primitive_id,
                                hit.distance,
                                hit.triangle_barycentric_coord.x,
                                hit.triangle_barycentric_coord.y};
    } else {
        output[index] = Closest{};
    }
}

kernel void rypath_occlusion(const device Ray *rays [[buffer(0)]],
                             device Word *output [[buffer(1)]],
                             constant uint &ray_count [[buffer(2)]],
                             instance_acceleration_structure scene [[buffer(3)]],
                             uint index [[thread_position_in_grid]]) {
    if (index >= ray_count) {
        return;
    }

    intersector<instancing> trace;
    trace.assume_geometry_type(geometry_type::triangle);
    trace.set_triangle_cull_mode(triangle_cull_mode::none);
    trace.accept_any_intersection(true);
    const auto hit =
        trace.intersect(native_ray(rays[index]), scene, rypath::geometry_wire::all_visible);
    output[index] = hit.type == intersection_type::triangle ? 1u : 0u;
}
