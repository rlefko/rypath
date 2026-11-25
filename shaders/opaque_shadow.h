// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "intersection.h"

// Undefined in generic/probe kernels: preserve their existing query path.
constant bool rypath_opaque_shadows [[function_constant(22)]];
inline bool native_opaque_shadows_enabled() {
    return is_function_constant_defined(rypath_opaque_shadows) ? rypath_opaque_shadows : false;
}
struct OpaqueShadowResult {
    bool occluded, valid;
};
// Boolean-only specialization. Host eligibility guarantees every AS primitive is
// an opaque native triangle, with no miss-tag slots or visibility callbacks.
// The original ray interval and visibility mask remain authoritative.
inline OpaqueShadowResult
native_opaque_shadow(metal::raytracing::ray incoming,
                     metal::raytracing::instance_acceleration_structure scene, uint mask) {
    using namespace metal;
    using namespace metal::raytracing;
    if (!pc_normal(incoming.min_distance) || incoming.min_distance < 0 ||
        isnan(incoming.max_distance) || incoming.max_distance < incoming.min_distance ||
        (isfinite(incoming.max_distance) && !pc_normal(incoming.max_distance)))
        return {false, false};
    for (uint axis = 0; axis < 3; ++axis)
        if (!pc_normal(incoming.origin[axis]) || !pc_normal(incoming.direction[axis]))
            return {false, false};
    if (all(incoming.direction == 0))
        return {false, false};
    intersector<triangle_data, instancing> trace;
    trace.force_opacity(forced_opacity::opaque);
    trace.set_triangle_cull_mode(triangle_cull_mode::none);
    trace.accept_any_intersection(true);
    const auto hit = trace.intersect(incoming, scene, mask);
    if (hit.type == intersection_type::none)
        return {false, true};
    if (hit.type != intersection_type::triangle || !isfinite(hit.distance) ||
        hit.distance < incoming.min_distance || hit.distance > incoming.max_distance ||
        !all(isfinite(hit.triangle_barycentric_coord)))
        return {false, false};
    return {true, true};
}
