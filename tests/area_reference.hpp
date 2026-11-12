// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>

namespace area_reference {

struct Moments {
    std::array<double, 3> mean{};
    std::array<double, 3> second{};
};

struct AngularMoments {
    double mean = 0;
    double variance = 0;
    double duffy_mean = 0;
};

constexpr std::array<float, 3> reflectance{0.25F, 0.875F, 0.5F};
constexpr std::array<float, 3> wavelengths{360, 525.5F, 830};
double emission(int wavelength_nm);
AngularMoments angular_moments();
Moments xyz_moments(AngularMoments angular);

} // namespace area_reference
