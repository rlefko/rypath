// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../scripts/texture_graph_probe_inputs.hpp"
#include "texture_graph_wire.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace checker_reference {

// Independent direct integration of the normalized tent against the alternating
// unit cells. Split at every cell boundary and the tent apex; the integrand is
// linear on each segment, so each trapezoid is exact in real arithmetic.
inline double axis(float coordinate, float radius) {
    const double center = std::fmod(double(coordinate), 2.0);
    if (radius == 0)
        return std::abs(std::fmod(std::floor(center), 2.0)) == 0 ? 1 : -1;
    if (radius > 32) {
        // Test cases above this bound have even-integer radii: a whole number
        // of periods has identically zero tent average for every phase.
        return 0;
    }
    const double r = radius;
    const double end = center + r;
    double begin = center - r;
    double result = 0;
    while (begin < end) {
        const double next = std::min({end, std::floor(begin) + 1, begin < center ? center : end});
        const double midpoint = (begin + next) * .5;
        const double sign = std::abs(std::fmod(std::floor(midpoint), 2.0)) == 0 ? 1 : -1;
        const auto tent = [&](double t) { return (1 - std::abs(t - center) / r) / r; };
        result += sign * (tent(begin) + tent(next)) * (next - begin) * .5;
        begin = next;
    }
    return result;
}

inline std::vector<std::pair<float, float>> cases() {
    std::vector<std::pair<float, float>> result;
    for (const float radius : {0.f, 1e-6f, .001f, .01f, .02f, .05f, .1f, .5f, .999f, 1.f, 1.001f,
                               2.f, 3.5f, 17.f, 31.5f}) {
        for (const float x :
             {-1048576.f, -3.04970002f, -1.f, -.99999994f, -.5f, -1e-7f, 0.f, 1e-7f, .5f,
              .99999994f, 1.f, 3.00966001f, 3.04970002f, 3.09972f, 1048576.f, 1e30f, -1e30f})
            result.emplace_back(x, radius);
    }
    for (const float radius : {64.f, 1048576.f, 1e10f, std::numeric_limits<float>::max() / 4})
        for (const float x : {-.25f, 0.f, .25f, 3.5f, 1e30f, -1e30f})
            result.emplace_back(x, radius);
    return result;
}

struct PinnedRegression {
    rypath::texture_graph_wire::Context context;
    rypath::texture_graph_wire::Mapping mapping;
    double expected;
    double pbrt;
};

// Exact contexts and graph operands from the unchanged pinned-PBRT graph
// comparison. Expected values come from double piecewise tent integration of
// the binary32 mapped coordinates; pinned values retain the original source's
// cancellation error. Neither list was fitted to renderer output.
inline std::vector<PinnedRegression> pinned_regressions() {
    namespace w = rypath::texture_graph_wire;
    constexpr int indices[] = {38, 58, 74, 89};
    constexpr double expected[] = {.811075709907, .362976722101, .281838691837, .274454666121};
    constexpr double pinned[] = {.811116993, .362892926, .281899244, .274496496};
    std::vector<PinnedRegression> result;
    for (unsigned j = 0; j < 4; ++j) {
        const auto p = graph_probe::query(indices[j]);
        w::Context c{.p = {p.p[0], p.p[1], p.p[2]},
                     .n = {},
                     .dpdx = {p.dx[0], p.dx[1], p.dx[2]},
                     .dpdy = {p.dy[0], p.dy[1], p.dy[2]},
                     .u = p.u,
                     .v = p.v,
                     .dudx = p.dudx,
                     .dvdx = p.dvdx,
                     .dudy = p.dudy,
                     .dvdy = p.dvdy};
        w::Mapping mapping{.kind = j == 1 || j == 2 ? w::spherical : w::spatial};
        for (unsigned axis = 0; axis < 3; ++axis) {
            mapping.texture_from_render[5 * axis] = graph_probe::stretch[axis];
            mapping.texture_from_render[4 * axis + 3] = graph_probe::translate[axis];
        }
        result.push_back({c, mapping, expected[j], pinned[j]});
    }
    return result;
}

} // namespace checker_reference
