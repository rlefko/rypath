// SPDX-License-Identifier: GPL-3.0-only

#include "../tests/image_lighting_probe_wire.h"
#include "image_lighting.h"

using namespace metal;
namespace probe = rypath::image_lighting_probe;
namespace wire = rypath::render_wire;
namespace lighting = rypath_image_light;

kernel void rypath_probe_image_lighting(
    const device probe::Query *queries [[buffer(0)]], device probe::Result *results [[buffer(1)]],
    const device wire::ImageTexture *textures [[buffer(2)]],
    const device wire::TextureLevel *levels [[buffer(3)]],
    const device wire::Word *texels [[buffer(4)]], const device float *transfer [[buffer(5)]],
    const device float *spectra [[buffer(6)]], const device float *illuminant [[buffer(7)]],
    constant wire::TextureParameters &image_parameters [[buffer(8)]],
    constant wire::TextureGraphParameters &graph_parameters [[buffer(9)]],
    const device float *cdf [[buffer(10)]], constant uint &count [[buffer(11)]],
    constant uint &compensated [[buffer(12)]], uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;

    const auto query = queries[index];
    const lighting::Images images{textures, levels,     texels,           transfer,
                                  spectra,  illuminant, &image_parameters};
    const float3 direction(query.direction.x, query.direction.y, query.direction.z);
    float3 sampled(0);
    float pdf = 0;
    const bool valid =
        lighting::sample_environment(graph_parameters, cdf, float2(query.sample.x, query.sample.y),
                                     sampled, pdf, compensated != 0);
    const float2 st(0.25f, 0.75f);
    const float scalar_bilinear = rypath_image::lookup_scalar(
        textures[1], levels, texels, transfer, image_parameters, st, float2(0), float2(0), true);
    const float scalar_point = rypath_image::lookup_scalar(
        textures[2], levels, texels, transfer, image_parameters, st, float2(0), float2(0), true);
    const float scalar_ewa =
        rypath_image::lookup_scalar(textures[3], levels, texels, transfer, image_parameters, st,
                                    float2(0.07f, 0), float2(0, 0.04f), true);
    const float scalar_coarsest =
        rypath_image::lookup_scalar(textures[1], levels, texels, transfer, image_parameters, st,
                                    float2(0.5f, 0), float2(0), true);
    results[index] = {
        lighting::area_radiance(images, 0, 1.75f, float2(query.uv.x, query.uv.y), query.wavelength),
        lighting::environment_radiance(images, graph_parameters, direction, query.wavelength),
        lighting::environment_pdf(graph_parameters, cdf, direction, compensated != 0),
        pdf,
        {sampled.x, sampled.y, sampled.z},
        valid ? 1u : 0u,
        scalar_bilinear,
        scalar_point,
        scalar_ewa,
        scalar_coarsest};
}

kernel void rypath_probe_environment_cell(
    const device wire::Point2 *queries [[buffer(0)]],
    device probe::StressResult *results [[buffer(1)]],
    const device wire::ImageTexture *textures [[buffer(2)]],
    const device wire::TextureLevel *levels [[buffer(3)]],
    const device wire::Word *texels [[buffer(4)]], const device float *transfer [[buffer(5)]],
    const device float *spectra [[buffer(6)]], const device float *illuminant [[buffer(7)]],
    constant wire::TextureParameters &image_parameters [[buffer(8)]],
    constant wire::TextureGraphParameters &graph_parameters [[buffer(9)]],
    const device float *cdf [[buffer(10)]], constant uint &count [[buffer(11)]],
    constant wire::Point2 &bright_texel_uv [[buffer(12)]],
    constant uint &compensated [[buffer(13)]], uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;

    const float2 sample(queries[index].x, queries[index].y);
    const uint width = graph_parameters.environment_width;
    const device float *selected_cdf =
        cdf + (compensated ? graph_parameters.environment_compensated_offset : 0u);
    uint row = 0, column = 0;
    const float y =
        lighting::sample_cdf(selected_cdf, width, min(sample.y, lighting::less_than_one), row);
    const float x =
        lighting::sample_cdf(selected_cdf + (row + 1) * (width + 1), width,
                             min(compensated ? sample.x
                                             : (sample.x - lighting::uniform_probability) /
                                                   (1 - lighting::uniform_probability),
                                 lighting::less_than_one),
                             column);
    const float3 initial = normalize(lighting::transform_direction(
        graph_parameters.world_from_environment, lighting::square_to_sphere(float2(x, y))));
    uint2 initial_cell;
    bool valid = lighting::environment_cell(graph_parameters, initial, initial_cell);
    const bool same_cell = valid && all(initial_cell == uint2(column, row));

    float pdf = 0;
    float3 direction(0);
    valid = valid && lighting::sample_environment(graph_parameters, cdf, sample, direction, pdf,
                                                  compensated != 0);
    const auto bright_direction = normalize(lighting::transform_direction(
        graph_parameters.world_from_environment,
        lighting::square_to_sphere(float2(bright_texel_uv.x, bright_texel_uv.y))));
    const lighting::Images images{textures, levels,     texels,           transfer,
                                  spectra,  illuminant, &image_parameters};
    results[index] = {
        pdf,
        lighting::environment_pdf(graph_parameters, cdf, direction, compensated != 0),
        length(direction - initial),
        lighting::environment_radiance(images, graph_parameters, bright_direction, 555),
        same_cell ? 1u : 0u,
        valid ? 1u : 0u};
}
