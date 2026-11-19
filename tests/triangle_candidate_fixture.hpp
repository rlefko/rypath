// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "geometry.hpp"
#include <bit>
namespace rypath::test::triangle_fixture {
using namespace rypath;
struct Fixture {
    std::uint32_t primitive;
    std::array<std::array<std::uint32_t, 3>, 3> points;
};
// Six-triangle excerpt from mesh_00052.ply, pinned pbrt-v4-scenes 30cf4a0346ae.
// Geometry: Hamza Cheggour, CC-BY-2.0; source and full attribution in
// tests/corpus/pbrt-pavilion.json. Original vertex bits and primitive IDs retained.
constexpr std::array<Fixture, 6> fixtures{{{773,
                                            {{{3227559924u, 1069601425u, 1087797478u},
                                              {3227559924u, 1069517539u, 1089894630u},
                                              {3227559924u, 1069517539u, 1087797478u}}}},
                                           {777,
                                            {{{3230684154u, 1069517539u, 1089894630u},
                                              {3227559924u, 1069517539u, 1087797478u},
                                              {3227559924u, 1069517539u, 1089894630u}}}},
                                           {556,
                                            {{{1089799945u, 1069601425u, 1091288105u},
                                              {1089799945u, 1069601425u, 1092336681u},
                                              {1089799945u, 1069517539u, 1092336681u}}}},
                                           {562,
                                            {{{1089799945u, 1069601425u, 1092336681u},
                                              {1089799945u, 1069601425u, 1091288105u},
                                              {1087702793u, 1069601425u, 1091288105u}}}},
                                           {601,
                                            {{{1089833875u, 1069601425u, 1092336681u},
                                              {1089833875u, 1069517539u, 1091288105u},
                                              {1089833875u, 1069517539u, 1092336681u}}}},
                                           {609,
                                            {{{1089833875u, 1069517539u, 1092336681u},
                                              {1091225034u, 1069517539u, 1091288105u},
                                              {1091225034u, 1069517539u, 1092336681u}}}}}};
inline GeometryPoint point(std::array<std::uint32_t, 3> bits) {
    return {std::bit_cast<float>(bits[0]), std::bit_cast<float>(bits[1]),
            std::bit_cast<float>(bits[2])};
}
inline GeometryData regression() {
    GeometryData g;
    g.meshes.resize(1);
    auto &m = g.meshes[0];
    m.positions.push_back({});
    m.triangles.resize(778, {0, 0, 0});
    for (auto &f : fixtures) {
        auto start = static_cast<std::uint32_t>(m.positions.size());
        for (auto p : f.points)
            m.positions.push_back(point(p));
        m.triangles[f.primitive] = {start, start + 1, start + 2};
    }
    g.instances.emplace_back();
    return g;
}
inline const std::array<GeometryRay, 2> rays{{{point({3212362521u, 1084198909u, 1090046791u}),
                                               point({3205793321u, 3209441262u, 3190355261u})},
                                              {point({3240095961u, 1074780756u, 1092621357u}),
                                               point({1065335849u, 3173765275u, 3163564518u})}}};
} // namespace rypath::test::triangle_fixture
