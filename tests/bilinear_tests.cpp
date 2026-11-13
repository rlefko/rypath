// SPDX-License-Identifier: GPL-3.0-only

#include "bilinear.hpp"
#include "metal_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>

namespace {

using namespace rypath;

constexpr std::array<GeometryPoint, 4> saddle{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 1}}};
constexpr std::array<GeometryPoint, 4> diamond{{{0, 0, 0}, {1, 1, 0}, {-1, 1, 0}, {0, 2, 0}}};
constexpr GeometryTransform mirrored_shear{{-2, 0.25f, 0.5f, 9, 0, 2, 0.25f, -3, 0, 0, 4, 1}};
constexpr GeometryTransform translated_shear{{0x1.8b8006p+0f, -0x1.7193eep-4f, -0x1.87ea5ap-5f, 0,
                                              0x1.22b8p-4f, 0x1.a39604p+0f, -0x1.775ae6p-6f,
                                              0x1.028a18p+9f, 0x1.10bf8ep-2f, -0x1.2d1024p-2f,
                                              0x1.a3ea7ap+0f, 0x1.9712ep+7f}};

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}

GeometryMesh patch_mesh(const std::array<GeometryPoint, 4> &corners) {
    GeometryMesh mesh;
    mesh.positions.assign(corners.begin(), corners.end());
    mesh.patches.push_back({0, 1, 2, 3});
    return mesh;
}

GeometryData make_scene() {
    auto mixed = patch_mesh(saddle);
    mixed.positions.insert(mixed.positions.end(),
                           {{0, 0, 5.0f / 16}, {2, 0, 5.0f / 16}, {0, 2, 5.0f / 16}});
    mixed.triangles.push_back({4, 5, 6});
    return {{patch_mesh(saddle), patch_mesh(diamond), mixed},
            {{0, {}},
             {1, {{1, 0, 0, -4, 0, 1, 0, 0, 0, 0, 1, 0}}},
             {2, {{1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 0}}},
             {0, mirrored_shear}}};
}

void check_bounds(const std::array<GeometryPoint, 4> &corners, double min_jacobian,
                  double max_jacobian, std::string_view name) {
    const auto bounds = bilinear_jacobian_bounds(corners);
    // The fixture normals below are well conditioned. Their numerical margin
    // should cover the true extrema and stay within 16 parts per million.
    constexpr double relative_tolerance = 16e-6;
    require(bounds && (*bounds)[0] <= min_jacobian && (*bounds)[1] >= max_jacobian &&
                (*bounds)[0] >= min_jacobian * (1 - relative_tolerance) &&
                (*bounds)[1] <= max_jacobian * (1 + relative_tolerance),
            name);
}

int run_cpu_tests() {
    auto geometry = make_scene();
    require(validate_geometry(geometry).empty(), "mixed and patch-only meshes must validate");

    const std::array<GeometryPoint, 4> centered_saddle{
        {{0, 0, 0}, {1, 0, -0.5f}, {0, 1, -0.5f}, {1, 1, 0}}};
    check_bounds(diamond, 2, 2, "planar patch Jacobian");
    check_bounds(saddle, 1, std::sqrt(3.0), "saddle Jacobian minimum at corner");
    check_bounds(centered_saddle, 1, std::sqrt(1.5), "saddle Jacobian minimum at interior");

    for (const float scale : {std::ldexp(1.0f, -100), std::ldexp(1.0f, 100)}) {
        auto scaled = centered_saddle;
        for (GeometryPoint &p : scaled) {
            p = {p.x * scale, p.y * scale, p.z * scale};
        }
        const double area_scale = double{scale} * scale;
        check_bounds(scaled, area_scale, area_scale * std::sqrt(1.5),
                     "Jacobian bounds scale with patch area");
    }

    const auto check_invalid = [&](std::array<GeometryPoint, 4> corners, std::string_view name) {
        geometry.meshes[0] = patch_mesh(corners);
        require(validate_geometry(geometry).find("bilinear patch 0 has a singular or numerically "
                                                 "indeterminate Jacobian") != std::string::npos,
                name);
    };
    auto folded = saddle;
    folded[3] = {-1, -1, 0};
    check_invalid(folded, "fold with four nonzero corner Jacobians");
    auto collapsed = saddle;
    collapsed[3] = collapsed[2];
    check_invalid(collapsed, "collapsed edge");
    check_invalid({{{0, 0, 0},
                    {1, 1, 0},
                    {1, std::nextafter(1.0f, 2.0f), 0},
                    {2, std::nextafter(2.0f, 3.0f), 0}}},
                  "Jacobian below the float cancellation bound");

    geometry.meshes[0] = patch_mesh(saddle);
    geometry.meshes[0].patches[0][3] = 4;
    require(validate_geometry(geometry).find("bilinear patch 0 has an out-of-range vertex index") !=
                std::string::npos,
            "patch index validation");

    std::cout << "Bilinear geometry validation: full-domain Jacobian, range, folds, collapsed "
                 "edges, and patch indices\n";
    return 0;
}

struct ExpectedHit {
    std::uint32_t mesh;
    std::uint32_t instance;
    PrimitiveKind kind;
    double t;
    double u;
    double v;
};

// The unit saddle is z=xy, so substituting the ray into that implicit
// equation yields an independent quadratic directly in t.
std::optional<ExpectedHit> saddle_hit(const GeometryRay &ray) {
    const double x = ray.origin.x, y = ray.origin.y, z = ray.origin.z;
    const double dx = ray.direction.x, dy = ray.direction.y, dz = ray.direction.z;
    const double a = double{dx} * dy;
    const double b = double{dx} * y + double{dy} * x - dz;
    const double c = double{x} * y - z;
    std::optional<ExpectedHit> best;
    const auto consider = [&](double t) {
        const double u = std::fma(t, dx, x);
        const double v = std::fma(t, dy, y);
        if (t >= ray.t_min && t <= ray.t_max && u >= 0 && u <= 1 && v >= 0 && v <= 1 &&
            (!best || t < best->t)) {
            best = ExpectedHit{0, 0, PrimitiveKind::bilinear_patch, t, u, v};
        }
    };
    if (a == 0) {
        if (b != 0) {
            consider(-c / b);
        }
    } else {
        const double discriminant = std::fma(b, b, -4 * a * c);
        if (discriminant > 0) {
            const double q = -0.5 * (b + std::copysign(std::sqrt(discriminant), b));
            consider(q / a);
            consider(c / q);
        } else if (discriminant == 0) {
            consider(-0.5 * b / a);
        }
    }
    return best;
}

std::vector<GeometryRay> saddle_stress_rays() {
    std::mt19937 random(0x17de);
    // Exact dyadic inputs make the batch independent of how the standard
    // library implements uniform_real_distribution.
    const auto sample = [&]() { return static_cast<float>(random() >> 16) * 0x1p-16f; };
    std::vector<GeometryRay> rays{
        {{0x1.b75d72p-1f, -0x1.f822a8p-3f, -0x1.aff18ep-3f},
         {-0x1.43ce08p-13f, 0x1.6b937p-1f, 0x1.37c858p-1f},
         0,
         1},
        {{0x1.2581c8p-2f, 0x1.3f4feap+0f, 0x1.6dfbf4p-2f},
         {0, -0x1.3ea20ep+0f, -0x1.6d1858p-2f},
         0,
         1},
        {{-0x1.893e18p-2f, 0x1.02e8f6p-1f, -0x1.617ad4p-2f},
         {0x1.bea1d4p+0f, 0x1.bf10ap-1f, 0x1.81934ap+0f},
         0,
         1},
    };
    for (int i = 0; i < 4096; ++i) {
        const float u = 0.0625f + 0.875f * sample();
        const float v = 0.0625f + 0.875f * sample();
        const float t = 0.0625f + 0.875f * sample();
        const float du = 4 * sample() - 2;
        const float dy = 4 * sample() - 2;
        const float noise = 4 * sample() - 2;
        const float dx = i % 4 == 0 ? 0 : (i % 4 == 1 ? du * 0x1p-13f : du);
        const float dz = dx * v + dy * u + (i % 4 == 3 ? noise : noise * 0x1p-12f);
        rays.push_back({{u - t * dx, v - t * dy, u * v - t * dz},
                        {dx, dy, dz},
                        i % 5 == 0 ? 0.5f : 0.0f,
                        1.0f});
    }
    return rays;
}

void check_saddle_stress() {
    const auto build = build_metal_geometry({{patch_mesh(saddle)}, {{0, {}}}});
    require(build.status == GeometryStatus::success && build.scene && build.diagnostic.empty(),
            "Metal saddle oracle build: " + build.diagnostic);
    const auto rays = saddle_stress_rays();
    const auto closest = build.scene->closest_hits(rays);
    const auto any = build.scene->occlusion(rays);
    require(closest.status == GeometryStatus::success && closest.hits.size() == rays.size() &&
                closest.guard_count > 0 && any.status == GeometryStatus::success &&
                any.occluded.size() == rays.size() && any.guard_count > 0,
            "Metal saddle oracle queries: " + closest.diagnostic + " " + any.diagnostic);

    double max_t_error = 0;
    double max_uv_error = 0;
    constexpr double eps = std::numeric_limits<float>::epsilon();
    for (std::size_t i = 0; i < rays.size(); ++i) {
        const auto expected = saddle_hit(rays[i]);
        const auto &actual = closest.hits[i];
        const auto context = " at saddle oracle ray " + std::to_string(i);
        require(actual.hit == expected.has_value() && any.occluded[i] == expected.has_value(),
                "Hit flag disagrees with analytic oracle" + context);
        if (!expected) {
            continue;
        }
        require(actual.mesh_index == 0 && actual.instance_index == 0 &&
                    actual.primitive_index == 0 && actual.kind == expected->kind,
                "Hit IDs disagree with analytic oracle" + context);
        const double t_error = std::abs(actual.t - expected->t);
        const double uv_error = std::max(std::abs(actual.patch_uv[0] - expected->u),
                                         std::abs(actual.patch_uv[1] - expected->v));
        require(t_error <= 32 * eps && uv_error <= 64 * eps,
                "Hit parameters disagree with analytic oracle" + context);
        max_t_error = std::max(max_t_error, t_error);
        max_uv_error = std::max(max_uv_error, uv_error);
    }
    std::cout << "Metal bilinear saddle oracle: " << rays.size()
              << " rays per query type, maximum errors t=" << max_t_error << ", UV=" << max_uv_error
              << '\n';
}

void check_patch_payload() {
    auto curved = saddle;
    curved[3].z = 0.3f;
    auto mesh = patch_mesh(diamond);
    mesh.patches.push_back({1, 3, 0, 2});
    mesh.positions.insert(mesh.positions.end(), {{0, 0, 0.5f}, {1, 0, 0.5f}, {0, 1, 0.5f}});
    mesh.triangles.push_back({4, 5, 6});
    const auto build = build_metal_geometry(
        {{mesh, patch_mesh(curved)}, {{0, {}}, {0, {}}, {1, translated_shear}}});
    require(build.status == GeometryStatus::success && build.scene && build.diagnostic.empty(),
            "Metal patch payload build: " + build.diagnostic);
    constexpr std::array rays{
        GeometryRay{{0.25f, 1, 1}, {0, 0, -1}},
        GeometryRay{{0.125f, 0.5f, 1}, {0, 0, -1}},
        GeometryRay{{0x1.e311d2p-2f, 0x1.030d7cp+9f, 0x1.9a6adp+7f},
                    {0x1.87ea5ap-5f, 0x1.775ae6p-6f, -0x1.a3ea7ap+0f},
                    0,
                    2},
    };
    const auto closest = build.scene->closest_hits(rays);
    const auto any = build.scene->occlusion(rays);
    require(closest.status == GeometryStatus::success && closest.hits.size() == rays.size() &&
                any.status == GeometryStatus::success && any.occluded.size() == rays.size() &&
                std::ranges::all_of(any.occluded, [](std::uint8_t hit) { return hit == 1; }),
            "Metal patch payload queries: " + closest.diagnostic + " " + any.diagnostic);
    const auto &[tie, triangle, affine] =
        std::tie(closest.hits[0], closest.hits[1], closest.hits[2]);
    require(tie.hit && tie.kind == PrimitiveKind::bilinear_patch && tie.t == 1 &&
                tie.mesh_index == 0 && tie.instance_index < 2 && tie.primitive_index < 2,
            "coincident patch identity");
    const std::array<float, 2> expected_uv =
        tie.primitive_index == 0 ? std::array{0.625f, 0.375f} : std::array{0.375f, 0.375f};
    require(tie.patch_uv == expected_uv, "coincident patch coordinates must match the returned ID");
    require(triangle.hit && triangle.kind == PrimitiveKind::triangle && triangle.t == 0.5f &&
                triangle.mesh_index == 0 && triangle.instance_index < 2 &&
                triangle.primitive_index == 0,
            "closer native triangle must supply its own identity");
    // Double-precision affine inversion gives this reference for the stored
    // world ray. Native float transforms can round more than the local solver.
    constexpr double affine_tolerance = 256 * std::numeric_limits<float>::epsilon();
    require(affine.hit && affine.kind == PrimitiveKind::bilinear_patch && affine.mesh_index == 1 &&
                affine.instance_index == 2 && affine.primitive_index == 0 &&
                std::abs(affine.t - 0.999996696) <= affine_tolerance &&
                std::abs(affine.patch_uv[0] - 0.375000432) <= affine_tolerance &&
                std::abs(affine.patch_uv[1] - 0.625008483) <= affine_tolerance,
            "patch UV must survive a non-dyadic affine transform");
}

void check_patch_edges() {
    const GeometryPoint start{2, 0, 1};
    const GeometryPoint end{1, 3, -2};
    GeometryMesh mesh;
    mesh.positions = {{-1, 0, -1}, start, {0, 3, 2}, end, {4, -1, 2}, {5, 4, 1}};
    // Reverse the shared edge in the second patch's tensor parameterization.
    mesh.patches = {{0, 1, 2, 3}, {3, 5, 1, 4}};
    const auto build = build_metal_geometry({{mesh}, {{0, {}}}});
    require(build.status == GeometryStatus::success && build.scene && build.diagnostic.empty(),
            "Metal shared patch edge build: " + build.diagnostic);
    std::vector<GeometryRay> rays{
        {{0x1.9b8p+1f, 0x1.34p+2f, -0x1.6fcp+2f},
         {-0x1.66p+0f, -0x1.10cp+2f, 0x1.8c8p+2f},
         0.9999f,
         1.0001f},
    };
    std::vector<float> edge_parameters{47.0f / 256};
    std::mt19937 random(3355);
    const auto direction_component = [&]() {
        return static_cast<float>(static_cast<int>(random() % 4097) - 2048) / 256;
    };
    for (int i = 0; i < 8192; ++i) {
        const float v = static_cast<float>(random() % 257) / 256;
        const GeometryPoint target{2 - v, 3 * v, 1 - 3 * v};
        GeometryPoint direction{};
        do {
            direction = {direction_component(), direction_component(), direction_component()};
        } while (direction.x == 0 && direction.y == 0 && direction.z == 0);
        // Both the edge point and the ray origin are exact binary fractions.
        rays.push_back({{target.x - direction.x, target.y - direction.y, target.z - direction.z},
                        direction,
                        0.9999f,
                        1.0001f});
        edge_parameters.push_back(v);
    }
    const auto closest = build.scene->closest_hits(rays);
    const auto any = build.scene->occlusion(rays);
    require(closest.status == GeometryStatus::success && closest.hits.size() == rays.size() &&
                any.status == GeometryStatus::success && any.occluded.size() == rays.size(),
            "Metal shared patch edge queries: " + closest.diagnostic + " " + any.diagnostic);
    constexpr float tolerance = 32 * std::numeric_limits<float>::epsilon();
    for (std::size_t i = 0; i < rays.size(); ++i) {
        const auto &hit = closest.hits[i];
        const auto &r = rays[i];
        const float v = edge_parameters[i];
        const auto label = " at shared patch edge ray " + std::to_string(i);
        require(hit.hit && any.occluded[i] == 1 && hit.kind == PrimitiveKind::bilinear_patch &&
                    hit.instance_index == 0 && hit.mesh_index == 0 && hit.primitive_index < 2,
                "Exact edge point must hit" + label);
        require(hit.t <= 1 + tolerance, "Exact edge root is in range" + label);
        const auto &indices = mesh.patches[hit.primitive_index];
        const double u_hit = hit.patch_uv[0];
        const double v_hit = hit.patch_uv[1];
        const std::array weights{(1 - u_hit) * (1 - v_hit), u_hit * (1 - v_hit),
                                 (1 - u_hit) * v_hit, u_hit * v_hit};
        std::array<double, 3> point{};
        for (std::size_t k = 0; k < weights.size(); ++k) {
            const auto &p = mesh.positions[indices[k]];
            point[0] += weights[k] * p.x;
            point[1] += weights[k] * p.y;
            point[2] += weights[k] * p.z;
        }
        const double t = hit.t;
        require(std::abs(point[0] - std::fma(t, r.direction.x, r.origin.x)) <= tolerance &&
                    std::abs(point[1] - std::fma(t, r.direction.y, r.origin.y)) <= tolerance &&
                    std::abs(point[2] - std::fma(t, r.direction.z, r.origin.z)) <= tolerance,
                "Edge hit must reconstruct on the ray" + label);
        if (std::abs(t - 1) <= tolerance) {
            const float expected_u = hit.primitive_index == 0 ? 1 : 0;
            const float expected_v = hit.primitive_index == 0 ? v : 1 - v;
            require(std::abs(u_hit - expected_u) <= tolerance &&
                        std::abs(v_hit - expected_v) <= tolerance,
                    "Exact edge hit UV must match the returned patch" + label);
        }
    }

    const auto planar = build_metal_geometry({{patch_mesh(diamond)}, {{0, {}}}});
    require(planar.status == GeometryStatus::success && planar.scene && planar.diagnostic.empty(),
            "Metal outer patch edge build: " + planar.diagnostic);
    const float inside = std::nextafter(0.5f, 0.0f);
    const float outside = std::nextafter(0.5f, 1.0f);
    const std::array outer{
        GeometryRay{{inside, 0.5f, 1}, {0, 0, -1}},
        GeometryRay{{0.5f, 0.5f, 1}, {0, 0, -1}},
        GeometryRay{{outside, 0.5f, 1}, {0, 0, -1}},
        GeometryRay{{1, std::nextafter(1.0f, 0.0f), 1}, {0, 0, -1}},
        GeometryRay{{1, 1, 1}, {0, 0, -1}},
        GeometryRay{{1, std::nextafter(1.0f, 2.0f), 1}, {0, 0, -1}},
    };
    constexpr std::array expected{true, true, false, false, true, false};
    const auto outer_closest = planar.scene->closest_hits(outer);
    const auto outer_any = planar.scene->occlusion(outer);
    require(outer_closest.status == GeometryStatus::success &&
                outer_closest.hits.size() == outer.size() &&
                outer_any.status == GeometryStatus::success &&
                outer_any.occluded.size() == outer.size(),
            "Metal outer patch edge queries: " + outer_closest.diagnostic + " " +
                outer_any.diagnostic);
    for (std::size_t i = 0; i < outer.size(); ++i) {
        require(outer_closest.hits[i].hit == expected[i] && outer_any.occluded[i] == expected[i],
                "Patch edge membership at outer ray " + std::to_string(i));
    }
    std::cout << "Metal bilinear shared edges: " << rays.size() << " exact edge rays and "
              << outer.size() << " outer edge cases per query type\n";
}

struct Fixture {
    std::string_view name;
    GeometryRay ray;
    std::optional<ExpectedHit> hit;
};

constexpr PrimitiveKind patch = PrimitiveKind::bilinear_patch;
constexpr PrimitiveKind triangle = PrimitiveKind::triangle;
constexpr GeometryPoint two_root_origin{0, 0, -3.0f / 16};
constexpr GeometryPoint two_root_direction{1, 1, 1};

constexpr GeometryPoint transform(GeometryPoint p, float w) {
    const auto &m = mirrored_shear.values;
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3] * w,
            m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7] * w,
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11] * w};
}

// The saddle is P(u,v)=(u,v,uv). For the diagonal ray, u=v=t and
// t^2=t-3/16, giving the two roots 1/4 and 3/4. Vertical rays read u and
// v directly from the origin. The planar diamond has x=u-v, y=u+v.
constexpr std::array fixtures{
    Fixture{"closest of two roots",
            {two_root_origin, two_root_direction, 0, 1},
            ExpectedHit{0, 0, patch, 0.25, 0.25, 0.25}},
    Fixture{"second root inside clipped interval",
            {two_root_origin, two_root_direction, 0.5f, 1},
            ExpectedHit{0, 0, patch, 0.75, 0.75, 0.75}},
    Fixture{"interval between roots",
            {two_root_origin, two_root_direction, 0.375f, 0.625f},
            std::nullopt},
    Fixture{
        "double root", {{0, 0, -0.25f}, {1, 1, 1}, 0, 1}, ExpectedHit{0, 0, patch, 0.5, 0.5, 0.5}},
    Fixture{
        "box only with negative discriminant", {{0, 0, -0.3125f}, {1, 1, 1}, 0, 1}, std::nullopt},
    Fixture{"linear root, asymmetric coordinates",
            {{0.25f, 0.75f, 1.1875f}, {0, 0, -2}},
            ExpectedHit{0, 0, patch, 0.5, 0.25, 0.75}},
    Fixture{"nearly parallel to ruling with an extraneous root",
            {{0.25f, 0.125f, 0.03125f + 0x1p-16f}, {0, 1, 0.25f - 0x1p-14f}},
            ExpectedHit{0, 0, patch, 0.25, 0.25, 0.375}},
    Fixture{"positive hit near the ray origin",
            {{0.25f, 0.75f, 0.1875f + 0x1p-20f}, {0, 0, -1}},
            ExpectedHit{0, 0, patch, 0x1p-20, 0.25, 0.75}},
    Fixture{"coincident ruling has no isolated hit",
            {{0.5f, 0, 0}, {0, 1, 0.5f}, 0.125f, 0.75f},
            std::nullopt},
    Fixture{"planar back hit",
            {{-3.75f, 1, -0.5f}, {0, 0, 2}},
            ExpectedHit{1, 1, patch, 0.25, 0.625, 0.375}},
    Fixture{"planar box only", {{-3.25f, 0.25f, 1}, {0, 0, -1}}, std::nullopt},
    Fixture{"planar coplanar ray", {{-4, 1, 0}, {1, 0, 0}, 0.125f, 0.25f}, std::nullopt},
    Fixture{"mixed mesh patch wins",
            {{4, 0, -3.0f / 16}, {1, 1, 1}, 0, 1},
            ExpectedHit{2, 2, patch, 0.25, 0.25, 0.25}},
    Fixture{"mixed mesh triangle wins",
            {{4, 0, -3.0f / 16}, {1, 1, 1}, 0.375f, 1},
            ExpectedHit{2, 2, triangle, 0.5, 0.25, 0.25}},
    Fixture{"mixed mesh selects second root",
            {{4, 0, -3.0f / 16}, {1, 1, 1}, 0.625f, 1},
            ExpectedHit{2, 2, patch, 0.75, 0.75, 0.75}},
    Fixture{"mirrored shear, non-unit ray",
            {transform(two_root_origin, 1), transform({2, 2, 2}, 0), 0, 0.5f},
            ExpectedHit{0, 3, patch, 0.125, 0.25, 0.25}},
};

int run_metal_tests(bool allow_no_device, bool allow_no_ray_tracing) {
    const auto build = build_metal_geometry(make_scene());
    if ((build.status == GeometryStatus::no_device && allow_no_device) ||
        (build.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
        require(build.scene == nullptr && !build.diagnostic.empty(), "malformed availability skip");
        std::cout << "Skipping native bilinear geometry: " << build.diagnostic << '\n';
        return 77;
    }
    require(build.status == GeometryStatus::success && build.scene && build.diagnostic.empty(),
            "Metal bilinear geometry build: " + build.diagnostic);
    std::array<GeometryRay, fixtures.size()> rays{};
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        rays[i] = fixtures[i].ray;
    }
    const auto closest = build.scene->closest_hits(rays);
    const auto any = build.scene->occlusion(rays);
    require(closest.status == GeometryStatus::success && closest.diagnostic.empty(),
            "Metal bilinear closest query: " + closest.diagnostic);
    require(any.status == GeometryStatus::success && any.diagnostic.empty(),
            "Metal bilinear occlusion query: " + any.diagnostic);
    require(closest.hits.size() == rays.size() && any.occluded.size() == rays.size() &&
                closest.guard_count > 0 && any.guard_count > 0,
            "malformed native query result");

    double max_t_error = 0;
    double max_uv_error = 0;
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const auto &fixture = fixtures[i];
        const auto &result = closest.hits[i];
        require(result.hit == fixture.hit.has_value(),
                std::string{fixture.name} + ": closest hit flag");
        require(any.occluded[i] == fixture.hit.has_value(),
                std::string{fixture.name} + ": occlusion flag");
        if (!fixture.hit) {
            continue;
        }
        const auto expected = *fixture.hit;
        require(result.kind == expected.kind && result.instance_index == expected.instance &&
                    result.mesh_index == expected.mesh && result.primitive_index == 0,
                std::string{fixture.name} + ": primitive kind and IDs");
        const double u = result.kind == patch ? result.patch_uv[0] : result.barycentric[1];
        const double v = result.kind == patch ? result.patch_uv[1] : result.barycentric[2];
        const double t_error = std::abs(result.t - expected.t);
        const double uv_error = std::max(std::abs(u - expected.u), std::abs(v - expected.v));
        constexpr double eps = std::numeric_limits<float>::epsilon();
        require(t_error <= 32 * eps * std::max(1.0, std::abs(expected.t)) && std::isfinite(u) &&
                    std::isfinite(v) && uv_error <= 64 * eps,
                std::string{fixture.name} + ": intersection parameter error");
        max_t_error = std::max(max_t_error, t_error);
        max_uv_error = std::max(max_uv_error, uv_error);
    }

    std::cout << "Metal bilinear geometry on " << build.device.name << ": " << rays.size()
              << " analytic rays per query type, maximum errors t=" << max_t_error
              << ", UV=" << max_uv_error << '\n';
    check_saddle_stress();
    check_patch_payload();
    check_patch_edges();
    return 0;
}

} // namespace

int main(int argc, char *argv[]) try {
    if (argc == 2 && std::string_view{argv[1]} == "cpu") {
        return run_cpu_tests();
    }
    if (argc >= 2 && std::string_view{argv[1]} == "metal") {
        bool allow_no_device = false;
        bool allow_no_ray_tracing = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (flag == "--allow-no-device" && !allow_no_device) {
                allow_no_device = true;
            } else if (flag == "--allow-no-ray-tracing" && !allow_no_ray_tracing) {
                allow_no_ray_tracing = true;
            } else {
                throw std::runtime_error("unexpected native test flag");
            }
        }
        return run_metal_tests(allow_no_device, allow_no_ray_tracing);
    }
    std::cerr << "Expected cpu or metal [--allow-no-device] [--allow-no-ray-tracing]\n";
    return 1;
} catch (const std::exception &error) {
    std::cerr << "Bilinear geometry test failed: " << error.what() << '\n';
    return 1;
}
