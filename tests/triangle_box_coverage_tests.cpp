// SPDX-License-Identifier: GPL-3.0-only
#include "metal_geometry.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
constexpr float f(std::uint32_t bits) { return std::bit_cast<float>(bits); }
// Exact binary32 ray captured after two original Sobol sample1 scatterings.
// At z=1, x-y=1.2471314549871051e-6: primitive0 is outside by
// u=-7.61188632194278e-11, while primitive1 is inside by the same positive v.
// The former native-triangle query offered only primitive0 and lost the hit.
const GeometryRay witness{{f(0x3fff2000), 0, f(0xbf7e3fee)},
                          {f(0xbf3504f3), f(0xb384c063), f(0x3f3504f3)}};
GeometryData geometry(bool reverse) {
    GeometryMesh mesh;
    mesh.positions = {{-8192, -8192, 1}, {-8192, 8192, 1}, {8192, 8192, 1}, {8192, -8192, 1},
                      {-8192, 0, 1},     {0, 0, 1},        {8192, 0, 1}};
    // Keep a distinct-index collinear face in slot0. Its native box must not
    // acquire a false surface, and the real triangle IDs must not be compacted.
    mesh.triangles = {{4, 5, 6}, {0, 1, 2}, {0, 2, 3}};
    if (reverse)
        for (auto &triangle : mesh.triangles)
            std::swap(triangle[1], triangle[2]);
    return {{mesh}, {{0, {}}}};
}
GeometryPoint transform(GeometryPoint p, const GeometryTransform &m, bool position) {
    const auto &v = m.values;
    return {v[0] * p.x + v[1] * p.y + v[2] * p.z + (position ? v[3] : 0),
            v[4] * p.x + v[5] * p.y + v[6] * p.z + (position ? v[7] : 0),
            v[8] * p.x + v[9] * p.y + v[10] * p.z + (position ? v[11] : 0)};
}
using V = std::array<long double, 3>;
V subtract(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V cross(V a, V b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
long double dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V world(GeometryPoint p, const GeometryTransform &m) {
    const auto &v = m.values;
    return {static_cast<long double>(v[0]) * p.x + static_cast<long double>(v[1]) * p.y +
                static_cast<long double>(v[2]) * p.z + v[3],
            static_cast<long double>(v[4]) * p.x + static_cast<long double>(v[5]) * p.y +
                static_cast<long double>(v[6]) * p.z + v[7],
            static_cast<long double>(v[8]) * p.x + static_cast<long double>(v[9]) * p.y +
                static_cast<long double>(v[10]) * p.z + v[11]};
}
// Independent forward-matrix Moller-Trumbore reference for these well-separated
// binary32 fixtures. The witnessed identity also has the exact rational proof above.
std::uint32_t reference(const GeometryData &g, const GeometryRay &ray) {
    const auto &mesh = g.meshes.front();
    const auto &matrix = g.instances.front().transform;
    const V origin{ray.origin.x, ray.origin.y, ray.origin.z};
    const V direction{ray.direction.x, ray.direction.y, ray.direction.z};
    long double closest = std::numeric_limits<long double>::infinity();
    std::uint32_t id = UINT32_MAX;
    for (std::uint32_t i = 0; i < mesh.triangles.size(); ++i) {
        const auto &indices = mesh.triangles[i];
        const auto a = world(mesh.positions[indices[0]], matrix);
        const auto e1 = subtract(world(mesh.positions[indices[1]], matrix), a);
        const auto e2 = subtract(world(mesh.positions[indices[2]], matrix), a);
        const auto p = cross(direction, e2);
        const auto determinant = dot(e1, p);
        if (determinant == 0)
            continue;
        const auto translated = subtract(origin, a);
        const auto u = dot(translated, p) / determinant;
        const auto q = cross(translated, e1);
        const auto v = dot(direction, q) / determinant;
        const auto t = dot(e2, q) / determinant;
        if (u >= 0 && v >= 0 && u + v <= 1 && t >= ray.t_min && t <= ray.t_max && t < closest) {
            closest = t;
            id = i;
        }
    }
    return id;
}
const std::array<GeometryTransform, 5> transforms{{
    {},
    {{-2, 0, 0, 4, 0, .5f, 0, -3, 0, 0, 4, 1}},
    {{1, .5f, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
    {{0, 0, 2, 0, 1, 0, 0, 0, 0, .5f, 0, 0}},
    {{1, 0, 0, 1048576, 0, 1, 0, -1048576, 0, 0, 1, 131072}},
}};
// These transforms contain exactly one signed power of two per row/column.
// No translation or rounded sum changes the captured binary32 geometry/ray.
const std::array<GeometryTransform, 3> dyadic_transforms{{
    {{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
    {{2, 0, 0, 0, 0, .5f, 0, 0, 0, 0, 4, 0}},
    {{0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0}},
}};
void relative(float actual, long double expected, std::string_view message) {
    require(std::isfinite(actual) && expected > 0 &&
                std::abs(static_cast<long double>(actual) - expected) <=
                    4 * std::numeric_limits<float>::epsilon() * expected,
            message);
}
std::size_t check_fallbacks(bool native, const std::filesystem::path &library, bool allow_no_device,
                            bool allow_no_ray_tracing) {
    std::size_t checked = 0;
    for (bool axial : {false, true}) {
        GeometryMesh mesh;
        mesh.positions =
            axial ? std::vector<GeometryPoint>{{-8, -8, 1}, {8, 8, 1}, {8, -8, 1}}
                  : std::vector<GeometryPoint>{{-30, -.5f, -15}, {30, -.5f, 30}, {30, -.5f, -15}};
        mesh.triangles = {{0, 1, 2}};
        const GeometryRay original =
            axial ? GeometryRay{{0, 0, 0}, {f(368197336), f(351475317), f(1065353216)}}
                  : GeometryRay{{5.5f, 8, -8}, {f(2360150648), f(3205440214), f(1062485707)}};
        // Independent exact-rational Area witness from the admitted lane10 capture.
        // Axial v is formed directly: subtracting two near-half barycentrics would
        // erase this positive coordinate even in the long-double reference.
        const long double t = axial ? 1.L : 4194304.L / 275899.L;
        const long double u = axial ? (static_cast<long double>(original.direction.y) + 8) / 16
                                    : 21634879.L / 49661820.L;
        const long double v =
            axial ? (static_cast<long double>(original.direction.x) - original.direction.y) / 16
                  : 9822218909212104592413301983567354899.L /
                        62953835931426287455837005178805944320.L;
        if (axial)
            require(std::bit_cast<std::uint32_t>(static_cast<float>(v)) == 330660155,
                    "Axial positive tiny coordinate reference bits");
        for (const auto &matrix : dyadic_transforms) {
            GeometryData g{{mesh}, {{0, matrix}}};
            require(validate_geometry(g).empty(), "Transformed fallback fixture must validate");
            GeometryRay ray{transform(original.origin, matrix, true),
                            transform(original.direction, matrix, false)};
            // Compare the actual uploaded binary32 values to the exact transformed
            // inputs, rather than assuming affine invariance after float rounding.
            for (const auto &point : mesh.positions) {
                const auto exact = world(point, matrix);
                const auto rounded = transform(point, matrix, true);
                require(exact == V{rounded.x, rounded.y, rounded.z},
                        "Dyadic corner transform must be exact");
            }
            require(world(original.origin, matrix) == V{ray.origin.x, ray.origin.y, ray.origin.z} &&
                        world(original.direction, matrix) ==
                            V{ray.direction.x, ray.direction.y, ray.direction.z},
                    "Dyadic ray transform must be exact");
            require(reference(g, ray) == 0, "Transformed fallback independent coverage oracle");
            if (!native)
                continue;
            auto built = build_metal_geometry(g, library);
            if ((allow_no_device && built.status == GeometryStatus::no_device) ||
                (allow_no_ray_tracing && built.status == GeometryStatus::no_ray_tracing))
                throw 77;
            require(built.status == GeometryStatus::success, built.diagnostic);
            const std::array rays{ray};
            const auto closest = built.scene->closest_hits(rays);
            const auto shadow = built.scene->occlusion(rays);
            require(closest.status == GeometryStatus::success, closest.diagnostic);
            require(shadow.status == GeometryStatus::success, shadow.diagnostic);
            require(closest.guard_count > 0 && shadow.guard_count > 0 && closest.hits.size() == 1 &&
                        shadow.occluded.size() == 1,
                    "Transformed fallback output accounting");
            const auto &hit = closest.hits.front();
            require(hit.hit && hit.kind == PrimitiveKind::triangle && hit.instance_index == 0 &&
                        hit.mesh_index == 0 && hit.primitive_index == 0 && shadow.occluded[0] == 1,
                    "Transformed fallback closest/occlusion kind and IDs");
            relative(hit.t, t, "Transformed fallback distance");
            relative(hit.barycentric[0], 1 - u - v, "Transformed fallback b0");
            relative(hit.barycentric[1], u, "Transformed fallback u");
            relative(hit.barycentric[2], v, "Transformed fallback positive relative v");
            ++checked;
        }
    }
    return checked;
}
void check(bool native, const std::filesystem::path &library, bool allow_no_device,
           bool allow_no_ray_tracing) {
    std::size_t checked = 0;
    for (bool reverse : {false, true}) {
        for (const auto &matrix : transforms) {
            auto g = geometry(reverse);
            g.instances.front().transform = matrix;
            g.instances.front().non_opaque = reverse;
            require(validate_geometry(g).empty(), "Triangle box fixture must validate");
            std::array<GeometryRay, 2> rays{witness, GeometryRay{{0, 0, 3}, {0, 0, -1}}};
            for (auto &ray : rays) {
                ray.origin = transform(ray.origin, matrix, true);
                ray.direction = transform(ray.direction, matrix, false);
            }
            const std::array expected{reference(g, rays[0]), reference(g, rays[1])};
            require(expected[0] == 2 && expected[1] == 1,
                    "Independent fixture oracle must retain neighbor and closed-edge IDs");
            if (!native)
                continue;
            auto built = build_metal_geometry(g, library);
            if ((allow_no_device && built.status == GeometryStatus::no_device) ||
                (allow_no_ray_tracing && built.status == GeometryStatus::no_ray_tracing))
                throw 77;
            require(built.status == GeometryStatus::success, built.diagnostic);
            const auto closest = built.scene->closest_hits(rays);
            const auto shadow = built.scene->occlusion(rays);
            require(closest.status == GeometryStatus::success, closest.diagnostic);
            require(shadow.status == GeometryStatus::success, shadow.diagnostic);
            require(closest.guard_count > 0 && shadow.guard_count > 0 &&
                        closest.hits.size() == rays.size() && shadow.occluded.size() == rays.size(),
                    "Triangle box output accounting");
            for (std::size_t i = 0; i < rays.size(); ++i) {
                const auto &hit = closest.hits[i];
                require(hit.hit && hit.kind == PrimitiveKind::triangle && hit.instance_index == 0 &&
                            hit.mesh_index == 0 && hit.primitive_index == expected[i] &&
                            std::isfinite(hit.t) && hit.t > 0 && shadow.occluded[i] == 1,
                        "Native boxes must recover original triangle coverage/kind/IDs");
                ++checked;
            }
        }
    }
    checked += check_fallbacks(native, library, allow_no_device, allow_no_ray_tracing);
    std::cout << "PASS triangle box supported identity/affine/winding/zero-slot/fallback coverage: "
              << checked << " native rays\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        bool native = false, allow_no_device = false, allow_no_ray_tracing = false;
        std::filesystem::path library;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option(argv[i]);
            if (option == "--native")
                native = true;
            else if (option == "--allow-no-device")
                allow_no_device = true;
            else if (option == "--allow-no-ray-tracing")
                allow_no_ray_tracing = true;
            else if (!option.starts_with("--"))
                library = option;
            else
                require(false, "Unknown triangle box test option");
        }
        check(native, library, allow_no_device, allow_no_ray_tracing);
        return 0;
    } catch (int code) {
        return code;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
