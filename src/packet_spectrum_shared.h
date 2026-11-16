// SPDX-License-Identifier: Apache-2.0
// Four-wavelength sampling adapted from PBRT v4 b0bdf461.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt.
#pragma once
#ifdef __METAL_VERSION__
#include <metal_stdlib>
namespace rypath_packet_spectrum {
using namespace metal;
#else
#include <cmath>
namespace rypath_packet_spectrum {
using std::atanh;
using std::cosh;
#endif
struct Wavelengths {
    float lambda[4], pdf[4];
    bool secondary_terminated() const { return pdf[1] == 0 && pdf[2] == 0 && pdf[3] == 0; }
    void terminate_secondary() {
        if (secondary_terminated())
            return;
        for (unsigned i = 1; i < 4; ++i)
            pdf[i] = 0;
        pdf[0] *= .25f;
    }
};
inline float visible_pdf(float lambda) {
    if (lambda < 360 || lambda > 830)
        return 0;
    float c = cosh(.0072f * (lambda - 538));
    return .0039398042f / (c * c);
}
inline Wavelengths sample_visible(float u) {
    Wavelengths w{};
    if (!(u >= 0 && u < 1)) {
        for (unsigned i = 0; i < 4; ++i) {
            w.lambda[i] = NAN;
            w.pdf[i] = NAN;
        }
        return w;
    }
    for (unsigned i = 0; i < 4; ++i) {
        float up = u + float(i) * .25f;
        if (up > 1)
            up -= 1;
        w.lambda[i] = 538 - 138.888889f * atanh(.85691062f - 1.82750197f * up);
        // The inverse CDF is mathematically supported on [360,830]. Rounded
        // fitted constants and binary32 atanh can overshoot an endpoint by an
        // ULP (notably up==1 after the stratified shift). Project this valid
        // CDF result onto its support, then evaluate its wavelength density.
        // Invalid original uniforms remain NaN above and are never repaired.
        if (w.lambda[i] < 360)
            w.lambda[i] = 360;
        if (w.lambda[i] > 830)
            w.lambda[i] = 830;
        w.pdf[i] = visible_pdf(w.lambda[i]);
    }
    return w;
}
} // namespace rypath_packet_spectrum
