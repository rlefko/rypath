// SPDX-License-Identifier: Apache-2.0
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Equal-area mapping and light image lookup adapted for rypath. See third_party/pbrt/README.md.
#pragma once

#include "image_texture.h"

namespace rypath_image_light {
using namespace metal;
namespace wire = rypath::render_wire;

struct Images {
    const device wire::ImageTexture *textures;
    const device wire::TextureLevel *levels;
    const device wire::Word *texels;
    const device float *transfer;
    const device float *spectra;
    const device float *illuminant; // Dense D65 samples at integer wavelengths 360 to 830 nm.
    const constant wire::TextureParameters *parameters;
};

constant constexpr float pi = 3.14159265358979323846f;
constant constexpr float sphere_density = 0.25f / pi;
constant constexpr float uniform_probability = 0.0625f;
constant constexpr uint max_importance_width = 8192;
constant constexpr float less_than_one = 0x1.fffffep-1f;

static inline float illuminant_spectrum(Images images, float3 rgb, float wavelength) {
    if (!all(isfinite(rgb)) || !isfinite(wavelength))
        return NAN;
    rgb = max(float3(0), rgb);
    const float amplitude = 2.0f * max(max(rgb.x, rgb.y), rgb.z);
    if (amplitude == 0)
        return 0;
    const float nearest = round(wavelength);
    if (nearest < 360.0f || nearest > 830.0f)
        return 0;
    return amplitude * rypath_image::spectrum(rgb / amplitude, wavelength, images.spectra) *
           images.illuminant[uint(nearest) - 360u];
}

static inline float area_radiance(Images images, uint index, float scale, float2 uv,
                                  float wavelength, bool checked_power = false) {
    if (index >= images.parameters->texture_count)
        return NAN;
    const device wire::ImageTexture &texture = images.textures[index];
    const float3 rgb =
        rypath_image::lookup_base(texture, images.levels, images.texels, images.transfer,
                                  *images.parameters, float2(uv.x, 1.0f - uv.y));
    if (!checked_power)
        return scale * illuminant_spectrum(images, rgb, wavelength);
    if (scale == 0)
        return 0;
    const float nearest = round(wavelength);
    if (!isfinite(wavelength) || nearest < 360 || nearest > 830)
        return NAN;
    // Preserve zero spectra and explicit zero scale, but fail the entire dispatch
    // when a nonzero intermediate is lost before a later multiplier can restore it.
    if (!all(isfinite(rgb)) || any(rgb < 0) || any((rgb != 0) & !isnormal(rgb)))
        return NAN;
    const float amplitude = 2 * max(max(rgb.x, rgb.y), rgb.z);
    if (amplitude == 0)
        return 0;
    if (!isnormal(amplitude))
        return NAN;
    const float3 normalized_rgb = rgb / amplitude;
    if (any((normalized_rgb != 0) & !isnormal(normalized_rgb)) ||
        any((rgb > 0) & (normalized_rgb == 0)))
        return NAN;
    const float reconstructed = rypath_image::spectrum(normalized_rgb, wavelength, images.spectra);
    const float untinted = amplitude * reconstructed;
    const float d65 = images.illuminant[uint(nearest) - 360u];
    const float spectral = untinted * d65;
    const float result = scale * spectral;
    if (!isfinite(reconstructed) || reconstructed < 0 ||
        !(reconstructed == 0 || isnormal(reconstructed)) ||
        !(untinted == 0 || isnormal(untinted)) || (reconstructed > 0 && untinted == 0) ||
        !(spectral == 0 || isnormal(spectral)) || (untinted > 0 && d65 > 0 && spectral == 0) ||
        !(result == 0 || isnormal(result)) || (scale > 0 && spectral > 0 && result == 0))
        return NAN;
    return result;
}

static inline float3 square_to_sphere(float2 uv) {
    const float2 signed_uv = 2.0f * uv - 1.0f;
    const float2 quadrant = abs(signed_uv);
    const float diagonal = 1.0f - (quadrant.x + quadrant.y);
    const float radius = 1.0f - abs(diagonal);
    const float phi =
        (radius == 0 ? 1.0f : (quadrant.y - quadrant.x) / radius + 1.0f) * (pi / 4.0f);
    const float height = copysign(1.0f - radius * radius, diagonal);
    const float radial = radius * sqrt(max(0.0f, 2.0f - radius * radius));
    return float3(copysign(cos(phi), signed_uv.x) * radial,
                  copysign(sin(phi), signed_uv.y) * radial, height);
}

static inline float2 sphere_to_square(float3 direction, bool pinned_radiance) {
    const float3 magnitude = abs(direction);
    float radius;
    float phi;
    if (pinned_radiance) {
        radius = sqrt(max(0.0f, 1.0f - magnitude.z));
        const float larger = max(magnitude.x, magnitude.y);
        const float ratio = larger == 0 ? 0.0f : min(magnitude.x, magnitude.y) / larger;
        phi = fma(ratio, -0.02513909723434835f, 0.041903881802916574f);
        phi = fma(ratio, phi, 0.08817706647753163f);
        phi = fma(ratio, phi, -0.24733373328126894f);
        phi = fma(ratio, phi, 0.006157201789828021f);
        phi = fma(ratio, phi, 0.6362265452740161f);
        phi = fma(ratio, phi, 0.000004067585662467885f);
        if (magnitude.x < magnitude.y)
            phi = 1.0f - phi;
    } else {
        // Use the analytic inverse for the sampling density. This form of
        // sqrt(1-abs(z)) avoids subtracting nearly equal values at the poles.
        const float horizontal = length(direction.xy);
        radius = horizontal / sqrt(1.0f + magnitude.z);
        phi = horizontal == 0 ? 0.0f : atan2(magnitude.y, magnitude.x) * (2.0f / pi);
    }
    float2 uv(radius - phi * radius, phi * radius);
    if (direction.z < 0)
        uv = 1.0f - uv.yx;
    uv = copysign(uv, direction.xy);
    return clamp(0.5f * (uv + 1.0f), float2(0), float2(1));
}

static inline float3 transform_direction(const constant float *matrix, float3 direction) {
    return float3(dot(float3(matrix[0], matrix[1], matrix[2]), direction),
                  dot(float3(matrix[3], matrix[4], matrix[5]), direction),
                  dot(float3(matrix[6], matrix[7], matrix[8]), direction));
}

// Goniometric diagrams are native scalar maps. PBRT uses nearest lookup here,
// not the bilinear filter used by area and environment emission.
static inline float goniometric_scale(Images images, uint index, float3 direction) {
    if (index >= images.parameters->texture_count || !all(isfinite(direction)) ||
        abs(dot(direction, direction) - 1.0f) > 0.001f)
        return NAN;
    const float2 uv = sphere_to_square(direction, true);
    const device wire::ImageTexture &texture = images.textures[index];
    if (!rypath_image::valid_lookup(texture, *images.parameters, uv, float2(0), float2(0)) ||
        texture.channels != 1u || texture.wrap != 1u || texture.level_count != 1u)
        return NAN;
    const device wire::TextureLevel &level = images.levels[texture.level_begin];
    if (level.width != level.height ||
        !rypath_image::valid_level(level, images.parameters->texel_word_count,
                                   texture.texel_format))
        return NAN;
    const int2 pixel = int2(uv * float2(level.width, level.height));
    return rypath_image::texel(images.texels, images.transfer, level, pixel.x, pixel.y,
                               texture.wrap, texture.texel_format, texture.transfer_offset)
        .x;
}

// Projection uses source-row UVs and nearest clamped RGB, independently of
// the bilinear area/environment image reconstruction.
static inline float3 projection_rgb(Images images, uint index, float2 uv) {
    if (index >= images.parameters->texture_count)
        return float3(NAN);
    const device wire::ImageTexture &texture = images.textures[index];
    if (!rypath_image::valid_lookup(texture, *images.parameters, uv, float2(0), float2(0)) ||
        texture.channels != 3u || texture.wrap != 1u || texture.level_count != 1u || texture.gltf)
        return float3(NAN);
    const device wire::TextureLevel &level = images.levels[texture.level_begin];
    if (!rypath_image::valid_level(level, images.parameters->texel_word_count,
                                   texture.texel_format))
        return float3(NAN);
    const int2 pixel = int2(uv * float2(level.width, level.height));
    return rypath_image::texel(images.texels, images.transfer, level, pixel.x, pixel.y,
                               texture.wrap, texture.texel_format, texture.transfer_offset);
}

static inline float environment_radiance(Images images, constant wire::TextureGraphParameters &p,
                                         float3 world_direction, float wavelength) {
    if (p.environment_image >= images.parameters->texture_count)
        return NAN;
    const device wire::ImageTexture &texture = images.textures[p.environment_image];
    const float3 direction =
        normalize(transform_direction(p.environment_from_world, world_direction));
    if (!all(isfinite(direction)) ||
        !rypath_image::valid_lookup(texture, *images.parameters, float2(0), float2(0), float2(0)) ||
        texture.level_count != 1u)
        return NAN;
    const device wire::TextureLevel &level = images.levels[texture.level_begin];
    if (!rypath_image::valid_level(level, images.parameters->texel_word_count,
                                   texture.texel_format) ||
        level.width != level.height)
        return NAN;
    const int size = int(level.width);
    const float2 uv = sphere_to_square(direction, true);
    int2 pixel = int2(uv * float(size));
    // The mapped square includes exact upper edges. Reflect the opposite
    // coordinate across each octahedral seam, in PBRT's x then y order.
    if (pixel.x == size) {
        pixel.x = size - 1;
        pixel.y = size - 1 - pixel.y;
    }
    if (pixel.y < 0) {
        pixel.x = size - 1 - pixel.x;
        pixel.y = -pixel.y;
    } else if (pixel.y == size) {
        pixel.x = size - 1 - pixel.x;
        pixel.y = size - 1;
    }
    if (size == 1)
        pixel = int2(0);
    const float3 rgb = rypath_image::texel(images.texels, images.transfer, level, pixel.x, pixel.y,
                                           1u, texture.texel_format, texture.transfer_offset);
    return p.environment_scale * illuminant_spectrum(images, rgb, wavelength);
}

static inline bool valid_cdf(constant wire::TextureGraphParameters &p, bool compensated = false) {
    const uint width = p.environment_width;
    if (width == 0 || width > max_importance_width)
        return false;
    const uint count = (width + 1u) * (width + 1u);
    const uint offset = compensated ? p.environment_compensated_offset : 0u;
    return (!compensated || offset >= count) && offset <= p.environment_cdf_count &&
           count <= p.environment_cdf_count - offset;
}

static inline bool environment_cell(constant wire::TextureGraphParameters &p,
                                    float3 world_direction, thread uint2 &cell) {
    const float3 direction =
        normalize(transform_direction(p.environment_from_world, world_direction));
    if (!all(isfinite(direction)))
        return false;
    const uint width = p.environment_width;
    const float2 uv = sphere_to_square(direction, false);
    cell = min(uint2(uv * float(width)), uint2(width - 1u));
    return true;
}

static inline float environment_density(constant wire::TextureGraphParameters &p,
                                        const device float *cdf, uint2 cell,
                                        bool compensated = false) {
    cdf += compensated ? p.environment_compensated_offset : 0u;
    const uint width = p.environment_width;
    const device float *row = cdf + (cell.y + 1u) * (width + 1u);
    const float marginal = cdf[cell.y + 1u] - cdf[cell.y];
    const float conditional = row[cell.x + 1u] - row[cell.x];
    if (!(marginal >= 0.0f && marginal <= 1.0f && conditional >= 0.0f && conditional <= 1.0f))
        return NAN;
    const float density = (float(width) * marginal) * (float(width) * conditional);
    return sphere_density *
           (compensated ? density : uniform_probability + (1.0f - uniform_probability) * density);
}

static inline float environment_pdf(constant wire::TextureGraphParameters &p,
                                    const device float *cdf, float3 world_direction,
                                    bool compensated = false) {
    uint2 cell;
    if (!valid_cdf(p, compensated) || !environment_cell(p, world_direction, cell))
        return NAN;
    return environment_density(p, cdf, cell, compensated);
}

static inline float sample_cdf(const device float *cdf, uint width, float u, thread uint &cell) {
    uint lower = 0, upper = width;
    while (upper - lower > 1u) {
        const uint middle = lower + (upper - lower) / 2u;
        if (u < cdf[middle])
            upper = middle;
        else
            lower = middle;
    }
    const float mass = cdf[upper] - cdf[lower];
    if (!(mass > 0.0f && mass <= 1.0f))
        return NAN;
    cell = lower;
    const float fraction = clamp((u - cdf[lower]) / mass, 0.0f, less_than_one);
    return (float(lower) + fraction) / float(width);
}

static inline bool sample_environment(constant wire::TextureGraphParameters &p,
                                      const device float *cdf, float2 u,
                                      thread float3 &world_direction, thread float &pdf,
                                      bool compensated = false) {
    if (!valid_cdf(p, compensated) || !all(isfinite(u)) || any(u < 0.0f) || any(u > 1.0f))
        return false;
    const device float *selected_cdf = cdf + (compensated ? p.environment_compensated_offset : 0u);
    float2 uv;
    uint row = 0, column = 0;
    const bool importance = compensated || u.x >= uniform_probability;
    const uint width = p.environment_width;
    if (!importance) {
        uv = float2(u.x / uniform_probability, u.y);
    } else {
        uv.y = sample_cdf(selected_cdf, width, min(u.y, less_than_one), row);
        uv.x = sample_cdf(
            selected_cdf + (row + 1u) * (width + 1u), width,
            min(compensated ? u.x : (u.x - uniform_probability) / (1.0f - uniform_probability),
                less_than_one),
            column);
    }
    if (!all(isfinite(uv)))
        return false;
    world_direction =
        normalize(transform_direction(p.world_from_environment, square_to_sphere(uv)));
    uint2 cell;
    if (!environment_cell(p, world_direction, cell))
        return false;
    const uint2 selected(column, row);
    if (importance && any(cell != selected)) {
        // Rounding can send a near-edge sample into a neighboring cell on
        // the inverse map. Its PDF could then lose nearly all proposal mass.
        // Move toward the selected cell's center in bounded increments.
        // This keeps the correction local while the native texel center is a
        // stable fallback, including the largest supported 8192-square maps.
        const float2 center = (float2(selected) + 0.5f) / float(width);
        for (float weight = 0x1p-16f; weight <= 1.0f; weight *= 2.0f) {
            const float2 inward = mix(uv, center, weight);
            world_direction =
                normalize(transform_direction(p.world_from_environment, square_to_sphere(inward)));
            if (!environment_cell(p, world_direction, cell))
                return false;
            if (all(cell == selected))
                break;
        }
        if (any(cell != selected))
            return false;
    }
    pdf = environment_density(p, cdf, cell, compensated);
    return isfinite(pdf) && pdf > 0;
}

} // namespace rypath_image_light
