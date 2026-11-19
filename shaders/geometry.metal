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
                          const device wire::ProceduralRecord *record [[primitive_data]],
                          uint instance [[instance_id]], uint primitive [[primitive_id]],
                          ray_data PatchPayload &payload [[payload]]) {
    AnalyticIntersection hit{false, INFINITY, {0, 0}};
    if (record->kind == wire::bilinear_patch) {
        const auto patch = intersect_patch(record->patch, origin, direction, t_min, t_max);
        hit = {patch.hit, patch.t, patch.uv};
    } else if (record->kind == wire::analytic) {
        hit = intersect_analytic(record->analytic, origin, direction, t_min, t_max);
    }
    if (hit.hit && (hit.t < payload.t ||
                    (hit.t == payload.t &&
                     (instance < payload.instance ||
                      (instance == payload.instance && primitive < payload.primitive))))) {
        payload = {hit.t, instance, primitive, hit.uv, record->kind};
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
    if (!hit.valid) {
        output[index] = Closest{wire::invalid_hit, 0, 0, 0, 0, 0, 0};
    } else if (hit.type == intersection_type::triangle) {
        output[index] =
            Closest{wire::triangle, hit.instance_id,   hit.geometry_id,  hit.primitive_id,
                    hit.distance,   hit.coordinates.x, hit.coordinates.y};
    } else if (hit.type == intersection_type::bounding_box) {
        output[index] = Closest{hit.valid ? hit.primitive_kind : wire::invalid_hit,
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
    if (!hit.valid) {
        output[index] = wire::invalid_hit;
    } else if (hit.type == intersection_type::triangle ||
               hit.type == intersection_type::bounding_box) {
        output[index] = 1u;
    } else {
        output[index] = hit.type == intersection_type::none ? 0u : wire::invalid_hit;
    }
}

// Direct arithmetic probes separate helper correctness from native traversal's
// documented candidate and endpoint inclusion limitations.
kernel void rypath_triangle_product_guard_probe(const device uint4 *inputs [[buffer(0)]],
                                                device uint4 *output [[buffer(1)]],
                                                constant uint &count [[buffer(2)]],
                                                uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    const auto words = inputs[index];
    const float a = as_type<float>(words.x), al = as_type<float>(words.y);
    const float b = as_type<float>(words.z), bl = as_type<float>(words.w);
    const float primary = a * b;
    const auto range =
        rypath::triangle_numeric::product_residual_range(words.x, words.z, as_type<uint>(primary));
    const float error = fma(a, b, -primary);
    bool valid = true, cross0 = true, cross1 = true;
    const auto pair = pc_mul(BilinearNumber(a, al), BilinearNumber(b, bl), valid);
    (void)pc_mul(a, bl, cross0);
    (void)pc_mul(al, b, cross1);
    output[index * 2] = {uint(range), as_type<uint>(primary), as_type<uint>(error), uint(valid)};
    output[index * 2 + 1] = {as_type<uint>(pair.high), as_type<uint>(pair.low), uint(cross0),
                             uint(cross1)};
}
kernel void rypath_triangle_witness_probe(const device wire::Triangle &triangle [[buffer(0)]],
                                          const device Ray &input [[buffer(1)]],
                                          device uint *output [[buffer(2)]]) {
    const float4x3 identity(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(0, 0, 0));
    const auto hit = precise_triangle_candidate(
        triangle, identity, float3(input.origin[0], input.origin[1], input.origin[2]),
        float3(input.direction[0], input.direction[1], input.direction[2]), input.t_min,
        input.t_max);
    output[0] = hit.valid;
    output[1] = hit.hit;
    output[2] = as_type<uint>(hit.t.high);
    output[3] = as_type<uint>(hit.t.low);
    output[4] = as_type<uint>(hit.uv.x);
    output[5] = as_type<uint>(hit.uv.y);
    output[6] = 0xc001cafe;
}
