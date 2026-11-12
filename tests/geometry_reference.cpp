// SPDX-License-Identifier: GPL-3.0-only

#include "geometry_reference.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace rypath::test {
namespace {

using Point = std::array<double, 3>;
using Triangle = std::array<Point, 3>;

constexpr double t_factor = 32 * std::numeric_limits<float>::epsilon();
constexpr double barycentric_limit = 64 * std::numeric_limits<float>::epsilon();
constexpr double position_factor = 64 * std::numeric_limits<float>::epsilon();

Point subtract(Point a, Point b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

Point cross(Point a, Point b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double dot(Point a, Point b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

double max_component(Point p) { return std::max({std::abs(p[0]), std::abs(p[1]), std::abs(p[2])}); }

Point point_on_ray(const GeometryRay &ray, double t) {
    return {ray.origin.x + t * ray.direction.x, ray.origin.y + t * ray.direction.y,
            ray.origin.z + t * ray.direction.z};
}

Point transform_point(const GeometryTransform &transform, GeometryPoint p) {
    const auto &m = transform.values;
    const double x = p.x;
    const double y = p.y;
    const double z = p.z;
    return {m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
            m[8] * x + m[9] * y + m[10] * z + m[11]};
}

Triangle world_triangle(const GeometryData &geometry, const GeometryInstance &instance,
                        std::uint32_t primitive) {
    const auto &mesh = geometry.meshes[instance.mesh_index];
    const auto [a, b, c] = mesh.triangles[primitive];
    return {transform_point(instance.transform, mesh.positions[a]),
            transform_point(instance.transform, mesh.positions[b]),
            transform_point(instance.transform, mesh.positions[c])};
}

double edge(Point a, Point b, Point p, std::size_t x, std::size_t y) {
    return (b[x] - a[x]) * (p[y] - a[y]) - (b[y] - a[y]) * (p[x] - a[x]);
}

// Deliberately independent of Metal's object-space traversal. Intersect the
// world-space plane, then use the dominant normal axis to project to 2D.
std::optional<ReferenceHit> intersect(Triangle points, const GeometryRay &ray, std::uint32_t mesh,
                                      std::uint32_t instance, std::uint32_t primitive) {
    const auto &[a, b, c] = points;
    const Point n = cross(subtract(b, a), subtract(c, a));
    const double denominator = dot(n, {ray.direction.x, ray.direction.y, ray.direction.z});
    if (denominator == 0) {
        return std::nullopt;
    }
    const double t = dot(n, subtract(a, {ray.origin.x, ray.origin.y, ray.origin.z})) / denominator;
    if (!(t > ray.t_min && t < ray.t_max)) {
        return std::nullopt;
    }
    const auto absolute_normal = std::array{std::abs(n[0]), std::abs(n[1]), std::abs(n[2])};
    const auto axis = static_cast<std::size_t>(std::ranges::max_element(absolute_normal) -
                                               absolute_normal.begin());
    const std::size_t x = (axis + 1) % 3;
    const std::size_t y = (axis + 2) % 3;
    const Point p = point_on_ray(ray, t);
    const double area = edge(a, b, c, x, y);
    const std::array barycentric{edge(b, c, p, x, y) / area, edge(c, a, p, x, y) / area,
                                 edge(a, b, p, x, y) / area};
    if (std::ranges::any_of(barycentric, [](double w) { return w < 0; })) {
        return std::nullopt;
    }
    return ReferenceHit{mesh, instance, primitive, t, barycentric};
}

} // namespace

std::vector<ReferenceHit> reference_closest(const GeometryData &geometry, const GeometryRay &ray) {
    std::vector<ReferenceHit> nearest;
    for (std::uint32_t instance = 0; instance < geometry.instances.size(); ++instance) {
        const auto &object = geometry.instances[instance];
        const auto &mesh = geometry.meshes[object.mesh_index];
        for (std::uint32_t primitive = 0; primitive < mesh.triangles.size(); ++primitive) {
            const auto hit = intersect(world_triangle(geometry, object, primitive), ray,
                                       object.mesh_index, instance, primitive);
            if (!hit || (!nearest.empty() && hit->t > nearest.front().t)) {
                continue;
            }
            if (!nearest.empty() && hit->t < nearest.front().t) {
                nearest.clear();
            }
            nearest.push_back(*hit);
        }
    }
    return nearest;
}

std::string compare_closest(const GeometryData &geometry, const GeometryRay &ray,
                            std::span<const ReferenceHit> expected, const GeometryHit &actual,
                            GeometryErrors &errors) {
    if (expected.empty()) {
        return actual.hit ? "expected a miss" : "";
    }
    if (!actual.hit) {
        return "expected a hit";
    }
    const auto matching = std::ranges::find_if(expected, [&](const ReferenceHit &candidate) {
        return actual.mesh_index == candidate.mesh_index &&
               actual.instance_index == candidate.instance_index &&
               actual.primitive_index == candidate.primitive_index;
    });
    if (matching == expected.end()) {
        return "hit IDs are not a closest candidate: mesh " + std::to_string(actual.mesh_index) +
               ", instance " + std::to_string(actual.instance_index) + ", primitive " +
               std::to_string(actual.primitive_index);
    }
    const double t_error = std::abs(actual.t - matching->t);
    if (!(t_error <= t_factor * std::max(1.0, std::abs(matching->t)))) {
        return "hit t is out of tolerance: actual " + std::to_string(actual.t) + ", expected " +
               std::to_string(matching->t);
    }
    double barycentric_error = 0;
    for (std::size_t i = 0; i < actual.barycentric.size(); ++i) {
        const double error = std::abs(actual.barycentric[i] - matching->barycentric[i]);
        if (!(error <= barycentric_limit)) {
            return "barycentric weight " + std::to_string(i) + " is out of tolerance";
        }
        barycentric_error = std::max(barycentric_error, error);
    }

    const Triangle triangle = world_triangle(geometry, geometry.instances[matching->instance_index],
                                             matching->primitive_index);
    Point barycentric_point{};
    for (std::size_t vertex = 0; vertex < triangle.size(); ++vertex) {
        for (std::size_t axis = 0; axis < barycentric_point.size(); ++axis) {
            barycentric_point[axis] += actual.barycentric[vertex] * triangle[vertex][axis];
        }
    }
    const Point expected_position = point_on_ray(ray, matching->t);
    const double position_error =
        std::max(max_component(subtract(point_on_ray(ray, actual.t), expected_position)),
                 max_component(subtract(barycentric_point, expected_position)));
    const double scale = std::max(
        {1.0,
         max_component({ray.origin.x, ray.origin.y, ray.origin.z}) +
             std::abs(matching->t) *
                 max_component({ray.direction.x, ray.direction.y, ray.direction.z}),
         max_component(triangle[0]), max_component(triangle[1]), max_component(triangle[2])});
    if (!(position_error <= position_factor * scale)) {
        return "reconstructed world hit position is out of tolerance";
    }
    errors.t = std::max(errors.t, t_error);
    errors.barycentric = std::max(errors.barycentric, barycentric_error);
    errors.position = std::max(errors.position, position_error);
    return {};
}

std::string compare_occlusion(bool expected, std::uint8_t actual) {
    return actual == static_cast<std::uint8_t>(expected)
               ? ""
               : "expected occlusion " + std::to_string(expected) + ", got " +
                     std::to_string(actual);
}

} // namespace rypath::test
