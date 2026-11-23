// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "geometry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace rypath {
// Reconstruction-only topology. Never weld equal positions: duplicated vertices,
// disconnected layers and nonmanifold edges intentionally retain separate groups.
inline std::vector<std::uint32_t> reconstruction_continuity(const GeometryMesh &mesh) {
    using V = std::array<double, 3>;
    const auto sub = [](V a, V b) { return V{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
    const auto dot = [](V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto point = [&](std::uint32_t i) {
        const auto p = mesh.positions[i];
        return V{p.x, p.y, p.z};
    };
    struct Face {
        V p{}, n{};
        double scale = 0;
        bool valid = false;
    };
    struct Edge {
        std::uint32_t face, from, to;
    };
    std::vector<Face> faces(mesh.triangles.size());
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<Edge>> edges;
    for (std::uint32_t i = 0; i < mesh.triangles.size(); ++i) {
        const auto t = mesh.triangles[i];
        if (t[0] >= mesh.positions.size() || t[1] >= mesh.positions.size() ||
            t[2] >= mesh.positions.size())
            continue;
        auto &f = faces[i];
        f.p = point(t[0]);
        const V a = sub(point(t[1]), f.p), b = sub(point(t[2]), f.p);
        f.n = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const double length = std::sqrt(dot(f.n, f.n));
        f.scale = std::max(std::sqrt(dot(a, a)), std::sqrt(dot(b, b)));
        if (!(length > 0) || !std::isfinite(length))
            continue;
        for (auto &c : f.n)
            c /= length;
        f.valid = true;
        for (unsigned e = 0; e < 3; ++e) {
            auto from = t[e], to = t[(e + 1) % 3];
            edges[std::minmax(from, to)].push_back({i, from, to});
        }
    }
    std::vector<std::vector<std::uint32_t>> neighbors(faces.size());
    for (const auto &[key, list] : edges) {
        (void)key;
        if (list.size() != 2 || list[0].from != list[1].to || list[0].to != list[1].from)
            continue;
        auto a = list[0].face, b = list[1].face;
        // Coincident duplicate faces sharing all vertices are not a continuous sheet.
        auto ta = mesh.triangles[a], tb = mesh.triangles[b];
        std::sort(ta.begin(), ta.end());
        std::sort(tb.begin(), tb.end());
        if (ta == tb)
            continue;
        neighbors[a].push_back(b);
        neighbors[b].push_back(a);
    }
    std::vector<std::uint32_t> groups(faces.size(), 0xffffffffu), queue;
    for (std::uint32_t seed = 0; seed < faces.size(); ++seed) {
        if (groups[seed] != 0xffffffffu || !faces[seed].valid)
            continue;
        const auto &anchor = faces[seed];
        groups[seed] = seed;
        queue = {seed};
        for (std::size_t q = 0; q < queue.size(); ++q)
            for (auto next : neighbors[queue[q]]) {
                if (groups[next] != 0xffffffffu || dot(anchor.n, faces[next].n) < 1. - 1e-10)
                    continue;
                bool planar = true;
                // Fixed anchor plane and scale: no transitive bending or tolerance growth.
                const double tolerance = std::max(1e-30, anchor.scale * 1e-7);
                for (auto vertex : mesh.triangles[next])
                    planar = planar &&
                             std::abs(dot(sub(point(vertex), anchor.p), anchor.n)) <= tolerance;
                if (planar) {
                    groups[next] = seed;
                    queue.push_back(next);
                }
            }
    }
    return groups;
}
} // namespace rypath
