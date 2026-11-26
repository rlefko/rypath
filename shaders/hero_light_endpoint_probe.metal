// SPDX-License-Identifier: GPL-3.0-only
#include "render.metal"
inline bool endpoint_same(float a, float b) { return as_type<uint>(a) == as_type<uint>(b); }
inline bool endpoint_same(float2 a, float2 b) {
    return all(as_type<uint2>(a) == as_type<uint2>(b));
}
inline bool endpoint_same(float3 a, float3 b) {
    return all(as_type<uint3>(a) == as_type<uint3>(b));
}
inline bool endpoint_same(float4 a, float4 b) {
    return all(as_type<uint4>(a) == as_type<uint4>(b));
}
inline bool endpoint_same_point(const thread SurfacePoint &a, const thread SurfacePoint &b) {
    return endpoint_same(a.position, b.position) && endpoint_same(a.error, b.error) &&
           endpoint_same(a.geometric_normal, b.geometric_normal) &&
           endpoint_same(a.winding_normal, b.winding_normal) &&
           endpoint_same(a.shading_normal, b.shading_normal) &&
           endpoint_same(a.inverse_area_pdf, b.inverse_area_pdf) &&
           endpoint_same(a.world_extent, b.world_extent) && a.patch == b.patch &&
           endpoint_same(a.analytic_position, b.analytic_position) &&
           endpoint_same(a.analytic_normal, b.analytic_normal) &&
           endpoint_same(a.analytic_dpdu, b.analytic_dpdu) &&
           endpoint_same(a.analytic_dpdv, b.analytic_dpdv) &&
           endpoint_same(a.analytic_dndu, b.analytic_dndu) &&
           endpoint_same(a.analytic_dndv, b.analytic_dndv);
}
inline bool endpoint_same_light(const thread rypath_bdpt::HeroLightEndpoint &a,
                                const thread rypath_bdpt::HeroLightEndpoint &b) {
    return endpoint_same_point(a.point, b.point) && endpoint_same(a.beta, b.beta) &&
           endpoint_same(a.uv, b.uv) && endpoint_same(a.pdf_fwd, b.pdf_fwd) && a.type == b.type &&
           a.instance == b.instance && a.primitive == b.primitive && a.kind == b.kind &&
           a.light == b.light && a.path_id == b.path_id && a.connectible == b.connectible &&
           a.delta_light == b.delta_light;
}
kernel void rypath_hero_light_endpoint_probe(
    device uint4 *out [[buffer(0)]], constant wire::Parameters &p [[buffer(1)]],
    device const wire::Point *positions [[buffer(2)]],
    device const wire::Triangle *triangles [[buffer(3)]],
    device const wire::Patch *patches [[buffer(4)]],
    device const wire::Instance *instances [[buffer(5)]], device const uint *data [[buffer(6)]],
    device const wire::Emitter *emitters [[buffer(7)]],
    device const wire::AreaEmission *emissions [[buffer(8)]], device const float *sky [[buffer(9)]],
    device const wire::ImageTexture *textures [[buffer(10)]],
    device const wire::TextureLevel *levels [[buffer(11)]],
    device const uint *texels [[buffer(12)]], device const float *rgb [[buffer(13)]],
    device const float *illuminant [[buffer(14)]],
    constant wire::TextureParameters &texture_p [[buffer(15)]],
    uint tid [[thread_position_in_grid]]) {
    using namespace rypath_bdpt;
    if (tid >= 8)
        return;
    wire::Sample original_sample{}, compact_sample{}, current_sample{};
    original_sample.status = compact_sample.status = current_sample.status = wire::success;
    Resources original{}, compact{}, current_resources{};
    original.alpha.parameters = &p;
    original.alpha.positions = positions;
    original.alpha.triangles = triangles;
    original.alpha.patches = patches;
    original.alpha.instances = instances;
    original.alpha.material_data = data;
    original.alpha.sample = &original_sample;
    original.emitters = emitters;
    original.emissions = emissions;
    original.environment = sky;
    original.images = {textures, levels, texels, nullptr, rgb, illuminant, &texture_p};
    compact = original;
    compact.alpha.sample = &compact_sample;
    current_resources = original;
    current_resources.alpha.sample = &current_sample;
    Wavelengths lambda{};
    for (uint k = 0; k < 4; ++k) {
        lambda.lambda[k] = 400 + 100 * k;
        lambda.pdf[k] = 1;
    }
    Vertex reference{};
    reference.type = surface_vertex;
    reference.point.position = tid == 6 ? float3(0, 0, 2) : float3(.125f, .25f, 3);
    reference.point.geometric_normal = float3(0, 0, -1);
    reference.point.shading_normal = tid == 5 ? float3(0) : float3(0, 0, -1);
    reference.point.error = float3(0x1p-22f);
    reference.frame = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
    reference.bsdf.active_mask = 15;
    for (uint k = 0; k < 4; ++k) {
        reference.bsdf.lane[k].kind = 0;
        reference.bsdf.lane[k].reflectance = .5f;
    }
    HeroCurrentVertex current_reference{};
    current_reference.point = reference.point;
    current_reference.frame = reference.frame;
    current_reference.type = reference.type;
    current_reference.seed = reference.seed;
    current_reference.bsdf.kind = 0;
    current_reference.bsdf.active_mask = 15;
    for (uint k = 0; k < 4; ++k)
        hero_current_store_lane(current_reference.bsdf, k, reference.bsdf.lane[k]);
    Vertex full{}; // Genuine full-size oracle, not expanded compact output.
    HeroLightEndpoint small{}, current_small{};
    float3 full_direction(99, 98, 97), small_direction = full_direction,
                                       current_direction = full_direction;
    float full_pdf = 77, small_pdf = full_pdf, current_pdf = full_pdf;
    const float4 u(.5f, (float(tid) + .5f) / 8.f, .375f, .875f);
    const bool full_valid =
        sample_li(original, reference, lambda, u, full, full_direction, full_pdf);
    const bool small_valid =
        sample_li(compact, reference, lambda, u, small, small_direction, small_pdf);
    const bool current_valid = sample_li(current_resources, current_reference, lambda, u,
                                         current_small, current_direction, current_pdf);
    uint failures = 0;
    if (full_valid != small_valid || original_sample.status != compact_sample.status)
        failures |= 1;
    if (!endpoint_same(full_direction, small_direction) || !endpoint_same(full_pdf, small_pdf))
        failures |= 2;
    if (!endpoint_same_point(full.point, small.point))
        failures |= 4;
    if (!endpoint_same(full.beta, small.beta) || !endpoint_same(full.uv, small.uv) ||
        !endpoint_same(full.pdf_fwd, small.pdf_fwd) || full.type != small.type ||
        full.instance != small.instance || full.primitive != small.primitive ||
        full.kind != small.kind || full.light != small.light || full.path_id != small.path_id ||
        full.connectible != small.connectible || full.delta_light != small.delta_light)
        failures |= 8;
    if (current_valid != small_valid || current_sample.status != compact_sample.status ||
        !endpoint_same(current_direction, small_direction) ||
        !endpoint_same(current_pdf, small_pdf) || !endpoint_same_light(current_small, small))
        failures |= 256;
    if (full_valid && full_pdf > 0 && nonzero(full.beta)) {
        if (!endpoint_same(direct_pdf(original, reference, full),
                           direct_pdf(compact, reference, small)))
            failures |= 16;
        if (!endpoint_same(offset(full, -full_direction), offset(small, -small_direction)))
            failures |= 32;
        Vertex before{};
        before.point.position = reference.point.position + float3(0, 0, 1);
        if (!endpoint_same(hero_represented_pdf(reference, before, full),
                           hero_represented_pdf(reference, before, small)))
            failures |= 64;
        if (!endpoint_same(direct_pdf(original, reference, full),
                           direct_pdf(current_resources, current_reference, current_small)) ||
            !endpoint_same(hero_represented_pdf(reference, before, full),
                           hero_represented_pdf(current_reference, before, current_small)))
            failures |= 512;
        if (p.instance_count == 0 &&
            visible(original, reference, full, original_sample) !=
                visible(current_resources, current_reference, current_small, current_sample))
            failures |= 1024;
        // Empty-scene visibility early return; occluder traversal is covered by
        // the existing renderer suite and the lead's end-to-end comparisons.
        if (p.instance_count == 0 && visible(original, reference, full, original_sample) !=
                                         visible(compact, reference, small, compact_sample))
            failures |= 128;
    }
    out[tid] = {failures, sizeof(HeroLightEndpoint), sizeof(Vertex),
                uint(full_valid) | (uint(full_pdf > 0 && nonzero(full.beta)) << 1)};
    out[8 + tid] = {as_type<uint>(full_pdf), as_type<uint>(full.beta[0]),
                    as_type<uint>(full.beta[1]), as_type<uint>(full.beta[2])};
    out[16 + tid] = {as_type<uint>(full.beta[3]), as_type<uint>(full_direction.x),
                     as_type<uint>(full_direction.y), as_type<uint>(full_direction.z)};
}
