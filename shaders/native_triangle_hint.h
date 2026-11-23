// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Packet specialization: 0 disables supplementary traversal; 3 enables
// validated opaque shadow hints and complete closest seeds.
constant uint native_triangle_hint_mode [[function_constant(19)]];

struct NativeTriangleHint {
    bool valid;
    bool available;
    SurfaceHit surface;
    triangle_extended::Pair distance;
    bool has_cap;
    float cap;
};
inline bool native_hint_ray_supported(metal::raytracing::ray ray) {
    if (!pc_normal(ray.min_distance) || ray.min_distance < 0 || isnan(ray.max_distance) ||
        ray.max_distance < ray.min_distance ||
        (isfinite(ray.max_distance) && !pc_normal(ray.max_distance)))
        return false;
    for (uint axis = 0; axis < 3; ++axis)
        if (!pc_normal(ray.origin[axis]) || !pc_normal(ray.direction[axis]))
            return false;
    return any(ray.direction != 0);
}
inline NativeTriangleHint
native_triangle_hint(metal::raytracing::ray ray,
                     metal::raytracing::instance_acceleration_structure hints, uint mask,
                     bool any_hit) {
    using namespace metal::raytracing;
    NativeTriangleHint result{};
    result.valid = true;
    intersection_params p;
    p.force_opacity(forced_opacity::opaque);
    p.accept_any_intersection(any_hit);
    p.set_triangle_cull_mode(triangle_cull_mode::none);
    rypath_intersection_detail::Query query(ray, hints, mask, p);
    // Every non-triangle placement is mask zero. Opaque triangles auto-commit.
    if (query.next()) {
        query.abort();
        return result;
    }
    const auto kind = query.get_committed_intersection_type();
    if (kind == intersection_type::none)
        return result;
    if (kind != intersection_type::triangle)
        return result;
    if (query.get_committed_geometry_id() != 0) {
        result.valid = false;
        return result;
    }
    auto record = static_cast<const device rypath::geometry_wire::ProceduralRecord *>(
        query.get_committed_primitive_data());
    if (!record) {
        result.valid = false;
        return result;
    }
    if (record->kind == rypath::geometry_wire::miss)
        return result;
    if (record->kind != rypath::geometry_wire::triangle) {
        result.valid = false;
        return result;
    }
    const auto transform = query.get_committed_object_to_world_transform();
    auto hit = precise_triangle_candidate(record->triangle, transform, ray.origin, ray.direction,
                                          ray.min_distance, ray.max_distance);
    // A failed optional solve is not a final miss: the original traversal decides.
    if (!hit.valid || !hit.hit)
        return result;
    if (!triangle_extended::valid(hit.distance)) {
        result.valid = false;
        return result;
    }
    result.available = true;
    result.surface = {intersection_type::triangle,
                      query.get_committed_instance_id(),
                      query.get_committed_geometry_id(),
                      query.get_committed_primitive_id(),
                      hit.t.high,
                      hit.uv,
                      true,
                      rypath::geometry_wire::triangle};
    result.distance = hit.distance;
    // The device record and transform getters remain inside this Query lifetime.
    // Conversion defaults do not indicate that a cap was computed.
    result.has_cap = false;
    if (!any_hit) {
        const auto converted =
            triangle_pruning_cap(record->triangle, transform, ray.origin, ray.direction,
                                 hit.distance, ray.min_distance, ray.max_distance);
        if (converted.finite) {
            const float cap = as_type<float>(converted.bits);
            const auto exterior = triangle_extended::compare(
                triangle_extended::pair_from_bits(converted.bits), hit.distance);
            if (pc_normal(cap) && cap > ray.min_distance && cap < ray.max_distance &&
                exterior.valid && exterior.order > 0) {
                result.has_cap = true;
                result.cap = cap;
            }
        }
    }
    return result;
}
// Shared by the actual packet visibility route and the bounded composition probe.
// This is not a generic alpha callback: only the built-in definite-accept case
// is permitted. Public/stateful masked APIs remain on their original paths.
template <typename Alpha>
inline NativeTriangleHint
native_validated_opaque_hint(metal::raytracing::ray ray,
                             metal::raytracing::instance_acceleration_structure hints, uint mask,
                             const thread Alpha &alpha, bool any_hit) {
    NativeTriangleHint result{};
    result.valid = true;
    if ((native_triangle_hint_mode & (any_hit ? 1u : 2u)) == 0 || !native_hint_ray_supported(ray))
        return result;
    result = native_triangle_hint(ray, hints, mask, any_hit);
    if (!result.valid || !result.available)
        return result;
    const auto hit = result.surface;
    if (hit.instance_id >= alpha.parameters->instance_count || hit.geometry_id != 0 ||
        !isfinite(hit.distance)) {
        result.valid = false;
        return result;
    }
    const device auto &instance = alpha.instances[hit.instance_id];
    if (hit.primitive_id >= instance.triangle_count ||
        instance.material >= alpha.parameters->material_count ||
        instance.alpha_root != 0xffffffffu || instance.invisible_emitter != 0) {
        result.valid = false;
        return result;
    }
    const uint scattering = alpha.materials[instance.material].scattering;
    if (scattering == 9u || scattering == 10u || scattering == 11u ||
        instance.inside_medium != instance.outside_medium) {
        result.valid = false;
        return result;
    }
    return result;
}
template <typename Alpha>
inline NativeTriangleHint
native_opaque_shadow_hint(metal::raytracing::ray ray,
                          metal::raytracing::instance_acceleration_structure hints, uint mask,
                          const thread Alpha &alpha) {
    return native_validated_opaque_hint(ray, hints, mask, alpha, true);
}
// Built-in packet closest only. Caller bypasses this entire seed path in modes0/1.
// Generic/stateful Alpha APIs retain their original unseeded overloads.
template <typename Alpha>
inline SurfaceHit trace_packet_seeded_closest(
    metal::raytracing::ray ray, metal::raytracing::instance_acceleration_structure scene, uint mask,
    metal::raytracing::instance_acceleration_structure hints, thread Alpha &alpha, bool masked) {
    auto hint = native_validated_opaque_hint(ray, hints, mask, alpha, false);
    if (!hint.valid)
        return rypath_intersection_detail::invalid_hit();
    TraceSurfaceSeed seed{};
    if (hint.available) {
        seed.available = true;
        seed.surface = hint.surface;
        seed.distance = hint.distance;
        seed.has_cap = hint.has_cap;
        if (hint.has_cap)
            seed.cap = hint.cap;
    }
    if (masked)
        return trace_validated_surface(ray, scene, mask, alpha, false, INFINITY, &seed);
    rypath_intersection_detail::AcceptAll accept;
    return trace_validated_surface(ray, scene, mask, accept, false, INFINITY, &seed);
}
// Packet buffer 14 is a supplementary AS instead of the scalar IFT. These
// overloads preserve the procedural path for disabled and generic callers.
inline SurfaceHit trace_surface(thread MixedIntersector &trace, metal::raytracing::ray ray,
                                metal::raytracing::instance_acceleration_structure scene, uint mask,
                                metal::raytracing::instance_acceleration_structure hints) {
    (void)trace;
    (void)hints;
    rypath_intersection_detail::AcceptAll alpha;
    return trace_validated_surface(ray, scene, mask, alpha);
}
inline SurfaceHit trace_shadow(thread MixedIntersector &trace, metal::raytracing::ray ray,
                               metal::raytracing::instance_acceleration_structure scene, uint mask,
                               metal::raytracing::instance_acceleration_structure hints) {
    (void)trace;
    (void)hints;
    rypath_intersection_detail::AcceptAll alpha;
    return trace_validated_surface(ray, scene, mask, alpha, true);
}
