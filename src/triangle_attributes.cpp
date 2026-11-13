// SPDX-License-Identifier: GPL-3.0-only
#include "triangle_attributes.hpp"

#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rypath {
namespace {
using Vector = std::array<double, 3>;

double magnitude(const Vector &v) {
    return std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])});
}

bool finite(GeometryPoint p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

GeometryPoint difference(GeometryPoint a, GeometryPoint b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

GeometryPoint cross(GeometryPoint a, GeometryPoint b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
} // namespace

std::string prepare_shading_normals(const GeometryMesh &mesh, const GeometryTransform &transform,
                                    bool reverse_winding, std::vector<GeometryPoint> &normals) {
    normals.clear();
    if (mesh.normals.empty()) {
        return {};
    }
    if (mesh.normals.size() != mesh.positions.size()) {
        return "Normal count must match the mesh vertex count";
    }
    const auto &m = transform.values;
    if (!std::ranges::all_of(m, [](float value) { return std::isfinite(value); })) {
        return "Normal transform must be finite";
    }
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[4], e = m[5], f = m[6];
    const double g = m[8], h = m[9], i = m[10];
    // C = det(A) A^-T. The existing winding flip removes det(A)'s sign
    // and applies authored reversal, leaving a common positive |det(A)|.
    // Products of binary32 matrix entries fit in binary64 without overflow.
    const std::array<Vector, 3> cofactor{{{e * i - f * h, f * g - d * i, d * h - e * g},
                                          {c * h - b * i, a * i - c * g, b * g - a * h},
                                          {b * f - c * e, c * d - a * f, a * e - b * d}}};
    const auto apply = [&](GeometryPoint n) {
        Vector result{};
        for (std::size_t row = 0; row < 3; ++row) {
            result[row] = cofactor[row][0] * n.x + cofactor[row][1] * n.y + cofactor[row][2] * n.z;
            if (reverse_winding) {
                result[row] = -result[row];
            }
        }
        return result;
    };
    double largest = 0;
    for (const auto normal : mesh.normals) {
        if (!finite(normal)) {
            return "Vertex normals must be finite";
        }
        const auto transformed = apply(normal);
        const double size = magnitude(transformed);
        if (!std::isfinite(size) ||
            (size == 0 && (normal.x != 0 || normal.y != 0 || normal.z != 0))) {
            return "Transformed vertex normal is nonfinite or collapses to zero";
        }
        largest = std::max(largest, size);
    }
    int exponent = 0;
    if (largest > 0) {
        std::frexp(largest, &exponent);
    }
    normals.reserve(mesh.normals.size());
    for (const auto normal : mesh.normals) {
        const auto transformed = apply(normal);
        const GeometryPoint packed{static_cast<float>(std::scalbn(transformed[0], -exponent)),
                                   static_cast<float>(std::scalbn(transformed[1], -exponent)),
                                   static_cast<float>(std::scalbn(transformed[2], -exponent))};
        const std::array components{packed.x, packed.y, packed.z};
        for (std::size_t axis = 0; axis < components.size(); ++axis) {
            if (transformed[axis] != 0 &&
                std::abs(components[axis]) < std::numeric_limits<float>::min()) {
                normals.clear();
                // Even a tiny component can decide the side of an emitter or
                // the outgoing shading hemisphere. Do not lose its sign to FTZ.
                return "Relative vertex normal components exceed the GPU normal range";
            }
        }
        normals.push_back(packed);
    }
    return {};
}

std::string validate_normal_emission(const GeometryMesh &mesh, const GeometryTransform &transform,
                                     bool reverse_winding, bool two_sided,
                                     std::span<const GeometryPoint> normals) {
    if (two_sided || normals.empty()) {
        return {};
    }
    if (normals.size() != mesh.positions.size()) {
        return "Prepared normal count must match the emitter vertex count";
    }
    for (std::size_t primitive = 0; primitive < mesh.triangles.size(); ++primitive) {
        const auto triangle = mesh.triangles[primitive];
        if (std::ranges::any_of(triangle, [&](auto index) { return index >= normals.size(); })) {
            return "Emitter normal index is out of range";
        }
        std::array<GeometryPoint, 3> p{};
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto world = area_world_vertex(mesh.positions[triangle[corner]], transform);
            if (!world) {
                return "Emitter normal plane has an unrepresentable world vertex";
            }
            p[corner] = *world;
        }
        const auto edge1 = difference(p[1], p[0]);
        const auto edge2 = difference(p[2], p[0]);
        const auto plane = cross(edge1, edge2);
        const std::array accurate_plane{double{edge1.y} * edge2.z - double{edge1.z} * edge2.y,
                                        double{edge1.z} * edge2.x - double{edge1.x} * edge2.z,
                                        double{edge1.x} * edge2.y - double{edge1.y} * edge2.x};
        const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        const GeometryPoint unit{plane.x / length, plane.y / length, plane.z / length};
        if (!finite(unit)) {
            return "Emitter normal plane cannot be normalized in binary32";
        }
        const std::array raw_components{plane.x, plane.y, plane.z};
        const std::array unit_components{unit.x, unit.y, unit.z};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (accurate_plane[axis] != 0 &&
                (raw_components[axis] == 0 ||
                 std::signbit(accurate_plane[axis]) != std::signbit(raw_components[axis]))) {
                return "Emitter geometric normal component is lost to binary32 cross cancellation";
            }
            if (raw_components[axis] != 0 &&
                std::abs(unit_components[axis]) < std::numeric_limits<float>::min()) {
                return "Emitter geometric normal component exceeds the GPU normal range";
            }
        }
        bool all_uncertain = true;
        bool exact_axis_tie = true;
        for (const auto index : triangle) {
            const auto n = normals[index];
            const std::array terms{double{unit.x} * n.x, double{unit.y} * n.y,
                                   double{unit.z} * n.z};
            const double alignment = terms[0] + terms[1] + terms[2];
            const double sum = std::abs(terms[0]) + std::abs(terms[1]) + std::abs(terms[2]);
            // Account conservatively for binary32 dot/normalization rounding
            // and flushing of subnormal products. Isolated crossing lines are
            // allowed; an entire plane of uncertain sidedness is not.
            const double uncertainty = 8 * std::numeric_limits<float>::epsilon() * sum +
                                       4 * double{std::numeric_limits<float>::min()};
            all_uncertain &= std::abs(alignment) <= uncertainty;
            exact_axis_tie &= sum == 0;
        }
        if (all_uncertain && (reverse_winding || !exact_axis_tie)) {
            return "One-sided emitter triangle " + std::to_string(primitive) +
                   " has everywhere tangent, zero or numerically indeterminate normals "
                   "that cannot retain PBRT hit/sample orientation";
        }
    }
    return {};
}
} // namespace rypath
