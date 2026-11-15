// SPDX-License-Identifier: GPL-3.0-only
#include <metal_stdlib>
using namespace metal;
// Correct symbol, deliberately incompatible returned contracts.
kernel void rypath_checkpoint_contract(device uint *result [[buffer(0)]]) {
    result[0] = 0;
    result[1] = 1;
    result[2] = 1;
    result[3] = 1;
}
