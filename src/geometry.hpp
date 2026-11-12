// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace rypath {

struct GeometryPoint {
    float x = 0;
    float y = 0;
    float z = 0;
};

// Row-major object-to-world affine matrix: three rows of four values.
struct GeometryTransform {
    std::array<float, 12> values{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};

struct GeometryMesh {
    std::vector<GeometryPoint> positions;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

struct GeometryInstance {
    std::uint32_t mesh_index = 0;
    GeometryTransform transform;
};

struct GeometryData {
    std::vector<GeometryMesh> meshes;
    std::vector<GeometryInstance> instances;
};

struct GeometryRay {
    GeometryPoint origin;
    GeometryPoint direction;
    float t_min = 0;
    float t_max = std::numeric_limits<float>::infinity();
};

struct GeometryHit {
    bool hit = false;
    std::uint32_t mesh_index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t instance_index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t primitive_index = std::numeric_limits<std::uint32_t>::max();
    float t = std::numeric_limits<float>::infinity();
    std::array<float, 3> barycentric{};
};

// An empty diagnostic means valid. These checks have no platform dependencies.
std::string validate_geometry(const GeometryData &geometry);
std::string validate_geometry_rays(std::span<const GeometryRay> rays);

} // namespace rypath
