// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "metal_backend.hpp"

#import <Metal/Metal.h>

namespace rypath::metal {

MetalDeviceInfo inspect_device(id<MTLDevice> device);
id<MTLLibrary> load_library(id<MTLDevice> device, const std::filesystem::path &path,
                            std::string &diagnostic);
// Always specialize optional function constants, including when no values are
// supplied. Bare function lookup may return an unspecialized function that Metal
// API validation rejects at pipeline creation. Empty values retain shader defaults.
id<MTLFunction> load_function(id<MTLLibrary> library, NSString *name, std::string &diagnostic,
                              MTLFunctionConstantValues *constants = nil);
id<MTLComputePipelineState> load_pipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                          NSString *name, std::string &diagnostic,
                                          id<MTLFunction> intersection = nil,
                                          MTLFunctionConstantValues *constants = nil);
id<MTLFunction> load_patch_intersection(id<MTLLibrary> library, std::string &diagnostic);
id<MTLIntersectionFunctionTable> make_intersection_table(id<MTLComputePipelineState> pipeline,
                                                         id<MTLFunction> intersection,
                                                         std::string &diagnostic);
NSUInteger group_width(id<MTLDevice> device, id<MTLComputePipelineState> pipeline);

// Commit an encoded command and return its completion error, if any.
std::string complete(id<MTLCommandBuffer> command, std::string stage);

} // namespace rypath::metal
