// SPDX-License-Identifier: GPL-3.0-only
#include "seed_wire.h"
namespace sp = closest_seed_probe;
static gw::Closest seed_packed_hit(SurfaceHit h) {
    if (!h.valid)
        return {gw::invalid_hit, 0, 0, 0, 0, 0, 0};
    if (h.type == intersection_type::none)
        return {};
    return {h.primitive_kind, h.instance_id,   h.geometry_id,  h.primitive_id,
            h.distance,       h.coordinates.x, h.coordinates.y};
}
kernel void closest_seed_cases(const device sp::Case *cases [[buffer(0)]],
                               device sp::Result *output [[buffer(1)]],
                               constant uint &count [[buffer(2)]],
                               instance_acceleration_structure scene [[buffer(3)]],
                               instance_acceleration_structure hints [[buffer(4)]],
                               uint lane [[thread_position_in_grid]]) {
    if (lane >= count)
        return;
    const device auto &c = cases[lane];
    const device auto &b = c.base;
    ray r(float3(b.ray.origin[0], b.ray.origin[1], b.ray.origin[2]),
          float3(b.ray.direction[0], b.ray.direction[1], b.ray.direction[2]), b.ray.t_min,
          b.ray.t_max);
    CompositionAlpha alpha{&b, b.instances, b.materials};
    NativeTriangleHint hint{};
    hint.valid = true;
    SurfaceHit observed_seed{};
    SurfaceHit hit{};
    hit.valid = true;
    if (c.test == 0) {
        if (native_triangle_hint_mode & 2u)
            hit = trace_packet_seeded_closest(r, scene, b.mask, hints, alpha, true);
        else
            hit = trace_validated_surface(r, scene, b.mask, alpha);
    } else {
        hint = native_validated_opaque_hint(r, hints, b.mask, alpha, false);
        if (!hint.valid)
            hit = rypath_intersection_detail::invalid_hit();
        else if (!hint.available)
            hit = trace_validated_surface(r, scene, b.mask, alpha);
        else {
            TraceSurfaceSeed seed{};
            seed.available = true;
            seed.surface = hint.surface;
            seed.distance = hint.distance;
            seed.has_cap = c.test != 1 && hint.has_cap;
            if (seed.has_cap)
                seed.cap = hint.cap;
            if (c.test == 3)
                seed.surface.instance_id =
                    c.seed_ordinal; // bounded coincident geometry state control only
            if (c.test == 4)
                seed.surface.instance_id = 0xffffffffu;
            observed_seed = seed.surface;
            // Query mask0 deliberately suppresses offers only in internal state controls.
            const uint procedural_mask = (c.test == 1 || c.test == 2) ? 0u : b.mask;
            hit = trace_validated_surface(r, scene, procedural_mask, alpha, c.test == 5, INFINITY,
                                          &seed);
        }
    }
    sp::Result out{};
    out.hit = seed_packed_hit(hit);
    out.seed = hint.available ? seed_packed_hit(observed_seed) : gw::Closest{};
    out.alpha_calls = alpha.alpha_calls;
    out.opaque_calls = alpha.opaque_calls;
    out.seed_available = hint.available;
    out.has_cap = c.test == 1 ? false : hint.has_cap;
    if (out.has_cap)
        out.cap_bits = as_type<uint>(hint.cap);
    out.test = c.test;
    if (hint.available) {
        out.pair[0] = uint(hint.distance.high.mantissa);
        out.pair[1] = as_type<uint>(hint.distance.high.exponent);
        out.pair[2] = hint.distance.high.negative;
        out.pair[3] = hint.distance.high.valid;
        out.pair[4] = uint(hint.distance.low.mantissa);
        out.pair[5] = as_type<uint>(hint.distance.low.exponent);
        out.pair[6] = hint.distance.low.negative;
        out.pair[7] = hint.distance.low.valid;
    }
    output[lane] = out;
}
