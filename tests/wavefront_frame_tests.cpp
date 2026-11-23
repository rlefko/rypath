// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
#if RYPATH_TEST_WAVEFRONT_METAL
constexpr std::uint64_t sample_index = (1ull << 32) + 17;
FrameConfig configuration(FrameExecution execution) {
    return {.width = 7,
            .height = 3,
            .seed = 19317,
            .material_seed = -11,
            .footprint_samples = 1,
            .execution = execution};
}
std::unique_ptr<Frame> submit(FrameSession &session, std::uint64_t index = sample_index) {
    auto result = session.submit({.sample_index = index, .epoch = 73, .camera = {}});
    require(result.status == GeometryStatus::success && !result.busy && result.frame,
            result.diagnostic.empty() ? "Frame submission failed" : result.diagnostic);
    return std::move(result.frame);
}
FrameCapture capture(Frame &frame, const FrameConfig &config, bool multiple_commands) {
    const auto info = frame.wait();
    require(info.status == FrameStatus::complete && info.sample_index == sample_index &&
                info.epoch == 73 &&
                info.completed_samples == std::uint64_t(config.width) * config.height,
            info.diagnostic.empty() ? "Frame identity/sample accounting" : info.diagnostic);
    require(info.command_count > 0 && (!multiple_commands || info.command_count > 1),
            "Suspended wavefront frame must span multiple completed command buffers");
    require(!info.gpu_time_valid || (std::isfinite(info.gpu_seconds) && info.gpu_seconds >= 0),
            "Invalid GPU command timestamp interval");
    auto image = frame.capture();
    require(image.status == GeometryStatus::success && image.width == config.width &&
                image.height == config.height && image.raw_xyz.size() == info.completed_samples,
            image.diagnostic.empty() ? "Capture dimensions/sample count" : image.diagnostic);
    require(frame.poll().status == FrameStatus::complete &&
                frame.wait().command_count == info.command_count,
            "Repeated wait/poll must preserve final completion");
    return image;
}
void compare(const FrameCapture &actual, const FrameCapture &expected, std::string_view label) {
    require(actual.width == expected.width && actual.height == expected.height &&
                actual.raw_xyz.size() == expected.raw_xyz.size(),
            "Parity dimensions");
    // These are paired samples of the same estimator. Allow only float arithmetic
    // differences, never an image-level statistical threshold that could hide lost paths.
    constexpr double relative = 3e-5, absolute = 3e-6;
    for (std::size_t pixel = 0; pixel < actual.raw_xyz.size(); ++pixel)
        for (unsigned channel = 0; channel < 3; ++channel) {
            const double value = actual.raw_xyz[pixel][channel];
            const double reference = expected.raw_xyz[pixel][channel];
            require(std::isfinite(value) && std::isfinite(reference) &&
                        std::abs(value - reference) <= absolute + relative * std::abs(reference),
                    std::string(label) + " pixel " + std::to_string(pixel) + " channel " +
                        std::to_string(channel) + " observed=" + std::to_string(value) +
                        " expected=" + std::to_string(reference));
        }
}
void sensor_oracle(const RenderScene &scene, const FrameConfig &config, const FrameCapture &image) {
    auto built =
        build_metal_renderer(scene, {}, {.intersection_profile = config.intersection_profile});
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    auto samples = built.scene->sample({.width = config.width,
                                        .height = config.height,
                                        .seed = config.seed,
                                        .material_seed = config.material_seed,
                                        .pixel_count = config.width * config.height,
                                        .sample_begin = sample_index,
                                        .sample_count = 1,
                                        .total_samples = sample_index + 1});
    require(samples.status == GeometryStatus::success, samples.diagnostic);
    const auto count = image.raw_xyz.size();
    require(scene.spectral_transport == SpectralTransport::packet
                ? samples.packet_samples.size() == count && samples.samples.empty() &&
                      samples.packet_splats.empty()
                : samples.samples.size() == count && samples.packet_samples.empty(),
            "Direct sample oracle exact owner accounting");
    FrameCapture expected;
    expected.width = image.width;
    expected.height = image.height;
    expected.raw_xyz.resize(image.raw_xyz.size());
    for (std::size_t pixel = 0; pixel < expected.raw_xyz.size(); ++pixel) {
        std::array<double, 3> xyz{};
        if (scene.spectral_transport == SpectralTransport::packet) {
            const auto &owner = samples.packet_samples.at(pixel);
            require(owner.pixel == pixel && owner.sample == sample_index && owner.splat_count == 0,
                    "Independent sensor oracle packet ownership");
            xyz = packet_sensor_xyz(owner, owner.radiance);
        } else {
            const auto &owner = samples.samples.at(pixel);
            require(owner.pixel == pixel && owner.sample == sample_index,
                    "Independent sensor oracle scalar ownership");
            xyz = pbrt_cie_xyz(owner.wavelength);
            for (auto &value : xyz)
                value *= owner.radiance * 470.;
        }
        for (unsigned channel = 0; channel < 3; ++channel)
            expected.raw_xyz[pixel][channel] = static_cast<float>(xyz[channel]);
    }
    compare(image, expected, "Independent host sensor reduction");
}
RenderScene base(bool packet) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = packet ? SpectralTransport::packet : SpectralTransport::scalar;
    scene.raster_camera = RasterCamera{{}, {-.3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
    scene.environment.fill(.7f);
    scene.max_depth = 4;
    return scene;
}
void plane(RenderScene &scene, float z, bool positive, MediumInterface medium = {},
           std::uint32_t material = 0) {
    GeometryMesh mesh;
    mesh.positions = {{-16, -16, z}, {16, -16, z}, {16, 16, z}, {-16, 16, z}};
    mesh.triangles = positive ? std::vector<std::array<unsigned, 3>>{{0, 1, 2}, {0, 2, 3}}
                              : std::vector<std::array<unsigned, 3>>{{0, 2, 1}, {0, 3, 2}};
    const auto index = static_cast<unsigned>(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({index, {}});
    scene.instance_materials.push_back(material);
    scene.instance_media.push_back(medium);
}
RenderScene diffuse(bool packet) {
    auto scene = base(packet);
    SpectralMaterial material;
    material.kind = SpectralMaterialKind::constant;
    material.constant = .6f;
    scene.materials.push_back(material);
    plane(scene, 3, false);
    PointLight light;
    light.position = {0, 1, 1};
    light.intensity.fill(3);
    scene.point_lights.push_back(light);
    return scene;
}
// The two receivers face each other. Camera rays first cross z=1 before
// scattering at z=3; reflected paths cross z=1 and z=-1 en route to z=-3.
// The rear receiver also emits, exercising endpoint MIS with surviving history.
RenderScene packet_history(bool invisible, unsigned depth) {
    auto scene = base(true);
    scene.environment.fill(0);
    scene.max_depth = depth;
    SpectralMaterial diffuse;
    diffuse.kind = SpectralMaterialKind::constant;
    diffuse.constant = .65f;
    scene.materials.push_back(diffuse);
    auto boundary = diffuse;
    boundary.constant = 0;
    boundary.scattering = ScatteringKind::null_interface;
    scene.materials.push_back(boundary);
    plane(scene, 3, false);
    plane(scene, -3, true);
    plane(scene, 1, false, {}, invisible ? 0 : 1);
    plane(scene, -1, true, {}, invisible ? 0 : 1);
    scene.area_emissions.emplace_back();
    scene.area_emissions.back().radiance.fill(.4f);
    scene.instance_emissions = {no_area_emission, 0, no_area_emission, no_area_emission};
    PointLight light;
    light.position = {.5f, .25f, 0};
    light.intensity.fill(4);
    scene.point_lights.push_back(light);
    if (invisible) {
        // Invisibility requires an emitting placement and certified constant-zero
        // alpha, even when that emitter's own radiance is zero.
        scene.area_emissions.emplace_back();
        scene.area_emissions.back().radiance.fill(0);
        scene.instance_emissions[2] = scene.instance_emissions[3] = 1;
        scene.texture_graph.nodes.emplace_back();
        scene.instance_alpha_roots = {UINT32_MAX, UINT32_MAX, 0, 0};
        scene.instance_invisible_emitters = {0, 0, 1, 1};
        scene.instance_surface_transforms.resize(scene.geometry.instances.size());
        scene.instance_texture_frames.resize(scene.geometry.instances.size());
        for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
            if (!instance_uses_texture_context(scene, i))
                continue;
            const auto &t = scene.instance_surface_transforms[i];
            const auto error = prepare_texture_triangle_frames(
                scene.geometry.meshes[scene.geometry.instances[i].mesh_index], t.shape, t.placement,
                scene.instance_texture_frames[i]);
            require(error.empty(), error);
        }
    }
    const auto error = validate_render_scene(scene);
    require(error.empty(), error);
    return scene;
}
RenderScene fixture(std::string_view name) {
    const auto path = std::filesystem::path(RYPATH_SOURCE_DIR) /
                      "tests/fixtures/pbrt-render/packet-transport" / name;
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, {path, 1, 1});
    if (translated.exit_code() != 0 || !translated.scene)
        throw std::runtime_error("Owned fixture translation");
    auto scene = std::move(*translated.scene);
    scene.integrator = IntegratorKind::path;
    scene.max_component_value = std::numeric_limits<double>::infinity();
    scene.spectral_transport = SpectralTransport::packet;
    return scene;
}
RenderScene glass(bool packet) {
    auto scene = fixture("glass-delta-chain.pbrt");
    scene.spectral_transport = packet ? SpectralTransport::packet : SpectralTransport::scalar;
    bool found = false;
    for (auto &material : scene.materials)
        if (material.scattering == ScatteringKind::dielectric) {
            material.eta.knots = {{360, 1.65f}, {500, 1.52f}, {830, 1.45f}};
            material.eta.extend_endpoints = true;
            found = true;
        }
    require(found && scene.geometry.meshes.size() >= 5, "Dispersive glass geometry coverage");
    return scene;
}
RenderScene medium(bool boundaries) {
    auto scene = base(false);
    scene.integrator = IntegratorKind::volpath;
    scene.volume_transport = true;
    scene.media.resize(1);
    scene.media[0].sigma_a.fill(.17f);
    scene.media[0].sigma_s.fill(.23f);
    scene.media[0].emission.fill(.11f);
    scene.media[0].g = .35f;
    PointLight light;
    light.position = {0, 1, .5f};
    light.intensity.fill(2);
    scene.point_lights.push_back(light);
    if (boundaries) {
        SpectralMaterial boundary;
        boundary.kind = SpectralMaterialKind::constant;
        boundary.scattering = ScatteringKind::null_interface;
        scene.materials.push_back(boundary);
        for (unsigned slab = 0; slab < 3; ++slab) {
            plane(scene, 1 + float(slab), false, {0, no_medium});
            plane(scene, 1.5f + float(slab), true, {0, no_medium});
        }
    } else
        scene.camera_medium = 0;
    return scene;
}
FrameCapture scheduling_case(const RenderScene &scene, std::string_view label,
                             IntersectionProfile profile = IntersectionProfile::native) {
    auto mega = configuration(FrameExecution::megakernel);
    mega.intersection_profile = profile;
    auto reference = build_frame_session(scene, mega);
    require(reference.status == GeometryStatus::success && reference.session, reference.diagnostic);
    auto reference_frame = submit(*reference.session);
    const auto expected = capture(*reference_frame, mega, false);
    sensor_oracle(scene, mega, expected);
    // Normal scheduling, single-slot refill, partial refill tail, and two supported
    // group sizes all consume exactly the same absolute paths.
    for (const auto &[capacity, quantum, width] :
         {std::array<unsigned, 3>{65536, 8, 32}, {1, 1, 32}, {17, 1, 64}, {17, 8, 32}}) {
        auto config = configuration(FrameExecution::wavefront);
        config.intersection_profile = profile;
        config.capacity = capacity;
        config.quantum = quantum;
        config.group_width = width;
        auto built = build_frame_session(scene, config);
        require(built.status == GeometryStatus::success && built.session, built.diagnostic);
        auto frame = submit(*built.session);
        compare(capture(*frame, config, quantum == 1), expected, label);
    }
    return expected;
}
void packet_history_cases() {
    for (const bool invisible : {false, true}) {
        const std::string name = invisible ? "Packet invisible history" : "Packet null history";
        const auto shallow = scheduling_case(packet_history(invisible, 1), name + " depth1");
        scheduling_case(packet_history(invisible, 2), name + " depth2");
        const auto deep = scheduling_case(packet_history(invisible, 4), name + " depth4");
        bool nonzero = false, indirect = false;
        for (std::size_t i = 0; i < deep.raw_xyz.size(); ++i) {
            nonzero |= shallow.raw_xyz[i][1] > 0;
            indirect |= deep.raw_xyz[i][1] > shallow.raw_xyz[i][1] + 1e-5f;
        }
        require(nonzero, name + " must illuminate the first receiver");
        require(indirect, name + " must retain a nonzero deeper-scattering contribution");
    }
    // A null child in a mix is unsupported; do not silently relabel it diffuse
    // in order to claim coverage of an unavailable transport model.
    auto invalid = packet_history(false, 4);
    SpectralMaterial mix;
    mix.scattering = ScatteringKind::mix;
    mix.surface.first_material = 0;
    mix.surface.second_material = 1;
    invalid.materials.push_back(mix);
    invalid.instance_materials[0] = 2;
    invalid.instance_surface_transforms.resize(invalid.geometry.instances.size());
    require(validate_render_scene(invalid).find("Mix materials cannot contain null boundaries") !=
                std::string::npos,
            "Mixed-null material must retain its explicit unsupported-model diagnostic");
}
RenderScene packet_alpha_coverage() {
    auto scene = diffuse(true);
    SpectralMaterial coverage;
    coverage.scattering = ScatteringKind::gltf;
    coverage.gltf.alpha_mode = 2;
    coverage.gltf.double_sided = true;
    coverage.gltf.base_color = {.1f, .2f, .3f, .5f};
    scene.materials.push_back(coverage);
    plane(scene, 1, false, {}, 1);
    // Match the existing packet alpha-visibility fixture's prepared bindings.
    scene.instance_surface_transforms.resize(scene.geometry.instances.size());
    scene.instance_texture_frames.resize(scene.geometry.instances.size());
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        if (!instance_uses_texture_context(scene, i))
            continue;
        const auto &surface = scene.instance_surface_transforms[i];
        const auto error = prepare_texture_triangle_frames(
            scene.geometry.meshes[scene.geometry.instances[i].mesh_index], surface.shape,
            surface.placement, scene.instance_texture_frames[i]);
        require(error.empty(), error);
    }
    require(validate_render_scene(scene).empty(), "Packet alpha-policy fixture validation");
    return scene;
}
RenderScene packet_mixed_geometry() {
    auto scene = diffuse(true);
    // Primary rays across the film reach triangle receivers at x<0 and a
    // bilinear receiver at x>0. Both kinds coexist in the same native scene.
    auto &left = scene.geometry.meshes.front();
    left.positions[1].x = left.positions[2].x = 0;
    GeometryMesh right;
    right.positions = {{0, -16, 3}, {16, -16, 3}, {16, 16, 3}, {0, 16, 3}};
    right.patches = {{0, 3, 1, 2}};
    scene.geometry.meshes.push_back(std::move(right));
    scene.geometry.instances.push_back({1, {}});
    auto material = scene.materials.front();
    material.constant = .2f;
    scene.materials.push_back(material);
    scene.instance_materials.push_back(1);
    scene.instance_media.push_back({});
    require(validate_render_scene(scene).empty(), "Packet mixed-policy fixture validation");
    return scene;
}
void packet_visibility_policy_cases() {
    const std::array<RenderScene, 2> scenes{packet_alpha_coverage(), packet_mixed_geometry()};
    for (std::size_t scene_index = 0; scene_index < scenes.size(); ++scene_index) {
        const auto &scene = scenes[scene_index];
        for (const auto profile : {IntersectionProfile::native, IntersectionProfile::compensated}) {
            auto control = configuration(FrameExecution::wavefront);
            control.capacity = 17;
            control.quantum = 1;
            control.intersection_profile = profile;
            auto baseline = build_frame_session(scene, control);
            require(baseline.status == GeometryStatus::success && baseline.session,
                    baseline.diagnostic);
            const auto expected = capture(*submit(*baseline.session), control, true);
            // Keep the existing numerical oracle tolerance separate from exact
            // same-profile scheduling comparisons below.
            sensor_oracle(scene, control, expected);
            for (const auto layout : {FramePacketLayout::packed, FramePacketLayout::hot_cold,
                                      FramePacketLayout::split_trace}) {
                for (const auto events : {1u, 2u, 4u}) {
                    if (events != 1 && (layout == FramePacketLayout::split_trace ||
                                        profile == IntersectionProfile::compensated))
                        continue;
                    auto config = control;
                    config.packet_layout = layout;
                    config.packet_events = events;
                    auto built = build_frame_session(scene, config);
                    require(built.status == GeometryStatus::success && built.session,
                            built.diagnostic);
                    auto frame = submit(*built.session);
                    built.session.reset();
                    const auto actual = capture(*frame, config, true);
                    const auto info = frame->wait();
                    require(info.completed_evaluations == expected.raw_xyz.size() &&
                                info.fresh_candidates == expected.raw_xyz.size(),
                            "Visibility policy exact evaluation accounting");
                    for (std::size_t pixel = 0; pixel < actual.raw_xyz.size(); ++pixel)
                        for (unsigned channel = 0; channel < 3; ++channel)
                            require(
                                std::bit_cast<std::uint32_t>(actual.raw_xyz[pixel][channel]) ==
                                    std::bit_cast<std::uint32_t>(expected.raw_xyz[pixel][channel]),
                                "Visibility policy raw bits: scene=" + std::to_string(scene_index) +
                                    " profile=" + std::to_string(static_cast<int>(profile)) +
                                    " layout=" + std::to_string(static_cast<int>(layout)) +
                                    " events=" + std::to_string(events) +
                                    " pixel=" + std::to_string(pixel));
                }
            }
        }
    }
}
void packet_scheduling_variants() {
    const std::array<RenderScene, 6> scenes{base(true),
                                            diffuse(true),
                                            glass(true),
                                            fixture("coated-path.pbrt"),
                                            packet_history(false, 4),
                                            packet_history(true, 4)};
    for (const auto &scene : scenes) {
        auto control = configuration(FrameExecution::wavefront);
        control.capacity = 17;
        control.quantum = 1;
        auto baseline = build_frame_session(scene, control);
        require(baseline.status == GeometryStatus::success && baseline.session,
                baseline.diagnostic);
        const auto expected = capture(*submit(*baseline.session), control, true);
        for (const auto layout : {FramePacketLayout::packed, FramePacketLayout::hot_cold,
                                  FramePacketLayout::split_trace}) {
            for (const auto events : {1u, 2u, 4u}) {
                if (layout == FramePacketLayout::split_trace && events != 1)
                    continue;
                for (const auto capacity : {1u, 17u}) {
                    auto config = control;
                    config.packet_layout = layout;
                    config.packet_events = events;
                    config.capacity = capacity;
                    auto built = build_frame_session(scene, config);
                    require(built.status == GeometryStatus::success && built.session,
                            built.diagnostic);
                    auto frame = submit(*built.session);
                    built.session.reset(); // All selected pools must survive continuations.
                    const auto actual = capture(*frame, config, true);
                    const auto info = frame->wait();
                    require(info.completed_evaluations == expected.raw_xyz.size() &&
                                info.fresh_candidates == expected.raw_xyz.size(),
                            "Packet variant exact evaluation accounting");
                    for (std::size_t pixel = 0; pixel < actual.raw_xyz.size(); ++pixel)
                        for (unsigned channel = 0; channel < 3; ++channel)
                            require(
                                std::bit_cast<std::uint32_t>(actual.raw_xyz[pixel][channel]) ==
                                    std::bit_cast<std::uint32_t>(expected.raw_xyz[pixel][channel]),
                                "Packet scheduling must preserve exact raw XYZ bits");
                }
            }
        }
    }
    for (const auto layout :
         {FramePacketLayout::packed, FramePacketLayout::hot_cold, FramePacketLayout::split_trace}) {
        for (const auto events : {1u, 2u, 4u}) {
            if (layout == FramePacketLayout::split_trace && events != 1)
                continue;
            auto config = configuration(FrameExecution::wavefront);
            config.packet_layout = layout;
            config.packet_events = events;
            config.capacity = 1;
            config.quantum = 1;
            auto overflow = base(true);
            overflow.environment.fill(std::numeric_limits<float>::max());
            auto built = build_frame_session(overflow, config);
            require(built.status == GeometryStatus::success && built.session, built.diagnostic);
            auto frame = submit(*built.session);
            built.session.reset();
            require(frame->wait().status == FrameStatus::failed &&
                        frame->capture().status == GeometryStatus::failure,
                    "Packet variant nonfinite failure must not publish an image");
        }
    }
    auto bad = configuration(FrameExecution::wavefront);
    for (const auto events : {0u, 3u, 5u}) {
        bad.packet_events = events;
        const auto built = build_frame_session(base(true), bad);
        require(built.status == GeometryStatus::failure && !built.session,
                "Invalid packet fusion factor accepted");
    }
    for (const auto layout : {FramePacketLayout::hot_cold, FramePacketLayout::split_trace}) {
        bad = configuration(FrameExecution::wavefront);
        bad.packet_layout = layout;
        require(build_frame_session(base(false), bad).status == GeometryStatus::failure,
                "Scalar transport must reject packet layout overrides");
        bad.execution = FrameExecution::megakernel;
        require(build_frame_session(base(true), bad).status == GeometryStatus::failure,
                "Megakernel must reject packet layout overrides");
    }
    bad = configuration(FrameExecution::wavefront);
    bad.packet_events = 2;
    require(build_frame_session(base(false), bad).status == GeometryStatus::failure,
            "Scalar transport must reject packet fusion overrides");
    bad.execution = FrameExecution::megakernel;
    require(build_frame_session(base(true), bad).status == GeometryStatus::failure,
            "Megakernel must reject packet fusion overrides");
    bad.execution = FrameExecution::wavefront;
    bad.packet_layout = FramePacketLayout::split_trace;
    require(build_frame_session(base(true), bad).status == GeometryStatus::failure,
            "Split trace must reject fusion");
}
void lifetime() {
    const auto scene = diffuse(true);
    auto config = configuration(FrameExecution::wavefront);
    config.capacity = 1;
    config.quantum = 1;
    auto built = build_frame_session(scene, config);
    require(built.status == GeometryStatus::success && built.session, built.diagnostic);
    auto first = submit(*built.session);
    auto second = submit(*built.session, sample_index + 1);
    const auto busy = built.session->submit({.sample_index = sample_index + 2});
    require(busy.busy && !busy.frame, "Both retained slots must reject a third frame");
    const auto expected = capture(*first, config, true);
    require(built.session->submit({}).busy, "Completion cannot release a retained frame slot");
    built.session.reset(); // Retained frames and any continuations own their resources.
    compare(first->capture(), expected, "Retained frame after session destruction");
    const auto second_info = second->wait();
    require(second_info.status == FrameStatus::complete &&
                second_info.sample_index == sample_index + 1 &&
                second_info.completed_samples == std::uint64_t(config.width) * config.height,
            "Retained continuation must finish after session destruction");
    require(second->capture().status == GeometryStatus::success, "Second retained capture");
    Frame moved(std::move(*first));
    require(first->wait().status == FrameStatus::failed, "Moved-from frame must fail visibly");
    compare(moved.capture(), expected, "Moved retained frame");

    auto pending = build_frame_session(scene, config);
    require(pending.status == GeometryStatus::success && pending.session, pending.diagnostic);
    auto orphan = submit(*pending.session);
    pending.session.reset(); // No preceding wait or capture may supply the lifetime ordering.
    compare(capture(*orphan, config, true), expected, "Immediate session destruction");
}
void invalid_and_async_failure() {
    const auto scene = diffuse(false);
    auto config = configuration(FrameExecution::wavefront);
    for (unsigned which = 0; which < 8; ++which) {
        auto invalid = config;
        switch (which) {
        case 0:
            invalid.capacity = 0;
            break;
        case 1:
            invalid.capacity = 65537;
            break;
        case 2:
            invalid.quantum = 0;
            break;
        case 3:
            invalid.quantum = 1025;
            break;
        case 4:
            invalid.group_width = 0;
            break;
        case 5:
            invalid.group_width = 257;
            break;
        case 6:
            invalid.transient_byte_limit = 1;
            break;
        default:
            invalid.width = UINT32_MAX;
            invalid.height = 2;
            break;
        }
        const auto rejected = build_frame_session(scene, invalid);
        require(rejected.status == GeometryStatus::failure && !rejected.session &&
                    !rejected.diagnostic.empty(),
                "Invalid wavefront resource configuration accepted");
    }
    // Finite authored radiance can overflow raw float sensor resolution. This
    // exercises an actual asynchronous error, not a model-changing queue knob.
    for (const bool packet : {false, true}) {
        auto overflow = base(packet);
        overflow.environment.fill(std::numeric_limits<float>::max());
        config.capacity = 1;
        config.quantum = 1;
        auto built = build_frame_session(overflow, config);
        require(built.status == GeometryStatus::success && built.session, built.diagnostic);
        auto frame = submit(*built.session);
        built.session.reset();
        const auto failed = frame->wait();
        require(failed.status == FrameStatus::failed && !failed.diagnostic.empty() &&
                    failed.command_count > 0,
                "Asynchronous nonfinite sensor failure must propagate through retained frame");
        const auto image = frame->capture();
        require(image.status == GeometryStatus::failure && image.raw_xyz.empty() &&
                    frame->poll().status == FrameStatus::failed,
                "Failed raw frame must not publish partial capture");
    }
}
#endif
} // namespace
int main() {
    try {
#if RYPATH_TEST_WAVEFRONT_METAL
        const auto device = query_metal_device();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
        if (device.status == MetalStatus::no_device)
            return 77;
#endif
        require(device.status == MetalStatus::success, device.diagnostic);
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
        if (!device.device.compute_ray_tracing)
            return 77;
#endif
        require(device.device.compute_ray_tracing, "Wavefront tests require compute ray tracing");
        scheduling_case(diffuse(false), "Scalar diffuse");
        scheduling_case(diffuse(true), "Packet diffuse");
        packet_history_cases();
        scheduling_case(glass(false), "Scalar dispersive glass chain");
        scheduling_case(glass(true), "Packet dispersive glass chain");
        scheduling_case(fixture("coated-path.pbrt"), "Packet coating");
        scheduling_case(medium(false), "Geometry-free scattering/emission medium");
        scheduling_case(medium(true), "Nonzero media/null boundary chains");
        scheduling_case(diffuse(false), "Compensated scalar diffuse",
                        IntersectionProfile::compensated);
        scheduling_case(diffuse(true), "Compensated packet diffuse",
                        IntersectionProfile::compensated);
        scheduling_case(glass(true), "Compensated dispersive glass",
                        IntersectionProfile::compensated);
        scheduling_case(medium(true), "Compensated media/null boundary chain",
                        IntersectionProfile::compensated);
        packet_scheduling_variants();
        packet_visibility_policy_cases();
        lifetime();
        invalid_and_async_failure();
#else
        const auto result = build_frame_session(
            {}, {.width = 1, .height = 1, .execution = FrameExecution::wavefront});
        require(result.status == GeometryStatus::not_built && !result.session &&
                    !result.diagnostic.empty(),
                "Portable wavefront availability contract");
#endif
        std::cout << "Wavefront frame validation passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
