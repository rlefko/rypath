// SPDX-License-Identifier: GPL-3.0-only
#include <metal_stdlib>
using namespace metal;
struct ViewerVertex {
    float4 position [[position]];
    float2 uv;
};
vertex ViewerVertex rypath_viewer_vertex(uint index [[vertex_id]]) {
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    const auto p = positions[index];
    return {float4(p, 0, 1), float2((p.x + 1) * .5f, (1 - p.y) * .5f)};
}
fragment float4 rypath_viewer_fragment(ViewerVertex in [[stage_in]],
                                       texture2d<float> image [[texture(0)]],
                                       constant float &exposure_ev [[buffer(0)]]) {
    constexpr sampler filtering(coord::normalized, address::clamp_to_edge, filter::linear);
    const float3 linear =
        max(image.sample(filtering, in.uv).rgb, float3(0)) * exp2(clamp(exposure_ev, -24.f, 24.f));
    const float3 mapped = linear / (1 + linear);
    const float3 encoded = select(1.055f * pow(mapped, float3(1.f / 2.4f)) - .055f, 12.92f * mapped,
                                  mapped <= .0031308f);
    // Target BGRA8Unorm: this is the only sRGB transfer, after tone mapping.
    return float4(encoded, 1);
}
