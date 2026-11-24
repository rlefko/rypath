// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace rypath {
// Experimental RGB edge weighting; paired preserves the existing denominator.
enum class SpatialVariancePolicy { paired = 0, center = 1, conservative = 2 };
} // namespace rypath
