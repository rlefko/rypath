// SPDX-License-Identifier: Apache-2.0
// Camera differential search, CoordinateSystem, and triangle partials adapted from pbrt-v4.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// License text: third_party/pbrt/LICENSE.txt.

#include "texture_footprint.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace rypath {
namespace {

using Vector = std::array<double, 3>;
using FloatVector = std::array<float, 3>;
using UV = std::array<float, 2>;

bool representable(double value) { return std::abs(value) <= std::numeric_limits<float>::max(); }

Vector vector(GeometryPoint point) { return {point.x, point.y, point.z}; }

// Call only after checking range, or for bounded unit-frame values.
GeometryPoint narrow(const Vector &value) {
    return {static_cast<float>(value[0]), static_cast<float>(value[1]),
            static_cast<float>(value[2])};
}

std::optional<GeometryPoint> pack(const Vector &value) {
    if (!std::ranges::all_of(value, representable)) {
        return std::nullopt;
    }
    return narrow(value);
}

template <typename T, std::size_t N>
std::array<T, N> difference(const std::array<T, N> &a, const std::array<T, N> &b) {
    std::array<T, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        result[i] = a[i] - b[i];
    }
    return result;
}

template <typename T> T difference_of_products(T a, T b, T c, T d) {
    const T cd = c * d;
    return std::fma(a, b, -cd) + std::fma(-c, d, cd);
}

template <typename T> std::array<T, 3> cross(const std::array<T, 3> &a, const std::array<T, 3> &b) {
    return {difference_of_products(a[1], b[2], a[2], b[1]),
            difference_of_products(a[2], b[0], a[0], b[2]),
            difference_of_products(a[0], b[1], a[1], b[0])};
}

float squared_length(const FloatVector &v) {
    const float xx = v[0] * v[0], yy = v[1] * v[1], zz = v[2] * v[2];
    return (xx + yy) + zz;
}

bool finite_float(float value) { return std::isfinite(value); }

double dot(const Vector &a, const Vector &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

std::optional<Vector> normalize(Vector v) {
    const double length = std::hypot(v[0], v[1], v[2]);
    if (!(length > 0 && std::isfinite(length))) {
        return std::nullopt;
    }
    for (auto &component : v) {
        component /= length;
    }
    return v;
}

std::array<Vector, 2> coordinate_system(const Vector &z) {
    const double sign = std::copysign(1., z[2]);
    const double a = -1 / (sign + z[2]);
    const double b = z[0] * z[1] * a;
    return {
        {{1 + sign * z[0] * z[0] * a, sign * b, -sign * z[0]}, {b, sign + z[1] * z[1] * a, -z[1]}}};
}

std::optional<GeometryPoint> apply_transform(const std::array<double, 16> &m, GeometryPoint v,
                                             bool point) {
    Vector result{};
    for (std::size_t row = 0; row < result.size(); ++row) {
        const auto *r = m.data() + 4 * row;
        result[row] = r[0] * v.x + r[1] * v.y + r[2] * v.z + (point ? r[3] : 0);
    }
    return pack(result);
}

struct DirectionMinimum {
    Vector delta{};
    double squared_length = std::numeric_limits<double>::infinity();

    void consider(const Vector &candidate) {
        const double distance = dot(candidate, candidate);
        if (distance < squared_length) {
            squared_length = distance;
            delta = candidate;
        }
    }
};

std::optional<Vector> raster_direction(const Vector &corner, const Vector &dx, const Vector &dy,
                                       double x, double y) {
    Vector direction{};
    for (std::size_t i = 0; i < direction.size(); ++i) {
        direction[i] = std::fma(y, dy[i], std::fma(x, dx[i], corner[i]));
    }
    if (!std::ranges::all_of(direction, representable)) {
        return std::nullopt;
    }
    return normalize(direction);
}

std::optional<TextureTriangleFrame> shape_frame(const std::array<FloatVector, 3> &p,
                                                const std::array<UV, 3> &uv) {
    // PBRT branches on binary32 UV differences, the determinant, and the
    // squared tangent cross length. Widening the UV differences can hide a
    // singular parameterization and produce large, nearly parallel tangents.
    const UV duv02 = difference(uv[0], uv[2]), duv12 = difference(uv[1], uv[2]);
    const FloatVector dp02 = difference(p[0], p[2]), dp12 = difference(p[1], p[2]);
    const float det = difference_of_products(duv02[0], duv12[1], duv02[1], duv12[0]);
    if (!finite_float(det) || !std::ranges::all_of(dp02, finite_float) ||
        !std::ranges::all_of(dp12, finite_float)) {
        return std::nullopt;
    }

    if (std::abs(det) >= 1e-9f) {
        const float inv_det = 1.0f / det;
        FloatVector dpdu{}, dpdv{};
        for (std::size_t i = 0; i < dpdu.size(); ++i) {
            dpdu[i] = difference_of_products(duv12[1], dp02[i], duv02[1], dp12[i]) * inv_det;
            dpdv[i] = difference_of_products(duv02[0], dp12[i], duv12[0], dp02[i]) * inv_det;
        }
        if (!std::ranges::all_of(dpdu, finite_float) || !std::ranges::all_of(dpdv, finite_float)) {
            return std::nullopt;
        }
        const auto area = cross(dpdu, dpdv);
        if (!std::ranges::all_of(area, finite_float)) {
            return std::nullopt;
        }
        if (squared_length(area) != 0.0f) {
            return TextureTriangleFrame{{dpdu[0], dpdu[1], dpdu[2]}, {dpdv[0], dpdv[1], dpdv[2]}};
        }
    }

    // Build the fallback from the negative shape-space winding. Widen the
    // rounded edges to normalize even when their binary32 cross length collapses.
    const FloatVector e20 = difference(p[2], p[0]), e10 = difference(p[1], p[0]);
    const auto n = normalize(cross(Vector{e20[0], e20[1], e20[2]}, Vector{e10[0], e10[1], e10[2]}));
    if (!n) {
        return std::nullopt;
    }
    const auto [dpdu, dpdv] = coordinate_system(*n);
    return TextureTriangleFrame{narrow(dpdu), narrow(dpdv)};
}

} // namespace

std::string prepare_texture_camera(const std::array<double, 9> &world_from_camera,
                                   TextureCamera &camera) {
    if (!std::ranges::all_of(world_from_camera, representable)) {
        return "Texture camera transform exceeds binary32 range";
    }
    const auto &[a, b, c, d, e, f, g, h, i] = world_from_camera;
    const Vector row0{a, b, c}, row1{d, e, f}, row2{g, h, i};
    const std::array cofactors{cross(row1, row2), cross(row2, row0), cross(row0, row1)};
    const double determinant = dot(row0, cofactors[0]);
    if (determinant == 0) {
        return "Texture camera transform is singular";
    }

    TextureCamera prepared;
    for (std::size_t column = 0; column < 3; ++column) {
        Vector inverse_column = cofactors[column];
        for (auto &component : inverse_column) {
            component /= determinant;
        }
        if (!std::ranges::all_of(inverse_column, representable)) {
            return "Inverse texture camera transform exceeds binary32 range";
        }
        for (std::size_t row = 0; row < 3; ++row) {
            const auto index = 3 * row + column;
            prepared.world_from_camera[index] = static_cast<float>(world_from_camera[index]);
            prepared.camera_from_world[index] = static_cast<float>(inverse_column[row]);
        }
    }
    camera = prepared;
    return {};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::string prepare_texture_camera_differentials(GeometryPoint corner, GeometryPoint dx,
                                                 GeometryPoint dy, std::uint32_t width,
                                                 std::uint32_t height,
                                                 TextureCameraDifferentials &differentials) {
    if (width == 0 || height == 0) {
        return "Texture camera raster extent must be positive";
    }
    const Vector c = vector(corner), u = vector(dx), v = vector(dy);
    const double pixel_x = 1. / width, pixel_y = 1. / height;
    DirectionMinimum x_min, y_min;
    constexpr unsigned diagonal_samples = 512;
    for (unsigned i = 0; i < diagonal_samples; ++i) {
        const double t = static_cast<double>(i) / (diagonal_samples - 1);
        const auto direction = raster_direction(c, u, v, t, t);
        const auto x_ray = raster_direction(c, u, v, t + pixel_x, t);
        const auto y_ray = raster_direction(c, u, v, t, t + pixel_y);
        if (!direction || !x_ray || !y_ray) {
            return "Texture camera differential ray is unrepresentable or zero";
        }
        const auto basis = coordinate_system(*direction);
        const auto local = [&](const Vector &ray) {
            return Vector{dot(basis[0], ray), dot(basis[1], ray), dot(*direction, ray)};
        };
        const auto base = local(*direction);
        const auto x = normalize(local(*x_ray));
        const auto y = normalize(local(*y_ray));
        if (!x || !y) {
            return "Texture camera differential ray collapses in its local frame";
        }
        x_min.consider(difference(*x, base));
        y_min.consider(difference(*y, base));
    }
    differentials = {narrow(x_min.delta), narrow(y_min.delta)};
    return {};
}

std::string prepare_texture_triangle_frames(const GeometryMesh &mesh,
                                            // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                            const std::array<double, 16> &shape_transform,
                                            const std::array<double, 16> &placement_transform,
                                            std::vector<TextureTriangleFrame> &frames,
                                            const std::array<double, 3> &reference_origin) {
    frames.clear();
    if (!mesh.uvs.empty() && mesh.uvs.size() != mesh.positions.size()) {
        return "Triangle texture coordinate count must match the mesh vertex count";
    }
    std::vector<TextureTriangleFrame> prepared;
    prepared.reserve(mesh.triangles.size());
    auto relative_shape = shape_transform;
    for (std::size_t row = 0; row < reference_origin.size(); ++row) {
        relative_shape[4 * row + 3] -= reference_origin[row];
    }
    // PBRT stores object definition vertices in this same camera-relative
    // frame. Conjugating the placement by a translation leaves its linear
    // action on the resulting tangents unchanged.
    for (const auto &triangle : mesh.triangles) {
        std::array<FloatVector, 3> p{};
        std::array<UV, 3> uv{{{0, 0}, {1, 0}, {1, 1}}};
        for (std::size_t i = 0; i < triangle.size(); ++i) {
            if (triangle[i] >= mesh.positions.size()) {
                return "Triangle texture frame vertex index is out of range";
            }
            const auto point = apply_transform(relative_shape, mesh.positions[triangle[i]], true);
            if (!point) {
                return "Shape-transformed texture frame position exceeds binary32 range";
            }
            p[i] = {point->x, point->y, point->z};
            if (!mesh.uvs.empty()) {
                uv[i] = mesh.uvs[triangle[i]];
            }
        }
        const auto frame = shape_frame(p, uv);
        if (!frame) {
            return "Shape-space triangle texture frame is degenerate or exceeds binary32 range";
        }
        const auto dpdu = apply_transform(placement_transform, frame->dpdu, false);
        const auto dpdv = apply_transform(placement_transform, frame->dpdv, false);
        if (!dpdu || !dpdv) {
            return "Placement-transformed triangle texture frame exceeds binary32 range";
        }
        prepared.push_back({*dpdu, *dpdv});
    }
    frames = std::move(prepared);
    return {};
}

} // namespace rypath
