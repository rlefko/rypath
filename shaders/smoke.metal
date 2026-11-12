// SPDX-License-Identifier: GPL-3.0-only

#include <metal_stdlib>

kernel void rypath_smoke(device const uint *input [[buffer(0)]], device uint *output [[buffer(1)]],
                         constant uint &logical_count [[buffer(2)]],
                         uint index [[thread_position_in_grid]]) {
    if (index < logical_count) {
        output[index] = 3u * input[index] + 17u + index;
    }
}
