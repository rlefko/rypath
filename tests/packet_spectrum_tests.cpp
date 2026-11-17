// SPDX-License-Identifier: GPL-3.0-only
#include "packet_spectrum_shared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
namespace {
bool check(float u) {
    using namespace rypath_packet_spectrum;
    auto w = sample_visible(u);
    for (unsigned i = 0; i < 4; ++i) {
        // Retain the pinned binary32 stratum shift (including up rounding
        // exactly to one); evaluate its inverse independently in double.
        float shifted = u + float(i) * .25f;
        if (shifted > 1)
            shifted -= 1;
        const double up = shifted;
        // Independent double precision pinned PBRT inverse CDF, projected onto
        // the exact mathematical visible support at fitted endpoints.
        double lambda =
            std::clamp(538 - 138.888889 * std::atanh(.85691062 - 1.82750197 * up), 360., 830.);
        double c = std::cosh(.0072 * (double(w.lambda[i]) - 538));
        if (!(w.lambda[i] >= 360 && w.lambda[i] <= 830 && w.pdf[i] > 0) ||
            !std::isfinite(w.pdf[i]) || std::abs(w.lambda[i] - lambda) > .001 ||
            std::abs(w.pdf[i] - .0039398042 / (c * c)) > 1e-8)
            return false;
    }
    auto pdf = w.pdf[0];
    w.terminate_secondary();
    w.terminate_secondary();
    return w.secondary_terminated() && w.pdf[0] == pdf * .25f;
}
} // namespace
int main() {
    for (unsigned n = 0; n < 4096; ++n) {
        if (!check((float(n) + .5f) / 4096) || !check(float(n) / 4096))
            return 1;
    }
    for (float u : std::array{0.f, .25f, .5f, .75f, std::nextafter(1.f, 0.f)}) {
        if (!check(u))
            return 2;
        if (u > 0 && !check(std::nextafter(u, 0.f)))
            return 3;
        if (u < .99f && !check(std::nextafter(u, 1.f)))
            return 4;
    }
    for (float invalid : std::array{-1.f, std::nextafter(0.f, -1.f), 1.f, 2.f,
                                    std::numeric_limits<float>::infinity(),
                                    std::numeric_limits<float>::quiet_NaN()}) {
        const auto w = rypath_packet_spectrum::sample_visible(invalid);
        for (unsigned i = 0; i < 4; ++i)
            if (std::isfinite(w.lambda[i]) || std::isfinite(w.pdf[i]))
                return 5;
    }
    std::cout << "8192 packet wavelength cases plus endpoints/invalid uniforms passed\n";
}
