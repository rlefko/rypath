// SPDX-License-Identifier: GPL-3.0-only

#include "../src/texture_probe_wire.h"
#include "image_texture.h"
#include "texture_footprint.h"

using namespace metal;
namespace wire = rypath::render_wire;
namespace probe = rypath::texture_probe;

static probe::Vector pack_probe(float3 value, float w = 0) {
    return {value.x, value.y, value.z, w};
}

kernel void rypath_texture_footprint_probe(
    constant uint *budgets [[buffer(0)]], const device wire::ImageTexture *textures [[buffer(1)]],
    const device wire::TextureLevel *levels [[buffer(2)]], const device uint *texels [[buffer(3)]],
    const device float *transfer [[buffer(4)]], const device float *table [[buffer(5)]],
    constant wire::TextureParameters &parameters [[buffer(6)]],
    device probe::Result *results [[buffer(7)]], uint id [[thread_position_in_grid]]) {
    device auto &result = results[id];
    const float scale = rypath_texture_differential_scale(budgets[id]);
    const float3 i(1, 0, 0), j(0, 1, 0), k(0, 0, 1);
    const float3 origin(4096, -16384, 65536);
    float3 dx(0), dy(0);
    bool ok =
        rypath_texture_primary_dp((origin + 2 * k) - origin, k, k, i + k, j + k, scale, dx, dy);
    result.primary[0] = pack_probe(dx, ok);
    result.primary[1] = pack_probe(dy, scale);
    ok = rypath_texture_primary_dp(2 * k, k, k, i, k, 1.0f, dx, dy);
    result.parallel = pack_probe(float3(0), ok);

    // RotateFromTo sends +X to +Z. The inverse maps +X to +Y and +Y to +Z.
    rypath_texture_approximate_dp(2 * i, i, float3x3(1), float3x3(1), float3(0.2f, 0.4f, 0),
                                  float3(0, 0.125f, 0), scale, dx, dy);
    result.rotated[0] = pack_probe(dx);
    result.rotated[1] = pack_probe(dy);
    const float3x3 a(i, 0.5f * i + j, k), b(i, -0.5f * i + j, k);
    rypath_texture_approximate_dp((origin + 2 * k) - origin, i + k, a, b, float3(0.25f, 0, 0),
                                  float3(0, 0.25f, 0), scale, dx, dy);
    result.sheared[0] = pack_probe(dx);
    result.sheared[1] = pack_probe(dy);
    rypath_texture_approximate_dp(2 * k, i, float3x3(1), float3x3(1), float3(0, 0.25f, 0),
                                  float3(0.25f, 0, 0), scale, dx, dy);
    result.grazing[0] = pack_probe(dx);
    result.grazing[1] = pack_probe(dy);

    float3 u, v;
    float2 st, sx, sy;
    const float2 hit(0.5f, 0.25f);
    ok = rypath_texture_patch_frame(hit, i, j, true, float2(0), float2(1, 0), float2(2, 1),
                                    float2(3, 1), st, u, v);
    result.patch[0] = pack_probe(float3(st, 0), ok);
    result.patch[1] = pack_probe(u);
    result.patch[2] = pack_probe(v);
    ok = rypath_texture_uv_derivatives(u, v, float3(2, 3, 0), float3(-1, -4, 0), sx, sy);
    result.patch_derivatives = ok ? probe::Vector{sx.x, sx.y, sy.x, sy.y} : probe::Vector{NAN};

    ok = rypath_texture_patch_frame(hit, i, j, true, float2(0), float2(0, 1), float2(1, 0),
                                    float2(1, 1), st, u, v);
    result.mirrored[0] = pack_probe(u, ok);
    result.mirrored[1] = pack_probe(v);
    ok = rypath_texture_patch_frame(hit, i, j, true, float2(0), float2(0), float2(0), float2(0), st,
                                    u, v);
    result.degenerate[0] = pack_probe(u, ok);
    result.degenerate[1] = pack_probe(v);
    ok = rypath_texture_uv_derivatives(i, j, float3(NAN), float3(2e9f, -2e9f, 0), sx, sy);
    result.sanitized = ok ? probe::Vector{sx.x, sx.y, sy.x, sy.y} : probe::Vector{NAN};

    const float2 base(0.25f, 0.75f);
    const float2 outside(-0.75f, 0.75f);
    const float3 fine = rypath_image::lookup(textures[0], levels, texels, transfer, parameters,
                                             base, float2(0), float2(0));
    const float3 middle = rypath_image::lookup(textures[0], levels, texels, transfer, parameters,
                                               base, float2(0.3f, 0), float2(0));
    result.fine = pack_probe(fine);
    result.middle = pack_probe(middle);
    result.coarse = pack_probe(rypath_image::lookup(
        textures[1], levels, texels, transfer, parameters, outside, float2(0.5f, 0), float2(0)));
    result.wrapped = pack_probe(rypath_image::lookup(textures[0], levels, texels, transfer,
                                                     parameters, outside, float2(0), float2(0)));
    result.black = pack_probe(rypath_image::lookup(textures[1], levels, texels, transfer,
                                                   parameters, outside, float2(0), float2(0)));
    result.spectrum = {
        rypath_image::spectrum(middle, 460, table), rypath_image::spectrum(middle, 550, table),
        rypath_image::spectrum(middle, 610, table), rypath_image::spectrum(fine, 550, table)};
}
