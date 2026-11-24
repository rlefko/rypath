// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#import <Metal/Metal.h>
#include <string>

namespace rypath::metal {
// Inspect the actual unspecialized entry. Metal ignores values supplied for
// unknown constants, so successful pipeline creation alone is insufficient.
inline bool verify_opaque_shadow_capability(id<MTLLibrary> library, NSString *entry,
                                            std::string &diagnostic) {
    auto function = [library newFunctionWithName:entry];
    for (MTLFunctionConstant *value in function.functionConstantsDictionary.allValues)
        if (value.index == 22 && value.type == MTLDataTypeBool)
            return true;
    diagnostic = "Metal opaque shadow capability missing (bool function constant 22) in " +
                 std::string(entry.UTF8String) + "; rebuild the metallib";
    return false;
}
} // namespace rypath::metal
