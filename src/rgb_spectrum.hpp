// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <optional>

namespace rypath {

struct RgbSpectrum {
    bool is_constant = false;
    float constant = 0.0f;
    // Coefficients of the quadratic in descending order, evaluated in nanometers.
    std::array<float, 3> coefficients{};
};

// Reconstruct a bounded spectrum from finite linear sRGB components in [0, 1].
std::optional<RgbSpectrum> reconstruct_srgb(const std::array<float, 3> &rgb);

// Evaluate the continuous reconstruction at a wavelength in [360, 830] nm.
float evaluate_srgb(const RgbSpectrum &spectrum, float wavelength);

// PBRT's luminance-normalized stdillum-D65 sampled at 360, 361, ..., 830 nm.
const std::array<float, 471> &pbrt_d65();

} // namespace rypath
