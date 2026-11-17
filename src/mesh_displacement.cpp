// SPDX-License-Identifier: GPL-3.0-only
// Refinement order and normal construction adapted from PBRT v4 util/mesh.h,
// Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// PBRT adaptations are licensed Apache-2.0; see third_party/pbrt/README.md.
#include "mesh_displacement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <utility>

namespace rypath {
namespace {
GeometryPoint add(GeometryPoint a, GeometryPoint b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
GeometryPoint normalize(GeometryPoint n) {
    const double length = std::hypot(double(n.x), double(n.y), double(n.z));
    if (length == 0) {
        return {};
    }
    return {float(n.x / length), float(n.y / length), float(n.z / length)};
}
bool finite(GeometryPoint p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool equal(GeometryPoint a, GeometryPoint b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
void compute_normals(GeometryMesh &mesh) {
    mesh.normals.assign(mesh.positions.size(), {});
    for (const auto &t : mesh.triangles) {
        const auto p0 = mesh.positions[t[0]], p1 = mesh.positions[t[1]], p2 = mesh.positions[t[2]];
        const std::array<double, 3> a{double(p1.x) - p0.x, double(p1.y) - p0.y,
                                      double(p1.z) - p0.z};
        const std::array<double, 3> b{double(p2.x) - p1.x, double(p2.y) - p1.y,
                                      double(p2.z) - p1.z};
        const double x = a[1] * b[2] - a[2] * b[1];
        const double y = a[2] * b[0] - a[0] * b[2];
        const double z = a[0] * b[1] - a[1] * b[0];
        const double length = std::hypot(x, y, z);
        if (length > 0 && std::isfinite(length)) {
            const GeometryPoint n{float(x / length), float(y / length), float(z / length)};
            for (const auto index : t) {
                mesh.normals[index] = add(mesh.normals[index], n);
            }
        }
    }
    for (auto &normal : mesh.normals) {
        normal = normalize(normal);
    }
}
DisplacementResult failure(std::string diagnostic, bool limit = false, std::size_t work = 0) {
    return {{}, std::move(diagnostic), limit, 0, work};
}
} // namespace

DisplacementResult displace_mesh(const GeometryMesh &source,
                                 const std::array<double, 16> &transform, float edge_length,
                                 const DisplacementEvaluator &evaluate,
                                 const DisplacementLimits &limits) {
    if (!(edge_length > 0) || !std::isfinite(edge_length) || !evaluate) {
        return failure("Displacement requires a positive finite edge length and evaluator");
    }
    if (!std::ranges::all_of(transform, [](double v) { return std::isfinite(v); }) ||
        transform[12] != 0 || transform[13] != 0 || transform[14] != 0 || transform[15] != 1) {
        return failure("Displacement requires a finite affine shape transform");
    }
    if (source.uvs.empty()) {
        return failure("PLY displacement requires authored vertex UVs");
    }
    if (const auto error = validate_geometry({{source}, {}}); !error.empty()) {
        return failure(error);
    }
    for (const auto &t : source.triangles)
        if (!triangle_has_area(source.positions[t[0]], source.positions[t[1]],
                               source.positions[t[2]]))
            return failure("Displacement requires nonzero-area source triangles");
    if (limits.work_per_vertex == 0) {
        return failure("Displacement evaluator work bound must be positive");
    }
    // A deliberately conservative envelope includes both input/output arrays,
    // vector growth, edge-tree nodes, and the bounded DFS stack. The scalar
    // ceiling is checked before every growth operation, not after refinement.
    struct Counts {
        std::size_t vertices, triangles, edges;
    };
    const auto fits = [&](Counts counts) {
        const auto [vertices, triangles, edges] = counts;
        if (vertices > UINT32_MAX || triangles > UINT32_MAX ||
            vertices > limits.scalar_values / 8 ||
            triangles > (limits.scalar_values - vertices * 8) / 3 ||
            vertices > limits.evaluation_work / limits.work_per_vertex) {
            return false;
        }
        const auto units = vertices * 8 + triangles * 3;
        if (units > limits.scratch_bytes / 16 || edges > limits.scratch_bytes / 128) {
            return false;
        }
        return units * 16 <= limits.scratch_bytes - edges * 128;
    };
    const auto original_triangles = source.triangles.size() + 2 * source.patches.size();
    if (!fits({source.positions.size(), original_triangles, 0})) {
        return failure("Displacement geometry, scratch, or evaluation budget exceeded", true);
    }
    std::size_t attempted_work = 0;
    try {
        GeometryMesh output = source;
        output.tangents.clear();
        for (const auto &q : source.patches) {
            output.triangles.push_back({q[0], q[1], q[3]});
            output.triangles.push_back({q[0], q[3], q[2]});
        }
        output.patches.clear();
        if (output.normals.empty()) {
            compute_normals(output);
        }
        auto input_triangles = std::move(output.triangles);
        output.triangles.clear();
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> splits;
        struct Pending {
            std::array<std::uint32_t, 3> vertices;
            unsigned depth;
        };
        std::vector<Pending> pending;
        const auto distance = [&](GeometryPoint a, GeometryPoint b) {
            const double x = double(a.x) - b.x, y = double(a.y) - b.y, z = double(a.z) - b.z;
            return std::hypot(transform[0] * x + transform[1] * y + transform[2] * z,
                              transform[4] * x + transform[5] * y + transform[6] * z,
                              transform[8] * x + transform[9] * y + transform[10] * z);
        };
        for (const auto &triangle : input_triangles) {
            pending.push_back({triangle, 0});
            while (!pending.empty()) {
                const auto current = pending.back();
                pending.pop_back();
                const auto [a, b, c] = current.vertices;
                const double ab = distance(output.positions[a], output.positions[b]);
                const double bc = distance(output.positions[b], output.positions[c]);
                const double ca = distance(output.positions[c], output.positions[a]);
                if (!std::isfinite(ab) || !std::isfinite(bc) || !std::isfinite(ca)) {
                    return failure("Displacement edge metric is nonfinite");
                }
                if (ab < edge_length && bc < edge_length && ca < edge_length) {
                    if (!fits({output.positions.size(), output.triangles.size() + 1,
                               splits.size()})) {
                        return failure("Displacement geometry or scratch budget exceeded", true);
                    }
                    output.triangles.push_back(current.vertices);
                    continue;
                }
                if (current.depth >= limits.refinement_depth) {
                    return failure("Displacement refinement depth limit exceeded", true);
                }
                const std::array<std::uint32_t, 3> v =
                    ab > bc ? (ab > ca ? std::array{a, b, c} : std::array{c, a, b})
                            : (bc > ca ? std::array{b, c, a} : std::array{c, a, b});
                const auto edge = std::minmax(v[0], v[1]);
                auto found = splits.find(edge);
                std::uint32_t middle = 0;
                if (found != splits.end()) {
                    middle = found->second;
                } else {
                    if (!fits({output.positions.size() + 1,
                               std::max(original_triangles, output.triangles.size()),
                               splits.size() + 1})) {
                        return failure("Displacement geometry, scratch, or work budget exceeded",
                                       true);
                    }
                    const auto p0 = output.positions[v[0]], p1 = output.positions[v[1]];
                    const GeometryPoint midpoint{float((double(p0.x) + p1.x) * 0.5),
                                                 float((double(p0.y) + p1.y) * 0.5),
                                                 float((double(p0.z) + p1.z) * 0.5)};
                    if (equal(midpoint, p0) || equal(midpoint, p1)) {
                        return failure("Displacement refinement cannot represent an edge midpoint");
                    }
                    middle = static_cast<std::uint32_t>(output.positions.size());
                    output.positions.push_back(midpoint);
                    output.normals.push_back(
                        normalize(add(output.normals[v[0]], output.normals[v[1]])));
                    const auto uv0 = output.uvs[v[0]], uv1 = output.uvs[v[1]];
                    output.uvs.push_back({float((double(uv0[0]) + uv1[0]) * 0.5),
                                          float((double(uv0[1]) + uv1[1]) * 0.5)});
                    splits.emplace(edge, middle);
                }
                pending.push_back({{middle, v[1], v[2]}, current.depth + 1});
                pending.push_back({{v[0], middle, v[2]}, current.depth + 1});
            }
        }
        for (std::size_t i = 0; i < output.positions.size(); ++i) {
            attempted_work += limits.work_per_vertex;
            const auto d = evaluate(output.positions[i], output.uvs[i]);
            if (!d || !std::isfinite(*d)) {
                return failure("Displacement texture evaluation is invalid", false, attempted_work);
            }
            auto &p = output.positions[i];
            const auto n = output.normals[i];
            p = {p.x + *d * n.x, p.y + *d * n.y, p.z + *d * n.z};
            if (!finite(p)) {
                return failure("Displacement produced a nonfinite vertex", false, attempted_work);
            }
        }
        // True displacement may not silently destroy a source surface, even
        // though ordinary PBRT zero-measure faces can be retained as null slots.
        for (const auto &t : output.triangles)
            if (!triangle_has_area(output.positions[t[0]], output.positions[t[1]],
                                   output.positions[t[2]]))
                return failure("Displacement collapsed a triangle to zero area", false,
                               attempted_work);
        compute_normals(output);
        if (const auto error = validate_geometry({{output}, {}}); !error.empty()) {
            return failure("Displaced " + error, false, attempted_work);
        }
        const auto values = output.positions.size() * 8 + output.triangles.size() * 3;
        const auto work = output.positions.size() * limits.work_per_vertex;
        return {std::move(output), {}, false, values, work};
    } catch (const std::bad_alloc &) {
        return failure("Cannot allocate displaced mesh", true, attempted_work);
    } catch (const std::length_error &) {
        return failure("Displaced mesh exceeds container limits", true, attempted_work);
    }
}
} // namespace rypath
