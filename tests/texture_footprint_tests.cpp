// SPDX-License-Identifier: GPL-3.0-only

#include "texture_footprint.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace rypath;

constexpr std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}

bool near(GeometryPoint actual, GeometryPoint expected, float tolerance = 1e-7f) {
    return std::abs(actual.x - expected.x) <= tolerance &&
           std::abs(actual.y - expected.y) <= tolerance &&
           std::abs(actual.z - expected.z) <= tolerance;
}

void test_triangle_frames() {
    const std::array<double, 16> shape{0, -1, 0, 2, 1, 0, 0, 3, 0, 0, 1, 4, 0, 0, 0, 1};
    auto placement = identity;
    placement[0] = 4;
    placement[3] = 50;
    GeometryMesh mesh{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 1, 2}}};
    std::vector<TextureTriangleFrame> frames;

    require(prepare_texture_triangle_frames(mesh, shape, placement, frames).empty() &&
                frames.size() == 1 && near(frames[0].dpdu, {0, 1, 0}) &&
                near(frames[0].dpdv, {-4, -1, 0}),
            "PBRT default triangle UVs use (1,1) at the third vertex");

    mesh.uvs = {{{0, 0}, {2, 0}, {0, 4}}};
    require(prepare_texture_triangle_frames(mesh, shape, placement, frames).empty() &&
                frames.size() == 1 && near(frames[0].dpdu, {0, 0.5f, 0}) &&
                near(frames[0].dpdv, {-1, 0, 0}),
            "Authored UVs produce partials transformed through both stages");

    mesh.uvs = {{{0, 0}, {1, 0}, {2, 1e-10f}}};
    require(prepare_texture_triangle_frames(mesh, shape, placement, frames).empty() &&
                frames.size() == 1 && near(frames[0].dpdu, {4, 0, 0}) &&
                near(frames[0].dpdv, {0, -1, 0}),
            "Degenerate UV fallback uses negative shape winding before placement");

    mesh.uvs = {{{0, 0}, {1, 0}, {0, 1}}};
    auto tiny_shape = identity;
    tiny_shape[0] = tiny_shape[5] = 1e-15;
    require(prepare_texture_triangle_frames(mesh, tiny_shape, placement, frames).empty() &&
                frames.size() == 1 && near(frames[0].dpdu, {4, 0, 0}) &&
                near(frames[0].dpdv, {0, -1, 0}),
            "Collapsed squared tangent cross length uses the triangle fallback");

    const double limit = std::numeric_limits<float>::max();
    auto overflowing_shape = identity;
    overflowing_shape[0] = 2 * limit;
    auto downscaled_placement = identity;
    downscaled_placement[0] = 0.25;
    require(!prepare_texture_triangle_frames(mesh, overflowing_shape, downscaled_placement, frames)
                    .empty() &&
                frames.empty(),
            "A finite composed transform cannot hide overflowing shape-space vertices");
    mesh.uvs[1][0] = 0.5f;
    auto overflowing_placement = identity;
    overflowing_placement[0] = limit;
    require(
        !prepare_texture_triangle_frames(mesh, identity, overflowing_placement, frames).empty() &&
            frames.empty(),
        "Placement rejects a tangent that exceeds binary32");
}

void test_triangle_uv_binary32_branch() {
    GeometryMesh mesh{{{1, 0, 2}, {2, 1, 2}, {0, 0, 2}}, {{0, 1, 2}}};
    auto placement = identity;
    placement[0] = 4;
    placement[3] = 50;
    std::vector<TextureTriangleFrame> frames;

    // Binary32 subtraction rounds 1-2e-9 and 2-2e-9 to 1 and 2,
    // leaving identical UV directions. In binary64 the determinant is 2e-9.
    mesh.uvs = {{{1, 1}, {2, 2}, {0, 2e-9f}}};
    require(prepare_texture_triangle_frames(mesh, identity, placement, frames).empty() &&
                frames.size() == 1 && near(frames[0].dpdu, {4, 0, 0}) &&
                near(frames[0].dpdv, {0, -1, 0}),
            "Rounded UV subtraction selects the shape-space fallback");

    mesh.positions = {{0, 0, 2}, {1, 0, 2}, {0, 1, 2}};
    constexpr float threshold = 1e-9f;
    mesh.uvs = {{{0, 0}, {1, 0}, {0, std::nextafter(threshold, 0.0f)}}};
    require(prepare_texture_triangle_frames(mesh, identity, placement, frames).empty() &&
                frames.size() == 1 && near(frames[0].dpdu, {4, 0, 0}) &&
                near(frames[0].dpdv, {0, -1, 0}),
            "Determinant immediately below the PBRT threshold uses the fallback");

    mesh.uvs[2][1] = threshold;
    require(prepare_texture_triangle_frames(mesh, identity, placement, frames).empty() &&
                frames.size() == 1 && near(frames[0].dpdu, {4, 0, 0}) &&
                near(frames[0].dpdv, {0, 1.0f / threshold, 0}),
            "Determinant exactly at the PBRT threshold uses the authored UV tangents");
}

void test_triangle_camera_relative_frames() {
    GeometryMesh mesh{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 1, 2}}};
    auto placement = identity;
    placement[0] = 4;
    placement[1] = 0.5;
    placement[3] = 10;
    auto shape = identity;
    shape[11] = 2;
    constexpr std::array reference_origin{1e8, -1e8, 1e8};
    auto translated_shape = shape;
    for (std::size_t row = 0; row < reference_origin.size(); ++row) {
        translated_shape[4 * row + 3] += reference_origin[row];
    }
    std::vector<TextureTriangleFrame> original, translated;
    require(prepare_texture_triangle_frames(mesh, shape, placement, original).empty() &&
                prepare_texture_triangle_frames(mesh, translated_shape, placement, translated,
                                                reference_origin)
                    .empty() &&
                original.size() == 1 && translated.size() == 1 &&
                near(translated[0].dpdu, original[0].dpdu) &&
                near(translated[0].dpdv, original[0].dpdv),
            "Camera-relative rebasing preserves one-unit triangle edges under a large translation");

    mesh.uvs = {{{0, 0}, {0, 0}, {0, 0}}};
    require(prepare_texture_triangle_frames(mesh, translated_shape, placement, translated,
                                            reference_origin)
                    .empty() &&
                translated.size() == 1 && near(translated[0].dpdu, {4, 0, 0}) &&
                near(translated[0].dpdv, {-0.5f, -1, 0}),
            "Rebased shape-space fallback tangents retain the placement's linear transform");
}

void test_camera() {
    TextureCamera camera;
    const std::array<double, 9> transform{2, 1, 0, 0, 1, 0, 0, 0, -0.5};
    const std::array<float, 9> expected_transform{2, 1, 0, 0, 1, 0, 0, 0, -0.5f};
    const std::array<float, 9> expected_inverse{0.5f, -0.5f, 0, 0, 1, 0, 0, 0, -2};
    require(prepare_texture_camera(transform, camera).empty() &&
                camera.world_from_camera == expected_transform &&
                camera.camera_from_world == expected_inverse,
            "Camera preparation retains and inverts the row-major transform");
    require(!prepare_texture_camera({}, camera).empty(), "Singular camera transform is rejected");
    auto overflow = transform;
    overflow[0] = 1e-40;
    require(!prepare_texture_camera(overflow, camera).empty(),
            "The camera inverse must also fit binary32");

    TextureCameraDifferentials differentials;
    constexpr std::uint32_t extent = 8;
    require(prepare_texture_camera_differentials({-1, -1, 1}, {2, 0, 0}, {0, 2, 0}, extent, extent,
                                                 differentials)
                .empty(),
            "Symmetric square raster supports finite minimum direction differentials");
    // The smallest angular shift occurs at the far diagonal corner, where the
    // shifted ray points along (1.25,1,1). Use the analytic basis at (1,1,1).
    const double a = 1 / std::sqrt(3.);
    const double b = -a * a / (1 + a);
    const double length = std::sqrt(1.25 * 1.25 + 2.);
    const float dx = static_cast<float>(0.25 * (1 + b) / length);
    const float dy = static_cast<float>(0.25 * b / length);
    const float dz = static_cast<float>(3.25 * a / length - 1);
    require(near(differentials.minimum_dx, {dx, dy, dz}) &&
                near(differentials.minimum_dy, {dy, dx, dz}),
            "World-space PBRT frame yields symmetric diagonal minima");
    require(prepare_texture_camera_differentials({-1, -1, -1}, {2, 0, 0}, {0, 2, 0}, extent, extent,
                                                 differentials)
                    .empty() &&
                near(differentials.minimum_dx, {dx, -dy, dz}) &&
                near(differentials.minimum_dy, {dy, -dx, dz}),
            "Mirrored world direction selects PBRT's negative-Z basis");
    const float limit = std::numeric_limits<float>::max();
    require(!prepare_texture_camera_differentials({limit, 0, 1}, {limit, 0, 0}, {0, 2, 0}, extent,
                                                  extent, differentials)
                 .empty(),
            "A shifted world ray outside binary32 is rejected");
}

} // namespace

int main() {
    try {
        test_triangle_frames();
        test_triangle_uv_binary32_branch();
        test_triangle_camera_relative_frames();
        test_camera();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
