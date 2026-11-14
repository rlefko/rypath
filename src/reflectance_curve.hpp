// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "spectral_knot.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rypath {

enum class ReflectanceCurveError { none, invalid_knots, limit_exceeded };

struct ReflectanceCurveResult {
    std::vector<SpectralKnot> knots;
    ReflectanceCurveError error = ReflectanceCurveError::none;
};

// Compile clamp(raw(lambda), 0, 1) for binary32 path wavelengths in [360, 830].
// Preserve authored support and knots; crossing neighbors keep sub-ULP transitions
// from changing any representable path sample. Interpolation between those neighbors
// (or outside the visible range) is not an exact raw-spectrum query interface.
// Failure returns no partial curve. max_knots bounds retained output and allocation.
ReflectanceCurveResult compile_reflectance_curve(std::span<const SpectralKnot> source,
                                                 std::size_t max_knots);

} // namespace rypath
