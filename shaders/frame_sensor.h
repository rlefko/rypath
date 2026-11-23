// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/packet_wire.h"
#include "../src/render_wire.h"
#include <metal_stdlib>
namespace rypath_frame_sensor {
using namespace metal;
namespace rw = rypath::render_wire;
namespace pw = rypath::packet_wire;
inline float3 cie(device const rw::Point *table, float wavelength, bool interpolate) {
    float x = wavelength - 360.f;
    uint low = uint(interpolate ? floor(x) : floor(wavelength + .5f) - 360.f);
    rw::Point a = table[low];
    float3 value(a.x, a.y, a.z);
    if (interpolate) {
        rw::Point b = table[min(low + 1, 470u)];
        value = mix(value, float3(b.x, b.y, b.z), x - float(low));
    }
    return value;
}
inline bool scalar(const thread rw::Sample &s, uint pixel, uint sample_low, uint sample_high,
                   uint model, device const rw::Point *table, thread float3 &value) {
    bool valid = s.status == rw::success && s.pixel == pixel && s.sample_low == sample_low &&
                 s.sample_high == sample_high && s.wavelength >= 360 && s.wavelength <= 830 &&
                 isfinite(s.radiance) && s.radiance >= 0;
    value = float3(0);
    if (valid)
        value = cie(table, s.wavelength, model == rw::demo_model) * s.radiance *
                (model == rw::pbrt_model ? 470.f : 1.f);
    return valid && all(isfinite(value));
}
inline bool packet(const thread pw::Result &s, uint pixel, uint sample_low, uint sample_high,
                   device const rw::Point *table, thread float3 &value) {
    bool valid = s.status == rw::success && s.pixel == pixel && s.sample_low == sample_low &&
                 s.sample_high == sample_high && s.splat_count == 0 && isfinite(s.filter_weight) &&
                 s.filter_weight > 0;
    bool secondary = false, terminated = false;
    value = float3(0);
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
            value += cie(table, w, false) * (r / pdf / 4.f);
    }
    // A camera sample's filter weight cancels against its own normalization.
    return valid && !(secondary && terminated) && all(isfinite(value));
}
} // namespace rypath_frame_sensor
