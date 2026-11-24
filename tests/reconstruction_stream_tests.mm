// SPDX-License-Identifier: GPL-3.0-only
#include "metal_frame_access.hpp"
#include "metal_reconstruction_access.hpp"
#include "reconstruction.hpp"
#include "texture_footprint.hpp"

#import <Foundation/Foundation.h>
#if __has_include(<MetalFX/MTLFXTemporalDenoisedScaler.h>)
#import <MetalFX/MetalFX.h>
#define RYPATH_TEST_HAS_METALFX_DENOISER 1
#else
#define RYPATH_TEST_HAS_METALFX_DENOISER 0
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
constexpr std::uint32_t side = 128;
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
RenderScene plane() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{{}, {-.4f, .4f, 1}, {.8f, 0, 0}, {0, -.8f, 0}};
    scene.environment.fill(.7f);
    scene.max_depth = 1;
    scene.materials = {{.kind = SpectralMaterialKind::constant, .constant = .6f}};
    scene.geometry.meshes = {{.positions = {{-4, -4, 2}, {4, -4, 2}, {-4, 4, 2}, {4, 4, 2}},
                              .triangles = {{0, 2, 1}, {2, 3, 1}}}};
    scene.geometry.instances = {{}};
    scene.instance_materials = {0};
    // Explicit valid matrices: the guides require source-frame texture context
    // even for this untextured fixture. Do not inherit malformed fixture defaults.
    const std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    scene.instance_surface_transforms.resize(1);
    scene.instance_surface_transforms[0].shape = identity;
    scene.instance_surface_transforms[0].placement = identity;
    return scene;
}
std::unique_ptr<FrameSession> session(RenderScene scene) {
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        if (!instance_uses_texture_context(scene, i))
            continue;
        if (scene.instance_texture_frames.empty())
            scene.instance_texture_frames.resize(scene.geometry.instances.size());
        const auto &surface = scene.instance_surface_transforms[i];
        const auto diagnostic = prepare_texture_triangle_frames(
            scene.geometry.meshes[scene.geometry.instances[i].mesh_index], surface.shape,
            surface.placement, scene.instance_texture_frames[i]);
        require(diagnostic.empty(), "Fixture texture frame construction: " + diagnostic);
    }
    auto result = build_frame_session(scene, {.width = side, .height = side, .seed = 197});
    require(result.status == GeometryStatus::success && result.session, result.diagnostic);
    return std::move(result.session);
}
FrameCamera snapshot(const RenderScene &scene) {
    return {.camera = scene.camera,
            .camera_medium = scene.camera_medium,
            .raster_camera = scene.raster_camera,
            .perspective_lens = scene.perspective_lens,
            .texture_camera = scene.texture_camera};
}
std::unique_ptr<Frame> source(FrameSession &owner, std::uint64_t index = 0, std::uint64_t epoch = 7,
                              const std::optional<FrameCamera> &camera = {}) {
    auto result = owner.submit({.sample_index = index, .epoch = epoch, .camera = camera});
    require(result.status == GeometryStatus::success && result.frame && !result.busy,
            result.diagnostic);
    return std::move(result.frame);
}
void source_complete(const Frame &frame) {
    const auto info = frame.wait();
    require(info.status == FrameStatus::complete &&
                info.completed_samples == std::uint64_t(side) * side,
            info.diagnostic.empty() ? "Source completion/accounting failed" : info.diagnostic);
}
std::unique_ptr<ReconstructionStream> stream(const Frame &frame, ReconstructionMode mode,
                                             double near = .001, std::uint32_t output = side,
                                             std::uint32_t spatial_iterations = 5) {
    auto result = build_reconstruction_stream(frame, {.mode = mode,
                                                      .output_width = output,
                                                      .output_height = output,
                                                      .near_plane = near,
                                                      .history_limit = 64,
                                                      .spatial_iterations = spatial_iterations});
    require(result.status == GeometryStatus::success && result.stream &&
                result.source_status == FrameStatus::complete,
            result.diagnostic);
    return std::move(result.stream);
}
std::unique_ptr<ReconstructedFrame> submit(ReconstructionStream &owner, const Frame &frame,
                                           bool reset = false) {
    auto result = owner.submit(frame, reset);
    require(result.status == GeometryStatus::success && result.frame && !result.busy &&
                result.source_status == FrameStatus::complete,
            result.diagnostic);
    return std::move(result.frame);
}
ReconstructionInfo complete(const ReconstructedFrame &frame, ReconstructionMode mode,
                            std::uint32_t output = side) {
    const auto info = frame.wait();
    require(info.status == ReconstructionStatus::complete, info.diagnostic);
    require(info.mode == mode && info.width == output && info.height == output &&
                info.counters.failure == 0 && info.counters.completed == side * side &&
                info.command_count == (mode == ReconstructionMode::raw_rgb ? 1u : 2u),
            "Reconstruction identity/counter/command contract");
    require(!info.gpu_time_valid || (std::isfinite(info.gpu_seconds) && info.gpu_seconds >= 0),
            "Invalid reconstruction GPU timing");
    if (mode == ReconstructionMode::temporal)
        require(std::uint64_t(info.history_accepted) + info.history_rejected ==
                    std::uint64_t(side) * side,
                "Temporal accepted/rejected history accounts for every pixel");
    return info;
}
ReconstructionCapture capture(const ReconstructedFrame &frame, std::uint32_t output = side) {
    auto result = frame.capture();
    require(result.status == GeometryStatus::success && result.width == output &&
                result.height == output && result.linear_rgb.size() == std::size_t(output) * output,
            result.diagnostic.empty() ? "Reconstruction capture shape" : result.diagnostic);
    for (const auto &pixel : result.linear_rgb)
        for (const auto value : pixel)
            require(std::isfinite(value), "Reconstruction capture contains nonfinite RGB");
    return result;
}
void unchanged(const Frame &source, const FrameCapture &before) {
    const auto after = source.capture();
    require(before.status == GeometryStatus::success && after.status == GeometryStatus::success &&
                before.width == after.width && before.height == after.height &&
                before.raw_xyz == after.raw_xyz,
            "Reconstruction modified raw XYZ");
}
void raw_oracle(const FrameCapture &raw, const ReconstructionCapture &rgb, RenderModel model) {
    require(raw.raw_xyz.size() == rgb.linear_rgb.size(), "Raw RGB oracle dimensions");
    // Independent double implementation of the documented unexposed matrices;
    // deliberately do not call production linear_rgb() or guide shader helpers.
    const std::array<std::array<double, 3>, 3> matrix =
        model == RenderModel::pbrt
            ? std::array<std::array<double, 3>, 3>{{{3.240444291419, -1.537133830545,
                                                     -.498529890988},
                                                    {-.969266752560, 1.876012242980, .041556048487},
                                                    {.055643519672, -.204026238798,
                                                     1.057226873771}}}
            : std::array<std::array<double, 3>, 3>{{{3.2404542, -1.5371385, -.4985314},
                                                    {-.9692660, 1.8760108, .0415560},
                                                    {.0556434, -.2040259, 1.0572252}}};
    for (std::size_t i = 0; i < raw.raw_xyz.size(); ++i)
        for (unsigned c = 0; c < 3; ++c) {
            double expected = 0, magnitude = 0;
            for (unsigned k = 0; k < 3; ++k) {
                const double term = matrix[c][k] * raw.raw_xyz[i][k];
                expected += term;
                magnitude += std::abs(term);
            }
            const auto half = static_cast<_Float16>(std::clamp(expected, 0., 65504.));
            const float quantized = static_cast<float>(half);
            const auto bits = std::bit_cast<std::uint16_t>(half);
            const auto neighbor =
                std::bit_cast<_Float16>(std::uint16_t(bits == 0x7bff ? bits - 1 : bits + 1));
            const double spacing = std::abs(double(neighbor) - quantized);
            // Two half-ULPs plus a conservative float matrix arithmetic bound
            // permits FMA/cancellation differences without masking RGB mistakes.
            const double tolerance =
                2 * spacing + 32 * std::numeric_limits<float>::epsilon() * magnitude;
            require(std::abs(double(rgb.linear_rgb[i][c]) - quantized) <= tolerance,
                    "Raw RGB disagrees with independent XYZ/matrix/clamp/half oracle");
        }
}
struct Gate {
    id<MTLSharedEvent> event;
    explicit Gate(id<MTLDevice> device) : event([device newSharedEvent]) {
        require(event != nil, "Shared-event gate allocation failed");
    }
    ~Gate() { release(); }
    void release() { event.signaledValue = 1; }
    id<MTLCommandBuffer> block(id<MTLCommandQueue> queue) {
        auto command = [queue commandBuffer];
        require(command != nil, "Gate command allocation failed");
        [command encodeWaitForEvent:event value:1];
        [command commit];
        return command;
    }
};
void lifecycle(ReconstructionMode mode) {
    const auto scene = plane();
    auto owner = session(scene);
    auto first_source = source(*owner, 19);
    source_complete(*first_source);
    const auto raw = first_source->capture();
    auto recon = stream(*first_source, mode);
    auto native = MetalFrameAccess::acquire(*first_source);
    require(native.lease != nullptr, native.diagnostic);
    std::unique_ptr<ReconstructedFrame> first;
    {
        // Guaranteed pending state, rather than relying on an immediate poll
        // winning a race against a small GPU workload.
        Gate gate(native.lease->device());
        auto blocker = gate.block(native.lease->queue());
        first = submit(*recon, *first_source);
        require(first->poll().status == ReconstructionStatus::pending,
                "Gated output is not pending");
        const auto pending = MetalReconstructedFrameAccess::acquire(*first);
        require(pending.status == ReconstructionStatus::pending && !pending.lease,
                "Pending output exposed a GPU lease");
        const auto busy = recon->submit(*first_source);
        require(busy.busy && !busy.frame && busy.status == GeometryStatus::failure,
                "Second pending reconstruction was accepted");
        gate.release();
        [blocker waitUntilCompleted];
    }
    require(complete(*first, mode).history_reset, "First reconstruction did not reset history");
    const auto rgb = capture(*first);
    if (mode == ReconstructionMode::raw_rgb)
        raw_oracle(raw, rgb, scene.model);
    unchanged(*first_source, raw);
    auto second = submit(*recon, *first_source); // Repeated sample index is legal.
    require(complete(*second, mode).history_reset == (mode == ReconstructionMode::raw_rgb),
            "Repeated source submission history contract");
    const auto busy = recon->submit(*first_source);
    require(busy.busy && !busy.frame, "Two retained output slots were overwritten");
    first.reset();
    auto reused = submit(*recon, *first_source, true);
    require(complete(*reused, mode).history_reset, "Explicit history reset ignored");
    reused.reset();
    second.reset();

    std::unique_ptr<Frame> next_source;
    {
        Gate gate(native.lease->device());
        auto blocker = gate.block(native.lease->queue());
        next_source = source(*owner, 1, 8); // Nonmonotone index is legal; new epoch resets.
        require(next_source->poll().status == FrameStatus::pending, "Gated source is not pending");
        const auto pending_build = build_reconstruction_stream(*next_source, {.mode = mode});
        const auto pending_submit = recon->submit(*next_source);
        require(pending_build.status == GeometryStatus::failure && !pending_build.stream &&
                    pending_build.source_status == FrameStatus::pending &&
                    pending_submit.status == GeometryStatus::failure && !pending_submit.frame &&
                    pending_submit.source_status == FrameStatus::pending && !pending_submit.busy,
                "Pending source must be reported without an implicit wait/fallback");
        gate.release();
        [blocker waitUntilCompleted];
    }
    native.lease.reset();
    source_complete(*next_source);
    auto epoch = submit(*recon, *next_source);
    require(complete(*epoch, mode).history_reset, "New epoch did not reset history");
    epoch.reset();
    auto foreign_owner = session(scene);
    auto foreign = source(*foreign_owner);
    source_complete(*foreign);
    const auto rejected = recon->submit(*foreign);
    require(rejected.status == GeometryStatus::failure && !rejected.busy && !rejected.frame &&
                !rejected.diagnostic.empty(),
            "Foreign frame session was accepted");

    auto a = submit(*recon, *next_source);
    complete(*a, mode);
    const auto expected_a = capture(*a);
    auto b = submit(*recon, *next_source);
    complete(*b, mode);
    auto consumer = MetalReconstructedFrameAccess::acquire(*a);
    require(consumer.status == ReconstructionStatus::complete && consumer.lease,
            consumer.diagnostic);
    // A separate same-device queue is valid because acquisition requires output
    // completion. Retention, rather than accidental queue order, pins storage.
    require(consumer.lease->output_texture().pixelFormat == (mode == ReconstructionMode::temporal
                                                                 ? MTLPixelFormatRGBA32Float
                                                                 : MTLPixelFormatRGBA16Float),
            "Reconstruction output storage format");
    auto consumer_queue = [consumer.lease->device() newCommandQueue];
    auto command = [consumer_queue commandBuffer];
    auto staging = [consumer.lease->device() newBufferWithLength:256
                                                         options:MTLResourceStorageModeShared];
    require(command != nil && staging != nil, "GPU output consumer allocation failed");
    auto encoder = [command blitCommandEncoder];
    require(encoder != nil, "GPU output consumer encoder failed");
    [encoder copyFromTexture:consumer.lease->output_texture()
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(1, 1, 1)
                        toBuffer:staging
               destinationOffset:0
          destinationBytesPerRow:256
        destinationBytesPerImage:256];
    [encoder endEncoding];
    require(consumer.lease->retain_until_completed(command).empty(), "GPU output retention failed");
    a.reset();
    consumer.lease.reset();
    require(recon->submit(*next_source).busy,
            "Uncommitted GPU consumer did not retain its output slot");
    const auto expected = capture(*b);
    first_source.reset();
    next_source.reset();
    owner.reset();
    recon.reset();
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted,
            "Retained GPU output consumer failed");
    const auto *values = staging.contents;
    if (!values)
        throw std::runtime_error("GPU output readback storage is unavailable");
    for (unsigned c = 0; c < 3; ++c) {
        const float value = mode == ReconstructionMode::temporal
                                ? static_cast<const float *>(values)[c]
                                : float(static_cast<const _Float16 *>(values)[c]);
        require(std::isfinite(value) && value == expected_a.linear_rgb[0][c],
                "Retained cross-queue GPU output changed");
    }
    require(capture(*b).linear_rgb == expected.linear_rgb,
            "Output changed after frame session/reconstruction stream destruction");
    ReconstructedFrame moved(std::move(*b));
    require(b->poll().status == ReconstructionStatus::failed &&
                !MetalReconstructedFrameAccess::acquire(*b).lease,
            "Moved-from result exposed a successful GPU output");
    capture(moved);
}
void camera_case(ReconstructionMode mode, bool rotate, bool defocus) {
    const auto scene = plane();
    auto owner = session(scene);
    auto original = source(*owner);
    source_complete(*original);
    auto recon = stream(*original, mode);
    auto initial = submit(*recon, *original);
    complete(*initial, mode);
    initial.reset();
    auto camera = snapshot(scene);
    if (!camera.raster_camera)
        throw std::runtime_error("Fixture requires a raster camera");
    if (defocus) {
        camera.perspective_lens = PerspectiveLens{.radius = .05f, .focal_distance = 2};
    } else if (rotate) {
        const auto yaw = [](GeometryPoint p) {
            constexpr float cosine = .9805806757f, sine = .1961161351f;
            return GeometryPoint{cosine * p.x + sine * p.z, p.y, -sine * p.x + cosine * p.z};
        };
        camera.raster_camera->corner = yaw(camera.raster_camera->corner);
        camera.raster_camera->dx = yaw(camera.raster_camera->dx);
        camera.raster_camera->dy = yaw(camera.raster_camera->dy);
    } else {
        camera.raster_camera->origin.x = .25f;
    }
    auto changed = source(*owner, 1, 7, camera);
    source_complete(*changed);
    const auto raw = changed->capture();
    auto result = submit(*recon, *changed);
    const auto info = complete(*result, mode);
    require(info.history_reset == (mode == ReconstructionMode::raw_rgb || defocus),
            "Smooth camera/lens history reset policy");
    require(info.defocus_approximation == (mode != ReconstructionMode::raw_rgb && defocus),
            "Pinhole-guide defocus approximation not reported correctly");
    const auto rgb = capture(*result);
    if (mode == ReconstructionMode::raw_rgb)
        raw_oracle(raw, rgb, scene.model);
    unchanged(*changed, raw);
    if (mode == ReconstructionMode::temporal && defocus) {
        result.reset();
        original.reset();
        auto restored = source(*owner, 2, 7, snapshot(scene));
        source_complete(*restored);
        auto back = submit(*recon, *restored);
        require(complete(*back, mode).history_reset,
                "Previous lens approximation must invalidate changed-camera history");
    }
}
void near_plane(ReconstructionMode mode) {
    const auto scene = plane();
    auto owner = session(scene);
    auto original = source(*owner);
    source_complete(*original);
    const auto raw = original->capture();
    auto recon = stream(*original, mode, 3); // Plane is two units in front of this camera.
    auto result = submit(*recon, *original);
    const auto info = result->wait();
    if (mode != ReconstructionMode::raw_rgb) {
        require(info.status == ReconstructionStatus::failed && info.counters.failure != 0 &&
                    !info.diagnostic.empty() && result->capture().status == GeometryStatus::failure,
                "Near-plane guide failure was silently denoised or hidden");
    } else {
        complete(*result, mode);
        raw_oracle(raw, capture(*result), scene.model);
    }
    unchanged(*original, raw);
    result.reset();
    auto camera = snapshot(scene);
    if (!camera.raster_camera)
        throw std::runtime_error("Fixture requires a raster camera");
    camera.raster_camera->origin.z = -2; // Same surface now has view depth four.
    auto moved = source(*owner, 1, 7, camera);
    source_complete(*moved);
    auto recovered = submit(*recon, *moved);
    require(complete(*recovered, mode).history_reset, "Failed guides did not invalidate history");
    capture(*recovered);
}
void approximations(ReconstructionMode mode) {
    for (bool medium : {false, true}) {
        auto scene = plane();
        if (medium) {
            scene.volume_transport = true;
            scene.integrator = IntegratorKind::volpath;
            scene.media.resize(1);
            scene.media[0].sigma_a.fill(.1f);
            scene.camera_medium = 0;
        } else {
            scene.materials[0].scattering = ScatteringKind::dielectric;
            scene.materials[0].constant = 0;
            scene.materials[0].eta.knots = {{360, 1.5f}, {830, 1.5f}};
            scene.materials[0].eta.extend_endpoints = true;
            scene.max_depth = 2;
        }
        auto owner = session(scene);
        auto original = source(*owner);
        source_complete(*original);
        const auto raw = original->capture();
        auto recon = stream(*original, mode);
        auto result = submit(*recon, *original);
        const auto info = complete(*result, mode);
        if (mode != ReconstructionMode::raw_rgb)
            require(medium ? info.counters.media_approximation > 0
                           : info.counters.transmission_approximation > 0,
                    "Material/media guide approximation was not reported");
        const auto rgb = capture(*result);
        if (mode == ReconstructionMode::raw_rgb)
            raw_oracle(raw, rgb, scene.model);
        unchanged(*original, raw);
        if (mode == ReconstructionMode::temporal) {
            result.reset();
            auto repeated = submit(*recon, *original);
            require(!complete(*repeated, mode).history_reset,
                    "Static approximate guides may retain same-pixel history");
            repeated.reset();
            auto moved_camera = snapshot(scene);
            if (!moved_camera.raster_camera)
                throw std::runtime_error("Approximation fixture raster camera missing");
            moved_camera.raster_camera->origin.x = .125f;
            auto moved = source(*owner, 1, 7, moved_camera);
            source_complete(*moved);
            auto output = submit(*recon, *moved);
            require(complete(*output, mode).history_reset,
                    "Moving current media/transmission approximation must reset history");
            output.reset();
            moved.reset();
            if (medium) {
                // The view matrix stays fixed, but the resolved source medium
                // changes. This is a distinct invalidation input.
                auto vacuum = moved_camera;
                vacuum.camera_medium = no_medium;
                auto relabeled = source(*owner, 2, 7, vacuum);
                source_complete(*relabeled);
                auto next = submit(*recon, *relabeled);
                require(complete(*next, mode).history_reset,
                        "Resolved camera-medium changes must invalidate history");
            }
        }
    }
}
void mixed_approximation_resets() {
    constexpr auto mode = ReconstructionMode::temporal;
    for (const bool transmit : {false, true}) {
        auto scene = plane();
        const auto diffuse = scene.materials.front();
        auto dielectric = diffuse;
        dielectric.constant = 0;
        dielectric.scattering = ScatteringKind::dielectric;
        dielectric.eta.knots = {{360, 1.5f}, {830, 1.5f}};
        dielectric.eta.extend_endpoints = true;
        scene.materials.push_back(dielectric);
        scene.materials.push_back(diffuse);
        // Endpoint amounts make the selected child deterministic. Both children
        // have valid independent payloads; neither is an unsupported null mix.
        auto &root = scene.materials.front();
        root.constant = 0;
        root.scattering = ScatteringKind::mix;
        root.surface.first_material = 1;
        root.surface.second_material = 2;
        root.surface.mix_amount = transmit ? 0.f : 1.f;
        scene.max_depth = 2;
        auto owner = session(scene);
        auto original = source(*owner);
        source_complete(*original);
        const auto before = original->capture();
        auto recon = stream(*original, mode);
        auto initial = submit(*recon, *original);
        const auto first = complete(*initial, mode);
        require(first.counters.mix_fallback == side * side &&
                    (transmit ? first.counters.transmission_approximation == side * side
                              : first.counters.transmission_approximation == 0),
                "Mix fallback must report the selected child's transmission independently");
        unchanged(*original, before);
        initial.reset();
        auto repeated = submit(*recon, *original);
        require(!complete(*repeated, mode).history_reset,
                "Static mixed surfaces may retain same-pixel history");
        repeated.reset();
        auto pose = snapshot(scene);
        if (!pose.raster_camera)
            throw std::runtime_error("Mixed fixture raster camera missing");
        pose.raster_camera->origin.x = .125f;
        auto moved = source(*owner, 1, 7, pose);
        source_complete(*moved);
        const auto moved_raw = moved->capture();
        auto output = submit(*recon, *moved);
        const auto current = complete(*output, mode);
        require(current.history_reset && current.counters.mix_fallback == side * side &&
                    (transmit ? current.counters.transmission_approximation == side * side
                              : current.counters.transmission_approximation == 0),
                "Moving mixed opaque/transmissive guide approximation must reset history");
        unchanged(*moved, moved_raw);
        output.reset();
        original.reset();
        // The same scene, epoch and source medium now see only sky: the plane
        // lies behind the camera. Current approximation counters must be zero,
        // so only the previous mixed surface can justify this camera reset.
        pose.raster_camera->origin.z = 3;
        auto sky = source(*owner, 2, 7, pose);
        source_complete(*sky);
        const auto sky_raw = sky->capture();
        auto background = submit(*recon, *sky);
        const auto previous_only = complete(*background, mode);
        require(previous_only.history_reset && previous_only.counters.background == side * side &&
                    previous_only.counters.mix_fallback == 0 &&
                    previous_only.counters.transmission_approximation == 0 &&
                    previous_only.counters.media_approximation == 0 &&
                    !previous_only.defocus_approximation,
                "Previous-only mix/transmission approximation must reset a clean sky view");
        capture(*background);
        unchanged(*sky, sky_raw);
    }
}
void dimensions(ReconstructionMode mode) {
    auto owner = session(plane());
    auto original = source(*owner);
    source_complete(*original);
    auto invalid = build_reconstruction_stream(*original, {.mode = mode, .near_plane = 0});
    require(invalid.status == GeometryStatus::failure && !invalid.stream &&
                !invalid.diagnostic.empty(),
            "Invalid near plane accepted");
    if (mode != ReconstructionMode::metalfx) {
        auto upscaled = build_reconstruction_stream(
            *original, {.mode = mode, .output_width = 256, .output_height = 256});
        require(upscaled.status == GeometryStatus::failure && !upscaled.stream &&
                    !upscaled.diagnostic.empty(),
                "Native-resolution reconstruction mode silently upscaled");
    } else {
        auto upscaled = stream(*original, mode, .001, 256);
        auto result = submit(*upscaled, *original);
        complete(*result, mode, 256);
        capture(*result, 256);
    }
}
void temporal_recurrence() {
    auto scene = plane();
    scene.geometry = {};
    scene.instance_materials.clear();
    scene.instance_surface_transforms.clear();
    scene.environment.fill(.01f);
    for (unsigned wavelength = 430; wavelength <= 470; ++wavelength)
        scene.environment[wavelength - 360] = 1;
    auto owner = session(scene);
    std::unique_ptr<ReconstructionStream> recon;
    std::vector<std::array<double, 3>> mean(std::size_t(side) * side);
    bool negative_input = false, negative_output = false, distinct_samples = false;
    constexpr std::array<std::array<double, 3>, 3> matrix{
        {{3.240444291419, -1.537133830545, -.498529890988},
         {-.969266752560, 1.876012242980, .041556048487},
         {.055643519672, -.204026238798, 1.057226873771}}};
    // Four fixed, distinct high absolute samples; no seed selection or retries.
    // Sky has zero motion and stable identity, so temporal-only history is an
    // independently computed same-pixel running average of signed raw RGB.
    for (unsigned n = 1; n <= 4; ++n) {
        auto input = source(*owner, (1ull << 32) + 19 + n);
        source_complete(*input);
        const auto raw = input->capture();
        if (!recon)
            recon = stream(*input, ReconstructionMode::temporal, .001, side, 0);
        auto output = submit(*recon, *input);
        const auto info = complete(*output, ReconstructionMode::temporal);
        require(info.history_reset == (n == 1) &&
                    info.history_accepted == (n == 1 ? 0u : side * side) &&
                    info.history_rejected == (n == 1 ? side * side : 0u),
                "Static temporal-only history recurrence/telemetry contract");
        const auto rgb = capture(*output);
        for (std::size_t i = 0; i < mean.size(); ++i)
            for (unsigned c = 0; c < 3; ++c) {
                double value = 0, magnitude = 0;
                for (unsigned k = 0; k < 3; ++k) {
                    const double term = matrix[c][k] * raw.raw_xyz[i][k];
                    value += term;
                    magnitude += std::abs(term);
                }
                negative_input |= value < -1e-4;
                distinct_samples |= n > 1 && std::abs(value - mean[i][c]) > .01;
                mean[i][c] += (value - mean[i][c]) / n;
                negative_output |= rgb.linear_rgb[i][c] < -1e-4f;
                const double tolerance = 2e-4 + 64 * std::numeric_limits<float>::epsilon() *
                                                    (magnitude + std::abs(mean[i][c]));
                require(std::abs(double(rgb.linear_rgb[i][c]) - mean[i][c]) <= tolerance,
                        "Temporal-only signed RGB differs from independent double recurrence");
            }
        unchanged(*input, raw);
    }
    require(negative_input && negative_output && distinct_samples,
            "Signed recurrence requires negative values and genuinely varying sample frames");
}
int availability(ReconstructionMode mode) {
    const auto device = query_metal_device();
    if (device.status == MetalStatus::no_device) {
        std::cerr << "Reconstruction tests unavailable: " << device.diagnostic << '\n';
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE) && RYPATH_ALLOW_NO_METAL_DEVICE
        return 77;
#endif
    }
    require(device.status == MetalStatus::success, device.diagnostic);
    if (!device.device.compute_ray_tracing) {
        std::cerr << "Reconstruction tests unavailable: no compute ray tracing\n";
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING) && RYPATH_ALLOW_NO_METAL_RAY_TRACING
        return 77;
#endif
        throw std::runtime_error("Reconstruction tests require compute ray tracing");
    }
    if (mode == ReconstructionMode::metalfx) {
        bool supported = false;
#if RYPATH_TEST_HAS_METALFX_DENOISER
        if (@available(macOS 26.0, *)) {
            id<MTLDevice> metal_device = MTLCreateSystemDefaultDevice();
            if (metal_device)
                supported = [MTLFXTemporalDenoisedScalerDescriptor supportsDevice:metal_device];
        }
#endif
        if (!supported) {
            std::cerr << "Reconstruction tests unavailable: MetalFX denoised scaling unsupported\n";
#if defined(RYPATH_ALLOW_NO_METALFX_DENOISER) && RYPATH_ALLOW_NO_METALFX_DENOISER
            return 77;
#endif
            throw std::runtime_error(
                "Requested MetalFX reconstruction is unavailable; no fallback");
        }
    }
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2 ||
        (std::string_view(argv[1]) != "--raw" && std::string_view(argv[1]) != "--metalfx" &&
         std::string_view(argv[1]) != "--temporal")) {
        std::cerr << "Usage: reconstruction_stream_tests --raw|--metalfx|--temporal\n";
        return 1;
    }
    @autoreleasepool {
        try {
            const auto mode = std::string_view(argv[1]) == "--raw" ? ReconstructionMode::raw_rgb
                              : std::string_view(argv[1]) == "--temporal"
                                  ? ReconstructionMode::temporal
                                  : ReconstructionMode::metalfx;
            if (const auto unavailable = availability(mode))
                return unavailable;
            lifecycle(mode);
            camera_case(mode, false, false);
            camera_case(mode, true, false);
            camera_case(mode, false, true);
            near_plane(mode);
            approximations(mode);
            dimensions(mode);
            if (mode == ReconstructionMode::temporal) {
                mixed_approximation_resets();
                temporal_recurrence();
            }
            std::cout << "Reconstruction stream " << argv[1]
                      << " qualification passed; image quality remains unqualified\n";
            return 0;
        } catch (const std::exception &error) {
            std::cerr << "Reconstruction stream qualification: " << error.what() << '\n';
            return 1;
        }
    }
}
