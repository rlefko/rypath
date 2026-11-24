// SPDX-License-Identifier: GPL-3.0-only
#include "reconstruction_camera.hpp"
#include "reconstruction_guide_cache.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace rc = rypath::reconstruction;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void close(double a, double b, double tolerance, const char *message) {
    require(std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance, message);
}
rc::Point along(const rc::Ray &ray, double distance) {
    return {ray.origin[0] + distance * ray.direction[0],
            ray.origin[1] + distance * ray.direction[1],
            ray.origin[2] + distance * ray.direction[2]};
}
std::array<double, 4> transform(const rc::Matrix &m, std::array<double, 4> p) {
    std::array<double, 4> result{};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c)
            result[r] += double(m[c * 4 + r]) * p[c];
    return result;
}
void matrix_matches(const rc::Camera &camera, rc::Point point, double tolerance) {
    auto projection = camera.project(point);
    require(projection.status == rc::ProjectionStatus::valid, "matrix test point invalid");
    const auto origin = camera.matrix_origin();
    auto view = transform(camera.world_to_view(),
                          {point[0] - origin[0], point[1] - origin[1], point[2] - origin[2], 1});
    auto clip = transform(camera.view_to_clip(), view);
    close((clip[0] / clip[3] + 1) * .5 * camera.width(), projection.pixel[0], tolerance,
          "column-major matrix X differs from projection");
    close((1 - clip[1] / clip[3]) * .5 * camera.height(), projection.pixel[1], tolerance,
          "column-major matrix Y differs from projection");
    close(clip[2] / clip[3], projection.depth, tolerance, "matrix reversed depth differs");
}
template <typename T> T checked(std::optional<T> value) {
    if (!value)
        throw std::runtime_error("Expected an available camera/projection value");
    return *value;
}
rc::Camera built(const rypath::RasterCamera &source, const rc::CameraConfig &config) {
    const auto result = rc::Camera::build(source, config);
    if (!result.camera)
        throw std::runtime_error(result.diagnostic);
    return *result.camera;
}
} // namespace

void guide_key_dependencies() {
    using namespace rypath;
    render_wire::Parameters p{};
    render_wire::TextureParameters t{};
    reconstruction_guide_wire::Config c{};
    const auto key = reconstruction::surface_guide_key(p, t, c);
    const auto check = [&](const auto &pp, const auto &tt, const auto &cc) {
        if (reconstruction::surface_guide_key(pp, tt, cc) == key)
            throw std::runtime_error("Guide key omitted a dependency");
    };
    auto samples = p;
    samples.sample_low = 123;
    samples.sample_high = 456;
    if (reconstruction::surface_guide_key(samples, t, c) != key)
        throw std::runtime_error("Guide key includes transport sample index");
    auto changed = p;
    changed.camera_medium = 1;
    check(changed, t, c);
    changed = p;
    changed.origin.x = 1;
    check(changed, t, c);
    auto texture = t;
    texture.spp_scale = 2;
    check(p, texture, c);
    texture = t;
    texture.minimum_dx.x = 1;
    check(p, texture, c);
    texture = t;
    texture.world_from_camera[4] = 2;
    check(p, texture, c);
    texture = t;
    texture.camera_from_world[8] = 2;
    check(p, texture, c);
    auto cfg = c;
    cfg.previous_valid = 1;
    check(p, t, cfg);
    cfg = c;
    cfg.flags = 1;
    check(p, t, cfg);
    for (unsigned i = 0; i < 16; ++i) {
        cfg = c;
        cfg.current_world_to_view[i] = 1;
        check(p, t, cfg);
        cfg = c;
        cfg.current_view_to_clip[i] = 1;
        check(p, t, cfg);
        cfg = c;
        cfg.previous_world_to_view[i] = 1;
        check(p, t, cfg);
        cfg = c;
        cfg.previous_view_to_clip[i] = 1;
        check(p, t, cfg);
    }
    for (unsigned i = 0; i < 4; ++i) {
        cfg = c;
        cfg.current_camera_to_matrix_origin[i] = 1;
        check(p, t, cfg);
        cfg = c;
        cfg.previous_camera_to_matrix_origin[i] = 1;
        check(p, t, cfg);
    }
}
int main() try {
    guide_key_dependencies();
    using namespace rypath;
    const rc::CameraConfig config{4, 2, .25, {}};
    RenderCamera demo;
    demo.origin = {0, 0, 0};
    demo.target = {0, 0, -1};
    demo.up = {0, 1, 0};
    demo.vertical_fov_degrees = 90;
    const auto demo_result = rc::Camera::build(demo, config);
    require(demo_result.camera.has_value(), "demo camera rejected");
    const auto camera = checked(demo_result.camera);
    // Independent demo primary-ray expression: F+(2u-1)*aspect*R+(1-2v)*U.
    const auto top_left = camera.center_ray(0, 0);
    if (!top_left)
        throw std::runtime_error("center ray unavailable");
    const double length = std::sqrt(1.5 * 1.5 + .5 * .5 + 1);
    close(top_left->direction[0], -1.5 / length, 1e-12, "demo X convention reversed");
    close(top_left->direction[1], .5 / length, 1e-12, "demo Y convention reversed");
    close(top_left->direction[2], -1 / length, 1e-12, "demo forward convention reversed");
    require(!camera.center_ray(4, 0) && !camera.center_ray(0, 2), "out-of-range ray accepted");
    for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 4; ++x) {
            const auto ray = checked(camera.center_ray(x, y));
            // Intersect z=-7, independently of the projection utility.
            const auto p = along(ray, -7 / ray.direction[2]);
            const auto projected = camera.project(p);
            require(projected.status == rc::ProjectionStatus::valid && projected.inside_raster,
                    "center ray did not hit the raster");
            close(projected.pixel[0], x + .5, 1e-12, "plane oracle X");
            close(projected.pixel[1], y + .5, 1e-12, "plane oracle Y");
            close(projected.depth, .25 / 7, 1e-12, "infinite-far depth");
            matrix_matches(camera, p, 1e-6);
        }
    require(camera.project({0, 0, -.1}).status == rc::ProjectionStatus::before_near_plane,
            "near-plane violation hidden");
    require(camera.project({0, 0, 1}).status == rc::ProjectionStatus::behind_camera,
            "behind-camera point accepted");
    close(camera.project({0, 0, -.25}).depth, 1, 1e-12, "near plane must map to one");
    const auto outside = camera.project({100, 0, -1});
    require(outside.status == rc::ProjectionStatus::valid && !outside.inside_raster,
            "offscreen projection must remain useful for motion");

    // Asymmetric, skewed raster. Independent plane inverse solves z, then y,x.
    const RasterCamera raster{{3, -2, 8}, {-.3f, .8f, -2}, {3, 0, 0}, {.4f, -1.7f, 0}};
    const rc::CameraConfig raster_config{640, 360, .01, {}};
    const auto asymmetric = built(raster, raster_config);
    for (unsigned y : {0u, 91u, 359u})
        for (unsigned x : {0u, 318u, 639u}) {
            const auto ray = checked(asymmetric.center_ray(x, y));
            const auto p = along(ray, -6 / ray.direction[2]);
            const double t = (p[2] - raster.origin.z) / raster.corner.z;
            const double v = ((p[1] - raster.origin.y) / t - raster.corner.y) / raster.dy.y;
            const double u =
                ((p[0] - raster.origin.x) / t - raster.corner.x - raster.dy.x * v) / raster.dx.x;
            const auto projected = asymmetric.project(p);
            close(projected.pixel[0], u * 640, 1e-10, "asymmetric inverse X");
            close(projected.pixel[1], v * 360, 1e-10, "asymmetric inverse Y");
            matrix_matches(asymmetric, p, 5e-5);
        }

    // Rotate the asymmetric basis by exactly 90 degrees around world Y. The
    // independent oracle rotates a world-space point back before solving the
    // original plane equations; no camera matrices enter that oracle.
    const auto turn = [](GeometryPoint p) { return GeometryPoint{p.z, p.y, -p.x}; };
    const RasterCamera turned{turn(raster.origin), turn(raster.corner), turn(raster.dx),
                              turn(raster.dy)};
    const auto turned_camera = built(turned, raster_config);
    const rc::Point turned_point{2, -.4, -4};
    const rc::Point unturned_point{-turned_point[2], turned_point[1], turned_point[0]};
    const double plane_t = (unturned_point[2] - raster.origin.z) / raster.corner.z;
    const double plane_v =
        ((unturned_point[1] - raster.origin.y) / plane_t - raster.corner.y) / raster.dy.y;
    const double plane_u = ((unturned_point[0] - raster.origin.x) / plane_t - raster.corner.x -
                            raster.dy.x * plane_v) /
                           raster.dx.x;
    const auto turned_projection = turned_camera.project(turned_point);
    require(turned_projection.status == rc::ProjectionStatus::valid,
            "rotated raster point invalid");
    close(turned_projection.pixel[0], plane_u * 640, 1e-10, "rotated asymmetric inverse X");
    close(turned_projection.pixel[1], plane_v * 360, 1e-10, "rotated asymmetric inverse Y");
    matrix_matches(turned_camera, turned_point, 5e-5);

    // Camera translation: a fixed point moves left, so current->previous is +X.
    RenderCamera shifted = demo;
    shifted.origin = {1, 0, 0};
    shifted.target = {1, 0, -1};
    const auto translated = checked(rc::Camera::build(shifted, config).camera);
    auto displacement = rc::motion(translated, camera, {0, 0, -5});
    if (!displacement.pixels)
        throw std::runtime_error("translated motion missing");
    close((*displacement.pixels)[0], .2, 1e-12, "motion must be previous minus current");
    close((*displacement.pixels)[1], 0, 1e-12, "translation induced vertical motion");
    auto background = rc::background_motion(translated, camera, {0, 0, -5});
    if (!background.pixels)
        throw std::runtime_error("background motion missing");
    close((*background.pixels)[0], 0, 1e-12, "background incorrectly uses camera translation");
    close(background.current.depth, 0, 0, "background depth is not zero");
    const auto background_view = transform(translated.world_to_view(), {0, 0, -5, 0});
    const auto background_clip = transform(translated.view_to_clip(), background_view);
    close(background_clip[2] / background_clip[3], 0, 0, "matrix background depth is not zero");
    close((background_clip[0] / background_clip[3] + 1) * 2, 2, 1e-12,
          "matrix background incorrectly uses translation");
    const auto resized = checked(rc::Camera::build(demo, {8, 4, .25, {}}).camera);
    require(!rc::motion(resized, camera, {0, 0, -5}).pixels &&
                !rc::background_motion(resized, camera, {0, 0, -1}).pixels,
            "cross-resolution motion accepted without a reset");

    RenderCamera rotated = demo;
    rotated.target = {1, 0, -1};
    const auto rotation = checked(rc::Camera::build(rotated, config).camera);
    const auto rotation_motion = rc::background_motion(rotation, camera, {0, 0, -1});
    if (!rotation_motion.pixels)
        throw std::runtime_error("rotation background motion missing");
    close((*rotation_motion.pixels)[0], 1, 1e-7, "rotation background sign/magnitude");
    matrix_matches(rotation, {0, 0, -5}, 1e-6);
    require(!rc::background_motion(camera, rotation, {0, 0, 1}).pixels,
            "behind-camera background motion accepted");

    // A large absolute float origin remains precise when the matrix consumes
    // positions rebased to that origin, rather than absolute float positions.
    RasterCamera distant{{1e12f, -1e12f, 1e12f}, {-1, 1, -1}, {2, 0, 0}, {0, -2, 0}};
    const auto far_camera = built(distant, {640, 360, .01, {}});
    const auto origin = far_camera.origin();
    const rc::Point p{origin[0] + 3, origin[1] - 2, origin[2] - 10};
    const auto far_projection = far_camera.project(p);
    close(far_projection.pixel[0], 416, 1e-10, "large-origin projection X");
    close(far_projection.pixel[1], 216, 1e-10, "large-origin projection Y");
    matrix_matches(far_camera, p, 1e-5);
    rc::CameraConfig shared{640, 360, .01, origin};
    distant.origin.x = std::nextafter(distant.origin.x, std::numeric_limits<float>::infinity());
    const auto next_camera = built(distant, shared);
    matrix_matches(next_camera, p, 1e-5);
    require(next_camera.matrix_origin() == far_camera.matrix_origin(), "shared origin lost");

    auto invalid = raster;
    invalid.dy = invalid.dx;
    require(!rc::Camera::build(invalid, raster_config).camera, "degenerate raster accepted");
    invalid = raster;
    invalid.corner.x = std::numeric_limits<float>::quiet_NaN();
    require(!rc::Camera::build(invalid, raster_config).camera, "nonfinite raster accepted");
    require(!rc::Camera::build(raster, {0, 360, .01, {}}).camera, "zero dimensions accepted");
    require(!rc::Camera::build(raster, {640, 360, 0, {}}).camera, "zero near plane accepted");
    require(!rc::Camera::build(raster, {640, 360, 1e-100, {}}).camera,
            "float-underflowing near plane accepted");
    const auto subnormal_near =
        rc::Camera::build(raster, {640, 360, double(std::numeric_limits<float>::denorm_min()), {}});
    require(!subnormal_near.camera &&
                subnormal_near.diagnostic.find("normal float") != std::string::npos,
            "Metal-FTZ subnormal near plane accepted without a diagnostic");
    require(!rc::Camera::build(raster, {640, 360, 1e100, {}}).camera,
            "float-overflowing near plane accepted");
    require(!rc::Camera::build(raster, {640, 360, .01, rc::Point{1e300, 0, 0}}).camera,
            "unrepresentable rebased matrix accepted");
    require(!rc::Camera::build(
                 raster, {640, 360, .01, rc::Point{std::numeric_limits<double>::infinity(), 0, 0}})
                 .camera,
            "nonfinite matrix origin accepted");
    demo.target = demo.origin;
    require(!rc::Camera::build(demo, config).camera, "zero camera viewing direction accepted");
    demo.target = {0, 0, -1};
    demo.up = {0, 0, 1};
    require(!rc::Camera::build(demo, config).camera, "parallel camera up accepted");
    require(camera.project({0, std::numeric_limits<double>::infinity(), 0}).status ==
                rc::ProjectionStatus::invalid_input,
            "nonfinite point accepted");
    require(camera.project_background({0, 0, 0}).status == rc::ProjectionStatus::invalid_input,
            "zero background direction accepted");
    const auto tiny_background = camera.project_background({0, 0, -1e-320});
    require(tiny_background.status == rc::ProjectionStatus::valid,
            "finite nonzero tiny background direction rejected");
    close(tiny_background.pixel[0], 2, 1e-12, "tiny background direction changed projection");
    const auto tiny_depth = camera.project({1, 0, -1e-300});
    require(tiny_depth.status == rc::ProjectionStatus::before_near_plane,
            "near plane domain does not precede division");
    const auto distant_depth = camera.project({0, 0, -1e6});
    require(distant_depth.status == rc::ProjectionStatus::valid && distant_depth.depth > 0 &&
                distant_depth.depth < camera.project({0, 0, -10}).depth,
            "infinite-far depth is not positive and monotone");
    std::cout << "reconstruction camera checks passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "reconstruction camera: " << error.what() << '\n';
    return 1;
}
