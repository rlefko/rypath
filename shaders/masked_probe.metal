// SPDX-License-Identifier: GPL-3.0-only

#include "../src/masked_probe_wire.h"
#include "alpha_hash.h"
#include "masked_intersection.h"

using namespace metal;
using namespace metal::raytracing;
namespace wire = rypath::geometry_wire;
namespace probe = rypath::masked_probe;

struct ProbeMask {
    const device probe::Case &input;
    uint triangle_calls = 0;
    uint patch_calls = 0;
    uint triangle_primitive_mask = 0;

    MaskedAlphaResult operator()(uint instance, uint, uint primitive, intersection_type kind,
                                 float2, float t) {
        const bool triangle = kind == intersection_type::triangle;
        const uint rule = triangle ? input.triangle_rule : input.patch_rule;
        if (triangle) {
            ++triangle_calls;
            uint matched = 0;
            for (uint i = 0; i < 2; ++i)
                if (input.expected_triangle_primitives[i] != probe::absent_primitive &&
                    primitive == input.expected_triangle_primitives[i])
                    matched |= 1u << i;
            triangle_primitive_mask |= matched ? matched : probe::unexpected_triangle_primitive;
        } else {
            ++patch_calls;
        }
        if (instance == input.ignore_instance) {
            return {false, true};
        }
        return {rule == probe::accept || (rule == probe::reject_near && t > 0.5f),
                rule != probe::invalid};
    }
};

kernel void rypath_masked_intersection_probe(instance_acceleration_structure scene [[buffer(0)]],
                                             const device probe::Case *cases [[buffer(1)]],
                                             device probe::Result *result [[buffer(2)]],
                                             constant uint &count [[buffer(3)]],
                                             uint lane [[thread_position_in_grid]]) {
    if (lane >= count) {
        return;
    }
    const device auto &input = cases[lane];
    const auto ray = metal::raytracing::ray(
        float3(input.ray.origin[0], input.ray.origin[1], input.ray.origin[2]),
        float3(input.ray.direction[0], input.ray.direction[1], input.ray.direction[2]),
        input.ray.t_min, input.ray.t_max);
    ProbeMask alpha{input};
    const auto hit =
        trace_masked_surface(ray, scene, wire::all_visible, alpha, input.any_hit != 0u);
    uint kind = wire::invalid_hit;
    if (hit.valid) {
        kind = hit.type == intersection_type::triangle       ? wire::triangle
               : hit.type == intersection_type::bounding_box ? wire::bilinear_patch
                                                             : wire::miss;
    }
    result[lane] = {{kind, hit.instance_id, hit.geometry_id, hit.primitive_id, hit.distance,
                     hit.coordinates.x, hit.coordinates.y},
                    alpha.triangle_calls,
                    alpha.patch_calls,
                    alpha.triangle_primitive_mask};
}

kernel void rypath_alpha_hash_probe(const device probe::HashCase *cases [[buffer(0)]],
                                    device uint *result [[buffer(1)]],
                                    constant uint &count [[buffer(2)]],
                                    uint lane [[thread_position_in_grid]]) {
    if (lane >= count) {
        return;
    }
    const device auto &bits = cases[lane].words;
    const float3 origin(as_type<float>(bits[0]), as_type<float>(bits[1]), as_type<float>(bits[2]));
    const float3 direction(as_type<float>(bits[3]), as_type<float>(bits[4]),
                           as_type<float>(bits[5]));
    result[lane] = as_type<uint>(alpha_uniform(origin, direction, as_type<float>(bits[6])));
}
