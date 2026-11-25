// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"
#include "geometry_reference.hpp"
#include "metal_geometry_access.hpp"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
using namespace rypath;
void require(bool value, const std::string &diagnostic) {
    if (!value)
        throw std::runtime_error(diagnostic);
}
void available(GeometryStatus status, const std::string &diagnostic) {
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (status == GeometryStatus::no_device)
        throw 77;
#endif
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
    if (status == GeometryStatus::no_ray_tracing)
        throw 77;
#endif
    require(status == GeometryStatus::success, diagnostic);
}
void metadata(const AccelerationBuildInfo &info, AccelerationBuildPolicy policy) {
    require(info.requested == policy && info.effective == policy && info.supported == true,
            "Requested/effective AS build policy differs");
    require(info.blas_bytes && info.tlas_bytes && info.blas_count == 2 && info.tlas_count == 1 &&
                info.fast_blas_count == (acceleration_fast_blas(policy) ? 2u : 0u) &&
                info.fast_tlas_count == (acceleration_fast_tlas(policy) ? 1u : 0u) &&
                std::isfinite(info.build_cpu_wall_seconds) && info.build_cpu_wall_seconds > 0,
            "Actual AS resource/build telemetry differs");
}
void empty_scene(AccelerationBuildPolicy policy, const std::filesystem::path &library) {
    for (const bool packet : {false, true}) {
        RenderScene scene;
        scene.model = RenderModel::pbrt;
        scene.raster_camera = RasterCamera{{}, {-.3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
        scene.spectral_transport = packet ? SpectralTransport::packet : SpectralTransport::scalar;
        scene.environment.fill(.7f);
        scene.max_depth = 1;
        auto session = build_frame_session(
            scene, {.width = 8, .height = 8, .acceleration_build_policy = policy}, library);
        available(session.status, session.diagnostic);
        const auto &info = session.acceleration_build;
        const auto context = std::string(packet ? "packet " : "scalar ") +
                             std::string(acceleration_build_policy_name(policy));
        require(info.requested == policy && info.effective == policy && info.supported == true,
                "Empty-scene requested/effective AS policy differs: " + context);
        // Packet sky builds this TLAS in the renderer; scalar sky adds it in
        // FrameSession preparation. The packet path must not build it twice.
        require(info.blas_count == 0 && info.blas_bytes == 0 && info.fast_blas_count == 0 &&
                    info.tlas_count == 1 && info.tlas_bytes > 0 &&
                    info.fast_tlas_count == (acceleration_fast_tlas(policy) ? 1u : 0u) &&
                    std::isfinite(info.build_cpu_wall_seconds) && info.build_cpu_wall_seconds > 0,
                "Empty-scene actual TLAS policy/resource telemetry differs: " + context);
        require(info.device_allocated_bytes && *info.device_allocated_bytes >= info.tlas_bytes,
                "Empty-scene session allocation snapshot is absent: " + context);
        auto frame = session.session->submit({});
        require(frame.frame != nullptr, frame.diagnostic);
        const auto complete = frame.frame->wait();
        require(complete.status == FrameStatus::complete && complete.completed_samples == 64,
                "Empty-scene resident frame completion/accounting differs: " + context);
        std::cout << "PASS empty AS policy " << context << " tlas_bytes=" << info.tlas_bytes
                  << '\n';
    }
}
void run(const std::filesystem::path &library) {
    GeometryMesh mesh;
    mesh.positions = {{-2, -2, 0}, {6, -2, 0}, {-2, 6, 0}, {-1, -1, 2},
                      {1, -1, 2},  {-1, 1, 2}, {1, 1, 2}};
    mesh.triangles = {{0, 1, 2}};
    mesh.patches = {{3, 4, 5, 6}};
    GeometryData geometry{{mesh}, {{0, {}}, {0, {{-1, 0, 0, 8, 0, 2, 0, -3, 0, 0, .5f, 2}}}}};
    // The independent reference helper intersects triangles. These fixture
    // patches are planar squares, so triangulate them only for the CPU oracle.
    auto planar_oracle = geometry;
    planar_oracle.meshes[0].triangles.push_back({3, 4, 5});
    planar_oracle.meshes[0].triangles.push_back({4, 6, 5});
    planar_oracle.meshes[0].patches.clear();
    const std::array<GeometryRay, 9> rays{{{{0, 0, 3}, {0, 0, -2}},
                                           {{3, 0, 3}, {0, 0, -2}},
                                           {{0, 0, -3}, {0, 0, 2}},
                                           {{0, 0, 3}, {0, 0, -2}, 0, .49f},
                                           {{0, 0, 3}, {0, 0, -2}, 0, .51f},
                                           {{20, 20, 3}, {0, 0, -2}},
                                           {{0, 0, 3}, {1, 0, 0}},
                                           {{8, -3, 5}, {0, 0, -2}},
                                           {{5, -3, 5}, {0, 0, -2}}}};
    auto baseline = build_metal_geometry(geometry, library, {IntersectionProfile::native});
    available(baseline.status, baseline.diagnostic);
    metadata(baseline.acceleration_build, AccelerationBuildPolicy::default_build);
    const auto base_hits = baseline.scene->closest_hits(rays);
    const auto base_shadow = baseline.scene->occlusion(rays);
    available(base_hits.status, base_hits.diagnostic);
    available(base_shadow.status, base_shadow.diagnostic);
    auto device = MetalGeometryAccess::device(*baseline.scene);
    bool supported = false;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *))
        supported = device.supportsRaytracing && [device supportsFamily:MTLGPUFamilyApple9];
#endif
    (void)device;
    for (const auto policy :
         {AccelerationBuildPolicy::default_build, AccelerationBuildPolicy::fast_blas,
          AccelerationBuildPolicy::fast_tlas, AccelerationBuildPolicy::fast_all}) {
        auto built = build_metal_geometry(geometry, library, {IntersectionProfile::native, policy});
        if (policy != AccelerationBuildPolicy::default_build && !supported) {
            require(built.status == GeometryStatus::failure && !built.scene &&
                        built.acceleration_build.requested == policy &&
                        built.acceleration_build.supported == false &&
                        !built.acceleration_build.effective &&
                        !built.acceleration_build.diagnostic.empty(),
                    "Unsupported explicit AS request silently fell back or skipped");
            std::cout << "PASS explicit unsupported policy "
                      << acceleration_build_policy_name(policy) << '\n';
            continue;
        }
        available(built.status, built.diagnostic);
        metadata(built.acceleration_build, policy);
        auto hits = built.scene->closest_hits(rays);
        auto shadow = built.scene->occlusion(rays);
        available(hits.status, hits.diagnostic);
        available(shadow.status, shadow.diagnostic);
        for (unsigned i = 0; i < rays.size(); ++i) {
            const auto oracle = test::reference_closest(planar_oracle, rays[i]);
            const auto &hit = hits.hits[i];
            const auto context =
                std::string(acceleration_build_policy_name(policy)) + " ray " + std::to_string(i);
            require(hit.hit == !oracle.empty(), "AS oracle hit/miss differs: " + context);
            if (hit.hit) {
                const bool matches = std::ranges::any_of(oracle, [&](const auto &expected) {
                    const bool patch = expected.primitive_index != 0;
                    return expected.mesh_index == hit.mesh_index &&
                           expected.instance_index == hit.instance_index &&
                           hit.kind ==
                               (patch ? PrimitiveKind::bilinear_patch : PrimitiveKind::triangle) &&
                           hit.primitive_index == 0 &&
                           std::abs(double(hit.t) - expected.t) <=
                               2e-5 * std::max(1., std::abs(expected.t));
                });
                require(matches,
                        "AS independent planar/triangle distance/identity oracle: " + context);
                if (hit.kind == PrimitiveKind::triangle) {
                    test::GeometryErrors errors;
                    const auto expected = test::reference_closest(geometry, rays[i]);
                    const auto difference =
                        test::compare_closest(geometry, rays[i], expected, hit, errors);
                    require(difference.empty(),
                            "AS triangle barycentric oracle: " + context + ": " + difference);
                } else {
                    require(std::abs(hit.patch_uv[0] - .5f) < 2e-5f &&
                                std::abs(hit.patch_uv[1] - .5f) < 2e-5f,
                            "AS planar patch parameter oracle: " + context);
                }
            }
            require(bool(shadow.occluded[i]) == !oracle.empty() &&
                        shadow.occluded[i] == base_shadow.occluded[i] &&
                        hits.hits[i].hit == base_hits.hits[i].hit &&
                        (!hits.hits[i].hit ||
                         (hits.hits[i].instance_index == base_hits.hits[i].instance_index &&
                          hits.hits[i].primitive_index == base_hits.hits[i].primitive_index)),
                    "AS build policy changed ordinary closest/occlusion identity");
        }
        auto scene = make_demo_scene();
        scene.geometry = geometry;
        scene.instance_materials = {0, 0};
        scene.materials.resize(1);
        auto session = build_frame_session(
            scene, {.width = 8, .height = 8, .acceleration_build_policy = policy}, library);
        available(session.status, session.diagnostic);
        metadata(session.acceleration_build, policy);
        require(session.acceleration_build.device_allocated_bytes &&
                    *session.acceleration_build.device_allocated_bytes >=
                        session.acceleration_build.blas_bytes +
                            session.acceleration_build.tlas_bytes,
                "Session allocation telemetry is absent or smaller than retained AS allocations");
        auto frame = session.session->submit({});
        require(frame.frame != nullptr, frame.diagnostic);
        require(frame.frame->wait().status == FrameStatus::complete,
                "AS policy resident frame failed");
        empty_scene(policy, library);
        std::cout << "PASS AS policy " << acceleration_build_policy_name(policy)
                  << " blas_bytes=" << built.acceleration_build.blas_bytes
                  << " tlas_bytes=" << built.acceleration_build.tlas_bytes << '\n';
    }
    auto rejected = build_metal_geometry(
        geometry, library, {IntersectionProfile::compensated, AccelerationBuildPolicy::fast_all});
    require(rejected.status == GeometryStatus::failure && !rejected.scene &&
                !rejected.acceleration_build.effective && !rejected.diagnostic.empty(),
            "Compensated profile silently accepted fast AS policy");
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            run(argc > 1 ? argv[1] : "");
            return 0;
        } catch (int code) {
            return code;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
