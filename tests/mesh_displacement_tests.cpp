// SPDX-License-Identifier: GPL-3.0-only
#include "mesh_displacement.hpp"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
} // namespace

int run() {
    using namespace rypath;
    constexpr std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    GeometryMesh triangle{
        {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 1, 2}}, {}, {{0, 0}, {1, 0}, {0, 1}}};
    const DisplacementEvaluator zero = [](GeometryPoint, std::array<float, 2>) {
        return std::optional<float>(0);
    };
    auto result = displace_mesh(triangle, identity, 1, zero);
    require(bool(result.mesh), "Refine a flat UV triangle");
    if (result.mesh) {
        require(result.mesh->positions.size() == 6 && result.mesh->triangles.size() == 4,
                "Strict longest-edge refinement shares midpoints");
        for (const auto &n : result.mesh->normals) {
            require(n.x == 0 && n.y == 0 && n.z == 1, "Flat generated normals remain exact");
        }
        require(result.scalar_values == 60 && result.evaluation_work == 6,
                "Returned budgets count refined data and evaluations");
    }
    require(triangle.positions.size() == 3 && triangle.normals.empty(), "Source remains immutable");

    GeometryMesh patch{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}},
                       {},
                       {},
                       {{0, 0}, {1, 0}, {0, 1}, {1, 1}},
                       {{0, 1, 2, 3}}};
    auto displaced =
        displace_mesh(patch, identity, 2, [](GeometryPoint p, std::array<float, 2> uv) {
            if (p.x != uv[0] || p.y != uv[1] || p.z != 0)
                return std::optional<float>{};
            return std::optional<float>(uv[0] + 2 * uv[1]);
        });
    require(bool(displaced.mesh), "Displace an authored UV patch in object coordinates");
    if (displaced.mesh) {
        const auto &mesh = *displaced.mesh;
        require(mesh.patches.empty() &&
                    mesh.triangles ==
                        std::vector<std::array<std::uint32_t, 3>>{{0, 1, 3}, {0, 3, 2}},
                "Displacement uses PBRT's tensor-order quad diagonal");
        const auto expected = std::sqrt(6.0);
        for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
            require(mesh.positions[i].z == patch.uvs[i][0] + 2 * patch.uvs[i][1],
                    "Displacement acts along original object normals");
            require(std::abs(mesh.normals[i].x + 1 / expected) < 1e-6 &&
                        std::abs(mesh.normals[i].y + 2 / expected) < 1e-6 &&
                        std::abs(mesh.normals[i].z - 1 / expected) < 1e-6,
                    "Displaced planar normals match analytic plane");
        }
    }
    auto metric = identity;
    metric[0] = metric[5] = 2;
    metric[3] = 1e20; // Translation cannot change the edge metric.
    auto scaled = displace_mesh(triangle, metric, 1, zero);
    require(scaled.mesh && scaled.mesh->triangles.size() > 4,
            "Shape-definition scale controls refinement independently of translation");
    if (scaled.mesh) {
        const auto &mesh = scaled.mesh.value();
        for (const auto &t : mesh.triangles) {
            for (unsigned edge = 0; edge < 3; ++edge) {
                const auto a = mesh.positions[t[edge]];
                const auto b = mesh.positions[t[(edge + 1) % 3]];
                require(2 * std::hypot(double(a.x) - b.x, double(a.y) - b.y) < 1,
                        "Every refined edge meets the strict transformed bound");
            }
        }
    }
    DisplacementLimits limits;
    limits.refinement_depth = 0;
    require(displace_mesh(triangle, identity, 1, zero, limits).limit_exceeded,
            "Refinement depth exhaustion is explicit");
    limits = {};
    limits.evaluation_work = 5;
    require(displace_mesh(triangle, identity, 1, zero, limits).limit_exceeded,
            "Vertex evaluation work is checked before expansion");
    limits = {};
    limits.scalar_values = 59;
    require(displace_mesh(triangle, identity, 1, zero, limits).limit_exceeded,
            "Final index and attribute values are charged");
    limits.scalar_values = 60;
    require(displace_mesh(triangle, identity, 1, zero, limits).mesh.has_value(),
            "Exact scalar budget succeeds");
    require(!displace_mesh(triangle, identity, 0, zero).mesh, "Zero edge length is invalid");
    require(!displace_mesh(triangle, identity, 1,
                           [](GeometryPoint, std::array<float, 2>) {
                               return std::optional<float>(std::numeric_limits<float>::infinity());
                           })
                 .mesh,
            "Nonfinite texture output fails transactionally");
    std::size_t attempts = 0;
    limits = {};
    limits.work_per_vertex = 17;
    const auto failed_work = displace_mesh(
        triangle, identity, 2,
        [&](GeometryPoint, std::array<float, 2>) -> std::optional<float> {
            return ++attempts == 3 ? std::nullopt : std::optional<float>(0);
        },
        limits);
    require(!failed_work.mesh && failed_work.evaluation_work == 51,
            "Failed evaluations retain all attempted work for the shared scene budget");
    auto missing_uv = triangle;
    missing_uv.uvs.clear();
    require(!displace_mesh(missing_uv, identity, 1, zero).mesh, "Displacement requires vertex UVs");
    triangle.normals = {{0, 0, 1}, {-1, 0, 0}, {0, -1, 0}};
    require(!displace_mesh(triangle, identity, 2,
                           [](GeometryPoint, std::array<float, 2> uv) {
                               return std::optional<float>(uv[0] + uv[1]);
                           })
                 .mesh,
            "Collapsed displaced geometry is rejected");
    return failures == 0 ? 0 : 1;
}

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
