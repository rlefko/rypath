// SPDX-License-Identifier: Apache-2.0
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Modified for rypath. See third_party/pbrt/README.md.
#pragma once

#include "../src/render_wire.h"
#include <metal_stdlib>

namespace rypath_image {
using namespace metal;
namespace wire = rypath::render_wire;

static float3 texel(const device wire::Word *texels, const device float *transfer,
                    const device wire::TextureLevel &level, int x, int y, uint wrap,
                    uint encoding) {
    const int width = int(level.width), height = int(level.height);
    if (wrap == 0u) {
        x = (x % width + width) % width;
        y = (y % height + height) % height;
    } else if (wrap == 1u) {
        x = clamp(x, 0, width - 1);
        y = clamp(y, 0, height - 1);
    } else if (x < 0 || x >= width || y < 0 || y >= height) {
        return float3(0);
    }
    const uint value = texels[level.offset + uint(y) * level.width + uint(x)];
    const uint begin = encoding * 256u;
    return float3(transfer[begin + (value & 255u)], transfer[begin + ((value >> 8) & 255u)],
                  transfer[begin + ((value >> 16) & 255u)]);
}

static float3 lookup(const device wire::ImageTexture &texture,
                     const device wire::TextureLevel *levels, const device wire::Word *texels,
                     const device float *transfer, constant wire::TextureParameters &p, float2 uv,
                     float2 dx, float2 dy) {
    if (!all(isfinite(uv)) || !all(isfinite(dx)) || !all(isfinite(dy)) ||
        texture.level_count == 0 || texture.level_begin > p.level_count ||
        texture.level_count > p.level_count - texture.level_begin)
        return float3(NAN);
    dx *= float2(texture.uscale, texture.vscale);
    dy *= float2(texture.uscale, texture.vscale);
    // A wide footprint selects the coordinate-independent 1x1 mip. Test before
    // doubling derivatives or mapping coordinates, which may overflow even for
    // finite authored inputs.
    const float extent = max(max(abs(dx.x), abs(dx.y)), max(abs(dy.x), abs(dy.y)));
    const float lod = extent >= 0.5f ? float(texture.level_count - 1u)
                                     : float(texture.level_count - 1u) +
                                           log(max(2.0f * extent, 1e-8f)) * 1.4426950408889634f;
    const uint index = lod >= float(texture.level_count - 1u) ? texture.level_count - 1u
                                                              : uint(max(0.0f, floor(lod)));
    const device wire::TextureLevel &level = levels[texture.level_begin + index];
    if (level.width == 0 || level.height == 0 || level.offset > p.texel_count ||
        ulong(level.width) * level.height > p.texel_count - level.offset)
        return float3(NAN);
    if (lod >= float(texture.level_count - 1u)) {
        return texel(texels, transfer, level, 0, 0, texture.wrap, texture.encoding);
    }
    float2 st =
        uv * float2(texture.uscale, texture.vscale) + float2(texture.udelta, texture.vdelta);
    st.y = 1.0f - st.y;
    if (!all(isfinite(st)))
        return float3(NAN);
    // Bound integer conversions even for large, finite authored coordinates.
    if (texture.wrap == 0u)
        st -= floor(st);
    else if (texture.wrap == 1u)
        st = clamp(st, float2(0), float2(1));
    else if (any(st < -1.0f) || any(st > 2.0f))
        return float3(0);
    const float2 xy = st * float2(level.width, level.height) - 0.5f;
    const int2 base = int2(floor(xy));
    const float2 d = xy - float2(base);
    const float3 a = texel(texels, transfer, level, base.x, base.y, texture.wrap, texture.encoding);
    const float3 b =
        texel(texels, transfer, level, base.x + 1, base.y, texture.wrap, texture.encoding);
    const float3 c =
        texel(texels, transfer, level, base.x, base.y + 1, texture.wrap, texture.encoding);
    const float3 e =
        texel(texels, transfer, level, base.x + 1, base.y + 1, texture.wrap, texture.encoding);
    return (1.0f - d.x) * (1.0f - d.y) * a + d.x * (1.0f - d.y) * b + (1.0f - d.x) * d.y * c +
           d.x * d.y * e;
}

static float blend(float t, float a, float b) { return (1.0f - t) * a + t * b; }

static float spectrum(float3 rgb, float wavelength, const device float *table) {
    if (!all(isfinite(rgb)))
        return NAN;
    rgb = clamp(rgb, float3(0), float3(1));
    if (rgb.x == rgb.y && rgb.y == rgb.z)
        return rgb.x;
    uint channel = 0;
    if (rgb.y >= rgb.x)
        channel = 1;
    if (rgb.z >= rgb[channel])
        channel = 2;
    const float z = rgb[channel];
    const float x = rgb[(channel + 1u) % 3u] * 63.0f / z;
    const float y = rgb[(channel + 2u) % 3u] * 63.0f / z;
    const uint xi = min(uint(x), 62u), yi = min(uint(y), 62u);
    uint lower = 1, upper = 63;
    while (lower < upper) {
        const uint mid = (lower + upper) / 2u;
        if (table[mid] < z)
            lower = mid + 1u;
        else
            upper = mid;
    }
    const uint zi = lower - 1u;
    const float dx = x - float(xi), dy = y - float(yi);
    const float dz = (z - table[zi]) / (table[zi + 1u] - table[zi]);
    float3 coefficients;
    for (uint i = 0; i < 3; ++i) {
        const uint cell = 64u + 3u * (((channel * 64u + zi) * 64u + yi) * 64u + xi) + i;
        const uint row = 3u * 64u, plane = row * 64u;
        coefficients[i] =
            blend(dz,
                  blend(dy, blend(dx, table[cell], table[cell + 3u]),
                        blend(dx, table[cell + row], table[cell + row + 3u])),
                  blend(dy, blend(dx, table[cell + plane], table[cell + plane + 3u]),
                        blend(dx, table[cell + plane + row], table[cell + plane + row + 3u])));
    }
    const float polynomial =
        fma(fma(coefficients.x, wavelength, coefficients.y), wavelength, coefficients.z);
    if (!isfinite(polynomial))
        return NAN;
    if (abs(polynomial) <= 1.0f)
        return 0.5f + 0.5f * polynomial / sqrt(1.0f + polynomial * polynomial);
    const float inverse = 1.0f / polynomial;
    return 0.5f + 0.5f * copysign(1.0f / sqrt(1.0f + inverse * inverse), polynomial);
}
} // namespace rypath_image
