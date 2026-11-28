// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session_test_access.hpp"
#include "pbrt_render.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace rypath;
void need(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
RenderScene load(const char *path) {
    auto checked = pbrt::check(path);
    need(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, {path}, {}, SpectralTransportSelection::hero);
    if (translated.exit_code() != 0 || !translated.scene)
        throw std::runtime_error("Vacuum fixture translation failed");
    auto scene = std::move(*translated.scene);
    // Live frames require unclamped output; keep authored integrator/media intact.
    scene.max_component_value = std::numeric_limits<double>::infinity();
    return scene;
}
FrameCapture capture(FrameSession &session) {
    auto submitted = session.submit({.sample_index = 0});
    need(submitted.status == GeometryStatus::success && bool(submitted.frame),
         submitted.diagnostic);
    const auto info = submitted.frame->wait();
    need(info.status == FrameStatus::complete && info.coating_proposal_capability_verified &&
             info.coating_proposal == FrameCoatingProposal::shared_diffuse,
         info.diagnostic);
    auto result = submitted.frame->capture();
    need(result.status == GeometryStatus::success, result.diagnostic);
    return result;
}
std::unique_ptr<FrameSession> build(const RenderScene &scene, const FrameConfig &config,
                                    const std::filesystem::path &library) {
    auto result = build_frame_session(scene, config, library);
    need(result.status == GeometryStatus::success && bool(result.session), result.diagnostic);
    return std::move(result.session);
}
} // namespace
int main(int argc, char **argv) {
    try {
        need(argc == 4, "frame_coating_vacuum_tests PT_SCENE DEFAULT_VOLPATH_SCENE METALLIB");
        const auto device = query_metal_device();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
        if (device.status == MetalStatus::no_device)
            return 77;
#endif
        need(device.status == MetalStatus::success, device.diagnostic);
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
        if (!device.device.compute_ray_tracing)
            return 77;
#endif
        need(device.device.compute_ray_tracing, "Compute ray tracing unavailable");
        auto pt = load(argv[1]), volume = load(argv[2]);
        need(pt.integrator == IntegratorKind::path && !pt.volume_transport,
             "PT fixture must retain authored surface mode");
        need(volume.integrator == IntegratorKind::volpath && volume.volume_transport &&
                 has_proven_vacuum_media(volume) && volume.max_depth == pt.max_depth,
             "Missing Integrator must translate to genuine vacuum volpath");
        // Explicit empty per-instance entries are also a supported translated form.
        volume.instance_media.assign(volume.geometry.instances.size(), {no_medium, no_medium});
        FrameConfig on;
        on.width = on.height = 8;
        on.seed = 702211;
        on.coating_proposal = FrameCoatingProposal::shared_diffuse;
        auto off = on;
        off.coating_proposal = FrameCoatingProposal::hero_only;
        const std::filesystem::path library = argv[3];
        auto path_session = build(pt, on, library), volume_session = build(volume, on, library);
        const auto a = capture(*path_session), b = capture(*volume_session);
        need(a.raw_xyz == b.raw_xyz, "Vacuum PT/volpath must use identical hero surface transport");
        auto pair = FrameSessionTestAccess::build_shared_scene(volume, off, on, library);
        need(pair.status == GeometryStatus::success && pair.shared_renderer, pair.diagnostic);
        need(capture(*pair.sessions[1]).raw_xyz == b.raw_xyz,
             "Normal/shared-AS vacuum admission and transport must agree");
        volume.media.emplace_back();
        volume.instance_media[0] = {0, 0};
        auto equal_sides = build(volume, on, library);
        need(capture(*equal_sides).raw_xyz == b.raw_xyz,
             "Equal zero-coefficient sides retain vacuum without optical changes");
        FrameCamera camera;
        camera.camera = volume.camera;
        camera.camera_medium = 0;
        const auto override_result = equal_sides->submit({.sample_index = 1, .camera = camera});
        need(override_result.status != GeometryStatus::success &&
                 override_result.diagnostic.find("scalar PBRT volume") != std::string::npos,
             "Hero session must reject later camera medium overrides");
        const auto reject = [&](const RenderScene &scene) {
            const auto normal = build_frame_session(scene, on, library);
            need(normal.status != GeometryStatus::success &&
                     normal.diagnostic.find("vacuum") != std::string::npos,
                 "Normal constructor must reject non-vacuum coating requests");
            const auto shared = FrameSessionTestAccess::build_shared_scene(scene, off, on, library);
            need(shared.status != GeometryStatus::success,
                 "Shared-AS constructor must reject the same non-vacuum scene");
        };
        auto participating = volume;
        participating.media[0].sigma_s[470] = .1f;
        reject(participating); // Includes conservative, unretained light-endpoint assignments.
        auto camera_medium = volume;
        camera_medium.camera_medium = 0;
        reject(camera_medium);
        auto boundary = volume;
        boundary.instance_media[0] = {0, no_medium};
        reject(boundary);
        std::cout << "Authored vacuum volpath matches PT exactly; normal/shared-AS, equal-side, "
                     "camera override and participating-medium controls passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
