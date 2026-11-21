// SPDX-License-Identifier: GPL-3.0-only
#include "geometry_reference.hpp"
#include "metal_geometry.hpp"
#include "triangle_candidate_fixture.hpp"
#include "triangle_product_guard_cases.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace rypath;
using namespace rypath::test::triangle_fixture;
namespace {
void require(bool b, std::string_view reason) {
    if (!b)
        throw std::runtime_error(std::string(reason));
}
GeometryPoint transform(GeometryPoint p, const GeometryTransform &m, bool origin) {
    const auto &a = m.values;
    return {a[0] * p.x + a[1] * p.y + a[2] * p.z + (origin ? a[3] : 0),
            a[4] * p.x + a[5] * p.y + a[6] * p.z + (origin ? a[7] : 0),
            a[8] * p.x + a[9] * p.y + a[10] * p.z + (origin ? a[11] : 0)};
}
const std::array<GeometryTransform, 5> transforms{{{},
                                                   {{-1, 0, 0, 0, 0, 2, 0, 0, 0, 0, .5f, 0}},
                                                   {{0, 0, 2, 0, 1, 0, 0, 0, 0, .5f, 0, 0}},
                                                   {{1, .25f, 0, 0, 0, 1, .125f, 0, .25f, 0, 1, 0}},
                                                   {{1, 0, 0, 2, 0, 1, 0, -1, 0, 0, 1, 4}}}};
void cpu() {
    test::product_guard::check_cpu();
    auto g = regression();
    require(validate_geometry(g).empty(), "regression geometry valid");
    for (unsigned i = 0; i < 2; ++i) {
        auto expected = test::reference_closest(g, rays[i]);
        require(expected.size() == 1 && expected[0].primitive_index == (i == 0 ? 773u : 601u),
                "independent oracle authenticates regression");
    }
}
int native(bool allow_no_device, bool allow_no_ray_tracing) {
    cpu();
    std::size_t checked = 0;
    for (auto &m : transforms)
        for (unsigned build = 0; build < 4; ++build) {
            auto g = regression();
            g.instances[0].transform = m;
            g.instances[0].non_opaque = (build % 2) != 0;
            std::array<GeometryRay, 2> input = rays;
            for (auto &r : input) {
                r.origin = transform(r.origin, m, true);
                r.direction = transform(r.direction, m, false);
            }
            auto b = build_metal_geometry(g);
            if ((allow_no_device && b.status == GeometryStatus::no_device) ||
                (allow_no_ray_tracing && b.status == GeometryStatus::no_ray_tracing))
                return 77;
            require(b.status == GeometryStatus::success, b.diagnostic);
            auto closest = b.scene->closest_hits(input);
            auto shadow = b.scene->occlusion(input);
            require(closest.status == GeometryStatus::success, closest.diagnostic);
            require(shadow.status == GeometryStatus::success, shadow.diagnostic);
            require(closest.guard_count > 0 && shadow.guard_count > 0, "native guards");
            for (unsigned i = 0; i < 2; ++i) {
                auto expected = test::reference_closest(g, input[i]);
                test::GeometryErrors errors;
                require(
                    test::compare_closest(g, input[i], expected, closest.hits[i], errors).empty(),
                    "transformed original-float geometry oracle");
                require(bool(shadow.occluded[i]) == !expected.empty(), "transformed shadow oracle");
                ++checked;
            }
        }
    GeometryData g;
    g.meshes = {{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 1, 2}, {0, 1, 2}}}};
    g.instances.emplace_back();
    auto b = build_metal_geometry(g);
    require(b.status == GeometryStatus::success, b.diagnostic);
    GeometryRay r{{.25f, .25f, 1}, {0, 0, -1}, .5f, 1.5f};
    auto hit = b.scene->closest_hits(std::span(&r, 1));
    require(hit.status == GeometryStatus::success && hit.hits[0].hit &&
                hit.hits[0].primitive_index == 0,
            "stable computed-distance ID tie");
    // docs/geometry.md: exact native interval endpoints have no stronger
    // inclusion promise. The helper's closed predicate cannot recover a
    // candidate that traversal never offers; characterize those endpoints.
    for (bool minimum : {true, false}) {
        r.t_min = minimum ? 1 : 0;
        r.t_max = minimum ? 2 : 1;
        hit = b.scene->closest_hits(std::span(&r, 1));
        const auto endpoint_shadow = b.scene->occlusion(std::span(&r, 1));
        require(hit.status == GeometryStatus::success &&
                    endpoint_shadow.status == GeometryStatus::success,
                "native endpoint characterization returns valid output");
        if (hit.hits[0].hit)
            require(hit.hits[0].primitive_index < 2 && hit.hits[0].t == 1,
                    "offered endpoint candidate retains original ID and distance");
        std::cout << "Exact native " << (minimum ? "minimum" : "maximum")
                  << " endpoint: closest=" << hit.hits[0].hit
                  << ", occluded=" << unsigned(endpoint_shadow.occluded[0]) << '\n';
    }
    r.t_min = std::nextafter(1.f, 0.f);
    r.t_max = std::nextafter(1.f, 2.f);
    hit = b.scene->closest_hits(std::span(&r, 1));
    require(hit.status == GeometryStatus::success && hit.hits[0].hit &&
                hit.hits[0].primitive_index == 0 && hit.hits[0].t == 1,
            "adjacent interior interval includes analytic t=1 without changing original bounds");
    r.t_min = std::nextafter(1.f, 2.f);
    r.t_max = 2;
    hit = b.scene->closest_hits(std::span(&r, 1));
    require(hit.status == GeometryStatus::success && !hit.hits[0].hit,
            "adjacent finite minimum excludes analytic t=1");
    r.t_min = 0;
    r.t_max = std::nextafter(1.f, 0.f);
    hit = b.scene->closest_hits(std::span(&r, 1));
    require(hit.status == GeometryStatus::success && !hit.hits[0].hit,
            "adjacent finite maximum excludes analytic t=1");
    r.t_max = .75f;
    hit = b.scene->closest_hits(std::span(&r, 1));
    require(hit.status == GeometryStatus::success && !hit.hits[0].hit,
            "finite maximum excludes surface");
    r.t_max = 1.5f;
    r.t_min = std::numeric_limits<float>::denorm_min();
    hit = b.scene->closest_hits(std::span(&r, 1));
    auto shadow = b.scene->occlusion(std::span(&r, 1));
    require(hit.status == GeometryStatus::failure && !hit.diagnostic.empty(),
            "subnormal endpoint invalid closest visible");
    require(shadow.status == GeometryStatus::failure && !shadow.diagnostic.empty(),
            "subnormal endpoint invalid shadow visible");
    g.meshes[0].positions[2].x = std::bit_cast<float>(std::uint32_t{512});
    b = build_metal_geometry(g);
    require(b.status == GeometryStatus::success, b.diagnostic);
    r.t_min = 0;
    hit = b.scene->closest_hits(std::span(&r, 1));
    shadow = b.scene->occlusion(std::span(&r, 1));
    require(hit.status == GeometryStatus::failure && !hit.diagnostic.empty(),
            "unsupported triangle arithmetic invalid closest visible");
    require(shadow.status == GeometryStatus::failure && !shadow.diagnostic.empty(),
            "unsupported triangle arithmetic invalid shadow visible");
    std::cout << "Validated " << checked
              << " regression/transformed rays across fresh native builds, ties, bounds and "
                 "invalid propagation\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "cpu") {
        cpu();
        return 0;
    }
    bool allow_no_device = false, allow_no_ray_tracing = false;
    if (argc >= 2 && std::string_view(argv[1]) == "metal") {
        for (int i = 2; i < argc; ++i) {
            auto flag = std::string_view(argv[i]);
            require(flag == "--allow-no-device" || flag == "--allow-no-ray-tracing",
                    "unexpected native flag");
            if (flag == "--allow-no-device")
                allow_no_device = true;
            else
                allow_no_ray_tracing = true;
        }
        return native(allow_no_device, allow_no_ray_tracing);
    }
    throw std::runtime_error("Expected cpu or metal [availability flags]");
} catch (const std::exception &e) {
    std::cerr << "Triangle candidate test failed: " << e.what() << '\n';
    return 1;
}
