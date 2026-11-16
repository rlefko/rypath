// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "geometry.hpp"
#include <cstddef>
#include <optional>
#include <string>
namespace rypath {
// Limits cover the final scalar payload, converter-owned live vector payloads
// (excluding the caller-owned control mesh), and conservative topology/stencil work.
struct LoopSubdivisionLimits {
    std::size_t scalar_values = 2'000'000;
    std::size_t scratch_bytes = 256 * 1024 * 1024;
    std::size_t work = 64 * 1024 * 1024;
};
struct LoopSubdivisionResult {
    std::optional<GeometryMesh> mesh;
    std::string diagnostic;
    bool limit_exceeded = false;
    std::size_t scalar_values = 0;
    std::size_t work = 0;
};
// Positions/triangles only. Nonpositive levels still project to the limit surface.
// Normals retain PBRT's raw Cross(S,T) magnitude and orientation.
LoopSubdivisionResult subdivide_loop(const GeometryMesh &control, int levels,
                                     const LoopSubdivisionLimits &limits = {});
} // namespace rypath
