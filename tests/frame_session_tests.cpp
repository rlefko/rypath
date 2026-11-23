// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"
#include "pbrt_render.hpp"
#include "rgb_spectrum.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
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
#if RYPATH_TEST_FRAME_METAL
RenderScene sky(bool packet = false) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{{}, {-.3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
    scene.spectral_transport = packet ? SpectralTransport::packet : SpectralTransport::scalar;
    scene.environment.fill(.7f);
    scene.max_depth = 1;
    return scene;
}
std::unique_ptr<Frame> submit(FrameSession &session, std::uint64_t index, std::uint64_t epoch = 0) {
    auto result = session.submit({.sample_index = index, .epoch = epoch, .camera = {}});
    require(result.status == GeometryStatus::success && result.frame && !result.busy,
            result.diagnostic);
    return std::move(result.frame);
}
void compare(const RenderScene &scene, const FrameConfig &config, std::uint64_t index,
             const FrameCapture &capture, double relative = 2e-5, double absolute = 2e-6) {
    require(capture.status == GeometryStatus::success, capture.diagnostic);
    const auto pixels = std::uint64_t(config.width) * config.height;
    require(capture.width == config.width && capture.height == config.height &&
                capture.raw_xyz.size() == pixels,
            "Capture dimensions/accounting");
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    const auto tile =
        std::min(render_tile_capacity, built.packet_lane_capacity != 0 ? built.packet_lane_capacity
                                                                       : render_tile_capacity);
    for (std::uint32_t begin = 0; begin < pixels;) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(tile, pixels - begin));
        auto samples =
            built.scene->sample({.width = config.width,
                                 .height = config.height,
                                 .seed = config.seed,
                                 .material_seed = config.material_seed,
                                 .pixel_begin = begin,
                                 .pixel_count = count,
                                 .sample_begin = index,
                                 .sample_count = 1,
                                 .total_samples = std::max(config.footprint_samples, index + 1)});
        require(samples.status == GeometryStatus::success, samples.diagnostic);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::array<double, 3> expected{};
            if (scene.spectral_transport == SpectralTransport::packet) {
                const auto &s = samples.packet_samples.at(i);
                expected = packet_sensor_xyz(s, s.radiance);
                require(s.splat_count == 0, "Live PT reference unexpectedly splats");
            } else {
                const auto &s = samples.samples.at(i);
                const bool pbrt = scene.model == RenderModel::pbrt;
                expected = pbrt ? pbrt_cie_xyz(s.wavelength) : wavelength_xyz(s.wavelength);
                for (auto &v : expected)
                    v *= s.radiance * (pbrt ? 470. : 1.);
            }
            for (unsigned c = 0; c < 3; ++c) {
                const auto actual = capture.raw_xyz[begin + i][c];
                // Same transport samples; only CPU double versus GPU float sensor arithmetic
                // differs.
                require(std::isfinite(actual) && std::abs(actual - expected[c]) <=
                                                     relative * std::abs(expected[c]) + absolute,
                        "GPU raw XYZ disagrees with independent host sensor reduction");
            }
        }
        begin += count;
    }
}
void reference_case(const RenderScene &scene, const FrameConfig &config, std::uint64_t index) {
    auto built = build_frame_session(scene, config);
    require(built.status == GeometryStatus::success && built.session, built.diagnostic);
    auto frame = submit(*built.session, index, 7);
    const auto info = frame->wait();
    require(info.status == FrameStatus::complete && info.sample_index == index && info.epoch == 7 &&
                info.completed_samples == std::uint64_t(config.width) * config.height,
            info.diagnostic.empty() ? "Frame completion identity/count" : info.diagnostic);
    require(!info.gpu_time_valid || (std::isfinite(info.gpu_seconds) && info.gpu_seconds >= 0),
            "Invalid GPU timing");
    compare(scene, config, index, frame->capture());
}
void lifetime_and_camera() {
    auto scene = sky();
    const FrameConfig config{.width = 17, .height = 9, .seed = 19317};
    auto built = build_frame_session(scene, config);
    require(built.status == GeometryStatus::success && built.session, built.diagnostic);
    auto first = submit(*built.session, 0, 11);
    auto second = submit(*built.session, 1, 12);
    auto busy = built.session->submit({.sample_index = 2, .epoch = 13, .camera = {}});
    require(busy.busy && !busy.frame && busy.status == GeometryStatus::failure,
            "Third retained frame must report busy");
    const auto original = first->capture();
    require(first->poll().status == FrameStatus::complete, "Completed frame poll");
    require(built.session->submit({}).busy, "Completion alone must not release retained slot");
    first.reset();
    auto third = submit(*built.session, 2, 13);
    built.session.reset();
    compare(scene, config, 0, original);
    compare(scene, config, 1, second->capture());
    compare(scene, config, 2, third->capture());
    Frame moved(std::move(*second));
    require(second->poll().status == FrameStatus::failed &&
                second->capture().status == GeometryStatus::failure,
            "Moved-from frame failure");
    compare(scene, config, 1, moved.capture());

    // A pending frame must survive release of both its handle and owning session.
    auto ephemeral = build_frame_session(scene, config);
    require(ephemeral.status == GeometryStatus::success, ephemeral.diagnostic);
    auto discarded = submit(*ephemeral.session, 3);
    discarded.reset();
    ephemeral.session.reset();

    scene = make_demo_scene();
    auto camera_session = build_frame_session(scene, config);
    require(camera_session.status == GeometryStatus::success, camera_session.diagnostic);
    auto old_camera = submit(*camera_session.session, 0);
    auto changed = scene;
    changed.camera.origin.x += 2;
    FrameCamera camera{.camera = changed.camera, .raster_camera = {}, .perspective_lens = {}};
    auto updated =
        camera_session.session->submit({.sample_index = 0, .epoch = 1, .camera = camera});
    require(updated.status == GeometryStatus::success && updated.frame, updated.diagnostic);
    compare(scene, config, 0, old_camera->capture());
    compare(changed, config, 0, updated.frame->capture());
}
void invalid_cases() {
    auto scene = sky();
    for (const auto config : {FrameConfig{}, FrameConfig{.width = UINT32_MAX, .height = 2},
                              FrameConfig{.width = 1, .height = 1, .footprint_samples = 0},
                              FrameConfig{.width = 1, .height = 1, .transient_byte_limit = 1}}) {
        auto built = build_frame_session(scene, config);
        require(built.status == GeometryStatus::failure && !built.session &&
                    !built.diagnostic.empty(),
                "Invalid frame configuration accepted");
    }
    const FrameConfig config{.width = 1, .height = 1};
    auto authored = sky(true);
    authored.integrator = IntegratorKind::bdpt;
    require(build_frame_session(authored, config).status == GeometryStatus::failure,
            "BDPT must require explicit live override");
    authored.integrator = IntegratorKind::path;
    authored.max_component_value = 1;
    require(build_frame_session(authored, config).status == GeometryStatus::failure,
            "Authored nonlinear clamp must require explicit live override");
    auto built = build_frame_session(scene, config);
    require(built.status == GeometryStatus::success, built.diagnostic);
    FrameCamera camera{}; // No raster camera for a PBRT scene.
    auto invalid = built.session->submit({.sample_index = 0, .epoch = 0, .camera = camera});
    require(invalid.status == GeometryStatus::failure && !invalid.frame && !invalid.busy,
            "Invalid camera must reject before submission");
    require(submit(*built.session, 0)->wait().status == FrameStatus::complete,
            "Rejected request must not poison session");

    // Valid spectral radiance can exceed the finite raw-float sensor domain.
    // This must fail GPU resolution and prohibit publication, not clamp or drop a sample.
    scene.environment.fill(std::numeric_limits<float>::max());
    auto overflow = build_frame_session(scene, config);
    require(overflow.status == GeometryStatus::success, overflow.diagnostic);
    auto failed = submit(*overflow.session, 0);
    require(failed->wait().status == FrameStatus::failed &&
                failed->capture().status == GeometryStatus::failure &&
                failed->capture().raw_xyz.empty(),
            "Nonfinite GPU output was published");
}
RenderScene translate_text(const std::string &text) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("rypath-frame-scenes-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    std::filesystem::create_directories(directory);
    const auto path = directory / "scene.pbrt";
    {
        std::ofstream file(path);
        file << text;
        require(bool(file), "Write frame scene fixture");
    }
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, {path, 1, 1});
    require(translated.exit_code() == 0 && translated.scene.has_value(), "Frame scene translation");
    return std::move(*translated.scene);
}
FrameCamera camera_snapshot(const RenderScene &scene) {
    return {.camera = scene.camera,
            .raster_camera = scene.raster_camera,
            .perspective_lens = scene.perspective_lens,
            .texture_camera = scene.texture_camera};
}
std::string camera_pattern_scene(bool alpha, bool translated) {
    // The moved oracle is freshly parsed and translated from a changed PBRT
    // camera transform. It never applies the native rebase formula to a scene.
    std::string text = translated ? "Translate 0.375 -0.125 0.25\n" : "Identity\n";
    text += R"(Camera "perspective" "float fov" [45]
Film "rgb" "integer xresolution" [24] "integer yresolution" [16]
 "string sensor" "cie1931" "bool savefp16" [false]
PixelFilter "box"
Sampler "independent" "integer pixelsamples" [1]
Integrator "path" "integer maxdepth" [1]
WorldBegin
LightSource "infinite" "spectrum L" [360 1 830 1]
)";
    if (alpha) {
        text += R"(Texture "coverage" "float" "checkerboard" "integer dimension" [3]
 "float tex1" [0] "float tex2" [1]
Material "diffuse" "spectrum reflectance" [360 0 830 0]
AttributeBegin
Translate 0.25 -0.125 0.375
Shape "trianglemesh" "point3 P" [-4 -4 2 4 -4 2 4 4 2 -4 4 2]
 "integer indices" [0 1 2 0 2 3] "texture alpha" "coverage"
AttributeEnd
)";
    } else {
        // Keep the plane away from a 3D checker's depth discontinuity: its
        // tent filter otherwise averages the entire plane to one constant.
        text += R"(AttributeBegin
Translate 0 0 0.125
Scale 1.75 2.5 1
Texture "pattern" "spectrum" "checkerboard" "integer dimension" [3]
 "spectrum tex1" [360 .1 830 .1] "spectrum tex2" [360 .8 830 .8]
AttributeEnd
Material "diffuse" "texture reflectance" "pattern"
Shape "trianglemesh" "point3 P" [-4 -4 2 4 -4 2 4 4 2 -4 4 2]
 "integer indices" [0 1 2 0 2 3]
)";
    }
    return text;
}
void translated_texture_camera() {
    const FrameConfig config{.width = 24, .height = 16, .seed = 19317};
    for (const bool alpha : {false, true}) {
        const auto original = translate_text(camera_pattern_scene(alpha, false));
        const auto changed = translate_text(camera_pattern_scene(alpha, true));
        auto built = build_frame_session(original, config);
        require(built.status == GeometryStatus::success, built.diagnostic);
        auto old_frame = submit(*built.session, 0);
        auto moved = built.session->submit({.sample_index = 0, .camera = camera_snapshot(changed)});
        require(moved.status == GeometryStatus::success && moved.frame, moved.diagnostic);
        const auto old_capture = old_frame->capture();
        const auto changed_capture = moved.frame->capture();
        compare(changed, config, 0, changed_capture);
        compare(original, config, 0, old_capture);
        require(moved.frame->poll().camera_upload_bytes > 0,
                "Translated patterned camera must report mapping uploads");
        double difference = 0, signal = 0;
        for (std::size_t pixel = 0; pixel < old_capture.raw_xyz.size(); ++pixel) {
            difference = std::max(difference, std::abs(double(old_capture.raw_xyz[pixel][1]) -
                                                       changed_capture.raw_xyz[pixel][1]));
            signal = std::max({signal, double(old_capture.raw_xyz[pixel][1]),
                               double(changed_capture.raw_xyz[pixel][1])});
        }
        require(signal > 0 && difference > .001 * signal,
                std::string(alpha ? "Alpha" : "World") +
                    " patterned camera translation must change the image: difference=" +
                    std::to_string(difference) + " signal=" + std::to_string(signal));
        moved.frame.reset(); // Old frame reserves slot zero; reuse the translated slot.
        auto cached =
            built.session->submit({.sample_index = 0, .camera = camera_snapshot(changed)});
        require(cached.status == GeometryStatus::success && cached.frame, cached.diagnostic);
        require(cached.frame->capture().raw_xyz == changed_capture.raw_xyz &&
                    cached.frame->poll().camera_upload_bytes == 0,
                "Same-origin slot must reuse correct rebased mappings without upload");
        cached.frame.reset();
        auto restored = submit(*built.session, 0);
        require(restored->capture().raw_xyz == old_capture.raw_xyz &&
                    restored->poll().camera_upload_bytes == 0,
                "Returning to the authored origin must use immutable original mappings");
        require(old_frame->capture().raw_xyz == old_capture.raw_xyz,
                "Camera rebase must not mutate another retained frame");
    }
}
void fixed_footprint_high_index() {
    // Independent coarsest-mip control from image_render_tests.cpp. At this
    // minification, footprint scale 1 selects the 1x1 mip, whereas the legacy
    // growing sample budget shrinks the footprint to .125 and preserves detail.
    auto scene = sky();
    scene.environment.fill(1);
    scene.raster_camera = RasterCamera{{}, {-.5f, .5f, 1}, {1, 0, 0}, {0, -1, 0}};
    scene.materials = {{.kind = SpectralMaterialKind::image}};
    scene.instance_materials = {0};
    scene.geometry.meshes = {{.positions = {{-4, -4, 2}, {4, -4, 2}, {-4, 4, 2}, {4, 4, 2}},
                              .triangles = {{0, 1, 2}, {2, 1, 3}},
                              .uvs = {{0, 0}, {1, 0}, {0, 1}, {1, 1}}}};
    scene.geometry.instances = {{.transform = {}}};
    constexpr std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    scene.instance_texture_frames.resize(1);
    require(prepare_texture_triangle_frames(scene.geometry.meshes.front(), identity, identity,
                                            scene.instance_texture_frames.front())
                .empty(),
            "Minified plane texture frames");
    DecodedImage image{8, 8, {}};
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            image.rgb.insert(image.rgb.end(), {static_cast<std::uint8_t>(23 * x + 3 * y),
                                               static_cast<std::uint8_t>(7 * x + 20 * y),
                                               static_cast<std::uint8_t>(250 - 12 * x - 9 * y)});
    auto pyramid = build_image_pyramid(image, ImageEncoding::linear, ImageWrap::repeat);
    require(pyramid.value.has_value(), pyramid.diagnostic);
    scene.image_textures.push_back(
        {.image = std::make_shared<ImagePyramid>(std::move(*pyramid.value)),
         .uscale = 40,
         .vscale = 40});
    auto coarse = scene;
    const auto rgb = reconstruct_srgb({91 / 255.f, 95 / 255.f, 177 / 255.f});
    require(rgb.has_value(), "Coarse-mip RGB reconstruction");
    coarse.materials.front() = {.kind = rgb->is_constant ? SpectralMaterialKind::constant
                                                         : SpectralMaterialKind::rgb_sigmoid,
                                .constant = rgb->constant,
                                .coefficients = rgb->coefficients};
    coarse.image_textures.clear();
    coarse.instance_texture_frames.clear();
    const FrameConfig config{
        .width = 8, .height = 8, .seed = 0x913b6a1f7957c251ULL, .footprint_samples = 1};
    constexpr std::uint64_t index = (1ull << 32) + 17;
    auto built = build_frame_session(scene, config);
    require(built.status == GeometryStatus::success, built.diagnostic);
    auto frame = submit(*built.session, index);
    const auto capture = frame->capture();
    compare(coarse, config, index, capture, 2e-4, .001);
    auto legacy = build_metal_renderer(scene);
    require(legacy.status == GeometryStatus::success, legacy.diagnostic);
    auto samples = legacy.scene->sample({.width = 8,
                                         .height = 8,
                                         .seed = config.seed,
                                         .pixel_count = 64,
                                         .sample_begin = index,
                                         .sample_count = 1,
                                         .total_samples = index + 1});
    require(samples.status == GeometryStatus::success, samples.diagnostic);
    double difference = 0;
    for (const auto &sample : samples.samples) {
        const auto cie = pbrt_cie_xyz(sample.wavelength);
        for (unsigned c = 0; c < 3; ++c)
            difference = std::max(difference, std::abs(cie[c] * sample.radiance * 470 -
                                                       capture.raw_xyz[sample.pixel][c]));
    }
    require(difference > .05, "Growing-budget control must differ from fixed footprint output");
}
void packet_dielectric_geometry() {
    const auto path = std::filesystem::path(RYPATH_SOURCE_DIR) /
                      "tests/fixtures/pbrt-render/packet-transport/glass-delta-chain.pbrt";
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, {path, 1, 1});
    require(translated.scene.has_value() && translated.exit_code() == 0,
            "Packet glass translation");
    auto &scene = *translated.scene;
    scene.spectral_transport = SpectralTransport::packet;
    require(!scene.geometry.meshes.empty() &&
                std::ranges::any_of(
                    scene.materials,
                    [](const auto &m) { return m.scattering == ScatteringKind::dielectric; }),
            "Packet fixture must exercise actual dielectric geometry");
    reference_case(scene, {.width = 17, .height = 9, .seed = 19317}, (1ull << 32) + 7);
}
void camera_media() {
    auto scene = sky();
    scene.volume_transport = true;
    scene.integrator = IntegratorKind::volpath;
    scene.media.resize(2);
    scene.media[0].sigma_a.fill(.25f);
    scene.media[0].sigma_s.fill(.15f);
    scene.media[0].emission.fill(1);
    scene.media[1].sigma_a.fill(.9f);
    scene.media[1].sigma_s.fill(.05f);
    scene.media[1].emission.fill(.1f);
    scene.camera_medium = 0;
    const FrameConfig config{.width = 17, .height = 9, .seed = 19317};
    auto built = build_frame_session(scene, config);
    require(built.status == GeometryStatus::success, built.diagnostic);
    auto camera = camera_snapshot(scene);
    auto preserved = built.session->submit({.sample_index = 3, .camera = camera});
    require(preserved.status == GeometryStatus::success && preserved.frame, preserved.diagnostic);
    const auto original = preserved.frame->capture();
    compare(scene, config, 3, original);
    preserved.frame.reset();
    for (const auto medium : {1u, no_medium}) {
        camera.camera_medium = medium;
        auto changed = built.session->submit({.sample_index = 3, .camera = camera});
        require(changed.status == GeometryStatus::success && changed.frame, changed.diagnostic);
        auto oracle = scene;
        oracle.camera_medium = medium;
        const auto capture = changed.frame->capture();
        compare(oracle, config, 3, capture);
        require(capture.raw_xyz != original.raw_xyz,
                "Medium override must change the rendered paths");
    }
    camera.camera_medium = 2;
    const auto invalid = built.session->submit({.sample_index = 3, .camera = camera});
    require(invalid.status == GeometryStatus::failure && !invalid.frame && !invalid.busy,
            "Out-of-range camera medium must reject before submission");
    compare(scene, config, 3, submit(*built.session, 3)->capture());
    // Defined but inactive media are legal in vacuum scenes; they must not turn
    // a path/packet pipeline into volume transport through a camera-only override.
    for (const bool packet : {false, true}) {
        auto vacuum = sky(packet);
        vacuum.media = scene.media;
        auto session = build_frame_session(vacuum, config);
        require(session.status == GeometryStatus::success, session.diagnostic);
        auto snapshot = camera_snapshot(vacuum);
        snapshot.camera_medium = 0;
        const auto rejected = session.session->submit({.camera = snapshot});
        require(rejected.status == GeometryStatus::failure && !rejected.frame && !rejected.busy,
                "Vacuum path/packet camera must reject an active medium override");
        snapshot.camera_medium = no_medium;
        auto accepted = session.session->submit({.camera = snapshot});
        require(accepted.status == GeometryStatus::success && accepted.frame, accepted.diagnostic);
        compare(vacuum, config, 0, accepted.frame->capture());
    }
}

void textured_and_media() {
    for (const auto name : {"image-minified.pbrt", "sampling-defaults/volume.pbrt"}) {
        const auto path =
            std::filesystem::path(RYPATH_SOURCE_DIR) / "tests/fixtures/pbrt-render" / name;
        const auto checked = pbrt::check(path);
        require(checked.exit_code() == 0, "Fixture preflight");
        auto translated = pbrt::translate(checked, {path, 1, 1});
        require(translated.scene.has_value() && translated.exit_code() == 0, "Fixture translation");
        reference_case(*translated.scene, {.width = 17, .height = 9, .footprint_samples = 64}, 17);
    }
}
#endif
} // namespace
int main() {
    try {
#if RYPATH_TEST_FRAME_METAL
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
        require(device.device.compute_ray_tracing, "Frame tests require compute ray tracing");
        reference_case(sky(), {.width = 257, .height = 257}, (1ull << 32) + 17);
        auto packet = sky(true);
        packet.max_depth = 64;
        reference_case(packet, {.width = 137, .height = 67}, 3);
        reference_case(make_demo_scene(), {.width = 17, .height = 9}, 0);
        lifetime_and_camera();
        invalid_cases();
        textured_and_media();
        translated_texture_camera();
        fixed_footprint_high_index();
        packet_dielectric_geometry();
        camera_media();
#else
        const auto result = build_frame_session({}, {.width = 1, .height = 1});
        require(result.status == GeometryStatus::not_built && !result.session &&
                    !result.diagnostic.empty(),
                "Portable frame availability contract");
#endif
        std::cout << "Frame session validation passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
