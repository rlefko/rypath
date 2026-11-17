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

enum class AnalyticKind : std::uint32_t { sphere = 1, disk = 2, cylinder = 3 };

// Object-space PBRT analytic surfaces. Angles are radians, already normalized
// by the scene interpreter; cylinders have no end caps.
struct GeometryAnalytic {
    AnalyticKind kind = AnalyticKind::sphere;
    float radius = 1;
    float z_min = -1;
    float z_max = 1;
    float phi_max = 6.2831853071795864769f;
    float inner_radius = 0;
    float height = 0;
};

struct GeometryMesh {
    std::vector<GeometryPoint> positions;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<GeometryPoint> normals{};
    std::vector<std::array<float, 2>> uvs{};
    // Tensor-product corner order: p00, p10, p01, p11 (cyclic PLY 0,1,3,2).
    std::vector<std::array<std::uint32_t, 4>> patches{};
    std::vector<GeometryPoint> tangents{};
    // Explicit glTF tangent-frame handedness, one +1/-1 per vertex when present.
    std::vector<float> tangent_signs{};
    std::vector<GeometryAnalytic> analytics{};
};

enum class PrimitiveKind : std::uint32_t { triangle = 1, bilinear_patch = 2, analytic = 4 };

struct GeometryInstance {
    std::uint32_t mesh_index = 0;
    GeometryTransform transform;
    bool non_opaque = false;
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
    PrimitiveKind kind = PrimitiveKind::triangle;
    std::array<float, 2> patch_uv{};
    std::array<float, 2> analytic_uv{};
};

// Exact zero-measure classification of finite binary32 vertices. Zero-area
// triangles remain in native geometry with stable primitive IDs and never hit.
bool triangle_has_area(GeometryPoint a, GeometryPoint b, GeometryPoint c);

// Exact singularity of a finite binary32 row-major 3x3 matrix.
bool singular_linear_transform(const std::array<float, 9> &matrix);

// An empty diagnostic means valid. These checks have no platform dependencies.
std::string validate_geometry(const GeometryData &geometry);
std::string validate_geometry_rays(std::span<const GeometryRay> rays);

} // namespace rypath
