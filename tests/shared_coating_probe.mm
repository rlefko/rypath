// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
static void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            require(argc == 3 || argc == 4, "shared_coating_probe METALLIB OUTPUT_PREFIX [MODE]");
            auto device = MTLCreateSystemDefaultDevice();
            require(device != nil, "Metal device unavailable");
            NSError *error = nil;
            auto library = [device
                newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]]
                            error:&error];
            require(library != nil, "Probe library unavailable");
            auto function = [library newFunctionWithName:@"rypath_shared_coating_probe"];
            auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
            if (!pipeline)
                std::cerr << error.localizedDescription.UTF8String << '\n';
            require(pipeline != nil, "Probe pipeline unavailable");
            auto queue = [device newCommandQueue];
            require(queue != nil, "Probe queue unavailable");
            constexpr std::uint32_t count = 8 * 32768;
            auto output = [device newBufferWithLength:std::size_t(count) * 32
                                              options:MTLResourceStorageModeShared];
            auto status = [device newBufferWithLength:std::size_t(count) * 4
                                              options:MTLResourceStorageModeShared];
            require(output && status, "Probe allocation unavailable");
            for (std::uint32_t angle = 0; angle < 2; ++angle)
                for (std::uint32_t black = 0; black < 2; ++black) {
                    const std::array<std::uint32_t, 4> config{
                        count, angle, black,
                        argc == 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 0u};
                    auto command = [queue commandBuffer];
                    auto encoder = [command computeCommandEncoder];
                    [encoder setComputePipelineState:pipeline];
                    [encoder setBuffer:output offset:0 atIndex:0];
                    [encoder setBuffer:status offset:0 atIndex:1];
                    [encoder setBytes:config.data() length:sizeof(config) atIndex:2];
                    [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                        threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
                    [encoder endEncoding];
                    [command commit];
                    [command waitUntilCompleted];
                    if (command.error)
                        std::cerr << command.error.localizedDescription.UTF8String << '\n';
                    require(command.status == MTLCommandBufferStatusCompleted,
                            "Probe command failed");
                    const auto *flags = static_cast<const std::uint32_t *>(status.contents);
                    for (std::uint32_t i = 0; i < count; ++i)
                        require(flags[i] == 0, "Probe invalid sampled walk");
                    const std::string path = std::string(argv[2]) + "-angle" +
                                             std::to_string(angle) + "-black" +
                                             std::to_string(black) + ".bin";
                    std::ofstream file(path, std::ios::binary);
                    file.write(static_cast<const char *>(output.contents), output.length);
                    file.close();
                    require(bool(file), "Probe output failed");
                    std::cout << "Completed " << count << " paired shared/scalar walks for angle "
                              << angle << " black " << black << '\n';
                }
            std::cout << "Device: " << device.name.UTF8String << '\n';
            return 0;
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
