// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_motion_host.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace rypath;
namespace host = enhanced_motion_host;
namespace motion = rypath_enhanced_motion;
using D3 = std::array<double, 3>;
unsigned checks = 0;
void need(bool ok, const char *why) {
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
void close(double actual, double expected, double tolerance, const char *why) {
    need(std::isfinite(actual) && std::isfinite(expected) &&
             std::abs(actual - expected) <= tolerance,
         why);
}
D3 point(GeometryPoint p) { return {p.x, p.y, p.z}; }
D3 sub(D3 a, D3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(D3 a, D3 b) { return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]; }
D3 cross(D3 a, D3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// Independent double Cramer's-rule pinhole projection. No tested packed matrix
// or reconstruction::Camera::project participates in the expected result.
std::array<double, 3> oracle(const RasterCamera &camera, D3 world, unsigned width,
                             unsigned height) {
    const auto c = point(camera.corner), dx = point(camera.dx), dy = point(camera.dy);
    const auto relative = sub(world, point(camera.origin));
    const auto normal = cross(dx, dy);
    const double t = dot(relative, normal);
    const double u = dot(c, cross(relative, dy)) / t;
    const double v = dot(c, cross(dx, relative)) / t;
    const double orientation = dot(normal, c) < 0 ? -1 : 1;
    const double view_depth = dot(normal, relative) * orientation / std::sqrt(dot(normal, normal));
    return {u * width, v * height, host::guide_near_plane / view_depth};
}
FrameCamera raster_camera(GeometryPoint origin = {}) {
    FrameCamera out;
    out.raster_camera = RasterCamera{origin, {-1, 1, 1}, {2, 0, 0}, {0, -2, 0}};
    return out;
}
void camera_packing() {
    auto current = raster_camera({2, -3, 1});
    auto previous = raster_camera({-1, 2, .5f});
    auto &raster = *previous.raster_camera;
    const auto rotated = [](GeometryPoint p) {
        constexpr double a = .37;
        return GeometryPoint{float(std::cos(a) * p.x + std::sin(a) * p.z), p.y,
                             float(-std::sin(a) * p.x + std::cos(a) * p.z)};
    };
    raster.corner = rotated(raster.corner);
    raster.dx = rotated(raster.dx);
    raster.dy = rotated(raster.dy);
    const auto packed = host::pack_config(current, previous, 128, 96, true, 0xfedcba9876543210ull);
    need(packed.config.has_value() && packed.diagnostic.empty() && packed.current_camera &&
             packed.previous_camera && packed.current_used_raster && packed.previous_used_raster,
         "Valid raster pair returns complete config and camera metadata");
    const auto &config = *packed.config;
    need(sizeof(config) == 352 && config.version == 1 && config.width == 128 &&
             config.height == 96 && config.history_valid == 1 && config.reserved == 0 &&
             config.serial_low == 0x76543210u && config.serial_high == 0xfedcba98u,
         "Exact wire version, dimensions and full64-bit serial survive packing");
    need(config.policy.minimum_normal_cosine == .99f &&
             config.policy.maximum_plane_footprints == .5f &&
             config.policy.relative_depth_separation ==
                 32 * std::numeric_limits<float>::epsilon() &&
             config.policy.require_group == 0,
         "Fixed explicit geometry-only heuristic policy");
    need(packed.common_origin == D3{2, -3, 1}, "Common origin is current built camera origin");
    need(config.current_camera.common_to_matrix_origin.x == 0 &&
             config.current_camera.common_to_matrix_origin.y == 0 &&
             config.current_camera.common_to_matrix_origin.z == 0,
         "Current matrix uses current origin");
    need(config.previous_camera.common_to_matrix_origin.x == 3 &&
             config.previous_camera.common_to_matrix_origin.y == -5 &&
             config.previous_camera.common_to_matrix_origin.z == .5f &&
             config.previous_origin_to_common.x == -3 && config.previous_origin_to_common.y == 5 &&
             config.previous_origin_to_common.z == -.5f,
         "Previous matrix and geometry origins have opposite delta signs");
    for (D3 world : {D3{3, -1, 9}, D3{-2, 1, 16}, D3{8, -2, 24}}) {
        const auto relative = sub(world, packed.common_origin);
        for (bool old : {false, true}) {
            const auto &camera = old ? config.previous_camera : config.current_camera;
            const auto &source = old ? *previous.raster_camera : *current.raster_camera;
            const auto expected = oracle(source, world, 128, 96);
            const auto actual = motion::project(
                camera, {float(relative[0]), float(relative[1]), float(relative[2])});
            need(actual.status == motion::ProjectionStatus::valid,
                 "Packed relative projection valid");
            close(actual.pixel.x, expected[0], 3e-5,
                  "Packed X agrees with independent double raster oracle");
            close(actual.pixel.y, expected[1], 3e-5,
                  "Packed Y agrees with independent double raster oracle");
            close(actual.depth, expected[2], 2e-10, "Packed near/view-Z agrees with double oracle");
        }
    }
    const auto cold =
        host::pack_config(current, current, 7, 3, false, std::numeric_limits<std::uint64_t>::max());
    need(cold.config && cold.config->history_valid == 0 && cold.config->serial_low == 0xffffffffu &&
             cold.config->serial_high == 0xffffffffu,
         "Cold config retains full all-ones serial");
    const auto before_near = motion::project(cold.config->current_camera, {0, 0, .0005f});
    need(before_near.status == motion::ProjectionStatus::before_near_plane,
         "Guide-only near remains a projection eligibility check");
}
void raster_preference_and_ranges() {
    auto a = raster_camera();
    a.camera.target = a.camera.origin; // Invalid fallback must be ignored when raster exists.
    const auto preferred = host::pack_config(a, a, 16, 8, true, 0);
    need(preferred.config && preferred.current_used_raster && preferred.previous_used_raster,
         "Raster camera preferred over invalid RenderCamera fallback");
    a.raster_camera->dx = {};
    a.camera = RenderCamera{}; // Valid fallback must NOT rescue an invalid explicit raster.
    auto failed = host::pack_config(a, a, 16, 8, true, 0);
    need(!failed.config && !failed.diagnostic.empty(),
         "Invalid explicit raster produces diagnostic without fallback substitution");
    a.raster_camera.reset();
    const auto render = host::pack_config(a, a, 16, 8, false, 0);
    need(render.config && !render.current_used_raster && !render.previous_used_raster,
         "RenderCamera builds when raster is absent");
    a.camera.vertical_fov_degrees = 180;
    failed = host::pack_config(a, a, 16, 8, true, 0);
    need(!failed.config && failed.diagnostic.find("current camera") != std::string::npos,
         "Invalid physical projection camera explicitly diagnosed");
    a = raster_camera();
    auto b = a;
    b.raster_camera->corner.z = std::numeric_limits<float>::quiet_NaN();
    failed = host::pack_config(a, b, 16, 8, false, 0);
    need(!failed.config && failed.diagnostic.find("previous camera") != std::string::npos,
         "Cold history still requires structurally valid previous wire camera");
    for (auto dims : {std::array<unsigned, 2>{0, 8},
                      {8, 0},
                      {host::maximum_dimension + 1, 1},
                      {1, host::maximum_dimension + 1},
                      {65536, 65536},
                      {0xffffffffu, 1}}) {
        failed = host::pack_config(a, a, dims[0], dims[1], true, 0);
        need(!failed.config && !failed.diagnostic.empty(),
             "Invalid dimensions reject before packing");
    }
    for (auto dims :
         {std::array<unsigned, 2>{host::maximum_dimension, 1}, {1, host::maximum_dimension}}) {
        const auto edge = host::pack_config(a, a, dims[0], dims[1], true, 0);
        need(edge.config.has_value(), "Exact pixel-center dimension boundary admitted");
    }
    a = raster_camera({std::numeric_limits<float>::max(), 0, 0});
    b = raster_camera({-std::numeric_limits<float>::max(), 0, 0});
    failed = host::pack_config(a, b, 8, 8, true, 0);
    need(!failed.config && failed.diagnostic.find("origin differences") != std::string::npos,
         "Finite camera origins with unrepresentable double delta reject explicitly");
    motion::Vec3 sentinel{1, 2, 3};
    need(!host::checked_difference({std::numeric_limits<double>::infinity(), 0, 0}, {}, sentinel) &&
             sentinel.x == 1 && sentinel.y == 2 && sentinel.z == 3,
         "Failed checked conversion leaves output untouched");
    need(!host::checked_difference({std::numeric_limits<double>::denorm_min(), 0, 0}, {}, sentinel),
         "Nonzero double offset cannot silently underflow to FLOAT zero");
}
void large_origin() {
    constexpr float base = 0x1p28f;
    auto a = raster_camera({base, -base, base});
    auto b = raster_camera({base + 32, -base - 64, base + 32});
    const auto packed = host::pack_config(a, b, 128, 96, true, 1ull << 32);
    need(packed.config.has_value(),
         "Large camera origin pair packs without absolute FLOAT surface positions");
    const auto &config = *packed.config;
    need(config.previous_camera.common_to_matrix_origin.x == -32 &&
             config.previous_camera.common_to_matrix_origin.y == 64 &&
             config.previous_camera.common_to_matrix_origin.z == -32 &&
             config.previous_origin_to_common.x == 32 &&
             config.previous_origin_to_common.y == -64 && config.previous_origin_to_common.z == 32,
         "Large-origin double differences retain exact sign and magnitude");
    const D3 world{double(base) + 3, -double(base) + 4, double(base) + 128};
    for (bool old : {false, true}) {
        const auto expected = oracle(old ? *b.raster_camera : *a.raster_camera, world, 128, 96);
        const auto actual =
            motion::project(old ? config.previous_camera : config.current_camera, {3, 4, 128});
        need(actual.status == motion::ProjectionStatus::valid,
             "Large-origin relative geometry projects");
        close(actual.pixel.x, expected[0], 2e-5, "Large-origin X double oracle");
        close(actual.pixel.y, expected[1], 2e-5, "Large-origin Y double oracle");
        close(actual.depth, expected[2], 2e-12, "Large-origin depth double oracle");
    }
    const motion::Vec3 wrong{float(world[0]) - base, float(world[1]) + base,
                             float(world[2]) - base};
    const auto incorrect = motion::project(config.current_camera, wrong);
    const auto correct = motion::project(config.current_camera, {3, 4, 128});
    need(std::abs(incorrect.pixel.x - correct.pixel.x) > 1 &&
             std::abs(incorrect.pixel.y - correct.pixel.y) > 1,
         "Subtracting absolute FLOAT positions is a failing negative origin-conversion control");
}
void resolved_wire_projection() {
    for (const auto model : {render_wire::pbrt_model, render_wire::demo_model})
        for (unsigned branch = 0; branch < 3; ++branch) {
            render_wire::Parameters p{};
            p.width = 128;
            p.height = 96;
            p.model = model;
            p.origin = {2, -3, 1};
            p.forward = {0, 0, 1};
            p.right = {1, 0, 0};
            p.up = {0, 1, 0};
            p.film_scale = .75f;
            p.filter_kind = branch == 1 ? render_wire::triangle_filter : render_wire::box_filter;
            p.filter_x_radius = p.filter_y_radius = branch == 2 ? 1.f : .5f;
            auto old = p;
            old.origin = {-1, 2, .5f};
            const auto unchanged = p, unchanged_old = old;
            const auto result = host::pack_config_from_parameters(p, old, true, (1ull << 32) + 7);
            need(result.config && result.current_camera && result.previous_camera,
                 "Resolved wire camera pair packs");
            need(std::memcmp(&p, &unchanged, sizeof(p)) == 0 &&
                     std::memcmp(&old, &unchanged_old, sizeof(old)) == 0,
                 "Projection bridge preserves both physical wire parameter blocks");
            // For this exact 4:3 film and scale3/4, demo default box has
            // horizontal extent1 and vertical extent3/4. Centered-filter and
            // PBRT branches instead use the supplied raster coefficients.
            RasterCamera expected{{2, -3, 1}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
            if (model == render_wire::demo_model && branch == 0) {
                expected.corner = {-1, .75f, 1};
                expected.dx = {2, 0, 0};
                expected.dy = {0, -1.5f, 0};
            }
            auto expected_old = expected;
            expected_old.origin = {-1, 2, .5f};
            for (D3 world : {D3{3, -1, 9}, D3{-2, 1, 16}, D3{8, -2, 24}})
                for (const bool previous : {false, true}) {
                    const auto reference =
                        oracle(previous ? expected_old : expected, world, 128, 96);
                    const auto relative = sub(world, result.common_origin);
                    const auto actual = motion::project(
                        previous ? result.config->previous_camera : result.config->current_camera,
                        {float(relative[0]), float(relative[1]), float(relative[2])});
                    need(actual.status == motion::ProjectionStatus::valid,
                         "Resolved wire projection valid");
                    close(actual.pixel.x, reference[0], 3e-5,
                          "Resolved wire X independent camera branch oracle");
                    close(actual.pixel.y, reference[1], 3e-5,
                          "Resolved wire Y independent camera branch oracle");
                    close(actual.depth, reference[2], 2e-10,
                          "Resolved wire depth independent camera branch oracle");
                }
            auto wrong_size = old;
            ++wrong_size.width;
            need(!host::pack_config_from_parameters(p, wrong_size, true, 1).config,
                 "Resolved wire camera dimensions must match");
            auto bad_basis = p;
            bad_basis.right = {};
            need(!host::pack_config_from_parameters(bad_basis, old, true, 1).config,
                 "Invalid resolved basis is not rescued by metadata reconstruction");
            auto nonfinite_basis = p;
            nonfinite_basis.forward.z = std::numeric_limits<float>::quiet_NaN();
            need(!host::pack_config_from_parameters(nonfinite_basis, old, true, 1).config,
                 "Nonfinite resolved physical camera is rejected");
            auto wrong_model = p;
            wrong_model.model = 99;
            need(!host::pack_config_from_parameters(wrong_model, old, true, 1).config,
                 "Unknown resolved camera branch cannot substitute metadata projection");
        }
}
void deterministic_parameters() {
    render_wire::Parameters physical{};
    physical.width = 17;
    physical.height = 19;
    physical.model = render_wire::pbrt_model;
    physical.sampler_kind = render_wire::sobol_sampler;
    physical.sample_low = 123;
    physical.sample_high = 456;
    physical.seed_low = 789;
    physical.seed_high = 987;
    physical.packet_transport = 2;
    physical.hero_wavelengths = 1;
    physical.bidirectional = 1;
    physical.lens_radius = .5f;
    physical.pixel_begin = 7;
    physical.pixel_count = physical.lane_count = 11;
    physical.origin = {2, 3, 4};
    physical.forward = {0, 0, 1};
    physical.material_seed = -17;
    physical.camera_medium = 3;
    physical.filter_kind = 2;
    physical.filter_sigma = .75f;
    physical.instance_count = 27;
    physical.material_count = 13;
    const auto unchanged = physical;
    const auto built = host::center_geometry_parameters(physical);
    need(built.parameters.has_value() && built.diagnostic.empty(),
         "Resolved geometry parameters copy succeeds");
    need(std::memcmp(&physical, &unchanged, sizeof(physical)) == 0,
         "Physical transport parameters remain byte-identical");
    auto expected = physical;
    expected.sampler_kind = render_wire::independent_sampler;
    expected.sample_low = expected.sample_high = expected.seed_low = expected.seed_high = 0;
    expected.packet_transport = expected.hero_wavelengths = expected.bidirectional = 0;
    expected.lens_radius = 0;
    expected.pixel_begin = 0;
    expected.pixel_count = expected.lane_count = 17 * 19;
    need(std::memcmp(&*built.parameters, &expected, sizeof(expected)) == 0,
         "Only declared deterministic representative-query overrides change");
    auto labels = physical;
    labels.sample_low++;
    labels.sample_high++;
    labels.seed_low++;
    labels.seed_high++;
    const auto relabeled = host::center_geometry_parameters(labels);
    need(relabeled.parameters &&
             std::memcmp(&*built.parameters, &*relabeled.parameters, sizeof(expected)) == 0,
         "Physical labels and seeds cannot affect representative parameters");
    auto bad = physical;
    bad.model = 0xffffffffu;
    auto rejected = host::center_geometry_parameters(bad);
    need(!rejected.parameters && !rejected.diagnostic.empty(), "Unknown resolved model rejected");
    for (auto dims : {std::array<unsigned, 2>{0, 19},
                      {17, 0},
                      {host::maximum_dimension + 1, 1},
                      {1, host::maximum_dimension + 1},
                      {65536, 65536}}) {
        bad = physical;
        bad.width = dims[0];
        bad.height = dims[1];
        rejected = host::center_geometry_parameters(bad);
        need(!rejected.parameters && !rejected.diagnostic.empty(),
             "Representative parameter dimensions validated");
    }
    for (auto dims :
         {std::array<unsigned, 2>{host::maximum_dimension, 1}, {1, host::maximum_dimension}}) {
        bad = physical;
        bad.width = dims[0];
        bad.height = dims[1];
        const auto edge = host::center_geometry_parameters(bad);
        need(edge.parameters && edge.parameters->pixel_count == host::maximum_dimension,
             "Representative parameter exact-center dimension boundary admitted");
    }
    bad = physical;
    bad.model = render_wire::demo_model;
    need(host::center_geometry_parameters(bad).parameters.has_value(),
         "Known demo scene model accepted by parameter packer");
}
} // namespace
int main() {
    try {
        camera_packing();
        raster_preference_and_ranges();
        large_origin();
        deterministic_parameters();
        resolved_wire_projection();
        std::cout << "Enhanced motion host packing passed " << checks << " CPU checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Enhanced motion host packing: " << error.what() << '\n';
        return 1;
    }
}
