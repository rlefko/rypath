// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <iostream>
namespace {
void metal_error(const char *operation, NSError *error) {
    std::cerr << operation << ": "
              << (error ? error.description.UTF8String : "Metal returned no NSError") << '\n';
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) {
            std::cerr << "Expected one packet scattering metallib argument; received " << argc - 1
                      << '\n';
            return 2;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::cerr << "No Metal device\n";
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            return 77;
#else
            return 1;
#endif
        }
        const auto maximum_group = device.maxThreadsPerThreadgroup;
        std::cout << "Packet scattering device: " << device.name.UTF8String << "; maximum group "
                  << maximum_group.width << 'x' << maximum_group.height << 'x'
                  << maximum_group.depth << "; library: " << argv[1] << std::endl;
        auto path = [NSString stringWithUTF8String:argv[1]];
        if (!path) {
            std::cerr << "Packet scattering metallib path is not valid UTF-8\n";
            return 2;
        }
        NSError *error = nil;
        auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
        if (!library) {
            metal_error("Packet scattering library load failed", error);
            return 2;
        }
        auto function = [library newFunctionWithName:@"packet_scattering_probe"];
        if (!function) {
            std::cerr << "Metal library has no packet_scattering_probe function\n";
            return 2;
        }
        error = nil;
        auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            metal_error("Packet scattering pipeline creation failed", error);
            return 2;
        }
        const auto maximum_threads = pipeline.maxTotalThreadsPerThreadgroup;
        const auto group_width = std::min(NSUInteger{32}, maximum_threads);
        std::cout << "Packet scattering pipeline: execution width " << pipeline.threadExecutionWidth
                  << ", maximum threads " << maximum_threads
                  << "; dispatch 1025 threads, group width " << group_width << std::endl;
        if (group_width == 0) {
            std::cerr << "Packet scattering pipeline reports zero maximum threads\n";
            return 3;
        }
        constexpr std::size_t output_values = std::size_t{1024} * 13 * 4;
        auto output = [device newBufferWithLength:(output_values + 4) * sizeof(float)
                                          options:MTLResourceStorageModeShared];
        if (!output) {
            std::cerr << "Packet scattering output allocation failed: requested "
                      << (output_values + 4) * sizeof(float) << " bytes; device maximum "
                      << device.maxBufferLength << '\n';
            return 3;
        }
        auto output_data = static_cast<float *>(output.contents);
        if (!output_data) {
            std::cerr << "Packet scattering shared output buffer has no CPU mapping\n";
            return 3;
        }
        auto guard = output_data + output_values;
        guard[0] = 12345;
        std::array<unsigned, 72> table{};
        table[0] = 1;
        table[1] = 3;
        table[2] = 1;
        table[3] = 1;
        table[4] = std::bit_cast<unsigned>(1.5f);
        table[5] = 32;
        table[6] = 35;
        table[7] = 44;
        table[8] = 62;
        for (unsigned i = 0; i < 3; ++i)
            table[32 + i] = std::bit_cast<unsigned>(float(i) - 1);
        for (unsigned o = 0; o < 3; ++o)
            for (unsigned i = 0; i < 3; ++i) {
                table[35 + o * 3 + i] = std::bit_cast<unsigned>(float(i));
                table[44 + 2 * (o * 3 + i)] = 0;
                table[45 + 2 * (o * 3 + i)] = 1;
            }
        table[62] = std::bit_cast<unsigned>(1.f);
        auto table_buffer = [device newBufferWithBytes:table.data()
                                                length:sizeof(table)
                                               options:MTLResourceStorageModeShared];
        if (!table_buffer) {
            std::cerr << "Packet scattering table allocation failed: requested " << sizeof(table)
                      << " bytes; device maximum " << device.maxBufferLength << '\n';
            return 3;
        }
        auto queue = [device newCommandQueue];
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (!queue || !command || !encoder) {
            std::cerr << "Packet scattering command allocation failed: queue " << (queue != nil)
                      << ", command " << (command != nil) << ", encoder " << (encoder != nil)
                      << '\n';
            return 3;
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBuffer:table_buffer offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(1025, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            std::cerr << "Packet scattering command status "
                      << static_cast<unsigned long>(command.status) << '\n';
            metal_error("Packet scattering GPU execution failed", command.error);
            return 3;
        }
        if (guard[0] != 12345) {
            std::cerr << "Packet scattering guard overwritten at byte offset "
                      << output_values * sizeof(float) << ": expected 12345, observed " << guard[0]
                      << '\n';
            return 3;
        }
        const float *p = static_cast<const float *>(output.contents);
        if (!p) {
            std::cerr << "Packet scattering output has no CPU mapping after GPU completion\n";
            return 3;
        }
        float worst[13] = {};
        for (unsigned n = 0; n < 1024; ++n)
            for (unsigned c = 0; c < 13; ++c) {
                auto q = p + (std::size_t{n} * 13 + c) * 4;
                worst[c] = std::max(worst[c], q[0]);
                if (c == 11 && !(q[3] > 1e-7f)) {
                    std::cerr
                        << "Coating permutation fixture did not exercise old hero-only density\n";
                    return 1;
                }
                if (!std::isfinite(q[0]) || q[0] > 2e-5f || q[1] != 1 || q[2] != 1) {
                    std::cerr << "Failure " << n << " case " << c << " error " << q[0] << " flags "
                              << q[1] << ',' << q[2] << '\n';
                    return 1;
                }
            }
        std::cout << "1024 packet GPU cases passed, max errors " << worst[0] << ' ' << worst[1]
                  << ' ' << worst[2] << ' ' << worst[3] << ' ' << worst[4] << ' ' << worst[5] << ' '
                  << worst[6] << ' ' << worst[7] << ' ' << worst[8] << ' ' << worst[9] << ' '
                  << worst[10] << ' ' << worst[11] << ' ' << worst[12] << '\n';
    }
}
