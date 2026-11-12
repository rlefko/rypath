// SPDX-License-Identifier: GPL-3.0-only

#include "geometry_reference.hpp"
#include "metal_geometry.hpp"
#include "metal_geometry_limits.hpp"

#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace rypath;
using namespace rypath::test;

constexpr float inf = std::numeric_limits<float>::infinity();
constexpr float nan = std::numeric_limits<float>::quiet_NaN();

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}

void require_diagnostic(std::string_view diagnostic, std::string_view expected) {
    if (diagnostic.find(expected) == std::string_view::npos) {
        throw std::runtime_error("expected diagnostic containing '" + std::string{expected} +
                                 "', got '" + std::string{diagnostic} + "'");
    }
}

GeometryTransform translation(float x, float y, float z) {
    return {{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}};
}

GeometryData make_geometry() {
    GeometryData geometry;
    // Empty mesh 0 precedes the first active instance. The unused position at
    // index 0 of mesh 1 makes vertex and primitive ordinals distinct.
    geometry.meshes = {
        {},
        {{{4, 4, 4}, {0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, -1}, {2, 0, -1}, {0, 2, -1}},
         {{1, 2, 3}, {4, 5, 6}}},
        {{{0, 0, 0},
          {2, 0, 0},
          {2, 2, 0},
          {0, 2, 0},
          {3, 0, 0},
          {5, 0, 0},
          {5, 2, 0},
          {3, 2, 0},
          {4, 1, 0}},
         {{0, 1, 2}, {0, 2, 3}, {4, 5, 8}, {5, 6, 8}, {6, 7, 8}, {7, 4, 8}}},
    };
    geometry.instances = {
        {0, {}},
        {1, {}},
        {1, translation(0, 0, -2)},
        {1, {{0, -2, 0.25f, -4, 0.5f, 0, 0, -2, 0.25f, 0, 4, 5}}},
        {1, {{-1, 0, 0, 5, 0, 2, 0, -3, 0, 0, 0.5f, 2}}},
        {2, translation(8, 0, 0)},
        {1, translation(6, 4, 0)},
        {1, translation(6, 4, 0)},
    };
    return geometry;
}

struct AnalyticRay {
    std::string_view name;
    GeometryRay ray;
    std::uint32_t mesh;
    std::uint32_t instance;
    std::uint32_t primitive;
    double t;
    std::size_t candidates = 1;
};

constexpr float edge_step = 1.0f / 4096.0f;
constexpr std::array analytic_rays{
    AnalyticRay{"non-unit front", {{0.25f, 0.75f, 3}, {0, 0, -2}}, 1, 1, 0, 1.5},
    AnalyticRay{
        "direction and interval scaled", {{0.25f, 0.75f, 3}, {0, 0, -4}, 0.5f, 1}, 1, 1, 0, 0.75},
    AnalyticRay{"back", {{0.25f, 0.75f, -0.25f}, {0, 0, 2}}, 1, 1, 0, 0.125},
    AnalyticRay{"negative zero axis", {{0.25f, 0.75f, 3}, {-0.0f, 0, -2}}, 1, 1, 0, 1.5},
    AnalyticRay{"t_min excludes nearest primitive",
                {{0.25f, 0.75f, 3}, {0, 0, -2}, 1.75f, 2.25f},
                1,
                1,
                1,
                2},
    AnalyticRay{"t_min excludes nearest instance",
                {{0.25f, 0.75f, 3}, {0, 0, -2}, 2.25f, 2.75f},
                1,
                2,
                0,
                2.5},
    AnalyticRay{
        "t_max excludes every hit", {{0.25f, 0.75f, 3}, {0, 0, -2}, 0, 1.25f}, 0, 0, 0, 0, 0},
    AnalyticRay{"t_min excludes every hit", {{0.25f, 0.75f, 3}, {0, 0, -2}, 4, 8}, 0, 0, 0, 0, 0},
    AnalyticRay{"behind", {{0.25f, 0.75f, 3}, {0, 0, 2}}, 0, 0, 0, 0, 0},
    AnalyticRay{"parallel above", {{0.25f, 0.75f, 3}, {1, 0, 0}}, 0, 0, 0, 0, 0},
    AnalyticRay{"coplanar isolated", {{8.25f, 0.75f, 0}, {1, 0, 0}, 0.125f, 0.25f}, 0, 0, 0, 0, 0},
    AnalyticRay{"axis-aligned exterior", {{-0.25f, 0.75f, 3}, {0, 0, -2}}, 0, 0, 0, 0, 0},
    AnalyticRay{"grazing", {{-0.75f, 0.75f, 1.0f / 64}, {1, 0, -1.0f / 64}, 0.5f, 2}, 1, 1, 0, 1},
    AnalyticRay{"rotated sheared nonuniform",
                {{-5.6875f, -1.5f, 8.0625f}, {0.125f, -0.25f, -2}},
                1,
                3,
                0,
                1.5},
    AnalyticRay{
        "rotated sheared back", {{-5.4375f, -2, 4.0625f}, {-0.125f, 0.25f, 2}}, 1, 3, 0, 0.5},
    AnalyticRay{"mirrored", {{4.75f, -1.5f, 5}, {0, 0, -2}}, 1, 4, 0, 1.5},
    AnalyticRay{"mirrored back", {{4.75f, -1.5f, 1.75f}, {0, 0, 2}}, 1, 4, 0, 0.125},
    AnalyticRay{"shared edge", {{8.75f, 0.75f, 3}, {0, 0, -2}}, 2, 5, 0, 1.5, 2},
    AnalyticRay{
        "shared edge lower side", {{8.75f, 0.75f - edge_step, 3}, {0, 0, -2}}, 2, 5, 0, 1.5},
    AnalyticRay{
        "shared edge upper side", {{8.75f, 0.75f + edge_step, 3}, {0, 0, -2}}, 2, 5, 1, 1.5},
    AnalyticRay{"interior shared vertex", {{12, 1, 3}, {0, 0, -2}}, 2, 5, 2, 1.5, 4},
    AnalyticRay{"outside patch edge", {{8 - edge_step, 0.75f, 3}, {0, 0, -2}}, 0, 0, 0, 0, 0},
    AnalyticRay{"coincident instances", {{6.25f, 4.75f, 3}, {0, 0, -2}}, 1, 6, 0, 1.5, 2},
};

constexpr GeometryRay transformed_ray{{-5.6875f, -1.5f, 8.0625f}, {0.125f, -0.25f, -2}};

std::uint32_t next_sample(std::uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

std::array<GeometryRay, 257> make_batch() {
    std::array<GeometryRay, 257> rays{};
    std::uint32_t state = 0x94f82e31;
    for (std::size_t index = 0; index < rays.size(); ++index) {
        const float x = static_cast<float>(16 + (next_sample(state) & 31)) / 64;
        const float y = static_cast<float>(24 + (next_sample(state) & 31)) / 64;
        const float z = static_cast<float>(32 + (next_sample(state) & 127)) / 32;
        const float dz = -static_cast<float>(1 + (next_sample(state) & 7)) / 4;
        const bool inside = index % 2 == 0;
        rays[index] = {{inside ? x : -x, y, z}, {0, 0, dz}};
    }
    return rays;
}

void check_geometry_validation(const GeometryData &geometry) {
    require(validate_geometry(geometry).empty(), "analytic geometry should be valid");
    require(validate_geometry({}).empty(), "empty geometry should be valid");
    const auto check = [&](auto mutate, std::string_view diagnostic) {
        auto invalid = geometry;
        mutate(invalid);
        require_diagnostic(validate_geometry(invalid), diagnostic);
    };
    check([](GeometryData &data) { data.meshes[1].positions[0].x = inf; }, "Mesh 1 vertex 0");
    check([](GeometryData &data) { data.meshes[1].triangles[0][1] = 42; },
          "triangle 0 has an out-of-range vertex index");
    check([](GeometryData &data) { data.meshes[1].triangles[0][1] = 1; },
          "triangle 0 has zero area");
    check([](GeometryData &data) { data.meshes[1].positions[3] = {1, 0, 0}; },
          "triangle 0 has zero area");
    check([](GeometryData &data) { data.instances[1].mesh_index = 42; },
          "Instance 1 has an out-of-range mesh index");
    check([](GeometryData &data) { data.instances[1].transform.values[3] = nan; },
          "Instance 1 has a nonfinite transform");
    check([](GeometryData &data) { data.instances[1].transform.values[0] = 0; },
          "Instance 1 has a singular transform");

    const float huge = std::ldexp(1.0f, 100);
    auto difficult = geometry;
    difficult.meshes[1].positions[1] = {huge, huge, 0};
    difficult.meshes[1].positions[2] = {0, 0, 0};
    difficult.meshes[1].positions[3] = {1, 0, 0};
    require(validate_geometry(difficult).empty(), "nonzero area after large subtraction must pass");
    difficult.meshes[1].positions[2] = {1, 1, 0};
    difficult.meshes[1].positions[3] = {2, 2, 0};
    require_diagnostic(validate_geometry(difficult), "triangle 0 has zero area");

    const float tiny = std::ldexp(1.0f, -100);
    difficult = geometry;
    difficult.meshes[1].positions[2] = {tiny, 0, 0};
    difficult.meshes[1].positions[3] = {0, tiny, 0};
    difficult.instances[1].transform.values = {tiny, 0, 0, 0, 0, tiny, 0, 0, 0, 0, tiny, 0};
    require(validate_geometry(difficult).empty(), "nonzero tiny triangle and transform must pass");
    difficult.instances[1].transform.values = {huge, huge, 0, 0, huge, huge, 1, 0, 0, 1, 1, 0};
    require(validate_geometry(difficult).empty(),
            "nonzero determinant after cancellation must pass");
    // In the first matrix, row three is the sum of rows one and two. The
    // second determinant is exactly one. Summing just the rounded high parts
    // gives -8 and 0 respectively, so both require the FMA residuals.
    constexpr float n = 262144;
    difficult.instances[1].transform.values = {n,     n + 1, n + 1,     0,         n + 2,     n,
                                               n + 2, 0,     2 * n + 2, 2 * n + 1, 2 * n + 3, 0};
    require_diagnostic(validate_geometry(difficult), "Instance 1 has a singular transform");
    difficult.instances[1].transform.values = {n, n, n + 1,     0,     n,     n + 1,
                                               n, 0, 2 * n - 1, 2 * n, 2 * n, 0};
    require(validate_geometry(difficult).empty(), "unit determinant needs the FMA residual");
}

void check_native_limits() {
    constexpr std::size_t primitives = 1u << 28;
    constexpr std::size_t instances = 1u << 24;
    require(metal::fits_primitive_limit(primitives) && !metal::fits_primitive_limit(primitives + 1),
            "standard Metal BLAS primitive limit");
    require(metal::fits_instance_limit(instances) && !metal::fits_instance_limit(instances + 1),
            "standard Metal TLAS instance limit");
}

void check_ray_validation() {
    const GeometryRay good = analytic_rays.front().ray;
    require(validate_geometry_rays({}).empty(), "empty ray batch should be valid");
    require(validate_geometry_rays(std::array{good}).empty(), "infinite t_max should be valid");
    const auto check = [&](auto mutate, std::string_view diagnostic) {
        auto invalid = good;
        mutate(invalid);
        require_diagnostic(validate_geometry_rays(std::array{good, invalid}), diagnostic);
    };
    check([](GeometryRay &ray) { ray.origin.x = nan; },
          "Ray 1 has a nonfinite origin or direction");
    check([](GeometryRay &ray) { ray.direction.z = inf; },
          "Ray 1 has a nonfinite origin or direction");
    check([](GeometryRay &ray) { ray.direction = {-0.0f, 0, 0}; }, "Ray 1 has zero direction");
    check([](GeometryRay &ray) { ray.t_min = -1; }, "Ray 1 has an invalid interval");
    check([](GeometryRay &ray) { ray.t_min = inf; }, "Ray 1 has an invalid interval");
    check([](GeometryRay &ray) { ray.t_min = nan; }, "Ray 1 has an invalid interval");
    check([](GeometryRay &ray) { ray.t_max = nan; }, "Ray 1 has an invalid interval");
    check([](GeometryRay &ray) { ray.t_max = -inf; }, "Ray 1 has an invalid interval");
    check(
        [](GeometryRay &ray) {
            ray.t_min = 2;
            ray.t_max = 1;
        },
        "Ray 1 has an invalid interval");
    check([](GeometryRay &ray) { ray.t_max = ray.t_min; }, "Ray 1 has an invalid interval");
}

void check_analytic(const GeometryData &geometry) {
    for (const auto &fixture : analytic_rays) {
        const auto actual = reference_closest(geometry, fixture.ray);
        require(actual.size() == fixture.candidates, fixture.name);
        if (!actual.empty()) {
            const auto &hit = actual.front();
            require(hit.mesh_index == fixture.mesh && hit.instance_index == fixture.instance &&
                        hit.primitive_index == fixture.primitive && hit.t == fixture.t,
                    fixture.name);
        }
    }
    const auto asymmetric = reference_closest(geometry, transformed_ray);
    require(asymmetric.size() == 1 &&
                asymmetric.front().barycentric == std::array{0.5, 0.125, 0.375},
            "asymmetric transformed barycentrics");
}

GeometryHit make_hit(const ReferenceHit &hit) {
    return {true,
            hit.mesh_index,
            hit.instance_index,
            hit.primitive_index,
            static_cast<float>(hit.t),
            {static_cast<float>(hit.barycentric[0]), static_cast<float>(hit.barycentric[1]),
             static_cast<float>(hit.barycentric[2])}};
}

void check_comparators(const GeometryData &geometry) {
    const auto expected = reference_closest(geometry, transformed_ray);
    const auto valid = make_hit(expected.front());
    GeometryErrors errors;
    require(compare_closest(geometry, transformed_ray, expected, valid, errors).empty(),
            "comparator rejected an exact hit");
    require(compare_closest(geometry, transformed_ray, {}, GeometryHit{}, errors).empty(),
            "comparator rejected a miss");
    require_diagnostic(compare_closest(geometry, transformed_ray, {}, valid, errors),
                       "expected a miss");
    const auto corrupt = [&](auto mutate, std::string_view diagnostic) {
        auto actual = valid;
        mutate(actual);
        require_diagnostic(compare_closest(geometry, transformed_ray, expected, actual, errors),
                           diagnostic);
    };
    corrupt([](GeometryHit &hit) { hit.hit = false; }, "expected a hit");
    corrupt([](GeometryHit &hit) { ++hit.mesh_index; }, "hit IDs");
    corrupt([](GeometryHit &hit) { ++hit.instance_index; }, "hit IDs");
    corrupt([](GeometryHit &hit) { ++hit.primitive_index; }, "hit IDs");
    corrupt([](GeometryHit &hit) { hit.t += 0.001f; }, "hit t");
    corrupt([](GeometryHit &hit) { hit.t = nan; }, "hit t");
    corrupt([](GeometryHit &hit) { std::swap(hit.barycentric[1], hit.barycentric[2]); },
            "barycentric");
    corrupt([](GeometryHit &hit) { hit.barycentric[0] = nan; }, "barycentric");
    corrupt(
        [](GeometryHit &hit) {
            for (auto &weight : hit.barycentric) {
                weight += 7e-6f;
            }
        },
        "reconstructed world hit position");
    require(compare_occlusion(false, 0).empty() && compare_occlusion(true, 1).empty(),
            "occlusion comparator rejected correct flags");
    require_diagnostic(compare_occlusion(true, 0), "expected occlusion");
    require_diagnostic(compare_occlusion(false, 1), "expected occlusion");
    require_diagnostic(compare_occlusion(true, 2), "expected occlusion");
}

bool may_skip(GeometryStatus status, bool allow_no_device, bool allow_no_ray_tracing) {
    return (status == GeometryStatus::no_device && allow_no_device) ||
           (status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing);
}

void check_availability_policy() {
    require(may_skip(GeometryStatus::no_device, true, false), "missing device opt-in must skip");
    require(may_skip(GeometryStatus::no_ray_tracing, false, true), "missing RT opt-in must skip");
    require(!may_skip(GeometryStatus::no_device, false, true),
            "missing RT opt-in cannot skip device");
    require(!may_skip(GeometryStatus::no_ray_tracing, true, false), "device opt-in cannot skip RT");
    require(!may_skip(GeometryStatus::failure, true, true), "execution failure cannot skip");
    require(!may_skip(GeometryStatus::not_built, true, true), "wrong backend cannot skip");
}

int run_cpu_tests() {
    const auto geometry = make_geometry();
    check_geometry_validation(geometry);
    check_native_limits();
    check_ray_validation();
    check_analytic(geometry);
    check_comparators(geometry);
    check_availability_policy();
    const auto batch = make_batch();
    require(validate_geometry_rays(batch).empty(), "deterministic batch should be valid");
    for (std::size_t index = 0; index < batch.size(); ++index) {
        const auto hits = reference_closest(geometry, batch[index]);
        require(hits.size() == (index % 2 == 0 ? 1u : 0u), "deterministic batch oracle count");
        if (!hits.empty()) {
            require(hits.front().mesh_index == 1 && hits.front().instance_index == 1 &&
                        hits.front().primitive_index == 0,
                    "deterministic batch oracle IDs");
        }
    }
    std::cout << "CPU geometry reference: " << analytic_rays.size() << " analytic rays, "
              << batch.size() << " fixed-seed rays, validation and comparator checks\n";
    return 0;
}

template <class Result> void require_success(const Result &result, std::string_view context) {
    if (result.status != GeometryStatus::success || !result.diagnostic.empty()) {
        throw std::runtime_error(std::string{context} + ": " + result.diagnostic);
    }
}

struct NativeEvidence {
    GeometryErrors errors;
    std::size_t rays = 0;
    std::size_t closest_guards = 0;
    std::size_t occlusion_guards = 0;
};

void compare_batch(MetalGeometryScene &scene, const GeometryData &geometry,
                   std::span<const GeometryRay> rays, std::string_view name,
                   NativeEvidence &evidence) {
    const auto closest = scene.closest_hits(rays);
    const auto any = scene.occlusion(rays);
    require_success(closest, name);
    require_success(any, name);
    require(closest.hits.size() == rays.size() && any.occluded.size() == rays.size(),
            "unexpected native batch length");
    require(closest.guard_count > 0 && any.guard_count > 0, "missing native output guard checks");
    for (std::size_t i = 0; i < rays.size(); ++i) {
        const auto expected = reference_closest(geometry, rays[i]);
        const auto closest_error =
            compare_closest(geometry, rays[i], expected, closest.hits[i], evidence.errors);
        const auto any_error = compare_occlusion(!expected.empty(), any.occluded[i]);
        if (!closest_error.empty() || !any_error.empty()) {
            const std::string ray_name =
                name == "analytic" ? std::string{analytic_rays[i].name} : std::to_string(i);
            throw std::runtime_error(std::string{name} + " ray " + ray_name + ": " +
                                     (closest_error.empty() ? any_error : closest_error));
        }
    }
    evidence.rays += rays.size();
    evidence.closest_guards += closest.guard_count;
    evidence.occlusion_guards += any.guard_count;
}

struct BoundaryCase {
    std::string_view name;
    GeometryRay query;
    GeometryRay reference;
};

void characterize_boundary(MetalGeometryScene &scene, const GeometryData &geometry,
                           const BoundaryCase &boundary) {
    const auto expected = reference_closest(geometry, boundary.reference);
    require(expected.size() == 1, "boundary characterization requires one reference candidate");
    const std::array rays{boundary.query};
    const auto closest = scene.closest_hits(rays);
    const auto any = scene.occlusion(rays);
    require_success(closest, boundary.name);
    require_success(any, boundary.name);
    require(closest.hits.size() == 1 && any.occluded.size() == 1 && any.occluded.front() <= 1,
            "malformed boundary result");
    if (closest.hits.front().hit) {
        GeometryErrors errors;
        const auto diagnostic =
            compare_closest(geometry, boundary.query, expected, closest.hits.front(), errors);
        require(diagnostic.empty(), diagnostic);
    }
    std::cout << boundary.name << ": closest=" << closest.hits.front().hit
              << ", any=" << static_cast<unsigned>(any.occluded.front()) << '\n';
}

void check_boundaries(MetalGeometryScene &scene, const GeometryData &geometry) {
    const GeometryRay interior = analytic_rays.front().ray;
    auto endpoint = interior;
    endpoint.t_min = 1.5f;
    endpoint.t_max = 1.75f;
    characterize_boundary(
        scene, geometry,
        {.name = "Exact t_min endpoint", .query = endpoint, .reference = interior});
    endpoint.t_min = 1.25f;
    endpoint.t_max = 1.5f;
    characterize_boundary(
        scene, geometry,
        {.name = "Exact t_max endpoint", .query = endpoint, .reference = interior});
    const GeometryRay edge{{8, 0.75f, 3}, {0, 0, -2}};
    const GeometryRay vertex{{10, 0, 3}, {0, 0, -2}};
    characterize_boundary(scene, geometry,
                          {.name = "Exact isolated outer edge", .query = edge, .reference = edge});
    characterize_boundary(
        scene, geometry,
        {.name = "Exact isolated outer vertex", .query = vertex, .reference = vertex});
}

void check_no_dispatch(MetalGeometryScene &scene, std::span<const GeometryRay> rays) {
    const auto closest = scene.closest_hits(rays);
    const auto any = scene.occlusion(rays);
    require_success(closest, "empty closest");
    require_success(any, "empty occlusion");
    require(closest.hits.size() == rays.size() && any.occluded.size() == rays.size() &&
                closest.guard_count == 0 && any.guard_count == 0,
            "empty work should return one result per ray without dispatch guards");
    for (std::size_t i = 0; i < rays.size(); ++i) {
        require(!closest.hits[i].hit && any.occluded[i] == 0, "empty scene must miss");
    }
}

void check_failed_queries(MetalGeometryScene &scene, std::span<const GeometryRay> rays,
                          std::string_view diagnostic) {
    const auto closest = scene.closest_hits(rays);
    const auto any = scene.occlusion(rays);
    require(closest.status == GeometryStatus::failure && any.status == GeometryStatus::failure &&
                closest.hits.empty() && any.occluded.empty() && closest.guard_count == 0 &&
                any.guard_count == 0,
            "failed queries should not return partial results");
    require_diagnostic(closest.diagnostic, diagnostic);
    require_diagnostic(any.diagnostic, diagnostic);
}

int run_native_tests(bool allow_no_device, bool allow_no_ray_tracing) {
    const auto geometry = make_geometry();
    auto invalid_geometry = geometry;
    invalid_geometry.meshes[1].triangles[0][0] = 999;
    const auto failed = build_metal_geometry(invalid_geometry);
    require(failed.status == GeometryStatus::failure && failed.scene == nullptr,
            "invalid geometry must fail before native build");
    require_diagnostic(failed.diagnostic, "out-of-range vertex index");

    auto build = [&] {
        const GeometryData transient = geometry;
        return build_metal_geometry(transient);
    }();
    if (may_skip(build.status, allow_no_device, allow_no_ray_tracing)) {
        require(build.scene == nullptr && !build.diagnostic.empty(), "malformed skip result");
        std::cout << "Skipping native geometry: " << build.diagnostic << '\n';
        return 77;
    }
    require_success(build, "Metal geometry build");
    require(build.scene != nullptr, "successful build must return a scene");
    require(build.device.compute_ray_tracing, "successful build must expose compute ray tracing");
    std::cout << "Metal geometry device: " << build.device.name << ", " << build.device.architecture
              << ", registry ID " << build.device.registry_id << '\n';

    auto scene = std::move(*build.scene);
    const std::array one{transformed_ray};
    check_failed_queries(*build.scene, one, "moved");
    check_no_dispatch(scene, {});
    auto invalid_ray = transformed_ray;
    invalid_ray.direction = {};
    check_failed_queries(scene, std::array{transformed_ray, invalid_ray},
                         "Ray 1 has zero direction");

    NativeEvidence evidence;
    compare_batch(scene, geometry, one, "single", evidence);
    compare_batch(scene, geometry, make_batch(), "fixed-seed", evidence);
    std::array<GeometryRay, analytic_rays.size()> named{};
    for (std::size_t i = 0; i < named.size(); ++i) {
        named[i] = analytic_rays[i].ray;
    }
    compare_batch(scene, geometry, named, "analytic", evidence);
    check_boundaries(scene, geometry);

    for (const GeometryData &empty :
         {GeometryData{}, GeometryData{{{}}, {{0, {}}}}, GeometryData{geometry.meshes, {}}}) {
        auto empty_build = build_metal_geometry(empty);
        require_success(empty_build, "empty Metal geometry build");
        require(empty_build.scene != nullptr, "successful empty build must return a scene");
        check_no_dispatch(*empty_build.scene, one);
    }

    std::cout << "Metal differential: " << evidence.rays << " rays per query type, "
              << evidence.closest_guards << " closest guards, " << evidence.occlusion_guards
              << " occlusion guards\nMaximum absolute errors: t=" << evidence.errors.t
              << ", barycentric=" << evidence.errors.barycentric
              << ", position=" << evidence.errors.position << '\n';
    return 0;
}

int run_stub_tests() {
    const auto result = build_metal_geometry(make_geometry());
    require(result.status == GeometryStatus::not_built && result.scene == nullptr &&
                !result.diagnostic.empty(),
            "portable geometry backend must report not_built");
    return 0;
}

} // namespace

int main(int argc, char *argv[]) try {
    if (argc == 2 && std::string_view{argv[1]} == "cpu") {
        return run_cpu_tests();
    }
    if (argc == 2 && std::string_view{argv[1]} == "stub") {
        return run_stub_tests();
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
        return run_native_tests(allow_no_device, allow_no_ray_tracing);
    }
    std::cerr << "Expected cpu, stub, or metal [--allow-no-device] [--allow-no-ray-tracing]\n";
    return 1;
} catch (const std::exception &error) {
    std::cerr << "Geometry test failed: " << error.what() << '\n';
    return 1;
}
