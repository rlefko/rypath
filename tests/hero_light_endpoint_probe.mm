// SPDX-License-Identifier: GPL-3.0-only
#include "geometry_wire.h"
#include "metal_execution.hpp"
#include "render_wire.h"
#include "rgb_spectrum.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace {
using namespace rypath;
namespace wire = render_wire;
void require(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
template <typename T, std::size_t Extent>
id<MTLBuffer> upload(id<MTLDevice> device, std::span<const T, Extent> values) {
    return [device newBufferWithBytes:values.data()
                               length:values.size_bytes()
                              options:MTLResourceStorageModeShared];
}
template <typename T> std::vector<wire::Word> words(const T &value) {
    const auto a = std::bit_cast<std::array<wire::Word, sizeof(T) / 4>>(value);
    return {a.begin(), a.end()};
}
int run(const std::filesystem::path &path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    require(device != nil, "No Metal device for hero light endpoint probe");
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
    if (!device.supportsRaytracing)
        return 77;
#endif
    require(device.supportsRaytracing,
            "Hero endpoint helper probe requires ray tracing capability");
    std::string why;
    auto library = metal::load_library(device, path, why);
    require(library != nil, why);
    MTLFunctionConstantValues *constants = [MTLFunctionConstantValues new];
    const bool yes = true, no = false;
    for (unsigned i = 0; i <= 18; ++i)
        [constants setConstantValue:&no type:MTLDataTypeBool atIndex:i];
    const unsigned no_hint = 0;
    [constants setConstantValue:&no_hint type:MTLDataTypeUInt atIndex:19];
    for (unsigned i : {10u, 11u, 12u})
        [constants setConstantValue:&yes type:MTLDataTypeBool atIndex:i];
    auto pipeline = metal::load_pipeline(device, library, @"rypath_hero_light_endpoint_probe", why,
                                         nil, constants);
    require(pipeline != nil, why);
    auto queue = [device newCommandQueue];
    require(queue != nil, "Endpoint probe queue");
    const auto rgb = upload(device, srgb_reconstruction_table());
    const std::array<wire::Triangle, 1> triangles{{{{0, 1, 2}}}};
    const std::array<wire::Patch, 1> patches{{{{0, 1, 2, 3}}}};
    const auto triangle_buffer = upload(device, std::span(triangles));
    const auto patch_buffer = upload(device, std::span(patches));
    std::array<float, 471> sky{}, illuminant{};
    for (unsigned i = 0; i < 471; ++i) {
        sky[i] = .25f + .001f * float(i);
        illuminant[i] = 1;
    }
    const auto illuminant_buffer = upload(device, std::span<const float>(illuminant));
    const std::array<wire::TextureLevel, 1> levels{{{1, 1, 0}}};
    const auto levels_buffer = upload(device, std::span(levels));
    const std::array<wire::Word, 4> texels{0x3f800000u, 0x3f800000u, 0x3f800000u, 0u};
    const auto texels_buffer = upload(device, std::span(texels));
    for (unsigned fixture = 0; fixture < 20; ++fixture) {
        wire::Parameters p{};
        // Exercise the uploaded spectral environment; the demo model has a
        // procedural unit sky that intentionally ignores this buffer.
        p.model = wire::pbrt_model;
        p.light_sampler = 2;
        p.light_count = 1;
        p.scene_radius = 10;
        p.bidirectional = 1;
        p.environment_light = UINT32_MAX;
        std::array<wire::Point, 4> positions{{{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, {1, 1, 0}}};
        wire::Instance instance{};
        instance.matrix[0] = instance.matrix[5] = instance.matrix[10] = 1;
        instance.normal_begin = wire::no_normals;
        instance.emission = 0;
        wire::Emitter emitter{0, 0, wire::triangle_primitive};
        wire::AreaEmission emission{};
        emission.two_sided = 1;
        std::copy(sky.begin(), sky.end(), emission.radiance);
        std::vector<wire::Word> data(1);
        if (fixture < 5 || fixture == 16 || fixture == 17) {
            p.instance_count = 1;
            p.emitter_count = 1;
            instance.triangle_count = 1;
            if (fixture == 1) {
                instance.triangle_count = 0;
                instance.patch_count = 1;
                emitter.kind = wire::patch_primitive;
            }
            if (fixture >= 2 && fixture <= 4) {
                geometry_wire::Analytic shape{.kind = geometry_wire::analytic,
                                              .subtype = fixture - 1,
                                              .radius = .75f,
                                              .z_min = -.75f,
                                              .z_max = .75f,
                                              .phi_max = 6.283185307179586f};
                instance.triangle_count = 0;
                instance.analytic_count = 1;
                instance.analytic_begin = 0;
                emitter.kind = wire::analytic_primitive;
                data = words(shape);
            }
            if (fixture == 16)
                std::fill(std::begin(emission.radiance), std::end(emission.radiance), 0.f);
            if (fixture == 17)
                positions.fill({0, 0, 0});
        } else if ((fixture >= 5 && fixture <= 8) || fixture == 13 || fixture == 14 ||
                   fixture == 15) {
            p.positional_count = 1;
            wire::PositionalLight light{};
            light.position = {0, 0, 2};
            light.kind = fixture >= 5 && fixture <= 8 ? fixture - 5 : wire::point_light;
            if (fixture == 14)
                light.kind = wire::goniometric_light;
            if (fixture == 15)
                light.kind = 99;
            light.light_from_world[0] = light.light_from_world[4] = light.light_from_world[8] = 1;
            light.cos_falloff_start = .9f;
            light.cos_falloff_end = .7f;
            light.image_texture = fixture == 14 ? 99u : 0u;
            light.image_scale = 1;
            light.inv_tan_half_fov = 1;
            light.screen_half_width = light.screen_half_height = 1;
            std::copy(sky.begin(), sky.end(), light.intensity);
            data = words(light);
        } else if (fixture == 9 || fixture == 18) {
            p.distant_count = 1;
            wire::DistantLight light{};
            light.direction = {0, 0, -1};
            std::copy(sky.begin(), sky.end(), light.radiance);
            data = words(light);
        } else {
            p.environment_light = 0;
        }
        if (fixture == 11)
            p.light_count = 0;
        if (fixture == 12)
            p.bidirectional = 0; // Environment proposal has no support.
        p.material_data_words = static_cast<wire::Word>(data.size());
        if (fixture == 13 || fixture == 18)
            p.material_data_words = 0;
        std::array<float, 471> current_sky = sky;
        if (fixture == 19)
            current_sky.fill(0);
        wire::ImageTexture texture{};
        texture.level_count = 1;
        texture.wrap = 1;
        texture.texel_format = wire::linear_float_texel_format;
        texture.uscale = texture.vscale = texture.scale = texture.max_anisotropy = 1;
        texture.channels = fixture == 7 || fixture == 14 ? 1u : 3u;
        wire::TextureParameters tp{};
        tp.texture_count = tp.level_count = 1;
        tp.texel_word_count = 4;
        const std::array instances{instance};
        const std::array emitters{emitter};
        const std::array emissions{emission};
        const std::array textures{texture};
        const std::array<id<MTLBuffer>, 15> buffers{
            nil,
            nil,
            upload(device, std::span<const wire::Point>(positions)),
            triangle_buffer,
            patch_buffer,
            upload(device, std::span(instances)),
            upload(device, std::span<const wire::Word>(data)),
            upload(device, std::span(emitters)),
            upload(device, std::span(emissions)),
            upload(device, std::span<const float>(current_sky)),
            upload(device, std::span(textures)),
            levels_buffer,
            texels_buffer,
            rgb,
            illuminant_buffer};
        auto output = [device newBufferWithLength:25 * 16 options:MTLResourceStorageModeShared];
        require(output != nil, "Endpoint probe output");
        std::memset(output.contents, 0xa5, output.length);
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        require(command != nil && encoder != nil, "Endpoint probe encoder");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBytes:&p length:sizeof(p) atIndex:1];
        for (unsigned i = 2; i < buffers.size(); ++i) {
            require(buffers[i] != nil, "Endpoint fixture buffer");
            [encoder setBuffer:buffers[i] offset:0 atIndex:i];
        }
        [encoder setBytes:&tp length:sizeof(tp) atIndex:15];
        [encoder dispatchThreads:MTLSizeMake(8, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        require(metal::complete(command, "hero endpoint differential").empty(),
                "Endpoint dispatch");
        const auto *result = static_cast<const wire::Word *>(output.contents);
        bool valid = false, positive = false, invalid = false;
        for (unsigned i = 0; i < 8; ++i) {
            require(result[4 * i] == 0, "Endpoint differential fixture=" + std::to_string(fixture) +
                                            " sample=" + std::to_string(i) +
                                            " failures=" + std::to_string(result[4 * i]));
            require(result[4 * i + 1] == 256 && result[4 * i + 2] == 1008,
                    "Endpoint layout contract");
            valid |= bool(result[4 * i + 3] & 1);
            positive |= bool(result[4 * i + 3] & 2);
            invalid |= !(result[4 * i + 3] & 1);
        }
        for (unsigned i = 96; i < 100; ++i)
            require(result[i] == 0xa5a5a5a5u, "Endpoint output guard");
        std::ostringstream observed;
        observed << " fixture=" << fixture << " samples=";
        for (unsigned i = 0; i < 8; ++i)
            observed << " [" << i << " flags=" << result[4 * i + 3]
                     << " pdf=" << std::bit_cast<float>(result[32 + 4 * i])
                     << " beta=" << std::bit_cast<float>(result[33 + 4 * i]) << ','
                     << std::bit_cast<float>(result[34 + 4 * i]) << ','
                     << std::bit_cast<float>(result[35 + 4 * i]) << ','
                     << std::bit_cast<float>(result[64 + 4 * i]) << ']';
        if (fixture <= 10)
            require(valid && positive,
                    "Positive endpoint fixture had no contributing sample" + observed.str());
        if (fixture == 13 || fixture == 14 || fixture == 15 || fixture == 18)
            require(invalid, "Invalid endpoint fixture was not rejected" + observed.str());
        if (fixture == 11 || fixture == 12 || fixture == 16 || fixture == 17 || fixture == 19)
            require(!positive, "Zero endpoint fixture contributed" + observed.str());
    }
    std::cout << "Hero light endpoints match full Vertex across 20 light/zero/invalid fixtures\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            require(argc == 2, "Expected probe metallib path");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
