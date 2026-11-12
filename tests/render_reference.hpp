// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "render.hpp"

namespace rypath::test {

struct AnalyticMoments {
    std::array<double, 3> mean{};
    std::array<double, 3> variance{};
};

// Null material selects the directly visible constant environment. A material
// selects the unobstructed Lambertian plane and includes both MIS techniques.
AnalyticMoments analytic_xyz(const SpectralMaterial *floor);

struct ReferenceImage {
    std::array<double, 3> mean{};
    std::uint32_t max_scatterings = 0;
};

ReferenceImage reference_image(const RenderScene &scene, const RenderSettings &settings);

} // namespace rypath::test
