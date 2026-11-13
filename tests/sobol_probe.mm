// SPDX-License-Identifier: GPL-3.0-only
#include "metal_execution.hpp"
#include "sampling.hpp"
#include "sobol_probe_wire.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace

void sobol_gpu_probe() {
    using namespace rypath;
    @autoreleasepool {
        const id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Sobol probe requires a Metal device");
        std::string diagnostic;
        const auto library = metal::load_library(device, {}, diagnostic);
        require(library != nil, diagnostic);
        const auto pipeline =
            metal::load_pipeline(device, library, @"rypath_sobol_probe", diagnostic);
        require(pipeline != nil, diagnostic);
        std::vector<sobol::Probe> queries;
        for (const auto m : {0U, 1U, 3U, 10U, 25U}) {
            const auto coordinate = (1U << m) - 1;
            const auto limit = std::uint64_t{1} << (52 - 2 * m);
            for (const auto frame : {std::uint64_t{0}, std::uint64_t{1}, limit - 1}) {
                for (const auto dimension : {0U, 1U, 2U, 7U, 11U, 523U, 1019U, 1023U}) {
                    for (std::uint32_t mode = 0; mode < 4; ++mode) {
                        for (const auto seed : {0U, 0x7fffffffU, 0x80000000U, 0xffffffffU}) {
                            queries.push_back({m, coordinate, coordinate, std::uint32_t(frame),
                                               std::uint32_t(frame >> 32), dimension, mode, seed});
                        }
                    }
                }
            }
        }
        // An asymmetric mapping and a nonmultiple of the group width exercise
        // both pixel axes and execution of the padded-lane guard.
        queries.push_back({3, 1, 4, 37, 0, 523, sobol::owen, 17});
        const auto &tables = sobol_tables();
        const auto table_buffer = [device newBufferWithBytes:&tables
                                                      length:sizeof(tables)
                                                     options:MTLResourceStorageModeShared];
        const auto inputs = [device newBufferWithBytes:queries.data()
                                                length:queries.size() * sizeof(sobol::Probe)
                                               options:MTLResourceStorageModeShared];
        constexpr std::uint32_t width = 64;
        const auto count = static_cast<std::uint32_t>(queries.size());
        const auto groups = (count + width - 1) / width;
        const auto bytes = (groups * width + 1) * sizeof(sobol::ProbeResult);
        const auto output = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        require(table_buffer != nil && inputs != nil && output != nil,
                "Sobol probe buffer allocation failed");
        std::memset(output.contents, 0xff, bytes);
        const auto queue = [device newCommandQueue];
        const auto command = [queue commandBuffer];
        const auto encoder = [command computeCommandEncoder];
        require(queue != nil && command != nil && encoder != nil, "Sobol probe command failed");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:table_buffer offset:0 atIndex:0];
        [encoder setBuffer:inputs offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&count length:sizeof(count) atIndex:3];
        [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
        diagnostic = metal::complete(command, "Sobol probe");
        require(diagnostic.empty(), diagnostic);
        const auto *values = static_cast<const sobol::ProbeResult *>(output.contents);
        for (std::size_t i = 0; i < queries.size(); ++i) {
            const auto &q = queries[i];
            const auto index = sobol::index(tables, q.exponent, q.x, q.y,
                                            (std::uint64_t{q.sample_high} << 32) | q.sample_low);
            const auto word = sobol::word(tables, index, q.dimension, q.mode, q.seed);
            const auto camera = sobol::word(tables, index, 0, sobol::no_randomization, 0);
            const auto &actual = values[i];
            require(actual.index_low == std::uint32_t(index) &&
                        actual.index_high == std::uint32_t(index >> 32) && actual.value == word &&
                        actual.hash == sobol::dimension_hash(q.dimension, q.seed) &&
                        actual.unit == sobol::uniform(word) &&
                        actual.camera == sobol::camera_uniform(camera, q.exponent, q.x),
                    "CPU/MSL Sobol probe mismatch at query " + std::to_string(i));
        }
        const auto *raw = static_cast<const unsigned char *>(output.contents);
        const std::span guard{raw + queries.size() * sizeof(sobol::ProbeResult),
                              bytes - queries.size() * sizeof(sobol::ProbeResult)};
        require(std::ranges::all_of(guard, [](unsigned char value) { return value == 0xff; }),
                "Sobol probe wrote a padded lane");
    }
}
