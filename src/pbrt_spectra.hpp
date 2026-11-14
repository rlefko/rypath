// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "spectral_knot.hpp"

#include <array>
#include <span>
#include <string_view>

namespace rypath {

// The case-sensitive names from the pinned PBRT v4 registry, in sorted order.
std::span<const std::string_view> pbrt_named_spectrum_names();

// Dense samples at integer wavelengths 360–830 nm, or nullptr for an unknown name.
const std::array<float, 471> *pbrt_named_spectrum(std::string_view name);

// PBRT's prepared piecewise-linear knots, or an empty span for an unknown name.
std::span<const SpectralKnot> pbrt_named_spectrum_knots(std::string_view name);

} // namespace rypath
