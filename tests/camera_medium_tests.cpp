// SPDX-License-Identifier: GPL-3.0-only
#include "camera_medium.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
namespace {
using namespace rypath;
namespace cm = camera_medium_wire;
void need(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
constexpr GeometryPoint outside{.25f, -.5f, 0}, inside{.25f, -.5f, 1.5f};
RenderScene base(ScatteringKind scattering = ScatteringKind::null_interface) {
    RenderScene s;
    s.model = RenderModel::pbrt;
    s.integrator = IntegratorKind::volpath;
    s.volume_transport = true;
    s.spectral_transport = SpectralTransport::scalar;
    s.raster_camera = RasterCamera{outside, {-.05f, -.05f, 1}, {.1f, 0, 0}, {0, .1f, 0}};
    s.environment.fill(.25f);
    s.max_depth = 2;
    s.media.resize(2);
    s.media[0].sigma_a.fill(.2f);
    s.media[1].sigma_a.fill(.35f);
    SpectralMaterial m;
    m.kind = SpectralMaterialKind::constant;
    m.scattering = scattering;
    if (scattering == ScatteringKind::diffuse)
        m.constant = .5f;
    if (scattering == ScatteringKind::dielectric) {
        m.eta.knots = {{360, 1.5f}, {830, 1.5f}};
        m.eta.extend_endpoints = true;
    }
    s.materials.push_back(m);
    return s;
}
void plane(RenderScene &s, float z, bool positive, MediumInterface medium,
           GeometryTransform placement = {}) {
    GeometryMesh mesh;
    mesh.positions = {{-8, -8, z}, {8, -8, z}, {8, 8, z}, {-8, 8, z}};
    mesh.triangles = positive ? std::vector<std::array<std::uint32_t, 3>>{{0, 1, 2}, {0, 2, 3}}
                              : std::vector<std::array<std::uint32_t, 3>>{{0, 2, 1}, {0, 3, 2}};
    const auto index = std::uint32_t(s.geometry.meshes.size());
    s.geometry.meshes.push_back(std::move(mesh));
    s.geometry.instances.push_back({index, placement});
    s.instance_materials.push_back(0);
    s.instance_media.push_back(medium);
    SurfaceTransforms transform;
    const auto &a = placement.values;
    transform.shape = {a[0], a[1], a[2],  a[3],  a[4], a[5], a[6], a[7],
                       a[8], a[9], a[10], a[11], 0,    0,    0,    1};
    s.instance_surface_transforms.push_back(transform);
}
void prepare(RenderScene &s) {
    const bool contexts = std::ranges::any_of(s.materials, [](const auto &m) {
        return m.scattering != ScatteringKind::diffuse &&
               m.scattering != ScatteringKind::null_interface;
    });
    if (contexts) {
        s.instance_texture_frames.resize(s.geometry.instances.size());
        for (std::size_t i = 0; i < s.geometry.instances.size(); ++i) {
            if (!instance_uses_texture_context(s, i))
                continue;
            const auto &t = s.instance_surface_transforms[i];
            const auto error = prepare_texture_triangle_frames(
                s.geometry.meshes[s.geometry.instances[i].mesh_index], t.shape, t.placement,
                s.instance_texture_frames[i]);
            need(error.empty(), "Classifier fixture derivative frames: " + error);
        }
    }
    const auto error = validate_render_scene(s);
    need(error.empty(), "Classifier fixture: " + error);
}
RenderScene slab(ScatteringKind kind = ScatteringKind::null_interface) {
    auto s = base(kind);
    plane(s, 1, false, {0, no_medium});
    plane(s, 2, true, {0, no_medium});
    prepare(s);
    return s;
}
std::unique_ptr<FrameSession> session(const RenderScene &s, const std::filesystem::path &library,
                                      IntersectionProfile profile = IntersectionProfile::native) {
    FrameConfig c;
    c.width = 4;
    c.height = 3;
    c.intersection_profile = profile;
    c.estimator = FrameEstimator::independent;
    auto built = build_frame_session(s, c, library);
    need(built.status == GeometryStatus::success && built.session, built.diagnostic);
    return std::move(built.session);
}
std::unique_ptr<Frame> frame(FrameSession &session, const RenderScene &s, GeometryPoint origin,
                             std::optional<std::uint32_t> medium) {
    FrameCamera snapshot{.camera = s.camera,
                         .camera_medium = medium,
                         .raster_camera = s.raster_camera,
                         .perspective_lens = s.perspective_lens,
                         .texture_camera = s.texture_camera};
    if (!snapshot.raster_camera)
        throw std::runtime_error("Classifier fixture requires raster camera");
    snapshot.raster_camera->origin = origin;
    auto result = session.submit(
        {.sample_index = (1ull << 32) + 23, .epoch = 3, .camera = std::move(snapshot)});
    need(result.status == GeometryStatus::success && result.frame && !result.busy,
         result.diagnostic);
    const auto info = result.frame->wait();
    need(info.status == FrameStatus::complete && info.completed_samples == 12,
         info.diagnostic.empty() ? "Classifier source-frame accounting" : info.diagnostic);
    return std::move(result.frame);
}
std::unique_ptr<CameraMediumTracker> tracker(const Frame &source) {
    auto built = CameraMediumTracker::build(source);
    need(built.status == GeometryStatus::success && built.tracker, built.diagnostic);
    return std::move(built.tracker);
}
CameraMediumResult classified(CameraMediumTracker &tracker, const Frame &source,
                              GeometryPoint destination, std::uint32_t medium,
                              std::uint32_t crossings) {
    const auto before = source.capture();
    need(before.status == GeometryStatus::success, "Classifier raw before-capture");
    const auto result = tracker.classify(source, destination);
    need(result.status == GeometryStatus::success && result.failure == cm::Status::success,
         result.diagnostic);
    need(result.medium == medium && result.crossings == crossings,
         "Independent interface medium/crossing oracle");
    need(!result.gpu_time_valid || (std::isfinite(result.gpu_seconds) && result.gpu_seconds >= 0),
         "Classifier GPU timing validity");
    const auto after = source.capture();
    need(after.status == GeometryStatus::success && after.raw_xyz == before.raw_xyz,
         "Camera classifier leaves raw XYZ unchanged");
    return result;
}
CameraMediumResult rejected(CameraMediumTracker &tracker, const Frame &source,
                            GeometryPoint destination, cm::Status expected,
                            std::uint32_t limit = medium_boundary_limit) {
    const auto before = source.capture();
    const auto result = tracker.classify(source, destination, limit);
    need(result.status == GeometryStatus::failure && result.failure == expected &&
             !result.diagnostic.empty(),
         "Expected typed classifier rejection " + std::to_string(unsigned(expected)) +
             "; status=" + std::to_string(unsigned(result.status)) +
             " failure=" + std::to_string(unsigned(result.failure)) + " destination=(" +
             std::to_string(destination.x) + "," + std::to_string(destination.y) + "," +
             std::to_string(destination.z) + "): " + result.diagnostic);
    need(source.capture().raw_xyz == before.raw_xyz, "Rejected classifier query preserves raw XYZ");
    return result;
}
void enter_exit(const std::filesystem::path &library, ScatteringKind kind) {
    auto s = slab(kind);
    auto owner = session(s, library);
    auto start = frame(*owner, s, outside, no_medium);
    auto query = tracker(*start);
    classified(*query, *start, outside, no_medium, 0);
    const auto enter = classified(*query, *start, inside, 0, 1);
    const auto limited = query->classify(*start, inside, 1);
    need(limited.status == GeometryStatus::success && limited.medium == 0 && limited.crossings == 1,
         "A future exit must not consume the already-reached crossing budget");
    rejected(*query, *start, {.25f, -.5f, 1}, cm::Status::unresolved_endpoint);
    rejected(*query, *start, {.25f, -.5f, 2}, cm::Status::unresolved_endpoint);
    classified(*query, *start, {.25f, -.5f, 2.5f}, no_medium, 2);
    auto moved = frame(*owner, s, inside, enter.medium);
    classified(*query, *moved, inside, 0, 0);
    rejected(*query, *moved, {.25f, -.5f, 2}, cm::Status::unresolved_endpoint);
    classified(*query, *moved, {.25f, -.5f, 2.5f}, no_medium, 1);
    start.reset();
    // Sessions do not remember an earlier request's explicit override. Omitting
    // it restores the scene's authored vacuum label, which the exit must reject.
    auto omitted = frame(*owner, s, inside, {});
    rejected(*query, *omitted, {.25f, -.5f, 2.5f}, cm::Status::inconsistent_side);
}
void multiple_and_limits(const std::filesystem::path &library) {
    auto s = base();
    plane(s, .5f, true, {1, 1});
    plane(s, 1, false, {0, no_medium});
    plane(s, 2, true, {0, no_medium});
    plane(s, 3, false, {1, no_medium});
    plane(s, 4, true, {1, no_medium});
    prepare(s);
    auto owner = session(s, library);
    auto start = frame(*owner, s, outside, no_medium);
    auto query = tracker(*start);
    classified(*query, *start, {.25f, -.5f, .75f}, no_medium, 0);
    const auto far = classified(*query, *start, {.25f, -.5f, 3.5f}, 1, 3);
    classified(*query, *start, {.25f, -.5f, 5}, no_medium, 4);
    const auto exhausted =
        rejected(*query, *start, {.25f, -.5f, 5}, cm::Status::boundary_exhaustion, 2);
    need(exhausted.crossings == 2 && exhausted.medium == no_medium,
         "Crossing exhaustion preserves the last completed interface label");
    for (const auto limit : {0u, cm::maximum_crossings + 1})
        rejected(*query, *start, inside, cm::Status::invalid_config, limit);
    for (const auto bad :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
        rejected(*query, *start, {bad, -.5f, 1.5f}, cm::Status::invalid_config);
    rejected(*query, *start, {std::numeric_limits<float>::denorm_min(), -.5f, 1.5f},
             cm::Status::unrepresentable_segment);
    // A represented endpoint strictly inside one triangle, away from all mesh
    // edges. This is a native profile regression, not universal point membership.
    rejected(*query, *start, {.25f, -.5f, 1}, cm::Status::unresolved_endpoint);
    auto moved = frame(*owner, s, {.25f, -.5f, 3.5f}, far.medium);
    classified(*query, *moved, {.25f, -.5f, 5}, no_medium, 1);
}
void transformed(const std::filesystem::path &library) {
    // Direct-API triangle normals follow transformed vertex winding. Reflection
    // reverses the two half-spaces; swap labels to represent the same world slab.
    for (const bool reflection : {false, true}) {
        auto s = base();
        const GeometryTransform transform =
            reflection ? GeometryTransform{{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}
                       : GeometryTransform{{1, 0, .4f, 0, 0, 1, 0, 0, .25f, 0, 1, 0}};
        const MediumInterface sides =
            reflection ? MediumInterface{no_medium, 0} : MediumInterface{0, no_medium};
        plane(s, 1, false, sides, transform);
        plane(s, 2, true, sides, transform);
        prepare(s);
        auto owner = session(s, library);
        auto start = frame(*owner, s, outside, no_medium);
        auto query = tracker(*start);
        // Shear gives z-.25x=.9*z_local; at x=.25 the planes are
        // z=.9625 and1.8625. Reflection keeps world z=1 and2.
        const GeometryPoint middle{.25f, -.5f, reflection ? 1.5f : 1.3f};
        const auto result = classified(*query, *start, middle, 0, 1);
        auto moved = frame(*owner, s, middle, result.medium);
        classified(*query, *moved, {.25f, -.5f, 3}, no_medium, 1);
    }
}
void analytic_two_roots(const std::filesystem::path &library) {
    auto s = base();
    const GeometryPoint origin{-2, .25f, .125f}, middle{0, .25f, .125f},
        destination{2, .25f, .125f};
    // A unit sphere has two distinct roots x=+/-sqrt(1-y*y-z*z).
    // Both intersections belong to the same primitive. Re-querying the original
    // ray must suppress only the earlier root, rather than the whole primitive.
    // Nonzero y,z keep this chord away from the sphere's seam and poles.
    s.raster_camera = RasterCamera{origin, {-1, -.05f, -.05f}, {0, .1f, 0}, {0, 0, .1f}};
    GeometryMesh mesh;
    mesh.analytics.emplace_back();
    s.geometry.meshes.push_back(std::move(mesh));
    s.geometry.instances.push_back({0, {}});
    s.instance_materials.push_back(0);
    s.instance_media.push_back({0, no_medium});
    s.instance_surface_transforms.emplace_back();
    prepare(s);
    auto owner = session(s, library);
    auto start = frame(*owner, s, origin, no_medium);
    auto query = tracker(*start);
    const auto entry = classified(*query, *start, middle, 0, 1);
    classified(*query, *start, destination, no_medium, 2);
    auto moved = frame(*owner, s, middle, entry.medium);
    classified(*query, *moved, destination, no_medium, 1);
}
void thin_slab_regression(const std::filesystem::path &library) {
    auto s = base();
    const GeometryPoint origin{0, -.5f, .25f}, destination{2, -.5f, .25f};
    // Raw camera rays point away from the thin slab. This test isolates point
    // classification from any independent radiance-offset policy at that slab.
    s.raster_camera = RasterCamera{origin, {-1, -.05f, -.05f}, {0, .1f, 0}, {0, 0, .1f}};
    const auto wall = [&](float x, bool positive) {
        GeometryMesh mesh;
        mesh.positions = {{x, -128, -128}, {x, 128, -128}, {x, 128, 128}, {x, -128, 128}};
        mesh.triangles = positive ? std::vector<std::array<std::uint32_t, 3>>{{0, 1, 2}, {0, 2, 3}}
                                  : std::vector<std::array<std::uint32_t, 3>>{{0, 2, 1}, {0, 3, 2}};
        const auto index = std::uint32_t(s.geometry.meshes.size());
        s.geometry.meshes.push_back(std::move(mesh));
        s.geometry.instances.push_back({index, {}});
        s.instance_materials.push_back(0);
        s.instance_media.push_back({0, no_medium});
        s.instance_surface_transforms.emplace_back();
    };
    wall(1, false);
    wall(std::nextafter(1.f, std::numeric_limits<float>::infinity()), true);
    prepare(s);
    auto owner = session(s, library);
    auto start = frame(*owner, s, origin, no_medium);
    auto query = tracker(*start);
    const auto before = start->capture();
    const auto result = query->classify(*start, destination);
    if (result.status == GeometryStatus::success)
        need(result.failure == cm::Status::success && result.medium == no_medium &&
                 result.crossings == 2,
             "A boundary offset must not silently skip the one-ULP fog exit");
    else
        need(result.status == GeometryStatus::failure && result.failure != cm::Status::success &&
                 !result.diagnostic.empty(),
             "Unresolved one-ULP slab must fail explicitly");
    need(start->capture().raw_xyz == before.raw_xyz, "Thin-slab classifier preserves raw XYZ");
}
void lifetime_and_foreign(const std::filesystem::path &library) {
    auto s = slab();
    auto owner = session(s, library);
    auto source = frame(*owner, s, outside, no_medium);
    auto query = tracker(*source);
    source.reset();
    // Building a tracker may retain pipelines, never the source frame's slot.
    auto a = frame(*owner, s, outside, no_medium), b = frame(*owner, s, outside, no_medium);
    const auto busy = owner->submit({});
    need(busy.busy && !busy.frame, "Only the two retained frames occupy slots after tracker build");
    owner.reset();
    classified(*query, *a, inside, 0, 1);
    classified(*query, *b, inside, 0, 1);
    a.reset();
    b.reset();
    auto foreign_owner = session(s, library);
    auto foreign = frame(*foreign_owner, s, outside, no_medium);
    const auto before = foreign->capture();
    const auto result = query->classify(*foreign, inside);
    need(foreign->capture().raw_xyz == before.raw_xyz,
         "Foreign-session rejection preserves raw XYZ");
    need(result.status == GeometryStatus::failure && !result.diagnostic.empty(),
         "Foreign render session cannot reuse a medium tracker");
}
} // namespace
int main(int argc, char **argv) {
    try {
        need(argc == 2, "Usage: camera-medium-tests LIBRARY.metallib");
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
        need(device.device.compute_ray_tracing, "Camera medium tests require compute ray tracing");
        const std::filesystem::path library = argv[1];
        enter_exit(library, ScatteringKind::null_interface);
        enter_exit(library, ScatteringKind::diffuse);
        enter_exit(library, ScatteringKind::dielectric);
        multiple_and_limits(library);
        transformed(library);
        analytic_two_roots(library);
        thin_slab_regression(library);
        lifetime_and_foreign(library);
        std::cout << "Camera medium crossing and lifetime validation passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
