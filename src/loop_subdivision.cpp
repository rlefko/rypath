// Loop stencils and tangent rules adapted from PBRT v4 util/loopsubdiv.cpp at
// b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Licensed under the Apache License, Version 2.0; see third_party/pbrt/LICENSE.txt.
// SPDX-License-Identifier: Apache-2.0

#include "loop_subdivision.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace rypath {
namespace {
using Index = std::size_t;
constexpr Index none = std::numeric_limits<Index>::max();
struct Failure {
    std::string diagnostic;
    bool limit = false;
};
Index add(Index a, Index b) {
    if (b > std::numeric_limits<Index>::max() - a)
        throw Failure{"Loop subdivision count overflow", true};
    return a + b;
}
Index mul(Index a, Index b) {
    if (a && b > std::numeric_limits<Index>::max() / a)
        throw Failure{"Loop subdivision count overflow", true};
    return a * b;
}
struct Budget {
    const LoopSubdivisionLimits &limits;
    Index work = 0;
    void charge(Index n) {
        if (n > limits.work - work)
            throw Failure{"Loop subdivision work limit exceeded", true};
        work += n;
    }
    void scratch(Index n) const {
        if (n > limits.scratch_bytes)
            throw Failure{"Loop subdivision scratch limit exceeded", true};
    }
};
GeometryPoint scale(GeometryPoint a, float s) { return {a.x * s, a.y * s, a.z * s}; }
GeometryPoint plus(GeometryPoint a, GeometryPoint b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
GeometryPoint minus(GeometryPoint a, GeometryPoint b) { return plus(a, scale(b, -1)); }
GeometryPoint cross(GeometryPoint a, GeometryPoint b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
bool finite(GeometryPoint p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
// Use binary64 for the input/output area test, avoiding binary32 cross underflow.
bool area(GeometryPoint a, GeometryPoint b, GeometryPoint c) {
    double x = double(b.x) - a.x, y = double(b.y) - a.y, z = double(b.z) - a.z;
    double u = double(c.x) - a.x, v = double(c.y) - a.y, w = double(c.z) - a.z;
    return y * w - z * v != 0 || z * u - x * w != 0 || x * v - y * u != 0;
}
struct Vertex {
    Index start = none, incidence = 0;
    bool boundary = false;
};
struct Face {
    std::array<Index, 3> adjacent{none, none, none};
    std::array<Index, 3> edges{};
};
struct HalfEdge {
    std::uint32_t lo, hi;
    Index face;
    unsigned slot;
};
struct Edge {
    std::uint32_t lo, hi;
    Index face0, face1 = none;
    unsigned slot0, slot1 = 0;
};
struct Topology {
    std::vector<Vertex> vertices;
    std::vector<Face> faces;
    std::vector<Edge> edges;
    Index boundary_edges = 0;
};
Index storage(Index nv, Index nf) {
    // Conservative live-vector payload bound, including topology construction,
    // sorted half edges, ring storage, projection and output normals.
    return add(mul(nv, sizeof(GeometryPoint) * 3 + sizeof(Vertex) + sizeof(Index) * 2),
               mul(nf, sizeof(std::array<std::uint32_t, 3>) + sizeof(Face) +
                           3 * (sizeof(HalfEdge) + sizeof(Edge) + sizeof(Index))));
}
unsigned slot(const std::array<std::uint32_t, 3> &face, Index vertex) {
    for (unsigned k = 0; k < 3; ++k)
        if (face[k] == vertex)
            return k;
    throw Failure{"Loop subdivision invalid vertex fan"};
}
std::vector<Index> ring(const GeometryMesh &mesh, const Topology &t, Index vertex, Budget &budget) {
    const auto &v = t.vertices[vertex];
    if (!v.incidence)
        throw Failure{"Loop subdivision unused vertex"};
    std::vector<Index> result;
    result.reserve(add(v.incidence, 1));
    Index face = v.start;
    if (v.boundary) {
        bool found = false;
        for (Index step = 0; step < v.incidence; ++step) {
            budget.charge(1);
            unsigned k = slot(mesh.triangles[face], vertex);
            Index next = t.faces[face].adjacent[k];
            if (next == none) {
                found = true;
                break;
            }
            face = next;
        }
        if (!found)
            throw Failure{"Loop subdivision disconnected vertex fan"};
        unsigned k = slot(mesh.triangles[face], vertex);
        result.push_back(mesh.triangles[face][(k + 1) % 3]);
        for (Index step = 0; step < v.incidence; ++step) {
            budget.charge(1);
            k = slot(mesh.triangles[face], vertex);
            result.push_back(mesh.triangles[face][(k + 2) % 3]);
            face = t.faces[face].adjacent[(k + 2) % 3];
            if (face == none) {
                if (step + 1 != v.incidence)
                    throw Failure{"Loop subdivision disconnected vertex fan"};
                return result;
            }
        }
    } else {
        for (Index step = 0; step < v.incidence; ++step) {
            budget.charge(1);
            unsigned k = slot(mesh.triangles[face], vertex);
            result.push_back(mesh.triangles[face][(k + 1) % 3]);
            face = t.faces[face].adjacent[k];
            if (face == none)
                break;
            if (face == v.start) {
                if (step + 1 != v.incidence)
                    throw Failure{"Loop subdivision disconnected vertex fan"};
                return result;
            }
        }
    }
    throw Failure{"Loop subdivision disconnected vertex fan"};
}
Topology topology(const GeometryMesh &mesh, Budget &budget) {
    Index nv = mesh.positions.size(), nf = mesh.triangles.size();
    budget.scratch(storage(nv, nf));
    Topology t;
    t.vertices.resize(nv);
    t.faces.resize(nf);
    std::vector<HalfEdge> halves;
    halves.reserve(mul(nf, 3));
    for (const auto &p : mesh.positions) {
        budget.charge(1);
        if (!finite(p))
            throw Failure{"Loop subdivision nonfinite position"};
    }
    for (Index i = 0; i < nf; ++i) {
        budget.charge(3);
        auto f = mesh.triangles[i];
        for (auto v : f)
            if (v >= nv)
                throw Failure{"Loop subdivision index out of range"};
        if (f[0] == f[1] || f[1] == f[2] || f[2] == f[0])
            throw Failure{"Loop subdivision repeated triangle index"};
        if (!area(mesh.positions[f[0]], mesh.positions[f[1]], mesh.positions[f[2]]))
            throw Failure{"Loop subdivision degenerate triangle"};
        for (unsigned k = 0; k < 3; ++k) {
            auto a = f[k], b = f[(k + 1) % 3];
            halves.push_back({std::min(a, b), std::max(a, b), i, k});
            t.vertices[a].start = i;
            ++t.vertices[a].incidence;
        }
    }
    Index lg = 0;
    for (Index n = halves.size(); n; n >>= 1)
        ++lg;
    budget.charge(mul(mul(halves.size(), lg), 4));
    std::sort(halves.begin(), halves.end(), [](const HalfEdge &a, const HalfEdge &b) {
        if (a.lo != b.lo)
            return a.lo < b.lo;
        if (a.hi != b.hi)
            return a.hi < b.hi;
        return a.face < b.face;
    });
    t.edges.reserve(halves.size());
    for (Index i = 0; i < halves.size();) {
        Index j = i + 1;
        while (j < halves.size() && halves[i].lo == halves[j].lo && halves[i].hi == halves[j].hi)
            ++j;
        budget.charge(j - i);
        if (j - i > 2)
            throw Failure{"Loop subdivision nonmanifold edge"};
        auto a = halves[i];
        Edge edge{a.lo, a.hi, a.face, none, a.slot, 0};
        if (j - i == 2) {
            auto b = halves[i + 1];
            auto fa = mesh.triangles[a.face], fb = mesh.triangles[b.face];
            if (fa[(a.slot + 2) % 3] == fb[(b.slot + 2) % 3])
                throw Failure{"Loop subdivision duplicate triangle"};
            if (fa[a.slot] == fb[b.slot])
                throw Failure{"Loop subdivision inconsistent winding"};
            edge.face1 = b.face;
            edge.slot1 = b.slot;
            t.faces[a.face].adjacent[a.slot] = b.face;
            t.faces[b.face].adjacent[b.slot] = a.face;
            t.faces[b.face].edges[b.slot] = t.edges.size();
        } else {
            ++t.boundary_edges;
            t.vertices[a.lo].boundary = t.vertices[a.hi].boundary = true;
        }
        t.faces[a.face].edges[a.slot] = t.edges.size();
        t.edges.push_back(edge);
        i = j;
    }
    for (Index i = 0; i < nv; ++i)
        (void)ring(mesh, t, i, budget);
    return t;
}
float beta(Index valence) { return valence == 3 ? 3.f / 16.f : 3.f / (8.f * float(valence)); }
GeometryPoint weight(const GeometryMesh &mesh, const std::vector<Index> &r, Index i, bool boundary,
                     float b, Budget &budget) {
    budget.charge(boundary ? 3 : add(r.size(), 1));
    auto p = scale(mesh.positions[i], 1 - (boundary ? 2 : float(r.size())) * b);
    if (boundary) {
        p = plus(p, scale(mesh.positions[r.front()], b));
        p = plus(p, scale(mesh.positions[r.back()], b));
    } else
        for (auto v : r)
            p = plus(p, scale(mesh.positions[v], b));
    return p;
}
LoopSubdivisionResult run(const GeometryMesh &control, int levels, Budget &budget) {
    if (control.positions.empty() || control.triangles.empty())
        throw Failure{"Loop subdivision requires nonempty positions and triangles"};
    if (control.positions.size() > std::numeric_limits<std::uint32_t>::max())
        throw Failure{"Loop subdivision index limit exceeded", true};
    if (!control.patches.empty() || !control.normals.empty() || !control.uvs.empty() ||
        !control.tangents.empty() || !control.tangent_signs.empty())
        throw Failure{"Loop subdivision accepts positions and triangles only"};
    Index v = control.positions.size(), f = control.triangles.size();
    if (add(mul(v, 6), mul(f, 3)) > budget.limits.scalar_values)
        throw Failure{"Loop subdivision scalar limit exceeded", true};
    // Face growth alone rejects impossible levels before allocating adjacency.
    Index forecast_faces = f;
    for (int level = 0; level < levels; ++level) {
        budget.charge(1);
        forecast_faces = mul(forecast_faces, 4);
        if (add(mul(v, 6), mul(forecast_faces, 3)) > budget.limits.scalar_values)
            throw Failure{"Loop subdivision scalar limit exceeded", true};
    }
    auto t = topology(control, budget);
    Index e = t.edges.size(), boundary = t.boundary_edges;
    for (int level = 0; level < levels; ++level) {
        budget.charge(1);
        Index nextv = add(v, e), nextf = mul(f, 4);
        if (nextv > std::numeric_limits<std::uint32_t>::max() ||
            add(mul(nextv, 6), mul(nextf, 3)) > budget.limits.scalar_values)
            throw Failure{"Loop subdivision scalar limit exceeded", true};
        budget.scratch(add(storage(v, f), storage(nextv, nextf)));
        e = add(mul(e, 2), mul(f, 3));
        boundary = mul(boundary, 2);
        v = nextv;
        f = nextf;
    }
    GeometryMesh mesh;
    mesh.positions = control.positions;
    mesh.triangles = control.triangles;
    for (int level = 0; level < levels; ++level) {
        Index nv = mesh.positions.size(), nf = mesh.triangles.size();
        GeometryMesh next;
        next.positions.resize(add(nv, t.edges.size()));
        next.triangles.resize(mul(nf, 4));
        for (Index i = 0; i < nv; ++i) {
            auto r = ring(mesh, t, i, budget);
            bool boundary_vertex = t.vertices[i].boundary;
            next.positions[i] = weight(mesh, r, i, boundary_vertex,
                                       boundary_vertex ? 1.f / 8.f : beta(r.size()), budget);
        }
        std::vector<Index> odd(t.edges.size(), none);
        Index ordinal = nv;
        for (Index i = 0; i < nf; ++i) {
            for (unsigned k = 0; k < 3; ++k) {
                budget.charge(1);
                Index id = t.faces[i].edges[k];
                if (odd[id] != none)
                    continue;
                odd[id] = ordinal;
                auto edge = t.edges[id];
                float b = edge.face1 == none ? .5f : 3.f / 8.f;
                auto p = plus(scale(mesh.positions[edge.lo], b), scale(mesh.positions[edge.hi], b));
                if (edge.face1 != none) {
                    p = plus(p,
                             scale(mesh.positions[mesh.triangles[edge.face0][(edge.slot0 + 2) % 3]],
                                   1.f / 8.f));
                    p = plus(p,
                             scale(mesh.positions[mesh.triangles[edge.face1][(edge.slot1 + 2) % 3]],
                                   1.f / 8.f));
                }
                budget.charge(4);
                next.positions[ordinal++] = p;
            }
            for (unsigned k = 0; k < 3; ++k) {
                budget.charge(3);
                auto edge = static_cast<std::uint32_t>(odd[t.faces[i].edges[k]]);
                next.triangles[4 * i + k][k] = mesh.triangles[i][k];
                next.triangles[4 * i + k][(k + 1) % 3] = edge;
                next.triangles[4 * i + (k + 1) % 3][k] = edge;
                next.triangles[4 * i + 3][k] = edge;
            }
        }
        auto nt = topology(next, budget);
        for (Index i = 0; i < nv; ++i)
            nt.vertices[i].start =
                4 * t.vertices[i].start + slot(mesh.triangles[t.vertices[i].start], i);
        for (Index id = 0; id < t.edges.size(); ++id)
            nt.vertices[odd[id]].start = 4 * t.edges[id].face0 + 3;
        mesh = std::move(next);
        t = std::move(nt);
    }
    std::vector<GeometryPoint> projected(mesh.positions.size());
    for (Index i = 0; i < mesh.positions.size(); ++i) {
        auto r = ring(mesh, t, i, budget);
        bool boundary_vertex = t.vertices[i].boundary;
        float gamma = 1.f / (float(r.size()) + 3.f / (8.f * beta(r.size())));
        projected[i] =
            weight(mesh, r, i, boundary_vertex, boundary_vertex ? 1.f / 5.f : gamma, budget);
    }
    mesh.positions = std::move(projected);
    mesh.normals.resize(mesh.positions.size());
    for (Index i = 0; i < mesh.positions.size(); ++i) {
        auto r = ring(mesh, t, i, budget);
        Index n = r.size();
        budget.charge(mul(n, 4));
        GeometryPoint s{}, tangent{};
        auto p = [&](Index j) { return mesh.positions[r[j]]; };
        if (!t.vertices[i].boundary) {
            for (Index j = 0; j < n; ++j) {
                float angle = 2 * std::numbers::pi_v<float> * float(j) / float(n);
                s = plus(s, scale(p(j), std::cos(angle)));
                tangent = plus(tangent, scale(p(j), std::sin(angle)));
            }
        } else {
            s = minus(p(n - 1), p(0));
            if (n == 2)
                tangent = minus(plus(p(0), p(1)), scale(mesh.positions[i], 2));
            else if (n == 3)
                tangent = minus(p(1), mesh.positions[i]);
            else if (n == 4)
                tangent = plus(plus(plus(plus(scale(p(0), -1), scale(p(1), 2)), scale(p(2), 2)),
                                    scale(p(3), -1)),
                               scale(mesh.positions[i], -2));
            else {
                float theta = std::numbers::pi_v<float> / float(n - 1);
                tangent = scale(plus(p(0), p(n - 1)), std::sin(theta));
                for (Index k = 1; k + 1 < n; ++k)
                    tangent = plus(tangent, scale(p(k), (2 * std::cos(theta) - 2) *
                                                            std::sin(float(k) * theta)));
                tangent = scale(tangent, -1);
            }
        }
        mesh.normals[i] = cross(s, tangent);
        if (!finite(mesh.positions[i]) || !finite(mesh.normals[i]))
            throw Failure{"Loop subdivision nonfinite output position or normal"};
    }
    for (const auto &face : mesh.triangles) {
        budget.charge(1);
        if (!area(mesh.positions[face[0]], mesh.positions[face[1]], mesh.positions[face[2]]))
            throw Failure{"Loop subdivision collapsed output triangle"};
    }
    LoopSubdivisionResult result;
    result.scalar_values = add(mul(mesh.positions.size(), 6), mul(mesh.triangles.size(), 3));
    result.work = budget.work;
    result.mesh = std::move(mesh);
    return result;
}
} // namespace
LoopSubdivisionResult subdivide_loop(const GeometryMesh &control, int levels,
                                     const LoopSubdivisionLimits &limits) {
    Budget budget{limits};
    try {
        return run(control, levels, budget);
    } catch (const Failure &failure) {
        return {std::nullopt, failure.diagnostic, failure.limit, 0, budget.work};
    } catch (const std::bad_alloc &) {
        return {std::nullopt, "Loop subdivision allocation limit exceeded", true, 0, budget.work};
    } catch (const std::length_error &) {
        return {std::nullopt, "Loop subdivision allocation length exceeded", true, 0, budget.work};
    }
}
} // namespace rypath
