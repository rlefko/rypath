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
float3 frame_cie(device const rw::Point *cie, float wavelength, bool interpolate) {
    float x = wavelength - 360.f;
    uint low = uint(interpolate ? floor(x) : floor(wavelength + .5f) - 360.f);
    rw::Point a = cie[low];
    float3 value(a.x, a.y, a.z);
    if (interpolate) {
        rw::Point b = cie[min(low + 1, 470u)];
        value = mix(value, float3(b.x, b.y, b.z), x - float(low));
    }
    return value;
}
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
        device const pw::Result &s =
            *reinterpret_cast<device const pw::Result *>(scratch + lane * p.stride);
        valid = valid && s.status == rw::success && s.pixel == p.pixel_begin + lane &&
                s.sample_low == p.sample_low && s.sample_high == p.sample_high &&
                s.splat_count == 0 && isfinite(s.filter_weight) && s.filter_weight > 0;
        bool secondary = false, terminated = false;
        for (uint i = 0; i < 4; ++i) {
            float w = s.wavelengths[i], pdf = s.wavelength_pdfs[i], r = s.radiance[i];
            bool good = w >= 360 && w <= 830 && isfinite(pdf) && pdf >= 0 && (i != 0 || pdf > 0) &&
                        isfinite(r) && r >= 0;
            valid = valid && good;
            if (i > 0) {
                secondary = secondary || pdf > 0;
                terminated = terminated || pdf == 0;
            }
            if (good && pdf > 0)
                value += frame_cie(cie, w, false) * (r / pdf / 4.f);
        }
        valid = valid && !(secondary && terminated);
        // One camera sample's reconstruction weight cancels against its own
        // normalization; PT has no light-tracing splats.
    } else {
        device const rw::Sample &s = reinterpret_cast<device const rw::Sample *>(scratch)[lane];
        valid = valid && s.status == rw::success && s.pixel == p.pixel_begin + lane &&
                s.sample_low == p.sample_low && s.sample_high == p.sample_high &&
                s.wavelength >= 360 && s.wavelength <= 830 && isfinite(s.radiance) &&
                s.radiance >= 0;
        if (valid)
            value = frame_cie(cie, s.wavelength, p.model == rw::demo_model) * s.radiance *
                    (p.model == rw::pbrt_model ? 470.f : 1.f);
    }
    valid = valid && all(isfinite(value));
    if (!valid) {
        atomic_fetch_or_explicit(status, 1u, memory_order_relaxed);
        return;
    }
    xyz[p.pixel_begin + lane] = {value.x, value.y, value.z};
    atomic_fetch_add_explicit(status + 1, 1u, memory_order_relaxed);
}
