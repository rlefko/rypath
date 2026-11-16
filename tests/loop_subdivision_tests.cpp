// SPDX-License-Identifier: GPL-3.0-only
#include "loop_subdivision.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
void require(bool b, std::string_view message) {
    if (!b)
        throw std::runtime_error(std::string(message));
}
void near(float a, float b) { require(std::abs(a - b) < 2e-6f, "numeric mismatch"); }
GeometryMesh triangle() { return {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 1, 2}}}; }
GeometryMesh square() {
    return {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {{0, 1, 2}, {0, 2, 3}}};
}
GeometryMesh tetrahedron() {
    return {{{1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {1, -1, -1}},
            {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}};
}
GeometryMesh fan(unsigned neighbors, bool closed) {
    if (neighbors < 2 || (closed && neighbors < 3))
        throw std::invalid_argument(
            "fan requires at least two boundary or three interior neighbors");
    GeometryMesh mesh;
    mesh.positions.push_back({0, 0, 0});
    for (unsigned i = 0; i < neighbors; ++i) {
        float angle = (closed ? 2.f : 1.f) * std::numbers::pi_v<float> * float(i) /
                      float(closed ? neighbors : neighbors - 1);
        mesh.positions.push_back({std::cos(angle), std::sin(angle), .1f * std::sin(2 * angle)});
    }
    for (unsigned i = 0; i < (closed ? neighbors : neighbors - 1); ++i)
        mesh.triangles.push_back({0, i + 1, (i + 1) % neighbors + 1});
    return mesh;
}
void invalid(GeometryMesh mesh, std::string_view diagnostic) {
    auto result = subdivide_loop(mesh, 0);
    require(!result.mesh && !result.limit_exceeded, "invalid topology accepted or misclassified");
    require(result.diagnostic.find(diagnostic) != std::string::npos, result.diagnostic);
}
void hand_cases() {
    for (int levels : {0, -1, std::numeric_limits<int>::min()}) {
        auto r = subdivide_loop(triangle(), levels);
        require(r.mesh.has_value(), r.diagnostic);
        auto &m = *r.mesh;
        require(m.positions.size() == 3 && m.triangles == triangle().triangles,
                "zero level topology");
        near(m.positions[0].x, .2f);
        near(m.positions[0].y, .2f);
        near(m.positions[1].x, .6f);
        near(m.positions[1].y, .2f);
        near(m.positions[2].x, .2f);
        near(m.positions[2].y, .6f);
        for (auto n : m.normals) {
            near(n.x, 0);
            near(n.y, 0);
            near(n.z, -.32f);
        }
        require(r.scalar_values == 21, "zero level scalar accounting");
    }
    auto r = subdivide_loop(triangle(), 1);
    require(r.mesh.has_value(), r.diagnostic);
    require(
        r.mesh->triangles ==
            std::vector<std::array<std::uint32_t, 3>>{{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}},
        "child/edge ordering");
    near(r.mesh->positions[0].x, .175f);
    near(r.mesh->positions[0].y, .175f);
    near(r.mesh->positions[1].x, .65f);
    near(r.mesh->positions[1].y, .175f);
    near(r.mesh->positions[3].x, .475f);
    near(r.mesh->positions[3].y, .05f);
    near(r.mesh->normals[0].z, -.14875f);
    auto sq = subdivide_loop(square(), 1);
    require(sq.mesh && sq.mesh->positions.size() == 9 && sq.mesh->triangles.size() == 8,
            "shared edge refinement");
    near(sq.mesh->positions[6].x, .5f);
    near(sq.mesh->positions[6].y, .5f);
    auto tet = subdivide_loop(tetrahedron(), 0);
    require(tet.mesh.has_value(), tet.diagnostic);
    auto input = tetrahedron();
    for (unsigned i = 0; i < 4; ++i) {
        near(tet.mesh->positions[i].x, .2f * input.positions[i].x);
        near(tet.mesh->positions[i].y, .2f * input.positions[i].y);
        near(tet.mesh->positions[i].z, .2f * input.positions[i].z);
    }
    for (auto fixture : {triangle(), square(), tetrahedron(), fan(6, true), fan(5, true),
                         fan(2, false), fan(3, false), fan(4, false), fan(7, false)}) {
        for (int levels : {0, 1, 3}) {
            auto a = subdivide_loop(fixture, levels), b = subdivide_loop(fixture, levels);
            require(a.mesh && b.mesh, a.diagnostic);
            require(a.mesh->triangles == b.mesh->triangles, "nondeterministic triangles");
            require(a.mesh->positions.size() == a.mesh->normals.size(), "normal cardinality");
            for (unsigned i = 0; i < a.mesh->positions.size(); ++i) {
                auto p = a.mesh->positions[i], q = b.mesh->positions[i];
                require(p.x == q.x && p.y == q.y && p.z == q.z, "nondeterministic positions");
            }
        }
    }
    auto disconnected = triangle();
    disconnected.positions.insert(disconnected.positions.end(), {{2, 0, 0}, {3, 0, 0}, {2, 1, 0}});
    disconnected.triangles.push_back({3, 4, 5});
    require(subdivide_loop(disconnected, 2).mesh.has_value(), "disconnected manifold components");
}
// Independent binary64 tangent evaluation at each fan's central vertex.
void central_stencils() {
    for (bool closed : {false, true}) {
        for (unsigned n :
             (closed ? std::vector<unsigned>{3, 5, 6, 8} : std::vector<unsigned>{2, 3, 4, 7})) {
            auto input = fan(n, closed);
            auto result = subdivide_loop(input, 0);
            if (!result.mesh.has_value())
                throw std::runtime_error(result.diagnostic);
            const auto &mesh = result.mesh.value();
            double b = closed ? 1. / (n + 3. / (8. * (n == 3 ? 3. / 16. : 3. / (8. * n)))) : .2;
            for (unsigned axis = 0; axis < 3; ++axis) {
                auto value = [axis](GeometryPoint p) {
                    return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
                };
                double expected = 0;
                if (closed)
                    for (unsigned j = 1; j <= n; ++j)
                        expected += b * value(input.positions[j]);
                else
                    expected = b * (value(input.positions[1]) + value(input.positions[n]));
                near(value(mesh.positions[0]), float(expected));
            }
            std::array<double, 3> tangent_s{}, tangent_t{};
            for (unsigned axis = 0; axis < 3; ++axis) {
                auto value = [axis](GeometryPoint p) {
                    return double(axis == 0 ? p.x : axis == 1 ? p.y : p.z);
                };
                auto ring = [&](unsigned j) {
                    return value(mesh.positions[closed ? n - j : j + 1]);
                };
                double center = value(mesh.positions[0]);
                if (closed) {
                    for (unsigned j = 0; j < n; ++j) {
                        double angle = 2 * std::numbers::pi * j / n;
                        tangent_s[axis] += std::cos(angle) * ring(j);
                        tangent_t[axis] += std::sin(angle) * ring(j);
                    }
                } else {
                    tangent_s[axis] = ring(n - 1) - ring(0);
                    if (n == 2)
                        tangent_t[axis] = ring(0) + ring(1) - 2 * center;
                    else if (n == 3)
                        tangent_t[axis] = ring(1) - center;
                    else if (n == 4)
                        tangent_t[axis] =
                            -ring(0) + 2 * ring(1) + 2 * ring(2) - ring(3) - 2 * center;
                    else {
                        double theta = std::numbers::pi / (n - 1);
                        double sum = std::sin(theta) * (ring(0) + ring(n - 1));
                        for (unsigned j = 1; j + 1 < n; ++j)
                            sum += (2 * std::cos(theta) - 2) * std::sin(j * theta) * ring(j);
                        tangent_t[axis] = -sum;
                    }
                }
            }
            near(mesh.normals[0].x,
                 float(tangent_s[1] * tangent_t[2] - tangent_s[2] * tangent_t[1]));
            near(mesh.normals[0].y,
                 float(tangent_s[2] * tangent_t[0] - tangent_s[0] * tangent_t[2]));
            near(mesh.normals[0].z,
                 float(tangent_s[0] * tangent_t[1] - tangent_s[1] * tangent_t[0]));
        }
    }
}
void failures() {
    auto m = triangle();
    m.triangles[0][2] = 3;
    invalid(m, "out of range");
    m = triangle();
    m.triangles[0][2] = 1;
    invalid(m, "repeated");
    m = triangle();
    m.triangles.push_back({0, 1, 2});
    invalid(m, "duplicate");
    m = triangle();
    m.triangles.push_back({2, 1, 0});
    invalid(m, "duplicate");
    m = triangle();
    m.positions[2] = {2, 0, 0};
    invalid(m, "degenerate");
    m = square();
    m.triangles[1] = {0, 3, 2};
    invalid(m, "winding");
    m = square();
    m.positions.push_back({.5, .5, 1});
    m.triangles.push_back({0, 2, 4});
    invalid(m, "nonmanifold");
    m = triangle();
    m.positions.push_back({-1, 0, 0});
    m.positions.push_back({0, -1, 0});
    m.triangles.push_back({0, 3, 4});
    invalid(m, "fan");
    m = tetrahedron();
    m.positions.push_back({2, 0, 0});
    m.positions.push_back({2, 1, 0});
    m.triangles.push_back({0, 4, 5});
    invalid(m, "fan");
    m = triangle();
    m.positions.push_back({0, 0, 1});
    invalid(m, "unused");
    m = triangle();
    m.positions[1].x = std::numeric_limits<float>::infinity();
    invalid(m, "nonfinite");
    m = triangle();
    m.positions[1].x = std::numeric_limits<float>::quiet_NaN();
    invalid(m, "nonfinite");
    m = triangle();
    m.normals.push_back({0, 0, 1});
    invalid(m, "positions and triangles only");
    m = triangle();
    for (auto &p : m.positions) {
        p.x *= 1e-30f;
        p.y *= 1e-30f;
    }
    require(subdivide_loop(m, 0).mesh.has_value(), "finite zero normals retain existing policy");
    m = triangle();
    float step = std::nextafter(1.f, 2.f) - 1.f;
    m.positions = {{1, 1, 0}, {1 + step, 1, 0}, {1, 1 + 3 * step, 0}};
    invalid(m, "collapsed output triangle");
    m = triangle();
    m.positions[1].x = 1e30f;
    m.positions[2].y = 1e30f;
    invalid(m, "nonfinite output");
    invalid({}, "nonempty");
}
void budgets() {
    auto check = [](LoopSubdivisionLimits limits, int levels) {
        auto result = subdivide_loop(triangle(), levels, limits);
        require(!result.mesh && result.limit_exceeded, "budget failure not classified");
        require(result.work <= limits.work, "reported work exceeds limit");
    };
    LoopSubdivisionLimits limits;
    limits.scalar_values = 20;
    check(limits, 0);
    limits = {};
    limits.scalar_values = 47;
    check(limits, 1);
    limits = {};
    limits.scalar_values = 48;
    require(subdivide_loop(triangle(), 1, limits).mesh.has_value(), "exact scalar limit");
    limits = {};
    limits.scratch_bytes = 1;
    check(limits, 0);
    std::size_t low = 0, high = LoopSubdivisionLimits{}.scratch_bytes;
    while (low < high) {
        auto mid = low + (high - low) / 2;
        limits = {};
        limits.scratch_bytes = mid;
        if (subdivide_loop(triangle(), 1, limits).mesh)
            high = mid;
        else
            low = mid + 1;
    }
    limits = {};
    limits.scratch_bytes = low;
    require(subdivide_loop(triangle(), 1, limits).mesh.has_value(), "exact scratch limit");
    limits.scratch_bytes = low - 1;
    check(limits, 1);
    limits = {};
    limits.work = 0;
    check(limits, 0);
    auto reference = subdivide_loop(triangle(), 1);
    limits = {};
    limits.work = reference.work - 1;
    check(limits, 1);
    limits.work = reference.work;
    require(subdivide_loop(triangle(), 1, limits).mesh.has_value(), "exact work limit");
    check({}, std::numeric_limits<int>::max());
    limits = {};
    limits.scalar_values = limits.scratch_bytes = limits.work =
        std::numeric_limits<std::size_t>::max();
    check(limits, std::numeric_limits<int>::max());
}
} // namespace
int main() {
    try {
        hand_cases();
        central_stencils();
        failures();
        budgets();
        std::cout << "Loop subdivision tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
