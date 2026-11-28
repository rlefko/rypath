// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "enhanced_motion_wire.h"
#include "frame_session.hpp"
#include "reconstruction_camera.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace rypath::enhanced_motion_host {
namespace motion = rypath_enhanced_motion;
namespace wire = enhanced_motion_wire;
// Representative geometry only. This guide plane never cuts physical transport.
inline constexpr double guide_near_plane = .001;
// Last pixel centers remain exactly representable in binary32 on either axis.
inline constexpr std::uint32_t maximum_dimension = 1u << 23;
inline bool valid_dimensions(std::uint32_t width, std::uint32_t height) {
    return width <= maximum_dimension && height <= maximum_dimension &&
           motion::dimensions(width, height);
}
struct ConfigResult {
    std::optional<wire::Config> config;
    std::string diagnostic;
    std::optional<reconstruction::Camera> current_camera, previous_camera;
    reconstruction::Point common_origin{};
    bool current_used_raster = false, previous_used_raster = false;
};
// Form differences BEFORE packing, as in reconstruction guide camera uploads.
// Inputs are double camera metadata, never absolute FLOAT surface positions.
inline bool checked_difference(reconstruction::Point a, reconstruction::Point b,
                               motion::Vec3 &out) {
    float words[3]{};
    for (unsigned k = 0; k < 3; ++k) {
        const double difference = a[k] - b[k];
        if (!std::isfinite(difference) || std::abs(difference) > std::numeric_limits<float>::max())
            return false;
        words[k] = static_cast<float>(difference);
        if (!std::isfinite(words[k]) || (difference != 0 && words[k] == 0))
            return false;
    }
    out = {words[0], words[1], words[2]};
    return true;
}
inline bool pack_camera(const reconstruction::Camera &source, reconstruction::Point common_origin,
                        motion::Camera &destination) {
    destination.width = source.width();
    destination.height = source.height();
    for (unsigned k = 0; k < 16; ++k) {
        destination.world_to_view[k] = source.world_to_view()[k];
        destination.view_to_clip[k] = source.view_to_clip()[k];
    }
    return checked_difference(common_origin, source.matrix_origin(),
                              destination.common_to_matrix_origin);
}
// Both cameras must be valid even when history is cold, matching Config's wire
// contract. A cold caller can pass current as previous. Raster preference exactly
// matches metal_reconstruction's make_camera; aperture is not a projection input.
inline ConfigResult pack_config(const FrameCamera &current, const FrameCamera &previous,
                                std::uint32_t width, std::uint32_t height, bool history_valid,
                                std::uint64_t serial) {
    ConfigResult result;
    if (!valid_dimensions(width, height)) {
        result.diagnostic = "Enhanced motion dimensions must be 1..2^23 with a 32-bit pixel count";
        return result;
    }
    const reconstruction::CameraConfig camera_config{width, height, guide_near_plane, {}};
    const auto make_camera = [&](const FrameCamera &camera) {
        return camera.raster_camera
                   ? reconstruction::Camera::build(*camera.raster_camera, camera_config)
                   : reconstruction::Camera::build(camera.camera, camera_config);
    };
    auto current_built = make_camera(current);
    if (!current_built.camera) {
        result.diagnostic = "Enhanced motion current camera: " + current_built.diagnostic;
        return result;
    }
    auto previous_built = make_camera(previous);
    if (!previous_built.camera) {
        result.diagnostic = "Enhanced motion previous camera: " + previous_built.diagnostic;
        return result;
    }
    const auto common_origin = current_built.camera->origin();
    wire::Config config{};
    config.version = wire::contract;
    config.width = width;
    config.height = height;
    config.history_valid = history_valid ? 1u : 0u;
    config.policy = {.99f, .5f, 32.f * std::numeric_limits<float>::epsilon(), 0};
    config.serial_low = static_cast<std::uint32_t>(serial);
    config.serial_high = static_cast<std::uint32_t>(serial >> 32);
    config.reserved = 0;
    if (!pack_camera(*current_built.camera, common_origin, config.current_camera) ||
        !pack_camera(*previous_built.camera, common_origin, config.previous_camera) ||
        !checked_difference(previous_built.camera->origin(), common_origin,
                            config.previous_origin_to_common)) {
        result.diagnostic =
            "Enhanced motion camera-origin differences cannot be packed to finite FLOAT";
        return result;
    }
    result.config = config;
    result.current_camera = *current_built.camera;
    result.previous_camera = *previous_built.camera;
    result.common_origin = common_origin;
    result.current_used_raster = current.raster_camera.has_value();
    result.previous_used_raster = previous.raster_camera.has_value();
    return result;
}
struct RasterParametersResult {
    std::optional<RasterCamera> camera;
    std::string diagnostic;
};
// Follow primary_ray's actual model/filter branch using its already packed basis.
// This does not rewrite physical Parameters or authored FrameCamera metadata.
inline RasterParametersResult raster_from_parameters(const render_wire::Parameters &p) {
    RasterParametersResult result;
    if (!valid_dimensions(p.width, p.height) ||
        (p.model != render_wire::demo_model && p.model != render_wire::pbrt_model)) {
        result.diagnostic = "Enhanced motion projection requires valid resolved model/dimensions";
        return result;
    }
    const auto point = [](render_wire::Point v) { return GeometryPoint{v.x, v.y, v.z}; };
    RasterCamera camera{point(p.origin), point(p.forward), point(p.right), point(p.up)};
    const bool centered = p.filter_kind != render_wire::box_filter || p.filter_x_radius != .5f ||
                          p.filter_y_radius != .5f;
    if (p.model == render_wire::demo_model && !centered) {
        const float horizontal = (float(p.width) / float(p.height)) * p.film_scale;
        const float vertical = p.film_scale;
        camera.corner = {(p.forward.x - horizontal * p.right.x) + vertical * p.up.x,
                         (p.forward.y - horizontal * p.right.y) + vertical * p.up.y,
                         (p.forward.z - horizontal * p.right.z) + vertical * p.up.z};
        camera.dx = {2.f * horizontal * p.right.x, 2.f * horizontal * p.right.y,
                     2.f * horizontal * p.right.z};
        camera.dy = {-2.f * vertical * p.up.x, -2.f * vertical * p.up.y, -2.f * vertical * p.up.z};
    }
    result.camera = camera; // Camera::build performs finite/conditioning checks.
    return result;
}
inline ConfigResult pack_config_from_parameters(const render_wire::Parameters &current,
                                                const render_wire::Parameters &previous,
                                                bool history_valid, std::uint64_t serial) {
    ConfigResult result;
    if (current.width != previous.width || current.height != previous.height) {
        result.diagnostic = "Enhanced motion resolved camera dimensions differ";
        return result;
    }
    const auto a = raster_from_parameters(current), b = raster_from_parameters(previous);
    if (!a.camera || !b.camera) {
        result.diagnostic = !a.camera ? a.diagnostic : b.diagnostic;
        return result;
    }
    FrameCamera ca, cb;
    ca.raster_camera = *a.camera;
    cb.raster_camera = *b.camera;
    return pack_config(ca, cb, current.width, current.height, history_valid, serial);
}
struct GeometryParametersResult {
    std::optional<render_wire::Parameters> parameters;
    std::string diagnostic;
};
// Input must already contain the resolved CURRENT physical camera/scene/texture
// context. Copy it; never mutate transport state or replace its camera origin.
// The kernel supplies center-film/lens words and 550nm directly, independently of
// random streams. Material seed, alpha/null visibility, filter and scene data stay
// identical; only the representative query's execution/sample controls change.
inline GeometryParametersResult
center_geometry_parameters(const render_wire::Parameters &resolved) {
    GeometryParametersResult result;
    if (!valid_dimensions(resolved.width, resolved.height)) {
        result.diagnostic =
            "Enhanced center geometry dimensions must be 1..2^23 with a 32-bit pixel count";
        return result;
    }
    if (resolved.model != render_wire::demo_model && resolved.model != render_wire::pbrt_model) {
        result.diagnostic = "Enhanced center geometry requires a known resolved scene model";
        return result;
    }
    auto p = resolved;
    p.sampler_kind = render_wire::independent_sampler;
    p.sample_low = p.sample_high = p.seed_low = p.seed_high = 0;
    p.packet_transport = p.hero_wavelengths = p.bidirectional = 0;
    p.lens_radius = 0;
    p.pixel_begin = 0;
    p.pixel_count = p.lane_count = p.width * p.height;
    result.parameters = p;
    return result;
}
} // namespace rypath::enhanced_motion_host
