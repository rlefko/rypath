// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "geometry.hpp"

namespace rypath {

struct TextureCamera {
    std::array<float, 9> world_from_camera{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 9> camera_from_world{1, 0, 0, 0, 1, 0, 0, 0, 1};
};

struct TextureCameraDifferentials {
    GeometryPoint minimum_dx;
    GeometryPoint minimum_dy;
};

struct TextureTriangleFrame {
    GeometryPoint dpdu;
    GeometryPoint dpdv;
};

// Camera matrices are row-major. The input describes the authored camera's linear transform.
std::string prepare_texture_camera(const std::array<double, 9> &world_from_camera,
                                   TextureCamera &camera);

// Directions are in world space, with dx and dy the increments across the full raster extent.
std::string prepare_texture_camera_differentials(GeometryPoint corner, GeometryPoint dx,
                                                 GeometryPoint dy, std::uint32_t width,
                                                 std::uint32_t height,
                                                 TextureCameraDifferentials &differentials);

// Rebase the shape transform by reference_origin before packing its vertices. Compute
// PBRT's triangle tangents in that frame, then apply the placement's linear transform.
// Both transforms are row-major affine matrices.
std::string prepare_texture_triangle_frames(const GeometryMesh &mesh,
                                            const std::array<double, 16> &shape_transform,
                                            const std::array<double, 16> &placement_transform,
                                            std::vector<TextureTriangleFrame> &frames,
                                            const std::array<double, 3> &reference_origin = {});

} // namespace rypath
