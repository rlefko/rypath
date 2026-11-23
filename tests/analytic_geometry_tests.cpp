// SPDX-License-Identifier: GPL-3.0-only
#include "analytic.hpp"
#include "metal_geometry.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
void near(double actual, double expected, double tolerance, std::string_view message) {
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(actual) + " vs " +
                                 std::to_string(expected));
}
GeometryAnalytic shape(AnalyticKind kind) {
    GeometryAnalytic s;
    s.kind = kind;
    return s;
}
struct SphereClipCase {
    GeometryAnalytic sphere;
    GeometryRay ray;
    bool hit;
    double t;
};
std::vector<SphereClipCase> sphere_clip_cases(float radius) {
    auto sphere = shape(AnalyticKind::sphere);
    sphere.radius = radius;
    sphere.z_min = -radius;
    sphere.z_max = radius;
    constexpr float distance = .35f;
    std::vector<SphereClipCase> out;
    for (float sign : {-1.f, 1.f}) {
        out.push_back(
            {sphere, {{0, 0, sign * distance}, {0, 0, -sign}}, true, double(distance) - radius});
        out.push_back({sphere, {{0, 0, 0}, {0, 0, sign * 2}}, true, double(radius) / 2});
        out.push_back({sphere,
                       {{0, 0, sign * distance}, {0, 0, -sign}, distance},
                       true,
                       double(distance) + radius});
    }
    auto upper = sphere;
    upper.z_min = 0;
    out.push_back({upper, {{0, 0, -distance}, {0, 0, 1}}, true, double(distance) + radius});
    out.push_back({upper, {{0, 0, distance}, {0, 0, -1}}, true, double(distance) - radius});
    auto lower = sphere;
    lower.z_max = 0;
    out.push_back({lower, {{0, 0, distance}, {0, 0, -1}}, true, double(distance) + radius});
    out.push_back({lower, {{0, 0, -distance}, {0, 0, 1}}, true, double(distance) - radius});
    auto band = sphere;
    band.z_min = -radius / 2;
    band.z_max = radius / 2;
    for (float sign : {-1.f, 1.f})
        out.push_back({band, {{0, 0, sign * distance}, {0, 0, -sign}}, false, 0});
    return out;
}
void portable_sphere_clipping() {
    for (float radius : {.1f, .125f}) {
        for (const auto &c : sphere_clip_cases(radius)) {
            const auto hit = intersect_analytic(c.sphere, c.ray);
            require(hit.hit == c.hit, "sphere active clipping decision at both full-sphere poles");
            if (!hit.hit)
                continue;
            near(hit.t, c.t, 1e-12, "independent axial sphere distance");
            require(std::isfinite(hit.uv[0]) && std::isfinite(hit.uv[1]), "finite pole UV");
            const auto frame = evaluate_analytic(c.sphere, hit.uv);
            for (const auto v :
                 {frame.position, frame.normal, frame.dpdu, frame.dpdv, frame.dndu, frame.dndv})
                require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z),
                        "finite pole surface frame");
        }
    }
}
void portable() {
    portable_sphere_clipping();
    for (auto kind : {AnalyticKind::sphere, AnalyticKind::disk, AnalyticKind::cylinder}) {
        auto s = shape(kind);
        require(validate_analytic(s).empty(), "defaults validate");
        near(analytic_area(s), kind == AnalyticKind::disk ? std::numbers::pi : 4 * std::numbers::pi,
             1e-6, "default area");
        for (auto uv : {std::array<float, 2>{0, 0}, {0, 1}, {.25f, .5f}, {1, 1}}) {
            const auto e = evaluate_analytic(s, uv);
            for (const auto v : {e.position, e.normal, e.dpdu, e.dpdv, e.dndu, e.dndv})
                require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z),
                        "finite surface frames including poles and center");
            near(std::hypot(e.normal.x, e.normal.y, e.normal.z), 1, 1e-6, "unit object normal");
        }
        s.radius = 0;
        require(!validate_analytic(s).empty(), "zero radius rejected");
        s = shape(kind);
        s.phi_max = 0;
        require(!validate_analytic(s).empty(), "zero angular extent rejected");
    }
    // Published shader domain boundaries, not merely finite host doubles.
    for (float radius : {0x1p-16f, 0x1p16f}) {
        auto disk = shape(AnalyticKind::disk);
        disk.radius = radius;
        require(validate_analytic(disk).empty(), "source radius domain boundary accepted");
        disk.radius = std::nextafter(radius, radius < 1 ? 0.f : INFINITY);
        require(!validate_analytic(disk).empty(), "outside source radius domain rejected");
    }
    for (auto kind : {AnalyticKind::disk, AnalyticKind::cylinder}) {
        auto large = shape(kind);
        large.radius = 1e20f;
        if (kind == AnalyticKind::disk)
            large.inner_radius = std::nextafter(large.radius, 0.f);
        else {
            large.z_min = 0;
            large.z_max = 1e-10f;
        }
        require(std::isfinite(analytic_area(large)), "unsafe shape has finite host area");
        require(!validate_analytic(large).empty(), "unsafe squared-radius source rejected");
    }
    auto thin = shape(AnalyticKind::cylinder);
    thin.z_min = 0;
    thin.z_max = 0x1p-16f;
    require(validate_analytic(thin).empty(), "minimum cylinder height accepted");
    thin.z_max = std::nextafter(thin.z_max, 0.f);
    require(!validate_analytic(thin).empty(), "below minimum cylinder height rejected");
    auto wedge = shape(AnalyticKind::disk);
    wedge.phi_max = 0x1p-16f;
    require(validate_analytic(wedge).empty(), "minimum angular extent accepted");
    wedge.phi_max = std::nextafter(wedge.phi_max, 0.f);
    require(!validate_analytic(wedge).empty(), "below minimum angular extent rejected");
    GeometryMesh domain_mesh;
    domain_mesh.analytics.push_back(shape(AnalyticKind::sphere));
    for (float scale : {1e10f, 1e-10f}) {
        GeometryTransform transform;
        transform.values[0] = transform.values[5] = transform.values[10] = scale;
        require(!validate_analytic_geometry(domain_mesh, transform).empty(),
                "unsafe affine scale diagnosed before shading");
        GeometryData scene{{domain_mesh}, {{0, transform}}};
        require(!validate_geometry(scene).empty(), "native geometry enforces analytic domain");
    }
    GeometryTransform edge;
    edge.values[0] = std::nextafter(0x1p16f, 0.f);
    require(validate_analytic_geometry(domain_mesh, edge).empty(),
            "inside affine norm domain accepted");
    edge.values[0] = 0x1p16f;
    require(!validate_analytic_geometry(domain_mesh, edge).empty(),
            "outside affine Frobenius norm domain rejected");
    auto sphere = shape(AnalyticKind::sphere);
    auto hit = intersect_analytic(sphere, {{-3, 0, 0}, {2, 0, 0}});
    require(hit.hit, "sphere hit");
    near(hit.t, 1, 1e-12, "sphere nonunit t");
    hit = intersect_analytic(sphere, {{-3, 0, 0}, {2, 0, 0}, 1.1f});
    require(hit.hit, "sphere farther root");
    near(hit.t, 2, 1e-12, "farther root t");
    hit = intersect_analytic(sphere, {{0, 0, 0}, {0, 0, 2}});
    require(hit.hit, "sphere inside pole");
    near(hit.t, .5, 1e-12, "inside t");
    hit = intersect_analytic(sphere, {{-3, 1, 0}, {1, 0, 0}});
    require(hit.hit, "sphere tangent");
    near(hit.t, 3, 1e-12, "tangent t");
    sphere.phi_max = std::numbers::pi_v<float> / 2;
    hit = intersect_analytic(sphere, {{-3, 0, 0}, {1, 0, 0}});
    require(hit.hit, "clipped near root reveals far root");
    near(hit.t, 4, 1e-12, "clipped far t");
    auto cylinder = shape(AnalyticKind::cylinder);
    require(!intersect_analytic(cylinder, {{0, 0, 3}, {0, 0, -1}}).hit,
            "cylinder remains uncapped");
    hit = intersect_analytic(cylinder, {{2, 0, 0}, {-1, 0, 0}});
    require(hit.hit, "cylinder side");
    near(hit.t, 1, 1e-12, "cylinder t");
    auto disk = shape(AnalyticKind::disk);
    disk.inner_radius = .5f;
    require(!intersect_analytic(disk, {{0, 0, 2}, {0, 0, -1}}).hit, "disk hole");
    hit = intersect_analytic(disk, {{.75f, 0, 2}, {0, 0, -2}});
    require(hit.hit, "disk annulus");
    near(hit.t, 1, 1e-12, "disk t");
    require(!intersect_analytic(disk, {{.75f, 0, 0}, {1, 0, 0}}).hit, "parallel disk ray");
    for (auto kind : {AnalyticKind::sphere, AnalyticKind::disk, AnalyticKind::cylinder}) {
        auto s = shape(kind);
        s.phi_max = 1.5f;
        s.z_min = -.5f;
        s.z_max = .75f;
        s.inner_radius = .25f;
        double sum = 0;
        constexpr int n = 1000;
        for (int i = 0; i < n; ++i) {
            const auto e = sample_analytic_area(s, {(static_cast<float>(i) + .5f) / n, .37f});
            sum += kind == AnalyticKind::disk
                       ? double(e.position.x) * e.position.x + double(e.position.y) * e.position.y
                       : e.position.z;
        }
        near(sum / n, kind == AnalyticKind::disk ? (1 + .25 * .25) / 2 : (-.5 + .75) / 2, 2e-7,
             "uniform clipped area proposal moment");
    }
    const GeometryTransform xf{{-2, .5f, 0, 3, 0, 3, 0, -2, 0, 0, 4, 1}};
    near(analytic_area_jacobian({0, 0, 1}, xf), 6, 1e-12, "affine planar Jacobian");
    near(analytic_area_jacobian({1, 0, 0}, xf), std::sqrt(148.), 1e-12, "affine curved Jacobian");
    GeometryData g;
    g.meshes.resize(1);
    g.meshes[0].analytics.push_back(shape(AnalyticKind::sphere));
    g.instances.push_back({0, {}});
    require(validate_geometry(g).empty(), "analytic geometry validates without vertices");
    g.meshes[0].analytics[0].z_min = 2;
    require(!validate_geometry(g).empty(), "invalid analytic geometry diagnosed");
    std::cout << "Analytic portable geometry checks passed\n";
}
GeometryPoint transform(GeometryPoint p, const GeometryTransform &xf, bool vector) {
    const auto &m = xf.values;
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + (vector ? 0 : m[3]),
            m[4] * p.x + m[5] * p.y + m[6] * p.z + (vector ? 0 : m[7]),
            m[8] * p.x + m[9] * p.y + m[10] * p.z + (vector ? 0 : m[11])};
}
int native_sphere_clipping(const std::filesystem::path &library) {
    for (float radius : {.1f, .125f}) {
        const auto cases = sphere_clip_cases(radius);
        // Reuse one AS for each of the four authored clipping policies.
        for (unsigned first : {0u, 6u, 8u, 10u}) {
            const unsigned count = first == 0 ? 6 : 2;
            GeometryData data;
            data.meshes.resize(1);
            data.meshes[0].analytics = {cases[first].sphere};
            data.instances = {{0, {}}};
            auto built = build_metal_geometry(data, library);
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
            if (built.status == GeometryStatus::no_device) {
                std::cerr << built.diagnostic << '\n';
                return 77;
            }
#endif
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
            if (built.status == GeometryStatus::no_ray_tracing) {
                std::cerr << built.diagnostic << '\n';
                return 77;
            }
#endif
            require(built.status == GeometryStatus::success, built.diagnostic);
            std::vector<GeometryRay> rays;
            for (unsigned i = 0; i < count; ++i)
                rays.push_back(cases[first + i].ray);
            const auto hits = built.scene->closest_hits(rays);
            const auto shadows = built.scene->occlusion(rays);
            require(hits.status == GeometryStatus::success, hits.diagnostic);
            require(shadows.status == GeometryStatus::success, shadows.diagnostic);
            require(hits.guard_count > 0 && shadows.guard_count > 0, "sphere pole dispatch guards");
            for (unsigned i = 0; i < count; ++i) {
                const auto &c = cases[first + i];
                const auto &hit = hits.hits[i];
                require(hit.hit == c.hit && bool(shadows.occluded[i]) == c.hit,
                        "native full sphere poles and active hemisphere/band clipping");
                if (!c.hit)
                    continue;
                require(hit.kind == PrimitiveKind::analytic && hit.instance_index == 0 &&
                            hit.primitive_index == 0,
                        "sphere pole source identity");
                near(hit.t, c.t,
                     32 * std::numeric_limits<float>::epsilon() * std::max(1., std::abs(c.t)),
                     "native independent axial sphere t");
                for (float uv : hit.analytic_uv)
                    require(std::isfinite(uv), "native finite pole UV");
            }
        }
        // Original failed mixed fixture: translated full sphere must beat the
        // farther triangle. The expected distance uses source binary32 values.
        GeometryMesh sphere;
        auto full = shape(AnalyticKind::sphere);
        full.radius = radius;
        full.z_min = -radius;
        full.z_max = radius;
        sphere.analytics = {full};
        GeometryMesh farther;
        farther.positions = {{-1, -1, .75f}, {1, -1, .75f}, {0, 1, .75f}};
        farther.triangles = {{0, 1, 2}};
        GeometryTransform translation;
        translation.values[11] = .35f;
        GeometryData data{{sphere, farther}, {{0, translation}, {1, {}}}};
        auto built = build_metal_geometry(data, library);
        require(built.status == GeometryStatus::success, built.diagnostic);
        const std::array rays{GeometryRay{{0, 0, 0}, {0, 0, 1}, 0, 1},
                              GeometryRay{{0, 0, .7f}, {0, 0, -1}, 0, 1},
                              GeometryRay{{0, 0, .35f}, {0, 0, 2}, 0, 1}};
        const std::array expected{double(.35f) - radius, double(.7f) - double(.35f) - radius,
                                  double(radius) / 2};
        const auto hits = built.scene->closest_hits(rays);
        const auto shadows = built.scene->occlusion(rays);
        require(hits.status == GeometryStatus::success, hits.diagnostic);
        require(shadows.status == GeometryStatus::success, shadows.diagnostic);
        require(hits.guard_count > 0 && shadows.guard_count > 0, "translated mixed pole guards");
        for (std::size_t i = 0; i < rays.size(); ++i) {
            require(hits.hits[i].hit && hits.hits[i].kind == PrimitiveKind::analytic &&
                        hits.hits[i].instance_index == 0 && hits.hits[i].mesh_index == 0 &&
                        hits.hits[i].primitive_index == 0 && shadows.occluded[i],
                    "translated full sphere must precede farther triangle");
            near(hits.hits[i].t, expected[i],
                 32 * std::numeric_limits<float>::epsilon() * std::max(1., expected[i]),
                 "translated full sphere independent t");
        }
    }
    return 0;
}
int native(const std::filesystem::path &library) {
    if (const int status = native_sphere_clipping(library); status != 0)
        return status;
    const std::array transforms{GeometryTransform{},
                                GeometryTransform{{-2, .5f, 0, 8, 0, 1, 0, -4, 0, 0, .5f, 2}}};
    for (auto kind : {AnalyticKind::sphere, AnalyticKind::disk, AnalyticKind::cylinder}) {
        for (const auto &xf : transforms) {
            auto s = shape(kind);
            s.phi_max = 1.5f * std::numbers::pi_v<float>;
            if (kind == AnalyticKind::disk)
                s.inner_radius = .25f;
            else {
                s.z_min = -.75f;
                s.z_max = .5f;
            }
            GeometryData g;
            g.meshes.resize(1);
            g.meshes[0].analytics = {s};
            g.instances = {{0, xf}};
            auto built = build_metal_geometry(g, library);
            if (built.status == GeometryStatus::no_device) {
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
                std::cerr << built.diagnostic << '\n';
                return 77;
#endif
            }
            if (built.status == GeometryStatus::no_ray_tracing) {
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
                std::cerr << built.diagnostic << '\n';
                return 77;
#endif
            }
            require(built.status == GeometryStatus::success, built.diagnostic);
            std::vector<GeometryRay> rays;
            std::vector<AnalyticIntersection> expected;
            // A deterministic independent angular grid provides inward/outward,
            // clipped, oblique, and non-unit rays. Host reference is binary64.
            for (int i = 0; i < 257; ++i) {
                const float x = float((i * 37) % 97 - 48) / 32, y = float((i * 19) % 89 - 44) / 32;
                GeometryRay local = kind == AnalyticKind::disk
                                        ? GeometryRay{{x, y, 3}, {.125f, -.0625f, -2}}
                                        : GeometryRay{{-3, x, y * .5f}, {2, .0625f, -.03125f}};
                expected.push_back(intersect_analytic(s, local));
                rays.push_back(
                    {transform(local.origin, xf, false), transform(local.direction, xf, true)});
            }
            const auto hits = built.scene->closest_hits(rays);
            const auto any = built.scene->occlusion(rays);
            require(hits.status == GeometryStatus::success, hits.diagnostic);
            require(any.status == GeometryStatus::success, any.diagnostic);
            require(hits.guard_count > 0 && any.guard_count > 0, "rounded dispatch guards");
            for (std::size_t i = 0; i < rays.size(); ++i) {
                const auto &h = hits.hits[i];
                const auto &r = expected[i];
                require(h.hit == r.hit, "native/CPU hit decision");
                require(bool(any.occluded[i]) == r.hit, "native occlusion decision");
                if (!h.hit)
                    continue;
                require(h.kind == PrimitiveKind::analytic && h.mesh_index == 0 &&
                            h.instance_index == 0 && h.primitive_index == 0,
                        "analytic source mapping");
                near(h.t, r.t,
                     32 * std::numeric_limits<float>::epsilon() * std::max(1., std::abs(r.t)),
                     "native t");
                for (int axis = 0; axis < 2; ++axis)
                    near(h.analytic_uv[axis], r.uv[axis],
                         64 * std::numeric_limits<float>::epsilon(), "native analytic uv");
            }
        }
    }
    std::cout << "Native analytic geometry checks passed\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--native")
            return native(argc > 2 ? argv[2] : "");
        portable();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
