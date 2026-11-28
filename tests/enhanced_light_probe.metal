// SPDX-License-Identifier: GPL-3.0-only
#include "../shaders/render.metal"

// Pure record/geometry/raw-product/proposal-density helpers: no Resources
// aggregate, AS, sampler, visibility or BSDF call. Independent host expectations.
kernel void rypath_enhanced_light_math(
    device const uint *queries [[buffer(0)]],
    device const rypath::enhanced_wire::Record *records [[buffer(1)]],
    device uint *out [[buffer(2)]], constant uint &count [[buffer(3)]],
    constant wire::Parameters *parameters [[buffer(4)]], device const uint *data [[buffer(5)]],
    constant wire::TextureGraphParameters *graphs [[buffer(6)]],
    device const float *cdf [[buffer(7)]], uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    namespace el = rypath_bdpt::enhanced_light;
    const auto q = queries + 32u * index;
    auto result = out + 16u * index;
    for (uint k = 0; k < 16; ++k)
        result[k] = 0;
    if (q[0] == 0) {
        const auto record = records[index];
        result[0] = uint(el::valid_record(record, q[1]));
    } else if (q[0] == 1) {
        rypath_bdpt::HeroLightEndpoint from{}, to{};
        from.point.position =
            float3(as_type<float>(q[2]), as_type<float>(q[3]), as_type<float>(q[4]));
        to.point.position =
            float3(as_type<float>(q[5]), as_type<float>(q[6]), as_type<float>(q[7]));
        to.point.geometric_normal =
            float3(as_type<float>(q[8]), as_type<float>(q[9]), as_type<float>(q[10]));
        el::rm::Scaled factor{.5f, 1};
        bool support = false;
        result[0] = uint(el::geometry(from, to, q[1], factor, support));
        result[1] = uint(support);
        result[2] = as_type<uint>(factor.fraction);
        result[3] = as_type<uint>(factor.exponent);
    } else if (q[0] == 2) {
        float4 beta, f, radiance;
        for (uint k = 0; k < 4; ++k) {
            beta[k] = as_type<float>(q[2 + k]);
            f[k] = as_type<float>(q[6 + k]);
            radiance[k] = as_type<float>(q[10 + k]);
        }
        float4 value;
        result[0] = uint(el::physical(beta, f, radiance, as_type<float>(q[14]),
                                      {as_type<float>(q[15]), as_type<int>(q[16])}, value));
        for (uint k = 0; k < 4; ++k)
            result[2 + k] = as_type<uint>(value[k]);
    } else if (q[0] == 3) {
        result[0] = 0x454c5031u;
        result[1] = sizeof(rypath::enhanced_wire::Record);
        result[2] = alignof(rypath::enhanced_wire::Record);
        result[3] = 32;
        result[4] = 16;
    } else if (q[0] == 4 || q[0] == 5) {
        el::rm::Scaled density{.5f, 1};
        bool support = false;
        const float3 direction(as_type<float>(q[2]), as_type<float>(q[3]), as_type<float>(q[4]));
        if (q[0] == 4) {
            const float3 normal(as_type<float>(q[5]), as_type<float>(q[6]), as_type<float>(q[7]));
            result[0] = uint(el::selection_density(parameters[index], data + q[17],
                                                   {direction, normal}, q[1], density, support));
        } else {
            result[0] = uint(el::environment_density(graphs[index], cdf + q[18], direction,
                                                     q[1] != 0, density, support));
        }
        result[1] = uint(support);
        result[2] = as_type<uint>(density.fraction);
        result[3] = as_type<uint>(density.exponent);
    } else {
        result[0] = 0xffffffffu;
    }
}
