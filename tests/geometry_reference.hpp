// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "geometry.hpp"

namespace rypath::test {

struct ReferenceHit {
    std::uint32_t mesh_index;
    std::uint32_t instance_index;
    std::uint32_t primitive_index;
    double t;
    std::array<double, 3> barycentric;
};

struct GeometryErrors {
    double t = 0;
    double barycentric = 0;
    double position = 0;
};

// Input fixtures are validated separately. Return all exact closest ties and
// evaluate only hits strictly between the interval endpoints.
std::vector<ReferenceHit> reference_closest(const GeometryData &geometry, const GeometryRay &ray);

std::string compare_closest(const GeometryData &geometry, const GeometryRay &ray,
                            std::span<const ReferenceHit> expected, const GeometryHit &actual,
                            GeometryErrors &errors);
std::string compare_occlusion(bool expected, std::uint8_t actual);

} // namespace rypath::test
