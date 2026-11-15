// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "geometry.hpp"

#include <functional>
#include <optional>

namespace rypath {

struct DisplacementLimits {
    std::size_t scalar_values = 2'000'000;
    std::size_t scratch_bytes = 256 * 1024 * 1024;
    std::size_t evaluation_work = std::size_t{1} << 24;
    std::size_t work_per_vertex = 1;
    unsigned refinement_depth = 64;
};
struct DisplacementResult {
    std::optional<GeometryMesh> mesh;
    std::string diagnostic;
    bool limit_exceeded = false;
    std::size_t scalar_values = 0;
    std::size_t evaluation_work = 0;
};
using DisplacementEvaluator =
    std::function<std::optional<float>(GeometryPoint, std::array<float, 2>)>;

// The metric is the shape-definition transform, before object placement. The
// evaluator receives undisplaced object-space positions and authored UVs.
DisplacementResult displace_mesh(const GeometryMesh &source,
                                 const std::array<double, 16> &shape_transform, float edge_length,
                                 const DisplacementEvaluator &evaluate,
                                 const DisplacementLimits &limits = {});

} // namespace rypath
