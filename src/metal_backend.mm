// SPDX-License-Identifier: GPL-3.0-only

#include "metal_backend.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <system_error>
#include <vector>

#include <mach-o/dyld.h>

namespace rypath {
namespace {

constexpr std::array<std::uint32_t, 2> smoke_counts{1, 257};
constexpr NSUInteger max_smoke_group_width = 256;
constexpr std::uint32_t sentinel = std::numeric_limits<std::uint32_t>::max();

std::string utf8(NSString *value) {
    const char *characters = value.UTF8String;
    return characters == nullptr ? std::string{} : std::string{characters};
}

MetalDeviceInfo inspect_device(id<MTLDevice> device) {
    return {
        .name = utf8(device.name),
        .architecture = utf8(device.architecture.name),
        .registry_id = device.registryID,
        .unified_memory = static_cast<bool>(device.hasUnifiedMemory),
        .compute_ray_tracing = static_cast<bool>(device.supportsRaytracing),
        .render_ray_tracing = static_cast<bool>(device.supportsRaytracingFromRender),
    };
}

MetalResult no_device() {
    return {
        .status = MetalStatus::no_device,
        .device = {},
        .diagnostic = "No system-default Metal device",
    };
}

std::string with_error(std::string message, NSError *error) {
    if (error != nil) {
        message += " (" + utf8(error.domain) + " " + std::to_string(error.code) +
                   "): " + utf8(error.localizedDescription);
    }
    return message;
}

std::filesystem::path default_library_path(std::string &diagnostic) {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (buffer.empty() || _NSGetExecutablePath(buffer.data(), &size) != 0) {
        diagnostic = "Metal library path lookup failed to locate the executable";
        return {};
    }

    std::error_code error;
    const auto executable = std::filesystem::canonical(buffer.data(), error);
    if (error) {
        diagnostic =
            "Metal library path lookup failed to resolve the executable: " + error.message();
        return {};
    }
    return executable.parent_path() / "rypath.metallib";
}

std::string run_dispatch(id<MTLDevice> device, id<MTLCommandQueue> queue,
                         id<MTLComputePipelineState> pipeline, NSUInteger group_width,
                         std::uint32_t logical_count, MetalResult &result) {
    const NSUInteger group_count = (logical_count + group_width - 1) / group_width;
    // Cover every padded lane and reserve one extra guard even if the grid fits exactly.
    const NSUInteger slot_count = group_count * group_width + 1;
    const NSUInteger byte_count = slot_count * sizeof(std::uint32_t);
    id<MTLBuffer> input_buffer = [device newBufferWithLength:byte_count
                                                     options:MTLResourceStorageModeShared];
    // CPU verification uses a raw pointer after Metal can release its retained resources.
    [[clang::objc_precise_lifetime]] id<MTLBuffer> output_buffer =
        [device newBufferWithLength:byte_count options:MTLResourceStorageModeShared];
    auto *input_data = static_cast<std::uint32_t *>(input_buffer.contents);
    auto *output_data = static_cast<std::uint32_t *>(output_buffer.contents);
    if (input_buffer == nil || output_buffer == nil || input_data == nullptr ||
        output_data == nullptr) {
        return "Metal shared buffer allocation failed";
    }
    const std::span input{input_data, slot_count};
    const std::span output{output_data, slot_count};
    for (std::uint32_t i = 0; i < slot_count; ++i) {
        input[i] = 13u + 5u * i;
    }
    std::ranges::fill(output, sentinel);

    // A regular Metal command buffer strongly retains its encoded resources until completion.
    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (command == nil) {
        return "Metal command buffer creation failed";
    }
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (encoder == nil) {
        return "Metal compute encoder creation failed";
    }
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input_buffer offset:0 atIndex:0];
    [encoder setBuffer:output_buffer offset:0 atIndex:1];
    [encoder setBytes:&logical_count length:sizeof(logical_count) atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(group_count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    const auto status = command.status;
    if (status != MTLCommandBufferStatusCompleted) {
        return with_error("Metal command buffer execution failed with status " +
                              std::to_string(static_cast<NSUInteger>(status)),
                          command.error);
    }

    // The completed command makes shared buffer writes visible to the CPU.
    // For input[i] = 13 + 5i, the shader's 3 * input[i] + 17 + i equals 56 + 16i.
    for (std::uint32_t i = 0; i < logical_count; ++i) {
        const std::uint32_t expected = 56u + 16u * i;
        if (output[i] != expected) {
            return "Metal smoke output mismatch at index " + std::to_string(i) + ": expected " +
                   std::to_string(expected) + ", got " + std::to_string(output[i]);
        }
    }
    for (NSUInteger i = logical_count; i < slot_count; ++i) {
        if (output[i] != sentinel) {
            return "Metal smoke guard mismatch at index " + std::to_string(i) + ": expected " +
                   std::to_string(sentinel) + ", got " + std::to_string(output[i]);
        }
    }
    ++result.dispatch_count;
    result.element_count += logical_count;
    result.guard_count += slot_count - logical_count;
    return {};
}

} // namespace

MetalResult query_metal_device() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return no_device();
        }
        return {.status = MetalStatus::success, .device = inspect_device(device), .diagnostic = {}};
    }
}

MetalResult run_metal_smoke_test(const std::filesystem::path &library_path) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return no_device();
        }
        MetalResult result{.device = inspect_device(device), .diagnostic = {}};
        const auto path =
            library_path.empty() ? default_library_path(result.diagnostic) : library_path;
        if (path.empty()) {
            return result;
        }
        NSURL *url = [NSURL fileURLWithFileSystemRepresentation:path.c_str()
                                                    isDirectory:NO
                                                  relativeToURL:nil];
        if (url == nil) {
            result.diagnostic = "Metal library URL creation failed for " + path.string();
            return result;
        }

        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
        if (library == nil) {
            result.diagnostic = with_error("Metal library load failed for " + path.string(), error);
            return result;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"rypath_smoke"];
        if (function == nil) {
            result.diagnostic = "Metal library function lookup failed for rypath_smoke";
            return result;
        }
        error = nil;
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                                                                                     error:&error];
        if (pipeline == nil) {
            result.diagnostic = with_error("Metal compute pipeline creation failed", error);
            return result;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (queue == nil) {
            result.diagnostic = "Metal command queue creation failed";
            return result;
        }
        const NSUInteger group_width =
            std::min({max_smoke_group_width, pipeline.maxTotalThreadsPerThreadgroup,
                      device.maxThreadsPerThreadgroup.width});
        if (group_width == 0) {
            result.diagnostic = "Metal compute dispatch has a zero threadgroup width limit";
            return result;
        }

        for (const auto count : smoke_counts) {
            result.diagnostic = run_dispatch(device, queue, pipeline, group_width, count, result);
            if (!result.diagnostic.empty()) {
                result.diagnostic += " (" + std::to_string(count) + " logical elements)";
                return result;
            }
        }
        result.status = MetalStatus::success;
        return result;
    }
}

} // namespace rypath
