// SPDX-License-Identifier: GPL-3.0-only

#include "gltf_texture_probe_wire.h"
#include "image_texture.hpp"
#include "metal_execution.hpp"
#include "render_wire.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace rypath;
namespace wire = render_wire;
namespace probe = gltf_texture_probe;
using Rgba = std::array<float, 4>;

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T success(ImageResult<T> result) {
    require(result.value.has_value(), result.diagnostic);
    return std::move(*result.value);
}

void run(id<MTLDevice> device) {
    std::vector<wire::ImageTexture> textures;
    std::vector<wire::TextureLevel> levels;
    std::vector<unsigned> texels;
    std::vector<probe::Query> queries;
    std::vector<std::optional<Rgba>> expected;
    const auto add_query = [&](probe::Query query, std::optional<Rgba> value) {
        queries.push_back(query);
        expected.push_back(value);
    };
    // Independent known-value UV, channel and seam checks precede CPU comparisons.
    const auto known_image =
        success(build_gltf_image_pyramid({2,
                                          2,
                                          {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255},
                                          ImageSampleFormat::unorm8,
                                          4,
                                          {0, 85, 170, 255}},
                                         ImageEncoding::linear));
    const auto add_texture = [&](const ImagePyramid &image, GltfSampler sampler) {
        wire::ImageTexture descriptor{};
        descriptor.level_begin = static_cast<unsigned>(levels.size());
        descriptor.level_count = static_cast<unsigned>(image.levels.size());
        descriptor.wrap = sampler.wrap_s;
        descriptor.wrap_t = sampler.wrap_t;
        descriptor.min_filter = sampler.min_filter;
        descriptor.mag_filter = sampler.mag_filter;
        descriptor.gltf = 1;
        descriptor.texel_format = wire::linear_float_texel_format;
        descriptor.channels = 4;
        const auto offset = static_cast<unsigned>(texels.size());
        for (const auto &level : image.levels)
            levels.push_back({level.width, level.height, level.offset + offset});
        texels.insert(texels.end(), image.texels.begin(), image.texels.end());
        textures.push_back(descriptor);
        return static_cast<unsigned>(textures.size() - 1);
    };
    const auto known = add_texture(known_image, {10497, 33071, 9728, 9728});
    const auto known_words = static_cast<unsigned>(texels.size());
    add_query({known, .25f, .25f, 0, 0, 0, 0, known_words}, Rgba{1, 0, 0, 0});
    add_query({known, .25f, .75f, 0, 0, 0, 0, known_words}, Rgba{0, 0, 1, 2.0f / 3});
    add_query({known, -.25f, .25f, 0, 0, 0, 0, known_words}, Rgba{0, 1, 0, 1.0f / 3});
    add_query({known, -0x1p-30f, .25f, 0, 0, 0, 0, known_words}, Rgba{0, 1, 0, 1.0f / 3});
    add_query({known, 1, 1, 0, 0, 0, 0, known_words}, Rgba{0, 0, 1, 2.0f / 3});
    const auto linear_known = add_texture(known_image, {10497, 33071, 9729, 9729});
    add_query({linear_known, 0, .25f, 0, 0, 0, 0, static_cast<unsigned>(texels.size())},
              Rgba{.5f, .5f, 0, 1.0f / 6});

    for (const auto dimensions : {std::array{3u, 5u}, std::array{4u, 2u}}) {
        DecodedImage source{dimensions[0], dimensions[1], {}};
        source.source_channels = 4;
        for (unsigned y = 0; y < source.height; ++y)
            for (unsigned x = 0; x < source.width; ++x) {
                source.rgb.push_back(static_cast<unsigned char>((31 * x + 17 * y + 7) % 256));
                source.rgb.push_back(static_cast<unsigned char>((79 * x + 43 * y + 53) % 256));
                source.rgb.push_back(static_cast<unsigned char>((11 * x + 97 * y + 101) % 256));
                source.alpha.push_back(static_cast<unsigned char>((61 * x + 37 * y + 19) % 256));
            }
        const auto image = std::make_shared<ImagePyramid>(
            success(build_gltf_image_pyramid(source, ImageEncoding::srgb)));
        for (unsigned wrap_s : {10497u, 33071u, 33648u})
            for (unsigned wrap_t : {10497u, 33071u, 33648u})
                for (unsigned min_filter : {9728u, 9729u, 9984u, 9985u, 9986u, 9987u})
                    for (unsigned mag_filter : {9728u, 9729u}) {
                        const GltfSampler sampler{wrap_s, wrap_t, min_filter, mag_filter};
                        const auto index = add_texture(*image, sampler);
                        ImageTexture cpu;
                        cpu.image = image;
                        cpu.gltf_sampler = sampler;
                        const auto words = static_cast<unsigned>(texels.size());
                        const float width = static_cast<float>(source.width);
                        const float unit = 1 / width;
                        const std::array derivatives{
                            std::array{0.f, 0.f, 0.f, 0.f},
                            std::array{unit * .5f, 0.f, 0.f, 0.f},
                            std::array{std::nextafter(unit, 0.f), 0.f, 0.f, 0.f},
                            std::array{unit, 0.f, 0.f, 0.f},
                            std::array{std::nextafter(unit, 1.f), 0.f, 0.f, 0.f},
                            std::array{std::exp2(.49f) / width, 0.f, 0.f, 0.f},
                            std::array{std::sqrt(2.f) / width, 0.f, 0.f, 0.f},
                            std::array{std::exp2(.51f) / width, 0.f, 0.f, 0.f},
                            std::array{2 / width, 0.f, 0.f, 0.f},
                            std::array{std::exp2(1.25f) / width, 0.f, 0.f, 0.f},
                            std::array{std::exp2(1.75f) / width, 0.f, 0.f, 0.f},
                            std::array{0.f, .7f, -.2f, .4f},
                            std::array{.31f, -.27f, .17f, .19f},
                            std::array{1e30f, -1e30f, 1e-30f, 0.f}};
                        const float edge = 1 / width;
                        const std::array coordinates{std::array{.173f, .619f},
                                                     std::array{0.f, 0.f},
                                                     std::array{1.f, 1.f},
                                                     std::array{-.25f, 1.25f},
                                                     std::array{-1.f, -1.f},
                                                     std::array{2.f, -2.f},
                                                     std::array{65.125f, -72.375f},
                                                     std::array{1e30f, -1e30f},
                                                     std::array{edge, .3f},
                                                     std::array{std::nextafter(edge, 0.f), .3f},
                                                     std::array{std::nextafter(edge, 1.f), .3f},
                                                     std::array{-0x1p-30f, .25f}};
                        for (const auto uv : coordinates) {
                            for (const auto d : derivatives) {
                                const auto value =
                                    gltf_image_rgba(cpu, uv[0], uv[1], d[0], d[1], d[2], d[3]);
                                require(value.has_value(), "Valid glTF CPU sampler query failed");
                                add_query({index, uv[0], uv[1], d[0], d[1], d[2], d[3], words},
                                          value);
                            }
                        }
                    }
    }
    // Each invalid case remains within allocated buffers, while its declared
    // range or selected descriptor is rejected before sampling memory.
    const auto total_words = static_cast<unsigned>(texels.size());
    const auto invalid_texture = [&](wire::ImageTexture descriptor) {
        textures.push_back(descriptor);
        add_query({static_cast<unsigned>(textures.size() - 1), .25f, .25f, 0, 0, 0, 0, total_words},
                  {});
    };
    auto bad = textures[known];
    bad.level_begin = std::numeric_limits<unsigned>::max();
    invalid_texture(bad);
    bad = textures[known];
    bad.level_count = std::numeric_limits<unsigned>::max();
    invalid_texture(bad);
    bad = textures[known];
    bad.level_count = 0;
    invalid_texture(bad);
    bad = textures[known];
    bad.wrap_t = 0;
    invalid_texture(bad);
    bad = textures[known];
    bad.min_filter = 0;
    invalid_texture(bad);
    bad = textures[known];
    bad.mag_filter = 0;
    invalid_texture(bad);
    bad = textures[known];
    bad.texel_format = wire::linear_half_texel_format;
    invalid_texture(bad);
    bad = textures[known];
    bad.gltf = 0;
    invalid_texture(bad);
    bad = textures[known];
    bad.level_begin = static_cast<unsigned>(levels.size());
    bad.level_count = 1;
    levels.push_back({2, 2, total_words - 4});
    invalid_texture(bad);
    levels.push_back({2, 2, 1});
    bad.level_begin++;
    invalid_texture(bad);
    levels.push_back({0, 2, 0});
    bad.level_begin++;
    invalid_texture(bad);
    levels.push_back({4097, 1, 0});
    bad.level_begin++;
    invalid_texture(bad);
    add_query({static_cast<unsigned>(textures.size()), 0, 0, 0, 0, 0, 0, total_words}, {});
    add_query({known, 0, 0, 0, 0, 0, 0, 0}, {});
    add_query({known, 0, 0, 0, 0, 0, 0, total_words + 1}, {});
    add_query({known, std::numeric_limits<float>::infinity(), 0, 0, 0, 0, 0, total_words}, {});
    add_query({known, 0, 0, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, total_words}, {});

    std::string diagnostic;
    const auto library = metal::load_library(device, {}, diagnostic);
    require(library != nil, diagnostic);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_gltf_texture_probe", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto upload = [device](const auto &values) {
        return [device newBufferWithBytes:values.data()
                                   length:values.size() * sizeof(values[0])
                                  options:MTLResourceStorageModeShared];
    };
    const auto texture_buffer = upload(textures), level_buffer = upload(levels),
               texel_buffer = upload(texels), query_buffer = upload(queries);
    const auto output = [device newBufferWithLength:(queries.size() + 1) * sizeof(probe::Result)
                                            options:MTLResourceStorageModeShared];
    require(texture_buffer && level_buffer && texel_buffer && query_buffer && output,
            "glTF texture probe allocation failed");
    std::memset(output.contents, 0x5a, output.length);
    const probe::Parameters parameters{static_cast<unsigned>(textures.size()),
                                       static_cast<unsigned>(levels.size()), total_words,
                                       static_cast<unsigned>(queries.size())};
    const auto queue = [device newCommandQueue];
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(queue && command && encoder, "glTF texture probe command allocation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:texture_buffer offset:0 atIndex:0];
    [encoder setBuffer:level_buffer offset:0 atIndex:1];
    [encoder setBuffer:texel_buffer offset:0 atIndex:2];
    [encoder setBuffer:query_buffer offset:0 atIndex:3];
    [encoder setBuffer:output offset:0 atIndex:4];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:5];
    const auto group = pipeline.threadExecutionWidth;
    [encoder dispatchThreadgroups:MTLSizeMake((queries.size() + group - 1) / group, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "glTF texture probe");
    require(diagnostic.empty(), diagnostic);
    const auto *results = static_cast<const probe::Result *>(output.contents);
    // Predeclared before hardware execution; applies to normal finite RGBA only.
    constexpr double absolute_tolerance = 2e-6, relative_tolerance = 2e-5;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::array actual{results[i].r, results[i].g, results[i].b, results[i].a};
        for (std::size_t c = 0; c < 4; ++c) {
            const auto detail = " at query " + std::to_string(i) + " channel " + std::to_string(c);
            if (!expected[i]) {
                require(std::isnan(actual[c]),
                        "Invalid glTF descriptor or range accepted" + detail);
            } else {
                const auto reference = (*expected[i])[c];
                require(std::isfinite(actual[c]) &&
                            std::abs(double(actual[c]) - reference) <=
                                absolute_tolerance +
                                    relative_tolerance * std::abs(double(reference)),
                        "CPU/GPU glTF sampler mismatch" + detail + " expected " +
                            std::to_string(reference) + " actual " + std::to_string(actual[c]));
            }
        }
    }
    std::array<unsigned char, sizeof(probe::Result)> guard;
    guard.fill(0x5a);
    require(std::memcmp(&results[expected.size()], guard.data(), guard.size()) == 0,
            "glTF texture probe wrote beyond output range");
    std::cout << "Validated " << queries.size() << " native glTF RGBA GPU sampler queries\n";
}
} // namespace

int main() {
    @autoreleasepool {
        try {
            const id<MTLDevice> device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP glTF texture probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "glTF texture probe requires Metal");
            run(device);
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
