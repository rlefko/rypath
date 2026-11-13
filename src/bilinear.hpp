// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "geometry.hpp"

#include <optional>

namespace rypath {

// Conservative lower and upper bounds on |dP/du x dP/dv| over the unit square,
// accounting for float arithmetic on the stored corners. Empty if the patch
// is singular or regularity cannot be established.
std::optional<std::array<double, 2>>
bilinear_jacobian_bounds(const std::array<GeometryPoint, 4> &corners);

} // namespace rypath
