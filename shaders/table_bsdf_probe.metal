// SPDX-License-Identifier: GPL-3.0-only
#include <metal_stdlib>
using namespace metal;
#include "table_bsdf.h"
kernel void rypath_table_bsdf_probe(const device uint *t [[buffer(0)]],
                                    device float *out [[buffer(1)]],
                                    uint id [[thread_position_in_grid]]) {
    float u = (float(id % 31) + .5f) / 31.f, v = (float(id / 31) + .5f) / 31.f;
    float3 wo = normalize(float3(.2f, .3f, id % 2 == 0 ? 1.f : -1.f));
    float wavelength = 400.f + 75.f * float(id % 5);
    auto s = table_bsdf_sample(t, wo, float2(u, v), wavelength);
    auto e = table_bsdf_evaluate(t, wo, s.wi, wavelength);
    uint p = id * 16;
    out[p] = s.wi.x;
    out[p + 1] = s.wi.y;
    out[p + 2] = s.wi.z;
    out[p + 3] = s.value.x;
    out[p + 4] = s.value.y;
    out[p + 5] = s.value.z;
    out[p + 6] = s.pdf;
    out[p + 7] = s.accepted;
    out[p + 8] = s.valid;
    out[p + 9] = s.rgb;
    out[p + 10] = e.value.x;
    out[p + 11] = e.pdf;
    out[p + 12] = e.valid;
    // Only measured tables have warp descriptors.
    if (t[0] == 2) {
        auto f = tb_warp(t, 16, float2(u, v), .2f, .5f, false);
        auto inv = tb_warp(t, 16, f.p, .2f, .5f, true);
        out[p + 13] = inv.p.x;
        out[p + 14] = inv.p.y;
        out[p + 15] = f.pdf;
    }
}
