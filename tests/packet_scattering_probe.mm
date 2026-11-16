// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2)
            return 2;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::cerr << "No Metal device\n";
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            return 77;
#else
            return 1;
#endif
        }
        NSError *error = nil;
        auto library = [device
            newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]]
                        error:&error];
        if (!library) {
            std::cerr << error.localizedDescription.UTF8String << '\n';
            return 2;
        }
        auto pipeline = [device
            newComputePipelineStateWithFunction:[library
                                                    newFunctionWithName:@"packet_scattering_probe"]
                                          error:&error];
        if (!pipeline)
            return 2;
        auto output = [device newBufferWithLength:(1024 * 13 * 4 + 4) * sizeof(float)
                                          options:MTLResourceStorageModeShared];
        if (!output)
            return 3;
        auto guard = static_cast<float *>(output.contents) + 1024 * 13 * 4;
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
        if (!table_buffer)
            return 3;
        auto queue = [device newCommandQueue];
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (!queue || !command || !encoder)
            return 3;
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBuffer:table_buffer offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(1025, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted)
            return 3;
        if (guard[0] != 12345)
            return 3;
        const float *p = static_cast<const float *>(output.contents);
        float worst[13] = {};
        for (unsigned n = 0; n < 1024; ++n)
            for (unsigned c = 0; c < 13; ++c) {
                auto q = p + (n * 13 + c) * 4;
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
