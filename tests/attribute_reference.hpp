// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace attribute_reference {

inline constexpr float footprint = 1.0F / 32;
inline constexpr float reflectance = 0.625F;
inline constexpr float light_xmin = 0.25F;
inline constexpr float light_xmax = 2.5F;
inline constexpr float light_ymin = -1;
inline constexpr float light_ymax = 1;
inline constexpr float light_z = 0.375F;

struct Moments {
    double mean = 0;
    double second = 0;
    double bsdf_mean = 0;
};

struct QuadratureSteps {
    int raster;
    int emitter;
};

Moments furnace_moments();
// Midpoint quadrature of camera-plane and emitter-area measures.
Moments area_moments(bool affine, QuadratureSteps steps);

} // namespace attribute_reference
