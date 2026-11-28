// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
static void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            require(argc == 3, "shared_coating_current_probe METALLIB OUTPUT_JSON");
            auto device = MTLCreateSystemDefaultDevice();
            require(device != nil, "Metal device unavailable");
            NSError *error = nil;
            auto library = [device
                newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]]
                            error:&error];
            require(library != nil, "Library unavailable");
            auto values = [MTLFunctionConstantValues new];
            const bool yes = true, no = false;
            for (unsigned i = 0; i <= 18; ++i)
                [values setConstantValue:&yes type:MTLDataTypeBool atIndex:i];
            [values setConstantValue:&no type:MTLDataTypeBool atIndex:20];
            [values setConstantValue:&yes type:MTLDataTypeBool atIndex:25];
            [values setConstantValue:&yes type:MTLDataTypeBool atIndex:32];
            auto function = [library newFunctionWithName:@"rypath_shared_coating_current_contract"
                                          constantValues:values
                                                   error:&error];
            require(function != nil, "Function specialization failed");
            auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
            if (!pipeline)
                std::cerr << error.localizedDescription.UTF8String << '\n';
            require(pipeline != nil, "Pipeline failed");
            auto output = [device newBufferWithLength:96 * 4 options:MTLResourceStorageModeShared];
            auto queue = [device newCommandQueue];
            require(output && queue, "Allocation failed");
            auto command = [queue commandBuffer];
            auto encoder = [command computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:output offset:0 atIndex:0];
            [encoder dispatchThreads:MTLSizeMake(96, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            require(command.status == MTLCommandBufferStatusCompleted, "Command failed");
            const auto *results = static_cast<const unsigned *>(output.contents);
            unsigned failed = 0;
            std::ofstream file(argv[2]);
            file << "{\"schema\":1,\"case_failure_masks\":[";
            for (unsigned i = 0; i < 96; ++i) {
                file << (i ? "," : "") << results[i];
                if (results[i]) {
                    ++failed;
                    std::cerr << "case " << i << " mask " << results[i] << '\n';
                }
            }
            file << "],\"failed\":" << failed << "}\n";
            file.close();
            require(bool(file), "Output failed");
            require(failed == 0, "Shared coating current/full contract mismatch");
            std::cout << "96 shared coating current/full/frozen/prefix controls passed\n";
            return 0;
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
