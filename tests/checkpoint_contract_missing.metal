// SPDX-License-Identifier: GPL-3.0-only
#include <metal_stdlib>
using namespace metal;
// Valid metallib without a compatibility entry point.
kernel void unrelated_contract(device uint *result [[buffer(0)]]) { result[0] = 1; }
