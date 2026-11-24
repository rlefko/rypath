// SPDX-License-Identifier: GPL-3.0-only
#include "metal_execution.hpp"
#include "wavefront_wire.h"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace rypath;
namespace wf = wavefront_wire;
void require(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
int run(const std::filesystem::path &path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    require(device != nil, "No Metal device for queue failure validation");
    std::string why;
    auto library = metal::load_library(device, path, why);
    require(library != nil, why);
    auto function = metal::load_function(library, @"rypath_wavefront_advance", why);
    require(function != nil, why);
    auto pipeline = metal::load_pipeline(device, library, @"rypath_wavefront_advance", why);
    require(pipeline != nil, why);
    auto argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
    require(argument_encoder != nil, "Queue argument encoder");
    auto arguments = [device newBufferWithLength:argument_encoder.encodedLength
                                         options:MTLResourceStorageModeShared];
    std::array<id<MTLBuffer>, wf::replay_input_id + 1> buffers{};
    [argument_encoder setArgumentBuffer:arguments offset:0];
    for (unsigned i = 0; i < buffers.size(); ++i) {
        buffers[i] = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        require(buffers[i] != nil, "Queue fault test allocation");
        [argument_encoder setBuffer:buffers[i] offset:0 atIndex:i];
    }
    auto queue = [device newCommandQueue];
    require(queue != nil && arguments != nil, "Queue fault test command resources");
    const wf::Config config{4, 2, 0, render_wire::pbrt_model, 32, 0, {0, 0}};
    const std::array<wf::Counters, 7> cases{{
        {2, 2, 0, 4, 2, 0, 7, 2}, // Valid two-owner continuation clears its range.
        {2, 3, 0, 4, 1, 0, 7, 0}, // Output exceeds physical capacity.
        {2, 1, 3, 4, 3, 0, 7, 0}, // Free list exceeds physical capacity.
        {2, 0, 0, 4, 5, 0, 7, 0}, // More completions than pixels.
        {2, 0, 0, 5, 4, 0, 7, 0}, // Allocator exceeds pixel domain.
        {2, 0, 0, 4, 2, 0, 7, 0}, // Lost work cannot become an empty successful queue.
        {2, 2, 0, 4, 2, wf::queue_overflow, 7, 0}, // Producer overflow stays sticky.
    }};
    for (std::size_t n = 0; n < cases.size(); ++n) {
        for (auto buffer : buffers)
            std::memset(buffer.contents, 0xa5, buffer.length);
        std::memcpy(buffers[wf::config_id].contents, &config, sizeof(config));
        std::memcpy(buffers[wf::counters_id].contents, &cases[n], sizeof(wf::Counters));
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        require(command != nil && encoder != nil, "Queue fault test encoding");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:arguments offset:0 atIndex:1];
        for (auto buffer : buffers)
            [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        require(metal::complete(command, "wavefront queue fault").empty(),
                "Queue dispatch failure");
        wf::Counters result{};
        std::memcpy(&result, buffers[wf::counters_id].contents, sizeof(result));
        const auto expected_failure = n == 0   ? 0u
                                      : n == 6 ? wf::queue_overflow
                                               : wf::invalid_counters;
        require(result.failure == expected_failure && result.current == (n == 0 ? 2u : 0u) &&
                    result.out == 0 && result.free == 0 && result.refill_begin == 0 &&
                    result.rounds == 8 && result.completed == cases[n].completed &&
                    result.next_pixel == cases[n].next_pixel,
                "Queue corruption must fail explicitly without inventing completions");
        for (unsigned i = 0; i < buffers.size(); ++i) {
            const auto written = i == wf::counters_id ? sizeof(wf::Counters)
                                 : i == wf::config_id ? sizeof(wf::Config)
                                 : i == wf::current_dispatch_id || i == wf::refill_dispatch_id
                                     ? sizeof(wf::DispatchArguments)
                                     : 0;
            const auto *bytes = static_cast<const unsigned char *>(buffers[i].contents);
            for (std::size_t j = written; j < buffers[i].length; ++j)
                require(bytes[j] == 0xa5, "Queue failure touched unrelated storage or a guard");
        }
    }
    auto prepare = metal::load_pipeline(device, library, @"rypath_wavefront_prepare_refill", why);
    require(prepare != nil, why);
    const std::array<wf::Counters, 12> refill_cases{{
        {2, 0, 2, 2, 2, 0, 7, 0}, // Full refill owns [2,4).
        {2, 0, 2, 3, 3, 0, 7, 0}, // Partial final range owns [3,4).
        {2, 0, 2, 4, 4, 0, 7, 0}, // Exhausted domain claims nothing.
        {2, 1, 1, 3, 2, 0, 7, 0}, // One continuing owner plus one new owner.
        {2, 2, 0, 2, 0, 0, 7, 0}, // All owners continue; no free slots.
        {0, 0, 0, 4, 4, 0, 7, 0}, // Extra encoded round after all work completed.
        {2, 0, 3, 3, 3, 0, 7, 0}, // Invalid free count.
        {2, 0, 2, 5, 5, 0, 7, 0}, // Invalid next owner.
        {2, 0, 2, 2, 3, 0, 7, 0}, // Completions exceed issued owners.
        {2, 0, 1, 3, 3, 0, 7, 0}, // Lost active owner.
        {2, 1, 1, 3, 3, 0, 7, 0}, // Continuing count disagrees with issued work.
        {2, 0, 2, 2, 2, wf::queue_overflow, 7, 0},
    }};
    for (std::size_t n = 0; n < refill_cases.size(); ++n) {
        for (auto buffer : buffers)
            std::memset(buffer.contents, 0xa5, buffer.length);
        std::memcpy(buffers[wf::config_id].contents, &config, sizeof(config));
        const auto before = refill_cases[n];
        std::memcpy(buffers[wf::counters_id].contents, &before, sizeof(before));
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        require(command != nil && encoder != nil, "Refill range encoding");
        [encoder setComputePipelineState:prepare];
        [encoder setBuffer:arguments offset:0 atIndex:1];
        for (auto buffer : buffers)
            [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        require(metal::complete(command, "wavefront range reservation").empty(),
                "Refill range dispatch");
        wf::Counters result{};
        wf::DispatchArguments dispatch{};
        std::memcpy(&result, buffers[wf::counters_id].contents, sizeof(result));
        std::memcpy(&dispatch, buffers[wf::refill_dispatch_id].contents, sizeof(dispatch));
        const bool valid = n < 6;
        const unsigned expected_count = n == 0 ? 2 : (n == 1 || n == 3) ? 1 : 0;
        require(result.failure == (valid     ? 0u
                                   : n == 11 ? wf::queue_overflow
                                             : wf::invalid_counters),
                "Invalid range reservation must fail before claiming owners");
        require(result.completed == before.completed && result.current == before.current &&
                    result.out == before.out && result.rounds == before.rounds &&
                    result.next_pixel ==
                        (valid ? before.next_pixel + expected_count : before.next_pixel) &&
                    result.free == (valid ? expected_count : before.free) &&
                    result.refill_begin == (valid ? before.next_pixel : before.refill_begin),
                "A single range reservation preserves ownership and completion accounting");
        require(dispatch.x == 1 && dispatch.y == 1 && dispatch.z == 1,
                "Small or empty refill uses one safely masked group");
        for (unsigned i = 0; i < buffers.size(); ++i) {
            const auto written = i == wf::counters_id          ? sizeof(wf::Counters)
                                 : i == wf::config_id          ? sizeof(wf::Config)
                                 : i == wf::refill_dispatch_id ? sizeof(wf::DispatchArguments)
                                                               : 0;
            const auto *bytes = static_cast<const unsigned char *>(buffers[i].contents);
            for (std::size_t j = written; j < buffers[i].length; ++j)
                require(bytes[j] == 0xa5, "Range reservation touched queue storage or a guard");
        }
    }
    std::cout
        << "Wavefront queue corruption, range reservation and sticky overflow checks passed\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            require(argc == 2, "Expected metallib path");
            return run(argv[1]);
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
