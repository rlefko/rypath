// SPDX-License-Identifier: GPL-3.0-only
#include "image_texture.hpp"
#include "light_sampling.hpp"
#include "light_sampling_probe_wire.h"
#include "metal_execution.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
namespace {
using namespace rypath;
namespace wire = render_wire;
namespace probe = light_sampling_probe;
void require(bool x, const char *message) {
    if (!x)
        throw std::runtime_error(message);
}
template <class Range> id<MTLBuffer> buffer(id<MTLDevice> d, const Range &r) {
    auto s = std::span(r);
    return [d newBufferWithBytes:s.data()
                          length:s.size_bytes()
                         options:MTLResourceStorageModeShared];
}
void run(id<MTLDevice> device, const std::filesystem::path &path) {
    std::string error;
    auto library = metal::load_library(device, path, error);
    require(library != nil, error.c_str());
    auto pipeline = metal::load_pipeline(device, library, @"rypath_probe_light_sampling", error);
    require(pipeline != nil, error.c_str());
    constexpr unsigned side = 256, count = side * side;
    std::vector<probe::Query> queries;
    queries.reserve(count);
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x)
            queries.push_back({{(x + .5f) / side, (y + .5f) / side}});
    std::vector<probe::Result> results(count);
    auto queryBuffer = buffer(device, queries);
    auto output = buffer(device, results);
    std::vector<LightSamplingInput> input(4);
    input[0].low = input[0].high = {-1, 0, 0};
    input[0].power = 1;
    input[1].low = input[1].high = {1, 0, 0};
    input[1].power = 10;
    input[2].infinite = true;
    input[2].power = 2;
    input[3].power = 0;
    const auto tables = build_light_sampling(input, 1024 * 1024, error);
    require(bool(tables), error.c_str());
    wire::Parameters parameters;
    parameters.light_count = 4;
    parameters.light_node_count = static_cast<wire::Word>(tables->nodes.size());
    parameters.infinite_light_count = 1;
    std::vector<wire::Word> data;
    const auto append = [&]<class T>(const std::vector<T> &values, wire::Word &offset) {
        offset = static_cast<wire::Word>(data.size());
        for (const auto &v : values) {
            auto words = std::bit_cast<std::array<wire::Word, sizeof(T) / 4>>(v);
            data.insert(data.end(), words.begin(), words.end());
        }
    };
    append(tables->alias, parameters.light_alias_begin);
    append(tables->records, parameters.light_records_begin);
    append(tables->nodes, parameters.light_nodes_begin);
    append(tables->infinite, parameters.infinite_lights_begin);
    ImagePyramid image{.encoding = ImageEncoding::linear,
                       .wrap = ImageWrap::clamp,
                       .levels = {{4, 4, 0}},
                       .storage = ImageStorage::linear_float,
                       .native_base = true};
    for (unsigned i = 0; i < 16; ++i) {
        float value = i == 5 ? 4.f : 1.f;
        image.texels.insert(image.texels.end(),
                            {std::bit_cast<wire::Word>(value), std::bit_cast<wire::Word>(value),
                             std::bit_cast<wire::Word>(value), 0});
    }
    for (unsigned kind = 0; kind < 4; ++kind)
        for (bool affine : {false, true}) {
            wire::PositionalLight light{};
            light.kind = kind;
            light.light_from_world[0] = affine ? 1.4f : 1;
            light.light_from_world[1] = affine ? .2f : 0;
            light.light_from_world[4] = affine ? .8f : 1;
            light.light_from_world[8] = affine ? 1.1f : 1;
            light.cos_falloff_start = .9f;
            light.cos_falloff_end = .7f;
            light.inv_tan_half_fov = 1;
            light.screen_half_width = 1;
            light.screen_half_height = 1;
            auto words = data;
            if (kind >= 2) {
                auto distribution = build_light_direction_sampling(image, 1024 * 1024, error,
                                                                   kind == 3 ? 1.f : 0.f);
                require(bool(distribution), error.c_str());
                light.sampling_begin = static_cast<wire::Word>(words.size());
                light.sampling_width = distribution->width;
                light.sampling_height = distribution->height;
                for (float v : distribution->cdf)
                    words.push_back(std::bit_cast<wire::Word>(v));
            }
            auto dataBuffer = buffer(device, words);
            const std::array lights{light};
            auto lightBuffer = buffer(device, lights);
            for (unsigned policy = 0; policy < 3; ++policy) {
                parameters.light_sampler = policy;
                auto command = [[device newCommandQueue] commandBuffer];
                auto encoder = [command computeCommandEncoder];
                [encoder setComputePipelineState:pipeline];
                [encoder setBuffer:queryBuffer offset:0 atIndex:0];
                [encoder setBuffer:output offset:0 atIndex:1];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2];
                [encoder setBuffer:dataBuffer offset:0 atIndex:3];
                [encoder setBuffer:lightBuffer offset:0 atIndex:4];
                [encoder setBytes:&count length:sizeof(count) atIndex:5];
                [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
                [encoder endEncoding];
                error = metal::complete(command, "light sampling probe");
                require(error.empty(), error.c_str());
                std::memcpy(results.data(), output.contents,
                            results.size() * sizeof(probe::Result));
                std::array<unsigned, 4> hits{};
                std::array<double, 4> pmfs{};
                double integral = 0;
                for (auto &r : results) {
                    require(r.valid == 1, "Emitted sample failed");
                    require(std::isfinite(r.sampled_pdf) && r.sampled_pdf > 0 &&
                                r.sampled_pdf == r.evaluated_pdf,
                            "Emitted sample/evaluation mismatch");
                    double length2 = double(r.direction.x) * r.direction.x +
                                     double(r.direction.y) * r.direction.y +
                                     double(r.direction.z) * r.direction.z;
                    require(std::abs(length2 - 1) < 1e-5, "Emitted direction norm");
                    require(r.light < 4 && r.pmf > 0, "Invalid selected light");
                    require(std::abs(r.pmf - r.queried_pmf) < 1e-6,
                            "Selection/evaluation PMF mismatch");
                    hits[r.light]++;
                    pmfs[r.light] = r.pmf;
                    integral += r.sphere_pdf * 4 * std::numbers::pi / count;
                }
                for (unsigned i = 0; i < 4; ++i)
                    require(std::abs(double(hits[i]) / count - pmfs[i]) < .004,
                            "Selected light frequency differs from PMF");
                require(std::abs(integral - 1) < .025, "Emitted directional PDF integral");
            }
        }
    std::cout << "Native light PMFs and all positional emission PDFs passed\n";
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil)
                return 77;
#endif
            require(device != nil, "Light probe requires a Metal device");
            run(device, argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
