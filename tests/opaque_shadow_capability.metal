// SPDX-License-Identifier: GPL-3.0-only
// Test-only library: a native-capable pre-opaque entry and a mixed library with
// a new main render entry but legacy wavefront shade entries. No external binary.
#include "../src/checkpoint_contract.h"
#include <metal_raytracing>
#include <metal_stdlib>
using namespace metal;
using namespace metal::raytracing;
constant bool native [[function_constant(21)]];
constant bool opaque [[function_constant(22)]];
kernel void rypath_native_intersection_contract(device uint *out [[buffer(0)]]) {
    out[0] = 1u;
    out[1] = is_function_constant_defined(native) && native ? 1u : 0u;
    out[2] = out[3] = 0u;
}
kernel void rypath_render_paths(device uint *out [[buffer(0)]]) {
    out[0] = is_function_constant_defined(opaque) && opaque ? 1u : 0u;
}
kernel void rypath_render_packets(device uint *out [[buffer(0)]]) {
    out[0] = is_function_constant_defined(native) && native ? 1u : 0u;
}
kernel void rypath_wavefront_shade_scalar(device uint *out [[buffer(0)]]) {
    out[0] = is_function_constant_defined(native) && native ? 1u : 0u;
}
kernel void rypath_wavefront_shade_packet(device uint *out [[buffer(0)]]) {
    out[0] = is_function_constant_defined(native) && native ? 1u : 0u;
}

// Lookup-only intersection stub lets the geometry builder reach the missing-v2
// handshake. This deliberately stale fixture must fail before AS construction.
struct LegacyIntersection {
    bool accept [[accept_intersection]];
    float distance [[distance]];
};
[[intersection(bounding_box, triangle_data, instancing, world_space_data)]]
LegacyIntersection rypath_patch_intersection(float t [[min_distance]]) {
    return {false, t};
}

kernel void rypath_checkpoint_contract(device uint *out [[buffer(0)]]) {
    out[0] = rypath::checkpoint_contract::renderer;
    out[1] = rypath::checkpoint_contract::sampling;
    out[2] = rypath::checkpoint_contract::accumulation;
    out[3] = rypath::checkpoint_contract::identity;
}
