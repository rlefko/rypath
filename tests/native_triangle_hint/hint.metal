// SPDX-License-Identifier: GPL-3.0-only
// Supplementary native hint and complete closest-seed regression probe.
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
kernel void hint_closest(const device wire::Ray *rays [[buffer(0)]],
                         device wire::Closest *output [[buffer(1)]],
                         constant uint &count [[buffer(2)]],
                         instance_acceleration_structure scene [[buffer(3)]],
                         constant uint &mask [[buffer(5)]], uint lane [[thread_position_in_grid]]) {
    if (lane >= count)
        return;
    rypath_intersection_detail::AcceptAll alpha;
    output[lane] = probe_hit(trace_validated_surface(probe_ray(rays[lane]), scene, mask, alpha));
}
kernel void hint_shadow(const device wire::Ray *rays [[buffer(0)]],
                        device uint *output [[buffer(1)]], constant uint &count [[buffer(2)]],
                        instance_acceleration_structure scene [[buffer(3)]],
                        instance_acceleration_structure hints [[buffer(4)]],
                        constant uint &mask [[buffer(5)]], uint lane [[thread_position_in_grid]]) {
    if (lane >= count)
        return;
    const auto ray = probe_ray(rays[lane]);
    NativeTriangleHint h{};
    h.valid = true;
    if ((native_triangle_hint_mode & 1u) != 0 && native_hint_ray_supported(ray))
        h = native_triangle_hint(ray, hints, mask, true);
    if (!h.valid) {
        output[lane] = wire::invalid_hit;
        return;
    }
    if (h.available) {
        output[lane] = 1;
        return;
    }
    rypath_intersection_detail::AcceptAll alpha;
    auto hit = trace_validated_surface(ray, scene, mask, alpha, true);
    output[lane] = !hit.valid ? wire::invalid_hit : hit.type == intersection_type::none ? 0u : 1u;
}
// This entry has no hint argument or FC19 use in either library.
#include "masked_probe.metal"

#include "hint_probe_wire.h"
// Separate helper observation, never claimed to be the wrapper's own query or
// candidate order. Disabled routes and unsupported rays must not query an AS.
static hint_probe::Observation observe_hint(ray r, instance_acceleration_structure hints,
                                            uint mask) {
    hint_probe::Observation out{};
    out.instance = out.geometry = out.primitive = 0xffffffffu;
    out.domain_supported = native_hint_ray_supported(r);
    out.enabled = (native_triangle_hint_mode & 1u) != 0;
    if (!out.domain_supported || !out.enabled)
        return out;
    out.queried = 1;
    const auto hint = native_triangle_hint(r, hints, mask, true);
    out.valid = hint.valid;
    out.available = hint.available;
    if (hint.available) {
        out.kind = hint.surface.primitive_kind;
        out.instance = hint.surface.instance_id;
        out.geometry = hint.surface.geometry_id;
        out.primitive = hint.surface.primitive_id;
        out.validated_t_bits = as_type<uint>(hint.surface.distance);
        out.validated_u_bits = as_type<uint>(hint.surface.coordinates.x);
        out.validated_v_bits = as_type<uint>(hint.surface.coordinates.y);
    }
    return out;
}
kernel void hint_observe_shadow(const device wire::Ray *rays [[buffer(0)]],
                                device hint_probe::Observation *output [[buffer(1)]],
                                constant uint &count [[buffer(2)]],
                                instance_acceleration_structure hints [[buffer(4)]],
                                constant uint &mask [[buffer(5)]],
                                uint lane [[thread_position_in_grid]]) {
    if (lane >= count)
        return;
    output[lane] = observe_hint(probe_ray(rays[lane]), hints, mask);
}

#include "composition.metal"

#include "seed.metal"
