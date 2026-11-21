// SPDX-License-Identifier: GPL-3.0-only
// Unseeded procedural reference; compiled separately from the hint probe.
#include "intersection.h"
using namespace metal;
using namespace metal::raytracing;
namespace wire = rypath::geometry_wire;
static ray probe_ray(const device wire::Ray &r) {
    return ray(float3(r.origin[0], r.origin[1], r.origin[2]),
               float3(r.direction[0], r.direction[1], r.direction[2]), r.t_min, r.t_max);
}
static wire::Closest probe_hit(SurfaceHit h) {
    if (!h.valid)
        return {wire::invalid_hit, 0, 0, 0, 0, 0, 0};
    if (h.type == intersection_type::none)
        return {};
    return {h.primitive_kind, h.instance_id,   h.geometry_id,  h.primitive_id,
            h.distance,       h.coordinates.x, h.coordinates.y};
}
kernel void control_closest(const device wire::Ray *rays [[buffer(0)]],
                            device wire::Closest *output [[buffer(1)]],
                            constant uint &count [[buffer(2)]],
                            instance_acceleration_structure scene [[buffer(3)]],
                            constant uint &mask [[buffer(5)]],
                            uint lane [[thread_position_in_grid]]) {
    if (lane >= count)
        return;
    rypath_intersection_detail::AcceptAll alpha;
    output[lane] = probe_hit(trace_validated_surface(probe_ray(rays[lane]), scene, mask, alpha));
}
kernel void control_shadow(const device wire::Ray *rays [[buffer(0)]],
                           device uint *output [[buffer(1)]], constant uint &count [[buffer(2)]],
                           instance_acceleration_structure scene [[buffer(3)]],
                           constant uint &mask [[buffer(5)]],
                           uint lane [[thread_position_in_grid]]) {
    if (lane >= count)
        return;
    rypath_intersection_detail::AcceptAll alpha;
    auto hit = trace_validated_surface(probe_ray(rays[lane]), scene, mask, alpha, true);
    output[lane] = !hit.valid ? wire::invalid_hit : hit.type == intersection_type::none ? 0u : 1u;
}
// This entry has no hint argument or FC19 use in either library.
#include "masked_probe.metal"
