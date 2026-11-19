// SPDX-License-Identifier: GPL-3.0-only
#include "metal_execution.hpp"
#include "qmc_probe_wire.h"
#include "qmc_shared.h"
#include "sampling.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
namespace {
void require(bool pass, const std::string &message) {
    if (!pass)
        throw std::runtime_error(message);
}
} // namespace
void qmc_gpu_probe() {
    using namespace rypath;
    @autoreleasepool {
        const auto device = MTLCreateSystemDefaultDevice();
        require(device != nil, "QMC probe device");
        std::string diagnostic;
        const auto library = metal::load_library(device, {}, diagnostic);
        require(library != nil, diagnostic);
        const auto pipeline =
            metal::load_pipeline(device, library, @"rypath_qmc_probe", diagnostic);
        require(pipeline != nil, diagnostic);
        std::vector<qmc_probe::Query> queries;
        for (auto kind : {2u, 3u})
            for (auto mode : {0u, 1u, 2u, 3u}) {
                if (kind == 2 && mode == 2)
                    continue;
                for (auto seed : {0u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
                    for (auto dim : {2u, 6u, 7u, 883u, 884u, 993u, 998u})
                        for (auto sample : {0ull, 1ull, 255ull, 0xffffffffull, 0x100000000ull}) {
                            const unsigned log = kind == 3 ? 33 : 0;
                            queries.push_back({kind, 3, 5, 1, 4, unsigned(sample),
                                               unsigned(sample >> 32), dim, mode, seed, 3, log});
                        }
            }
        for (const auto size :
             {std::array{128u, 128u}, std::array{129u, 129u}, std::array{320u, 170u},
              std::array{1600u, 850u}, std::array{2944u, 1u}}) {
            const auto width = size[0], height = size[1];
            const auto stride =
                std::uint64_t{qmc::base_scale(width, 2)} * qmc::base_scale(height, 3);
            const auto limit = (std::uint64_t{INT64_MAX} - (stride - 1)) / stride;
            const auto last = (limit - (qmc::halton_light_tile_count(width, height) - 1)) /
                              qmc::halton_light_tile_slots(width, height);
            for (auto sample : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xffffffff},
                                std::uint64_t{0x100000000}, last})
                for (auto x : {0u, 127u, width - 1})
                    for (auto y : {0u, height - 1})
                        for (auto mode : {0u, 1u, 3u})
                            for (auto dim : {8u, 12u, 276u, 792u})
                                queries.push_back({qmc_probe::halton_light, width, height, x, y,
                                                   unsigned(sample), unsigned(sample >> 32), dim,
                                                   mode, 0xffffffffu, 0, 0});
        }
        const auto &tables = sobol_tables();
        const auto table = [device newBufferWithBytes:&tables
                                               length:sizeof(tables)
                                              options:MTLResourceStorageModeShared];
        const auto input = [device newBufferWithBytes:queries.data()
                                               length:queries.size() * sizeof(queries[0])
                                              options:MTLResourceStorageModeShared];
        const unsigned count = unsigned(queries.size()), slots = ((count + 63) / 64) * 64 + 1;
        const auto output = [device newBufferWithLength:slots * sizeof(qmc_probe::Result)
                                                options:MTLResourceStorageModeShared];
        require(table != nil && input != nil && output != nil, "QMC probe allocation");
        std::memset(output.contents, 0xff, output.length);
        const auto queue = [device newCommandQueue];
        const auto command = [queue commandBuffer];
        const auto encoder = [command computeCommandEncoder];
        require(encoder != nil, "QMC probe encoder");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:table offset:0 atIndex:0];
        [encoder setBuffer:input offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&count length:sizeof(count) atIndex:3];
        [encoder dispatchThreadgroups:MTLSizeMake((count + 63) / 64, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        require(metal::complete(command, "QMC probe").empty(), "QMC probe execution");
        const auto *actual = static_cast<const qmc_probe::Result *>(output.contents);
        for (unsigned i = 0; i < count; ++i) {
            const auto q = queries[i];
            const auto sample = (std::uint64_t(q.sample_high) << 32) | q.sample_low;
            std::uint64_t index;
            float expected[5];
            if (q.kind == 2 || q.kind == qmc_probe::halton_light) {
                const auto camera_index = qmc::halton_index(q.width, q.height, q.x, q.y, sample);
                index = q.kind == qmc_probe::halton_light
                            ? qmc::halton_light_index(q.width, q.height, q.x, q.y, sample)
                            : camera_index;
                expected[0] = qmc::halton_camera(0, q.width, camera_index);
                expected[1] = qmc::halton_camera(1, q.height, camera_index);
                expected[2] = qmc::radical(q.dimension, index, q.mode, q.seed);
                expected[3] = expected[2];
                expected[4] = qmc::radical(q.dimension + 1, index, q.mode, q.seed);
            } else {
                const auto morton = qmc::morton(q.x, q.y, q.log_spp, sample);
                index = qmc::zindex(morton, q.dimension, q.exponent, q.log_spp);
                expected[0] = sobol::uniform(
                    qmc::zword(tables, morton, 1, 0, true, q.exponent, q.log_spp, q.mode, q.seed));
                expected[1] = sobol::uniform(
                    qmc::zword(tables, morton, 1, 1, true, q.exponent, q.log_spp, q.mode, q.seed));
                expected[2] = sobol::uniform(qmc::zword(tables, morton, q.dimension, 0, false,
                                                        q.exponent, q.log_spp, q.mode, q.seed));
                expected[3] = sobol::uniform(qmc::zword(tables, morton, q.dimension, 0, true,
                                                        q.exponent, q.log_spp, q.mode, q.seed));
                expected[4] = sobol::uniform(qmc::zword(tables, morton, q.dimension, 1, true,
                                                        q.exponent, q.log_spp, q.mode, q.seed));
            }
            require(actual[i].values[0] == unsigned(index) &&
                        actual[i].values[1] == unsigned(index >> 32) &&
                        actual[i].values[7] == 0x514d4331,
                    "QMC GPU index mismatch");
            for (unsigned j = 0; j < 5; ++j) {
                const float observed = qmc::bits_float(actual[i].values[j + 2]);
                const float bound = 8 * std::numeric_limits<float>::epsilon() *
                                    std::max(expected[j], std::numeric_limits<float>::min());
                require(std::abs(observed - expected[j]) <= bound,
                        "QMC CPU/GPU float mismatch at query " + std::to_string(i) + " component " +
                            std::to_string(j));
            }
        }
        for (unsigned i = count; i < slots; ++i)
            for (auto word : actual[i].values)
                require(word == 0xffffffffu, "QMC padded guard changed");
    }
}
