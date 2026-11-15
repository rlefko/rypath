// SPDX-License-Identifier: GPL-3.0-only
#include "image_texture.h"
using namespace metal;
kernel void rypath_unbounded_spectrum_probe(const device float4 *queries [[buffer(0)]],
                                            const device float *table [[buffer(1)]],
                                            device float *results [[buffer(2)]],
                                            constant uint &count [[buffer(3)]],
                                            uint id [[thread_position_in_grid]]) {
    if (id >= count)
        return;
    results[id] = rypath_image::unbounded_spectrum(queries[id].xyz, queries[id].w, table);
}
