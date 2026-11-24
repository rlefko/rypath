// SPDX-License-Identifier: GPL-3.0-only
#include "frame_benchmark_motion.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "Invalid motion accepted");
}
} // namespace
int main() try {
    using namespace rypath;
    using namespace rypath::frame_benchmark;
    constexpr std::array<std::string_view, 10> phases{
        "warmup",    "warmup",   "hold-start", "hold-start", "translate",
        "translate", "hold-end", "hold-end",   "cut-start",  "cut-start"};
    constexpr std::array<float, 10> offsets{0, 0, 0, 0, .25f, .5f, .5f, .5f, 0, 0};
    for (std::uint64_t sample = 0; sample < phases.size(); ++sample) {
        const auto pose = motion_pose(sample, 2, 8, .5f);
        require(pose.phase == phases[sample] && pose.offset == offsets[sample] &&
                    pose.epoch == (sample >= 8 ? 1u : 0u),
                "Motion warmup, quarters or cut boundary changed");
        require(motion_pose(sample, 2, 8, {}).phase == "static", "Static control moved");
    }
    require(motion_pose(5, 0, 8, -.5f).offset == -.5f, "Negative translation reversed");
    rejects([] { (void)motion_pose(0, 0, 7, .5f); });
    rejects([] { (void)motion_pose(8, 0, 8, .5f); });
    rejects([] { (void)motion_pose(0, UINT64_MAX, 8, .5f); });
    rejects([] { (void)motion_pose(0, 0, 8, std::numeric_limits<float>::infinity()); });
    RenderScene scene;
    scene.camera.origin = {1, 2, 3};
    scene.camera.target = {1, 2, -3};
    scene.camera_medium = 3;
    scene.perspective_lens.emplace();
    scene.perspective_lens->radius = .125f;
    auto pose = translated_camera(scene, .5f);
    if (!pose.perspective_lens)
        throw std::runtime_error("Translated camera lost perspective lens");
    if (!pose.texture_camera)
        throw std::runtime_error("Translated camera lost texture transform");
    require(pose.camera.origin.x == 1.5f && pose.camera.target.x == 1.5f &&
                pose.camera.origin.y == 2 && pose.camera.target.z == -3 &&
                pose.camera_medium == 3 && pose.perspective_lens->radius == .125f &&
                pose.texture_camera->world_from_camera == scene.texture_camera.world_from_camera,
            "Demo translation changed lens, medium, linear camera or direction");
    scene.raster_camera = RasterCamera{{4, 5, 6}, {-.5f, .5f, -1}, {1, 0, 0}, {0, -1, 0}};
    pose = translated_camera(scene, -.25f);
    if (!pose.raster_camera)
        throw std::runtime_error("Translated camera lost raster snapshot");
    if (!scene.raster_camera)
        throw std::runtime_error("Source camera lost raster snapshot");
    require(pose.raster_camera->origin.x == 3.75f && pose.raster_camera->origin.y == 5 &&
                pose.raster_camera->corner.x == -.5f && pose.raster_camera->dx.x == 1 &&
                pose.raster_camera->dy.y == -1,
            "Raster translation changed ray directions");
    scene.raster_camera->origin.x = 0x1p30f;
    rejects([&] { (void)translated_camera(scene, .125f); });
    if (!scene.raster_camera)
        throw std::runtime_error("Source camera lost raster snapshot");
    scene.raster_camera->origin.x = std::numeric_limits<float>::max();
    rejects([&] { (void)translated_camera(scene, std::numeric_limits<float>::max()); });
    require(motion_medium_supported(scene), "Uniform camera medium rejected");
    scene.instance_media.push_back({1, 1});
    require(motion_medium_supported(scene), "Equal medium interface rejected");
    scene.instance_media.push_back({1, 2});
    require(!motion_medium_supported(scene), "Unclassified boundary crossing allowed");
    std::cout << "Frame motion schedule and camera controls passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
