// SPDX-License-Identifier: GPL-3.0-only
#include "reconstruction_camera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace rypath::reconstruction {
namespace {
Point point(GeometryPoint p) { return {p.x, p.y, p.z}; }
bool finite(Point p) {
    return std::ranges::all_of(p, [](double x) { return std::isfinite(x); });
}
Point add(Point a, Point b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Point sub(Point a, Point b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Point mul(Point a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(Point a, Point b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Point cross(Point a, Point b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
std::optional<Point> unit(Point p) {
    if (!finite(p))
        return {};
    const double scale = std::max({std::abs(p[0]), std::abs(p[1]), std::abs(p[2])});
    if (!(scale > 0))
        return {};
    for (auto &value : p)
        value /= scale;
    const double length = std::sqrt(dot(p, p));
    if (!std::isfinite(length) || !(length > 0))
        return {};
    return mul(p, 1 / length);
}
bool representable(double x) {
    if (!std::isfinite(x) || std::abs(x) > std::numeric_limits<float>::max())
        return false;
    return x == 0 || static_cast<float>(x) != 0;
}
bool pack(std::array<double, 16> source, Matrix &destination) {
    for (unsigned i = 0; i < 16; ++i) {
        if (!representable(source[i]))
            return false;
        destination[i] = static_cast<float>(source[i]);
    }
    return true;
}
Point host_pack(Point p) {
    for (auto &v : p)
        v = static_cast<float>(v);
    return p;
}
Motion combine(Projection current, Projection previous) {
    Motion result{current, previous, {}};
    if (current.status == ProjectionStatus::valid && previous.status == ProjectionStatus::valid) {
        const std::array<double, 2> delta{previous.pixel[0] - current.pixel[0],
                                          previous.pixel[1] - current.pixel[1]};
        if (representable(delta[0]) && representable(delta[1]))
            result.pixels = delta;
    }
    return result;
}
} // namespace

CameraBuildResult Camera::build(const RenderCamera &source, const CameraConfig &config) {
    if (!finite(point(source.origin)) || !finite(point(source.target)) ||
        !finite(point(source.up)) || !std::isfinite(source.vertical_fov_degrees) ||
        !(source.vertical_fov_degrees > 0 && source.vertical_fov_degrees < 180) || !config.width ||
        !config.height)
        return {.diagnostic = "Invalid reconstruction demo camera or dimensions"};
    const auto forward = unit(sub(point(source.target), point(source.origin)));
    if (!forward)
        return {.diagnostic = "Reconstruction camera target equals its origin"};
    const auto right = unit(cross(*forward, point(source.up)));
    if (!right)
        return {.diagnostic = "Reconstruction camera up is parallel to its viewing direction"};
    // Match camera_parameters' double construction and binary32 packing before
    // applying primary_ray's +right / -up raster convention. This is not a
    // claim of bitwise identity with shader float arithmetic.
    const auto f = host_pack(*forward), r = host_pack(*right);
    const auto u = host_pack(cross(*right, *forward));
    const double scale = static_cast<float>(
        std::tan(double(source.vertical_fov_degrees) * (std::numbers::pi / 360)));
    const double horizontal = (double(config.width) / config.height) * scale;
    return from_basis(point(source.origin), add(sub(f, mul(r, horizontal)), mul(u, scale)),
                      mul(r, 2 * horizontal), mul(u, -2 * scale), config);
}

CameraBuildResult Camera::build(const RasterCamera &source, const CameraConfig &config) {
    return from_basis(point(source.origin), point(source.corner), point(source.dx),
                      point(source.dy), config);
}

CameraBuildResult Camera::from_basis(Point origin, Point corner, Point dx, Point dy,
                                     const CameraConfig &config) {
    if (!config.width || !config.height || !representable(config.near_plane) ||
        !(config.near_plane > 0) || !std::isnormal(static_cast<float>(config.near_plane)))
        return {.diagnostic = "Reconstruction dimensions must be nonzero and the guide near plane "
                              "must pack to a positive normal float"};
    if (!finite(origin) || !finite(corner) || !finite(dx) || !finite(dy))
        return {.diagnostic = "Reconstruction camera basis must be finite"};
    const auto right = unit(dx), across = unit(dy), center = unit(corner);
    if (!right || !across || !center)
        return {.diagnostic = "Degenerate reconstruction raster basis"};
    auto forward = unit(cross(*right, *across));
    if (!forward ||
        std::abs(dot(*forward, *center)) <= 128 * std::numeric_limits<double>::epsilon())
        return {.diagnostic = "Degenerate reconstruction raster projection"};
    if (dot(*forward, corner) < 0)
        *forward = mul(*forward, -1);
    const auto up = unit(cross(*right, *forward));
    if (!up)
        return {.diagnostic = "Degenerate reconstruction view basis"};
    const double extent = std::max({std::abs(corner[0]), std::abs(corner[1]), std::abs(corner[2]),
                                    std::abs(dx[0]), std::abs(dx[1]), std::abs(dx[2]),
                                    std::abs(dy[0]), std::abs(dy[1]), std::abs(dy[2])});
    const auto a = mul(corner, 1 / extent), b = mul(dx, 1 / extent), c = mul(dy, 1 / extent);
    const double determinant = dot(a, cross(b, c));
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= 128 * std::numeric_limits<double>::epsilon())
        return {.diagnostic = "Ill-conditioned reconstruction raster basis"};
    Camera camera;
    camera.origin_ = origin;
    camera.matrix_origin_ = config.matrix_origin.value_or(origin);
    if (!finite(camera.matrix_origin_))
        return {.diagnostic = "Reconstruction matrix origin must be finite"};
    camera.corner_ = corner;
    camera.dx_ = dx;
    camera.dy_ = dy;
    camera.forward_ = *forward;
    camera.width_ = config.width;
    camera.height_ = config.height;
    camera.near_ = config.near_plane;
    camera.inverse_ = {mul(cross(b, c), 1 / (determinant * extent)),
                       mul(cross(c, a), 1 / (determinant * extent)),
                       mul(cross(a, b), 1 / (determinant * extent))};
    const std::array<Point, 3> view{*right, *up, *forward};
    std::array<double, 16> world_to_view{}, view_to_clip{};
    const auto rebased_camera = sub(origin, camera.matrix_origin_);
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned col = 0; col < 3; ++col)
            world_to_view[col * 4 + row] = view[row][col];
        world_to_view[12 + row] = -dot(view[row], rebased_camera);
    }
    world_to_view[15] = 1;
    // q=inverse([corner dx dy])*relative_world. u=q.y/q.x,
    // v=q.z/q.x. Multiplication by plane depth makes clip W=view Z.
    const double plane = dot(*forward, corner);
    for (unsigned col = 0; col < 3; ++col) {
        const auto &axis = view[col]; // Inverse of the orthonormal view rotation.
        const double q0 = dot(camera.inverse_[0], axis);
        const double q1 = dot(camera.inverse_[1], axis);
        const double q2 = dot(camera.inverse_[2], axis);
        view_to_clip[col * 4] = plane * (2 * q1 - q0);
        view_to_clip[col * 4 + 1] = plane * (q0 - 2 * q2);
        view_to_clip[col * 4 + 3] = plane * q0;
    }
    view_to_clip[14] = config.near_plane;
    if (!pack(world_to_view, camera.world_to_view_) || !pack(view_to_clip, camera.view_to_clip_))
        return {.diagnostic = "Reconstruction camera matrices are not representable as float"};
    return {.camera = camera};
}

std::optional<Ray> Camera::center_ray(std::uint32_t x, std::uint32_t y) const {
    if (x >= width_ || y >= height_)
        return {};
    const auto direction = unit(add(add(corner_, mul(dx_, (double(x) + .5) / width_)),
                                    mul(dy_, (double(y) + .5) / height_)));
    if (!direction)
        return {};
    return Ray{origin_, *direction};
}

Projection Camera::project_relative(Point relative, bool background) const {
    Projection result;
    if (!finite(relative))
        return result;
    const double depth = dot(forward_, relative);
    if (!std::isfinite(depth)) {
        result.status = ProjectionStatus::unrepresentable;
        return result;
    }
    if (!(depth > 0)) {
        result.status = ProjectionStatus::behind_camera;
        return result;
    }
    if (!background && depth < near_) {
        result.status = ProjectionStatus::before_near_plane;
        return result;
    }
    const double q0 = dot(inverse_[0], relative);
    result.pixel = {width_ * (dot(inverse_[1], relative) / q0),
                    height_ * (dot(inverse_[2], relative) / q0)};
    result.depth = background ? 0 : near_ / depth;
    if (!representable(result.pixel[0]) || !representable(result.pixel[1]) ||
        !representable(result.depth)) {
        result.status = ProjectionStatus::unrepresentable;
        return result;
    }
    result.inside_raster = result.pixel[0] >= 0 && result.pixel[0] < width_ &&
                           result.pixel[1] >= 0 && result.pixel[1] < height_;
    result.status = ProjectionStatus::valid;
    return result;
}

Projection Camera::project(Point absolute_world_point) const {
    if (!finite(absolute_world_point))
        return {};
    return project_relative(sub(absolute_world_point, origin_), false);
}

Projection Camera::project_background(Point world_direction) const {
    const auto direction = unit(world_direction);
    return direction ? project_relative(*direction, true) : Projection{};
}

Motion motion(const Camera &current, const Camera &previous, Point absolute_world_point) {
    const auto a = current.project(absolute_world_point),
               b = previous.project(absolute_world_point);
    if (current.width() != previous.width() || current.height() != previous.height())
        return {a, b, {}};
    return combine(a, b);
}
Motion background_motion(const Camera &current, const Camera &previous, Point world_direction) {
    const auto a = current.project_background(world_direction);
    const auto b = previous.project_background(world_direction);
    if (current.width() != previous.width() || current.height() != previous.height())
        return {a, b, {}};
    return combine(a, b);
}

} // namespace rypath::reconstruction
