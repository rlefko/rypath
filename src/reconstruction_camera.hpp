// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "render.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace rypath::reconstruction {

using Point = std::array<double, 3>;
using Matrix = std::array<float, 16>; // Column-major, column-vector multiplication.

struct CameraConfig {
    std::uint32_t width = 0, height = 0;
    // Guide-only near plane, in scene units; must pack to a positive normal
    // float because Metal can flush subnormal depth coefficients to zero.
    // Raw transport has no new cutoff.
    // Surfaces closer than this return before_near_plane; callers must diagnose
    // reconstruction separately instead of clamping their guide depth.
    double near_plane = .001;
    // Both cameras can use a shared origin, or the GPU must account for their
    // origin delta. Defaults to this camera's origin for stable float matrices.
    std::optional<Point> matrix_origin;
};

enum class ProjectionStatus {
    valid,
    invalid_input,
    behind_camera,
    before_near_plane,
    unrepresentable
};
struct Projection {
    ProjectionStatus status = ProjectionStatus::invalid_input;
    // Pixel boundaries are [0,width] x [0,height]; pixel centers are i+.5.
    std::array<double, 2> pixel{};
    double depth = 0; // Infinite-far reversed Z: near/view_depth; background=0.
    bool inside_raster = false;
};
struct Ray {
    Point origin, direction;
};
struct CameraBuildResult;

class Camera {
  public:
    static CameraBuildResult build(const RenderCamera &, const CameraConfig &);
    static CameraBuildResult build(const RasterCamera &, const CameraConfig &);
    std::optional<Ray> center_ray(std::uint32_t x, std::uint32_t y) const;
    Projection project(Point absolute_world_point) const;
    // Direction is a nonzero world-space vector; camera translation is ignored.
    Projection project_background(Point world_direction) const;
    const Matrix &world_to_view() const { return world_to_view_; }
    const Matrix &view_to_clip() const { return view_to_clip_; }
    // world_to_view acts on absolute_world_point - matrix_origin(), not on
    // absolute float positions. View Z is positive in the viewing direction.
    Point matrix_origin() const { return matrix_origin_; }
    Point origin() const { return origin_; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }

  private:
    Camera() = default;
    static CameraBuildResult from_basis(Point origin, Point corner, Point dx, Point dy,
                                        const CameraConfig &);
    Projection project_relative(Point relative, bool background) const;
    Point origin_{}, matrix_origin_{}, corner_{}, dx_{}, dy_{}, forward_{};
    std::array<Point, 3> inverse_{};
    std::uint32_t width_ = 0, height_ = 0;
    double near_ = 0;
    Matrix world_to_view_{}, view_to_clip_{};
};

struct CameraBuildResult {
    std::optional<Camera> camera;
    std::string diagnostic;
};

struct Motion {
    Projection current, previous;
    // previous.pixel-current.pixel, in input pixels. Empty if either projection
    // fails, the delta cannot be packed to float, or dimensions differ (reset).
    std::optional<std::array<double, 2>> pixels;
};
Motion motion(const Camera &current, const Camera &previous, Point absolute_world_point);
Motion background_motion(const Camera &current, const Camera &previous, Point world_direction);

} // namespace rypath::reconstruction
