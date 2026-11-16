// SPDX-License-Identifier: GPL-3.0-only
#include "lens_probe.hpp"
#include "metal_execution.hpp"
#include "sampling.hpp"
#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>

namespace {
void require(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
} // namespace
std::vector<rypath::lens_probe::Result> lens_gpu_probe(const rypath::render_wire::Parameters &p) {
    using namespace rypath;
    @autoreleasepool {
        const auto device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Lens probe requires Metal");
        std::string diagnostic;
        const auto library = metal::load_library(device, {}, diagnostic);
        require(library != nil, diagnostic);
        const auto pipeline =
            metal::load_pipeline(device, library, @"rypath_lens_probe", diagnostic);
        require(pipeline != nil, diagnostic);
        const auto &tables = sobol_tables();
        const auto table_buffer = [device newBufferWithBytes:&tables
                                                      length:sizeof(tables)
                                                     options:MTLResourceStorageModeShared];
        constexpr unsigned width = 64;
        const auto groups = (p.lane_count + width - 1) / width;
        const auto bytes = (groups * width + 1) * sizeof(lens_probe::Result);
        const auto output = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        require(table_buffer != nil && output != nil, "Lens probe allocation");
        std::memset(output.contents, 0xff, bytes);
        const auto queue = [device newCommandQueue];
        const auto command = [queue commandBuffer];
        const auto encoder = [command computeCommandEncoder];
        require(queue != nil && command != nil && encoder != nil, "Lens probe command");
        [encoder setComputePipelineState:pipeline];
        [encoder setBytes:&p length:sizeof(p) atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBuffer:table_buffer offset:0 atIndex:12];
        [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
        diagnostic = metal::complete(command, "Lens probe");
        require(diagnostic.empty(), diagnostic);
        const auto *raw = static_cast<const unsigned char *>(output.contents);
        const std::span guard{raw + p.lane_count * sizeof(lens_probe::Result),
                              bytes - p.lane_count * sizeof(lens_probe::Result)};
        require(std::ranges::all_of(guard, [](auto v) { return v == 0xff; }),
                "Lens probe wrote padded output");
        const auto *values = static_cast<const lens_probe::Result *>(output.contents);
        return {values, values + p.lane_count};
    }
}
