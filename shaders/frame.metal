// SPDX-License-Identifier: GPL-3.0-only
#include "../src/frame_wire.h"
#include "../src/packet_wire.h"
#include <metal_stdlib>
using namespace metal;
namespace fw = rypath::frame_wire;
namespace rw = rypath::render_wire;
namespace pw = rypath::packet_wire;
kernel void rypath_frame_contract(device uint *out [[buffer(0)]]) { out[0] = fw::contract; }
kernel void rypath_frame_initialize(constant fw::Parameters &p [[buffer(0)]],
                                    device uchar *scratch [[buffer(1)]],
                                    uint lane [[thread_position_in_grid]]) {
    if (lane >= p.count)
        return;
    uint bytes = p.packet ? sizeof(pw::Result) : sizeof(rw::Sample);
    for (uint i = 0; i < bytes; ++i)
        scratch[lane * p.stride + i] = 255;
    if (lane == 0)
        for (uint i = p.count * p.stride; i < p.scratch_bytes; ++i)
            scratch[i] = 255;
}
#include "frame_sensor.h"
kernel void rypath_frame_resolve(constant fw::Parameters &p [[buffer(0)]],
                                 device const uchar *scratch [[buffer(1)]],
                                 device rw::Point *xyz [[buffer(2)]],
                                 device atomic_uint *status [[buffer(3)]],
                                 device const rw::Point *cie [[buffer(4)]],
                                 uint lane [[thread_position_in_grid]]) {
    if (lane >= p.count)
        return;
    bool valid = true;
    if (lane == 0)
        for (uint i = p.count * p.stride; i < p.scratch_bytes; ++i)
            valid = valid && scratch[i] == 255;
    float3 value(0);
    if (p.packet) {
        const pw::Result sample =
            *reinterpret_cast<device const pw::Result *>(scratch + lane * p.stride);
        valid = rypath_frame_sensor::packet(sample, p.pixel_begin + lane, p.sample_low,
                                            p.sample_high, cie, value) &&
                valid;
    } else {
        const rw::Sample sample = reinterpret_cast<device const rw::Sample *>(scratch)[lane];
        valid = rypath_frame_sensor::scalar(sample, p.pixel_begin + lane, p.sample_low,
                                            p.sample_high, p.model, cie, value) &&
                valid;
    }
    if (!valid) {
        atomic_fetch_or_explicit(status, 1u, memory_order_relaxed);
        return;
    }
    xyz[p.pixel_begin + lane] = {value.x, value.y, value.z};
    atomic_fetch_add_explicit(status + 1, 1u, memory_order_relaxed);
}
