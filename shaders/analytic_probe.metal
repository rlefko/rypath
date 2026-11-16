// SPDX-License-Identifier: GPL-3.0-only
#include "../src/masked_probe_wire.h"
#include "masked_intersection.h"
using namespace metal;
using namespace metal::raytracing;
namespace probe = rypath::masked_probe;
namespace wire = rypath::geometry_wire;
struct AnalyticMask {
    const device probe::Case &input;
    uint calls = 0;
    MaskedAlphaResult operator()(uint, uint, uint, intersection_type, float2, float t) {
        ++calls;
        const uint rule = input.patch_rule;
        return {rule == probe::accept || (rule == probe::reject_near && t > .5f),
                rule != probe::invalid};
    }
};
kernel void rypath_analytic_masked_probe(instance_acceleration_structure scene [[buffer(0)]],
                                         const device probe::Case *cases [[buffer(1)]],
                                         device probe::Result *output [[buffer(2)]],
                                         constant uint &count [[buffer(3)]],
                                         uint lane [[thread_position_in_grid]]) {
    if (lane >= count)
        return;
    const device auto &input = cases[lane];
    const ray r(float3(input.ray.origin[0], input.ray.origin[1], input.ray.origin[2]),
                float3(input.ray.direction[0], input.ray.direction[1], input.ray.direction[2]),
                input.ray.t_min, input.ray.t_max);
    AnalyticMask mask{input};
    const auto hit = trace_masked_surface(r, scene, wire::all_visible, mask, input.any_hit != 0);
    const uint kind = !hit.valid                            ? wire::invalid_hit
                      : hit.type == intersection_type::none ? wire::miss
                                                            : hit.primitive_kind;
    output[lane] = {{kind, hit.instance_id, hit.geometry_id, hit.primitive_id, hit.distance,
                     hit.coordinates.x, hit.coordinates.y},
                    0,
                    mask.calls};
}
