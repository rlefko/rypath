// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "frame_session.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace rypath::frame_benchmark {
// Timing-only camera protocol. Moving frames have no reference-quality claim.
// Warmup stays at A; measured quarters hold A, translate A->B, hold B, cut to A.
struct MotionPose {
    std::string_view phase = "static";
    float offset = 0;
    std::uint64_t epoch = 0;
};
inline MotionPose motion_pose(std::uint64_t sample, std::uint64_t warmup, std::uint64_t frames,
                              std::optional<float> offset) {
    if (!offset)
        return {};
    if (!std::isfinite(*offset) || *offset == 0 || frames < 8 || frames % 4 != 0 ||
        frames > std::numeric_limits<std::uint64_t>::max() - warmup || sample >= warmup + frames)
        throw std::invalid_argument("Invalid frame motion schedule");
    if (sample < warmup)
        return {"warmup", 0, 0};
    const auto index = sample - warmup;
    const auto quarter = frames / 4;
    if (index < quarter)
        return {"hold-start", 0, 0};
    if (index < 2 * quarter)
        return {"translate", float(double(*offset) * double(index - quarter + 1) / double(quarter)),
                0};
    if (index < 3 * quarter)
        return {"hold-end", *offset, 0};
    return {"cut-start", 0, 1};
}

// This driver cannot classify a moving camera's crossings of medium interfaces.
inline bool motion_medium_supported(const RenderScene &scene) {
    for (const auto &medium : scene.instance_media)
        if (medium.inside != medium.outside)
            return false;
    return true;
}

inline FrameCamera translated_camera(const RenderScene &scene, float offset) {
    if (!std::isfinite(offset))
        throw std::invalid_argument("Motion offset must be finite");
    FrameCamera pose{.camera = scene.camera,
                     .camera_medium = scene.camera_medium,
                     .raster_camera = scene.raster_camera,
                     .perspective_lens = scene.perspective_lens,
                     .texture_camera = scene.texture_camera};
    const auto translate = [offset](GeometryPoint &point, bool active) {
        const double x = double(point.x) + offset;
        if (!std::isfinite(x) || std::abs(x) > std::numeric_limits<float>::max())
            throw std::invalid_argument("Translated camera exceeds float range");
        const float translated = float(x);
        if (active && offset != 0 && translated == point.x)
            throw std::invalid_argument("Motion offset is below active camera float resolution");
        point.x = translated;
    };
    if (pose.raster_camera) {
        translate(pose.raster_camera->origin, true);
        // Compatibility fields do not generate raster rays. As in the quality
        // driver, preserve them if the translated value is not representable.
        for (auto *point : {&pose.camera.origin, &pose.camera.target}) {
            const double x = double(point->x) + offset;
            if (std::isfinite(x) && std::abs(x) <= std::numeric_limits<float>::max())
                point->x = float(x);
        }
    } else {
        translate(pose.camera.origin, true);
        translate(pose.camera.target, false);
        const double dx = double(pose.camera.target.x) - pose.camera.origin.x;
        const double dy = double(pose.camera.target.y) - pose.camera.origin.y;
        const double dz = double(pose.camera.target.z) - pose.camera.origin.z;
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
            (dx == 0 && dy == 0 && dz == 0))
            throw std::invalid_argument("Translated camera direction is nonfinite or collapsed");
    }
    // TextureCamera holds linear matrices; translations leave them unchanged.
    return pose;
}
} // namespace rypath::frame_benchmark
