// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "metal_backend.hpp"

#import <Metal/Metal.h>

namespace rypath::metal {

MetalDeviceInfo inspect_device(id<MTLDevice> device);
id<MTLLibrary> load_library(id<MTLDevice> device, const std::filesystem::path &path,
                            std::string &diagnostic);
id<MTLComputePipelineState> load_pipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                          NSString *name, std::string &diagnostic);
NSUInteger group_width(id<MTLDevice> device, id<MTLComputePipelineState> pipeline);

// Commit an encoded command and return its completion error, if any.
std::string complete(id<MTLCommandBuffer> command, std::string stage);

} // namespace rypath::metal
