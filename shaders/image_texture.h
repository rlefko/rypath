// SPDX-License-Identifier: Apache-2.0
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Modified for rypath. See third_party/pbrt/README.md.
#pragma once

#include "../src/ewa_weights.h"
#include "../src/render_wire.h"
#include <metal_stdlib>

namespace rypath_image {
using namespace metal;
namespace wire = rypath::render_wire;

static float unpack_half(uint bits) {
    const uint sign = (bits & 0x8000u) << 16u;
    const uint exponent = bits >> 10u & 31u;
    const uint mantissa = bits & 1023u;
    // Construct a binary32 value without half arithmetic, which can flush subnormals.
    if (exponent == 0u) {
        if (mantissa == 0u)
            return as_type<float>(sign);
        const uint leading = 31u - clz(mantissa);
        return as_type<float>(sign | ((103u + leading) << 23u) |
                              ((mantissa << (23u - leading)) & 0x7fffffu));
    }
    return as_type<float>(sign | ((exponent == 31u ? 255u : exponent + 112u) << 23u) |
                          (mantissa << 13u));
}

static float3 texel(const device wire::Word *texels, const device float *transfer,
                    const device wire::TextureLevel &level, int x, int y, uint wrap,
                    uint texel_format, uint transfer_offset, bool alpha = false) {
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
    const uint index = uint(y) * level.width + uint(x);
    if (texel_format == wire::linear_float_texel_format) {
        const uint word = level.offset + 4u * index;
        if (alpha)
            return float3(as_type<float>(texels[word + 3u]), 0, 0);
        return float3(as_type<float>(texels[word]), as_type<float>(texels[word + 1u]),
                      as_type<float>(texels[word + 2u]));
    }
    if (texel_format == wire::linear_half_texel_format) {
        const uint word = level.offset + 2u * index;
        const uint rg = texels[word], b = texels[word + 1u];
        if (alpha)
            return float3(unpack_half(b >> 16), 0, 0);
        return float3(unpack_half(rg & 0xffffu), unpack_half(rg >> 16u), unpack_half(b & 0xffffu));
    }
    const uint value = texels[level.offset + index];
    const uint begin = transfer_offset;
    if (alpha)
        return float3(transfer[begin + (value >> 24)], 0, 0);
    return float3(transfer[begin + (value & 255u)], transfer[begin + ((value >> 8) & 255u)],
                  transfer[begin + ((value >> 16) & 255u)]);
}

static bool valid_level(const device wire::TextureLevel &level, uint texel_word_count,
                        uint texel_format) {
    const uint words_per_texel = texel_format == wire::linear_float_texel_format  ? 4u
                                 : texel_format == wire::linear_half_texel_format ? 2u
                                                                                  : 1u;
    return level.width > 0 && level.height > 0 && level.width <= 4096 && level.height <= 4096 &&
           level.offset <= texel_word_count &&
           level.width * level.height * words_per_texel <= texel_word_count - level.offset;
}

static float3 bilinear_texels(const device wire::ImageTexture &texture,
                              const device wire::TextureLevel &level,
                              const device wire::Word *texels, const device float *transfer,
                              float2 xy, bool scalar = false) {
    const int2 base = int2(floor(xy));
    const float2 d = xy - float2(base);
    const bool alpha = scalar && texture.channels == 4u;
    const float3 a = texel(texels, transfer, level, base.x, base.y, texture.wrap,
                           texture.texel_format, texture.transfer_offset, alpha);
    const float3 b = texel(texels, transfer, level, base.x + 1, base.y, texture.wrap,
                           texture.texel_format, texture.transfer_offset, alpha);
    const float3 c = texel(texels, transfer, level, base.x, base.y + 1, texture.wrap,
                           texture.texel_format, texture.transfer_offset, alpha);
    const float3 e = texel(texels, transfer, level, base.x + 1, base.y + 1, texture.wrap,
                           texture.texel_format, texture.transfer_offset, alpha);
    float3 rgb = (1.0f - d.x) * (1.0f - d.y) * a + d.x * (1.0f - d.y) * b + (1.0f - d.x) * d.y * c +
                 d.x * d.y * e;
    if (scalar && texture.channels == 3u)
        rgb.x = (rgb.x + rgb.y + rgb.z) / 3.0f;
    return rgb;
}

static float3 bilinear(const device wire::ImageTexture &texture,
                       const device wire::TextureLevel &level, const device wire::Word *texels,
                       const device float *transfer, float2 st, bool scalar = false) {
    // Bound integer conversions even for large, finite authored coordinates.
    if (texture.wrap == 0u)
        st -= floor(st);
    else if (texture.wrap == 1u)
        st = clamp(st, float2(0), float2(1));
    else if (any(st < -1.0f) || any(st > 2.0f))
        return float3(0);
    return bilinear_texels(texture, level, texels, transfer,
                           st * float2(level.width, level.height) - 0.5f, scalar);
}

static float2 point_mapping(float2 uv, float2 scale, float2 offset) {
    // Pinned PBRT rounds the product before adding the offset. Contracting this
    // expression can move a coordinate off an exact half-texel tie.
#pragma clang fp contract(off)
    const float2 scaled = uv * scale;
    return scaled + offset;
}

static float point_lod(float width, float coarsest) {
    // The floor is sensitive to rounding at integer LOD transitions.
#pragma clang fp contract(off)
    const float log_width = log(width) * 1.4426950408889634f;
    return coarsest + log_width;
}

static int point_coordinate(float st, float dimension, uint wrap) {
    if (wrap == 1u)
        st = clamp(st, 0.0f, 1.0f);
    else if (wrap == 2u)
        st = clamp(st, -1.0f, 2.0f);
    const float index = round(st * dimension - 0.5f);
    if (wrap == 0u) {
        // Preserve signed ties and bound the cast. A finite coordinate that
        // overflows on scaling spans whole periods of the power-of-two mip.
        return isfinite(index) ? int(fmod(index, dimension)) : 0;
    }
    return int(index);
}

static bool valid_ewa_level(const device wire::TextureLevel &level, uint texel_word_count,
                            uint texel_format, uint levels_above) {
    return valid_level(level, texel_word_count, texel_format) &&
           (level.width & (level.width - 1u)) == 0u && (level.height & (level.height - 1u)) == 0u &&
           max(level.width, level.height) == (1u << levels_above);
}

static float ewa_center(float st, float dimension, uint wrap) {
#pragma clang fp contract(off)
    if (abs(st) <= 64.0f || wrap == 2u)
        return st * dimension - 0.5f;
    const float phase = fmod(st, 1.0f) * dimension;
    if (wrap == 0u)
        return phase - 0.5f;
    // Keep distant clamp taps on the same edge and preserve subtexel phase.
    return (st < 0.0f ? -64.0f : dimension + 64.0f) + phase - 0.5f;
}

static bool ewa_black_disjoint(float2 st, float2 dimensions, float2 radius) {
    // Work in UV space for large coordinates where conversion to texel space
    // could overflow. The ordinary-coordinate test uses the integer tap bounds.
    return any((abs(st) > 64.0f) & ((st < (0.5f - radius) / dimensions) |
                                    (st > (dimensions - 0.5f + radius) / dimensions)));
}

static float cross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }

static uint magnitude_bits(float value) { return as_type<uint>(value) & 0x7fffffffu; }

static bool subnormal_bits(uint magnitude) { return magnitude != 0u && magnitude < 0x00800000u; }

static float ewa_product(float value, float scale) {
    const float mapped = value * scale;
    const uint a = magnitude_bits(value), b = magnitude_bits(scale);
    if (a == 0u || b == 0u ||
        (magnitude_bits(mapped) != 0u && !subnormal_bits(a) && !subnormal_bits(b)))
        return mapped;

    // Metal may flush binary32 subnormals even with fast math disabled. Only
    // denormal operands and products flushed to zero need exact reconstruction.
    const int a_exp = int(a >> 23), b_exp = int(b >> 23);
    const uint a_sig = (a & 0x007fffffu) | (a_exp == 0 ? 0u : 0x00800000u);
    const uint b_sig = (b & 0x007fffffu) | (b_exp == 0 ? 0u : 0x00800000u);
    const ulong product = ulong(a_sig) * b_sig;
    const int exponent = max(1, a_exp) + max(1, b_exp) - 300;
    const int leading = 63 - int(clz(product));
    const uint sign = (as_type<uint>(value) ^ as_type<uint>(scale)) & 0x80000000u;
    if (exponent + leading > 127)
        return as_type<float>(sign | 0x7f800000u);
    const int shift = max(leading - 23, -149 - exponent);
    if (shift > 48)
        return as_type<float>(sign);
    uint rounded;
    if (shift <= 0) {
        rounded = uint(product << -shift);
    } else {
        const ulong halfway = 1ul << (shift - 1);
        const ulong truncated = product >> shift;
        // Round to nearest with ties to even before encoding. The significand
        // carry advances the packed exponent at normal and denormal seams.
        rounded = uint((product + halfway - 1ul + (truncated & 1ul)) >> shift);
    }
    return as_type<float>(sign | ((uint(exponent + shift + 149) << 23) + rounded));
}

static float lift_derivative(float value) {
    const uint magnitude = magnitude_bits(value);
    if (subnormal_bits(magnitude))
        return copysign(float(magnitude) * 0x1p-125f, value);
    return value * 0x1p24f;
}

static float3 ewa_level(const device wire::ImageTexture &texture,
                        const device wire::TextureLevel &level, const device wire::Word *texels,
                        const device float *transfer, float2 st, float2 dx, float2 dy) {
    const float2 dimensions(level.width, level.height);
    const float2 u = dx * dimensions, v = dy * dimensions;
    const float area = cross2(u, v);
    // The footprint covariance is I + uuᵀ + vvᵀ. The determinant and adjugate
    // quadratic below avoid cancellation for nearly collinear derivatives.
    const float determinant = 1.0f + dot(u, u) + dot(v, v) + area * area;
    const float2 radius = sqrt(1.0f + u * u + v * v);
    if (texture.wrap == 2u && ewa_black_disjoint(st, dimensions, radius))
        return float3(0);
    const float2 center(ewa_center(st.x, dimensions.x, texture.wrap),
                        ewa_center(st.y, dimensions.y, texture.wrap));
    const float2 begin = ceil(center - radius), end = floor(center + radius);
    // Bound conversions and loop work before converting floating tap bounds.
    // The supported cap and mip selection require at most 2152 candidates.
    const float2 count = end - begin + 1.0f;
    if (!all(isfinite(begin)) || !all(isfinite(end)) || any(abs(begin) > 0x1p19f) ||
        any(abs(end) > 0x1p19f) || any(count <= 0.0f) || count.x * count.y > 4096.0f)
        return float3(NAN);
    if (texture.wrap == 2u && (any(end < 0.0f) || any(begin >= dimensions)))
        return float3(0);
    const int2 first = int2(begin), last = int2(end);
    float3 sum(0);
    float weight_sum = 0;
#pragma clang loop unroll(disable)
    for (int y = first.y; y <= last.y; ++y) {
#pragma clang loop unroll(disable)
        for (int x = first.x; x <= last.x; ++x) {
            const float2 d = float2(x, y) - center;
            const float a = cross2(u, d), b = cross2(v, d);
            const float r2 = (dot(d, d) + a * a + b * b) / determinant;
            if (r2 < 1.0f) {
                const float weight = rypath_ewa::weights[min(uint(128.0f * r2), 127u)];
                sum += weight * texel(texels, transfer, level, x, y, texture.wrap,
                                      texture.texel_format, texture.transfer_offset);
                weight_sum += weight;
            }
        }
    }
    return isfinite(weight_sum) && weight_sum > 0.0f ? sum / weight_sum : float3(NAN);
}

static float3 ewa(const device wire::ImageTexture &texture, const device wire::TextureLevel *levels,
                  const device wire::Word *texels, const device float *transfer,
                  uint texel_word_count, float2 uv, float2 dx, float2 dy, bool scalar,
                  bool already_mapped) {
#pragma clang fp contract(off)
    const float cap = texture.max_anisotropy;
    const float2 axis_scale(texture.uscale, texture.vscale), offset(texture.udelta, texture.vdelta);
    if (texture.level_count > 13u || !(cap >= 1.0f && cap <= 16.0f) || !all(isfinite(axis_scale)) ||
        !all(isfinite(offset)))
        return float3(NAN);
    if (!already_mapped) {
        dx = float2(ewa_product(dx.x, axis_scale.x), ewa_product(dx.y, axis_scale.y));
        dy = float2(ewa_product(dy.x, axis_scale.x), ewa_product(dy.y, axis_scale.y));
    }
    if (!all(isfinite(dx)) || !all(isfinite(dy)))
        return float3(NAN);
    const uint top = texture.level_count - 1u;
    const device wire::TextureLevel *mips = levels + texture.level_begin;
    const uint x_bits = max(magnitude_bits(dx.x), magnitude_bits(dx.y));
    const uint y_bits = max(magnitude_bits(dy.x), magnitude_bits(dy.y));
    const bool zero = x_bits == 0u || y_bits == 0u;
    float minor = 0.0f;
    if (!zero) {
        // A major axis of at least 32 forces the capped minor to at least 2.
        // Check this before taking lengths, whose intermediates could overflow.
        if (max(x_bits, y_bits) >= as_type<uint>(32.0f))
            minor = 2.0f;
        else {
            const bool lifted =
                subnormal_bits(magnitude_bits(dx.x)) || subnormal_bits(magnitude_bits(dx.y)) ||
                subnormal_bits(magnitude_bits(dy.x)) || subnormal_bits(magnitude_bits(dy.y));
            const float2 a = lifted ? float2(lift_derivative(dx.x), lift_derivative(dx.y)) : dx;
            const float2 b = lifted ? float2(lift_derivative(dy.x), lift_derivative(dy.y)) : dy;
            const float x_scale = max(abs(a.x), abs(a.y)), y_scale = max(abs(b.x), abs(b.y));
            const float2 x = a / x_scale, y = b / y_scale;
            const float x_norm = length(x), y_norm = length(y);
            const float x_length = x_scale * x_norm, y_length = y_scale * y_norm;
            const bool x_major = x_length >= y_length;
            const float major = x_major ? x_length : y_length;
            minor = x_major ? y_length : x_length;
            const float capped = major / cap;
            if (minor < capped) {
                const float actual = lifted ? capped * 0x1p-24f : capped;
                if (x_major)
                    dy = (y / y_norm) * actual;
                else
                    dx = (x / x_norm) * actual;
                minor = capped;
            }
            // If rescaling flushes a subnormal length, the base mip is still
            // selected. Only its direction and exact nonzero status were needed.
            if (lifted)
                minor *= 0x1p-24f;
        }
        if (minor >= 2.0f) {
            if (!valid_ewa_level(mips[top], texel_word_count, texture.texel_format, 0))
                return float3(NAN);
            return texel(texels, transfer, mips[top], 0, 0, texture.wrap, texture.texel_format,
                         texture.transfer_offset);
        }
    }
    float2 st = uv;
    if (!already_mapped) {
        const float2 scaled(ewa_product(uv.x, axis_scale.x), ewa_product(uv.y, axis_scale.y));
        st = scaled + offset;
    }
    // PBRT flips the mapped V center while keeping the signed V derivatives.
    st.y = 1.0f - st.y;
    if (!all(isfinite(st)))
        return float3(NAN);
    if (zero) {
        if (!valid_ewa_level(mips[0], texel_word_count, texture.texel_format, top))
            return float3(NAN);
        const float2 dimensions(mips[0].width, mips[0].height);
        if (texture.wrap == 2u && ewa_black_disjoint(st, dimensions, float2(1.0f)))
            return float3(0);
        const float2 center(ewa_center(st.x, dimensions.x, texture.wrap),
                            ewa_center(st.y, dimensions.y, texture.wrap));
        return bilinear_texels(texture, mips[0], texels, transfer, center, scalar);
    }
    const float lod = max(0.0f, float(top) + log2(minor));
    // For minor below 2, rounding near the upper boundary can put lod at
    // top+1. The exact level below that boundary is the actual top mip.
    const uint lower = min(uint(floor(lod)), top);
    if (!valid_ewa_level(mips[lower], texel_word_count, texture.texel_format, top - lower))
        return float3(NAN);
    if (lower < top && (!valid_ewa_level(mips[lower + 1u], texel_word_count, texture.texel_format,
                                         top - lower - 1u) ||
                        mips[lower + 1u].width != max(1u, mips[lower].width / 2u) ||
                        mips[lower + 1u].height != max(1u, mips[lower].height / 2u)))
        return float3(NAN);
    const float3 a = ewa_level(texture, mips[lower], texels, transfer, st, dx, dy);
    const float3 b = lower == top
                         ? texel(texels, transfer, mips[top], 0, 0, texture.wrap,
                                 texture.texel_format, texture.transfer_offset)
                         : ewa_level(texture, mips[lower + 1u], texels, transfer, st, dx, dy);
    const float delta = lod - float(lower);
    return (1.0f - delta) * a + delta * b;
}

static bool valid_lookup(const device wire::ImageTexture &texture,
                         constant wire::TextureParameters &p, float2 uv, float2 dx, float2 dy) {
    return all(isfinite(uv)) && all(isfinite(dx)) && all(isfinite(dy)) && texture.wrap <= 2u &&
           (texture.channels == 1u || texture.channels == 3u || texture.channels == 4u) &&
           texture.texel_format <= wire::linear_float_texel_format &&
           (texture.texel_format >= wire::linear_half_texel_format ||
            (texture.transfer_offset <= p.transfer_count &&
             p.transfer_count - texture.transfer_offset >= 256u)) &&
           texture.level_count != 0 && texture.level_begin <= p.level_count &&
           texture.level_count <= p.level_count - texture.level_begin;
}

static float3 lookup_standard(const device wire::ImageTexture &texture,
                              const device wire::TextureLevel *levels,
                              const device wire::Word *texels, const device float *transfer,
                              constant wire::TextureParameters &p, float2 uv, float2 dx, float2 dy,
                              bool scalar = false, bool already_mapped = false) {
    const bool point = texture.filter == wire::point_texture_filter;
    if (!valid_lookup(texture, p, uv, dx, dy) ||
        (!point && texture.filter != wire::bilinear_texture_filter &&
         texture.filter != wire::trilinear_texture_filter))
        return float3(NAN);
    if (!already_mapped) {
        dx *= float2(texture.uscale, texture.vscale);
        dy *= float2(texture.uscale, texture.vscale);
    }
    // A wide footprint selects the coordinate-independent 1x1 mip. Test before
    // doubling derivatives or mapping coordinates, which may overflow even for
    // finite authored inputs.
    const float extent = max(max(abs(dx.x), abs(dx.y)), max(abs(dy.x), abs(dy.y)));
    const float coarsest = float(texture.level_count - 1u);
    float lod = coarsest;
    if (!(extent >= 0.5f)) {
        const float width = max(2.0f * extent, 1e-8f);
        lod = point ? point_lod(width, coarsest) : coarsest + log(width) * 1.4426950408889634f;
    }
    const bool wide = lod >= coarsest;
    const uint index = wide ? texture.level_count - 1u : uint(max(0.0f, floor(lod)));
    const device wire::TextureLevel &level = levels[texture.level_begin + index];
    if (!valid_level(level, p.texel_word_count, texture.texel_format))
        return float3(NAN);
    if (wide) {
        return texel(texels, transfer, level, 0, 0, texture.wrap, texture.texel_format,
                     texture.transfer_offset);
    }
    // Pinned PBRT keeps the base level unblended even for 0 <= lod < 1.
    const bool interpolate = texture.filter == wire::trilinear_texture_filter && index > 0;
    if (interpolate && !valid_level(levels[texture.level_begin + index + 1u], p.texel_word_count,
                                    texture.texel_format))
        return float3(NAN);
    const float2 scale(texture.uscale, texture.vscale), offset(texture.udelta, texture.vdelta);
    float2 st = uv;
    if (!already_mapped)
        st = point ? point_mapping(uv, scale, offset) : uv * scale + offset;
    st.y = 1.0f - st.y;
    if (!all(isfinite(st)))
        return float3(NAN);
    if (point) {
        const int x = point_coordinate(st.x, float(level.width), texture.wrap);
        const int y = point_coordinate(st.y, float(level.height), texture.wrap);
        return texel(texels, transfer, level, x, y, texture.wrap, texture.texel_format,
                     texture.transfer_offset);
    }
    const float3 lower = bilinear(texture, level, texels, transfer, st, scalar);
    if (!interpolate)
        return lower;
    // The upper 1x1 mip still uses wrap-aware bilinear sampling below the coarsest LOD.
    const float3 upper =
        bilinear(texture, levels[texture.level_begin + index + 1u], texels, transfer, st, scalar);
    const float delta = lod - float(index);
    return (1.0f - delta) * lower + delta * upper;
}

static float3 lookup_filtered(const device wire::ImageTexture &texture,
                              const device wire::TextureLevel *levels,
                              const device wire::Word *texels, const device float *transfer,
                              constant wire::TextureParameters &p, float2 uv, float2 dx, float2 dy,
                              bool scalar, bool already_mapped) {
    if (texture.filter != wire::ewa_texture_filter)
        return lookup_standard(texture, levels, texels, transfer, p, uv, dx, dy, scalar,
                               already_mapped);
    if (!valid_lookup(texture, p, uv, dx, dy))
        return float3(NAN);
    return ewa(texture, levels, texels, transfer, p.texel_word_count, uv, dx, dy, scalar,
               already_mapped);
}

static inline float3 lookup(const device wire::ImageTexture &texture,
                            const device wire::TextureLevel *levels,
                            const device wire::Word *texels, const device float *transfer,
                            constant wire::TextureParameters &p, float2 uv, float2 dx, float2 dy,
                            bool already_mapped = false) {
    return lookup_filtered(texture, levels, texels, transfer, p, uv, dx, dy, false, already_mapped);
}

static inline float lookup_scalar(const device wire::ImageTexture &texture,
                                  const device wire::TextureLevel *levels,
                                  const device wire::Word *texels, const device float *transfer,
                                  constant wire::TextureParameters &p, float2 uv, float2 dx,
                                  float2 dy, bool already_mapped = false) {
    return lookup_filtered(texture, levels, texels, transfer, p, uv, dx, dy, true, already_mapped)
        .x;
}

// Native-image callers provide coordinates in source image row order.
static inline float3 lookup_base(const device wire::ImageTexture &texture,
                                 const device wire::TextureLevel *levels,
                                 const device wire::Word *texels, const device float *transfer,
                                 constant wire::TextureParameters &p, float2 st) {
    if (!valid_lookup(texture, p, st, float2(0), float2(0)) || texture.level_count != 1u)
        return float3(NAN);
    const device wire::TextureLevel &level = levels[texture.level_begin];
    if (!valid_level(level, p.texel_word_count, texture.texel_format))
        return float3(NAN);
    return bilinear(texture, level, texels, transfer, st);
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
