// SPDX-License-Identifier: GPL-3.0-only
#include "area_wire.h"
#include "camera_medium.hpp"
#include "image_output.hpp"
#include "metal_execution.hpp"
#include "metal_reconstruction_access.hpp"
#include "pbrt_render.hpp"
#include "viewer_options.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <mutex>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace rypath;
using Clock = std::chrono::steady_clock;
using Vec = std::array<double, 3>;
Vec vec(GeometryPoint p) { return {p.x, p.y, p.z}; }
GeometryPoint point(Vec p) { return {float(p[0]), float(p[1]), float(p[2])}; }
Vec add(Vec a, Vec b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec sub(Vec a, Vec b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec mul(Vec a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(Vec a, Vec b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(Vec a, Vec b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec unit(Vec p) {
    const double length = std::hypot(p[0], p[1], p[2]);
    if (!(length > 0) || !std::isfinite(length))
        throw std::runtime_error("Invalid camera direction");
    return mul(p, 1 / length);
}
struct Basis {
    Vec right, up, forward;
};
Basis basis(const FrameCamera &camera) {
    if (camera.raster_camera) {
        const auto &r = *camera.raster_camera;
        auto forward = unit(cross(vec(r.dx), vec(r.dy)));
        if (dot(forward, vec(r.corner)) < 0)
            forward = mul(forward, -1);
        const auto right = unit(vec(r.dx));
        return {right, unit(cross(right, forward)), forward};
    }
    const auto forward = unit(sub(vec(camera.camera.target), vec(camera.camera.origin)));
    const auto right = unit(cross(forward, vec(camera.camera.up)));
    return {right, unit(cross(right, forward)), forward};
}
GeometryPoint origin(const FrameCamera &camera) {
    return camera.raster_camera ? camera.raster_camera->origin : camera.camera.origin;
}
void translate(FrameCamera &camera, Vec displacement) {
    camera.camera.origin = point(add(vec(camera.camera.origin), displacement));
    camera.camera.target = point(add(vec(camera.camera.target), displacement));
    if (camera.raster_camera)
        camera.raster_camera->origin = point(add(vec(camera.raster_camera->origin), displacement));
}
void rotate(FrameCamera &camera, Vec axis, double angle) {
    axis = unit(axis);
    const auto rotated = [&](Vec p) {
        return add(add(mul(p, std::cos(angle)), mul(cross(axis, p), std::sin(angle))),
                   mul(axis, dot(axis, p) * (1 - std::cos(angle))));
    };
    camera.camera.target =
        point(add(vec(camera.camera.origin),
                  rotated(sub(vec(camera.camera.target), vec(camera.camera.origin)))));
    camera.camera.up = point(rotated(vec(camera.camera.up)));
    if (camera.raster_camera) {
        auto &r = *camera.raster_camera;
        r.corner = point(rotated(vec(r.corner)));
        r.dx = point(rotated(vec(r.dx)));
        r.dy = point(rotated(vec(r.dy)));
    }
    if (camera.perspective_lens) {
        auto &lens = *camera.perspective_lens;
        lens.camera_x = point(rotated(vec(lens.camera_x)));
        lens.camera_y = point(rotated(vec(lens.camera_y)));
    }
    if (camera.texture_camera) {
        const auto old = *camera.texture_camera;
        std::array<Vec, 3> columns{rotated({1, 0, 0}), rotated({0, 1, 0}), rotated({0, 0, 1})};
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned col = 0; col < 3; ++col) {
                double world = 0;
                for (unsigned k = 0; k < 3; ++k) {
                    world += columns[k][row] * old.world_from_camera[k * 3 + col];
                }
                camera.texture_camera->world_from_camera[row * 3 + col] = float(world);
            }
        // Recompute the inverse of the packed matrix rather than accumulating
        // an independently rounded inverse across thousands of camera updates.
        const auto &m = camera.texture_camera->world_from_camera;
        const Vec a{m[0], m[3], m[6]}, b{m[1], m[4], m[7]}, c{m[2], m[5], m[8]};
        const double determinant = dot(a, cross(b, c));
        if (!std::isfinite(determinant) || determinant == 0)
            throw std::runtime_error("Rotated texture camera is singular");
        const std::array<Vec, 3> rows{mul(cross(b, c), 1 / determinant),
                                      mul(cross(c, a), 1 / determinant),
                                      mul(cross(a, b), 1 / determinant)};
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned col = 0; col < 3; ++col)
                camera.texture_camera->camera_from_world[row * 3 + col] = float(rows[row][col]);
    }
}
NSString *native(const std::string &text) {
    auto value = [[NSString alloc] initWithBytes:text.data()
                                          length:text.size()
                                        encoding:NSUTF8StringEncoding];
    return value ? value : @"Diagnostic contains invalid UTF-8";
}
void stop_application() {
    [NSApp stop:nil];
    auto event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:0
                                     context:nil
                                     subtype:0
                                       data1:0
                                       data2:0];
    if (event != nil)
        [NSApp postEvent:event atStart:NO];
}
void set_parameter(pbrt::Descriptor &d, const std::string &name,
                   std::optional<std::int32_t> value) {
    std::vector<pbrt::Parameter> effective;
    for (const auto index : d.effective_authored)
        effective.push_back(d.authored.parameters[index]);
    d.authored.parameters = std::move(effective);
    std::erase_if(d.authored.parameters, [&](const auto &p) { return p.name == name; });
    std::erase_if(d.inherited, [&](const auto &p) { return p.parameter->name == name; });
    if (value) {
        pbrt::Parameter p;
        p.type = "integer";
        p.name = name;
        pbrt::Value v;
        v.kind = pbrt::ValueKind::integer;
        v.integer = *value;
        v.number = *value;
        v.text = std::to_string(*value);
        p.values.push_back(v);
        d.authored.parameters.push_back(std::move(p));
    }
    d.effective_authored.clear();
    for (std::size_t i = 0; i < d.authored.parameters.size(); ++i)
        d.effective_authored.push_back(i);
}
RenderScene load_scene(const viewer::Options &options) {
    if (options.demo) {
        auto scene = make_demo_scene();
        const auto error = select_spectral_transport(scene, options.spectral_transport);
        if (!error.empty())
            throw std::runtime_error(error);
        if (options.hero_wavelengths && scene.spectral_transport != SpectralTransport::hero)
            throw std::runtime_error("--hero-wavelengths requires effective hero transport");
        if (options.hero_wavelengths)
            scene.hero_wavelengths = *options.hero_wavelengths;
        return scene;
    }
    const auto limits = resource_limits(options.resource_profile).pbrt;
    auto checked = pbrt::check(options.scene, limits);
    if (checked.exit_code() || !checked.scene) {
        std::cerr << pbrt::report(checked);
        throw std::runtime_error("PBRT preflight failed; viewer did not render");
    }
    for (const auto &[name, type] :
         {std::pair{"Sampler", "independent"}, std::pair{"Film", "rgb"}}) {
        if (std::ranges::none_of(checked.scene->options,
                                 [&](const auto &d) { return d.authored.name == name; })) {
            pbrt::Descriptor descriptor;
            descriptor.authored.name = name;
            pbrt::Value value;
            value.kind = pbrt::ValueKind::string;
            value.text = type;
            descriptor.authored.arguments.push_back(value);
            checked.scene->options.push_back(std::move(descriptor));
        }
    }
    for (auto &d : checked.scene->options) {
        if (d.authored.name != "Sampler" && d.authored.name != "Film" &&
            d.authored.name != "Integrator")
            continue;
        std::cout << "Authored " << d.authored.name << ' ' << d.authored.arguments.front().text
                  << '\n';
        const auto report = [&](const auto &p) {
            std::cout << "  " << p.type << ' ' << p.name;
            for (const auto &v : p.values)
                std::cout << ' ' << v.text;
            std::cout << '\n';
        };
        for (const auto &p : d.authored.parameters)
            report(p);
        for (const auto &p : d.inherited)
            report(*p.parameter);
        if (d.authored.name == "Integrator" && d.authored.arguments.front().text == "bdpt")
            d.authored.arguments.front().text = "path";
        else if (d.authored.name == "Sampler") {
            d.authored.arguments.front().text = "independent";
            d.authored.parameters.clear();
            d.inherited.clear();
            d.effective_authored.clear();
            set_parameter(d, "pixelsamples", 1);
        } else if (d.authored.name == "Film") {
            set_parameter(d, "xresolution", std::int32_t(options.width));
            set_parameter(d, "yresolution", std::int32_t(options.height));
            set_parameter(d, "maxcomponentvalue", {});
        }
    }
    auto translated =
        pbrt::translate(checked, {options.scene.string()}, limits, options.spectral_transport);
    for (const auto &d : translated.diagnostics)
        std::cerr << pbrt::format_diagnostic(d);
    for (const auto &p : translated.provenance)
        std::cout << p << '\n';
    if (translated.exit_code() || !translated.scene)
        throw std::runtime_error("PBRT rendering translation failed");
    auto scene = std::move(*translated.scene);

    std::string spectral_provenance;
    if (const auto error =
            select_spectral_transport(scene, options.spectral_transport, &spectral_provenance);
        !error.empty())
        throw std::runtime_error(error);
    if (options.hero_wavelengths && scene.spectral_transport != SpectralTransport::hero)
        throw std::runtime_error("--hero-wavelengths requires effective hero transport");
    if (options.hero_wavelengths)
        scene.hero_wavelengths = *options.hero_wavelengths;
    std::cout << spectral_provenance << '\n';
    return scene;
}
double movement_speed(const RenderScene &scene) {
    double extent = 0;
    for (const auto &mesh : scene.geometry.meshes) {
        if (mesh.positions.empty())
            continue;
        Vec low = vec(mesh.positions.front()), high = low;
        for (const auto p : mesh.positions)
            for (unsigned k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], double(k == 0 ? p.x : k == 1 ? p.y : p.z));
                high[k] = std::max(high[k], double(k == 0 ? p.x : k == 1 ? p.y : p.z));
            }
        extent = std::max({extent, high[0] - low[0], high[1] - low[1], high[2] - low[2]});
    }
    return std::clamp(extent * .15, .1, 10000.);
}
std::string write_rgb(const std::filesystem::path &path, const ReconstructionCapture &image,
                      const std::vector<std::pair<std::string, std::string>> &metadata) {
    std::vector<float> values;
    values.reserve(image.linear_rgb.size() * 3);
    for (const auto &pixel : image.linear_rgb)
        values.insert(values.end(), pixel.begin(), pixel.end());
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0)
        return "Could not create fresh reconstructed EXR";
    auto diagnostic =
        encode_exr({image.width, image.height, values, ExrPrecision::float32, metadata},
                   [&](std::uint64_t offset, std::span<const std::uint8_t> bytes) {
                       if (offset > std::uint64_t(std::numeric_limits<off_t>::max()) ||
                           bytes.size() > std::uint64_t(std::numeric_limits<off_t>::max()) - offset)
                           return false;
                       std::size_t done = 0;
                       while (done < bytes.size()) {
                           const auto written = ::pwrite(fd, bytes.data() + done,
                                                         bytes.size() - done, off_t(offset + done));
                           if (written < 0 && errno == EINTR)
                               continue;
                           if (written <= 0)
                               return false;
                           done += std::size_t(written);
                       }
                       return true;
                   });
    if (::close(fd) != 0 && diagnostic.empty())
        diagnostic = "Could not close reconstructed EXR";
    if (!diagnostic.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    return diagnostic;
}
struct Presentation {
    std::atomic<bool> pending{false};
    std::mutex mutex;
    std::string error;
};
} // namespace

class ViewerState {
  public:
    viewer::Options options;
    RenderScene scene;
    FrameCamera authored, pose, submitted_pose;
    std::unique_ptr<FrameSession> renderer;
    std::unique_ptr<CameraMediumTracker> medium_tracker;
    std::unique_ptr<ReconstructionStream> reconstruction;
    std::optional<ReconstructionMode> stream_mode;
    std::unique_ptr<Frame> pending_raw, latest_raw;
    std::unique_ptr<ReconstructedFrame> pending_image, latest_image;
    ReconstructionMode pipeline_mode;
    const ReconstructionMode comparison_denoiser;
    std::set<unsigned short> keys;
    Vec translation{};
    double yaw = 0, pitch = 0, speed = 1, ev = 0, latest_pipeline_seconds = 0;
    bool fast = false, home = false, capture_requested = false, redraw = true;
    bool paused = false, closing = false, reset_submission = false;
    bool interfaces = false;
    int exit_code = 0;
    std::uint64_t sample = 0, epoch = 0, completed = 0, capture_serial = 0;
    std::string diagnostic;
    Clock::time_point previous_tick = Clock::now(), pipeline_start = previous_tick;
    std::deque<Clock::time_point> completed_times;
    std::shared_ptr<Presentation> presentation = std::make_shared<Presentation>();
    __weak NSWindow *window = nil;
    __weak NSView *view = nil;
    CAMetalLayer *layer = nil;
    id<MTLCommandQueue> display_queue = nil;
    id<MTLRenderPipelineState> display_pipeline = nil;

    ViewerState(viewer::Options settings, RenderScene source)
        : options(std::move(settings)), scene(std::move(source)),
          pipeline_mode(options.reconstruction),
          comparison_denoiser(viewer::preferred_denoiser(options.reconstruction)),
          ev(options.exposure_ev) {
        scene.integrator = scene.volume_transport ? IntegratorKind::volpath : IntegratorKind::path;
        scene.max_component_value = std::numeric_limits<double>::infinity();
        authored = {.camera = scene.camera,
                    .camera_medium = scene.camera_medium,
                    .raster_camera = scene.raster_camera,
                    .perspective_lens = scene.perspective_lens,
                    .texture_camera = scene.texture_camera};
        pose = authored;
        submitted_pose = authored;
        speed = movement_speed(scene);
        interfaces = std::ranges::any_of(scene.instance_media,
                                         [](const auto &m) { return m.inside != m.outside; });
        const auto config = viewer::frame_configuration(options);
        auto built = build_frame_session(scene, config);
        if (built.status != GeometryStatus::success || !built.session)
            throw std::runtime_error(built.diagnostic);
        renderer = std::move(built.session);
        std::cout << "Live overrides: " << options.width << 'x' << options.height
                  << ", PT/volpath, independent sample stream, one fresh path/pixel/frame, "
                     "footprint=1, unclamped raw XYZ\n"
                  << "Native wavefront; estimator=" << estimator_name()
                  << "; mutations=" << config.mutation_steps
                  << "; domain=" << frame_area_path_domain_name(config.area_path_domain)
                  << "; replay=" << frame_contribution_replay_name(config.contribution_replay)
                  << "; spatial=" << frame_spatial_mode_name(config.spatial_mode)
                  << "; history cap policy="
                  << frame_history_cap_policy_name(config.history_cap_policy)
                  << "; history cap maximum=" << config.enhanced_history_cap
                  << "; resources=" << resource_profile_name(options.resource_profile)
                  << "; spectral=" << spectral_transport_name(scene.spectral_transport)
                  << "; film exposure=" << scene.exposure << "; display EV=" << ev << '\n';
    }
    const char *estimator_name() const {
        return options.estimator == FrameEstimator::area_replay ? "area-replay" : "independent";
    }
    static const char *mode_name(ReconstructionMode mode) {
        switch (mode) {
        case ReconstructionMode::metalfx:
            return "MetalFX";
        case ReconstructionMode::temporal:
            return "Signed Temporal";
        case ReconstructionMode::raw_rgb:
            return "Raw RGB";
        }
        return "Unknown";
    }
    void close() {
        closing = true;
        stop_application();
    }
    void halt(const std::string &error) {
        diagnostic = error;
        paused = true;
        pending_image.reset();
        pending_raw.reset();
        std::cerr << "Viewer paused: " << error << '\n';
        if (options.frames) {
            exit_code = 1;
            close();
        }
    }
    void toggle() {
        options.reconstruction =
            viewer::toggle_comparison(options.reconstruction, comparison_denoiser);
        if (paused) {
            paused = false;
            reconstruction.reset();
            stream_mode.reset();
        }
        diagnostic = std::string("Requested ") + mode_name(options.reconstruction);
    }
    void reset() {
        home = true;
        paused = false;
        diagnostic.clear();
    }
    void exposure(double amount) {
        ev = std::clamp(ev + amount, -24., 24.);
        redraw = true;
    }
    void input(double dt) {
        const double step = speed * dt * (fast ? 4 : 1);
        translation[0] += (double(keys.contains(2)) - double(keys.contains(0))) * step;
        translation[1] += (double(keys.contains(14)) - double(keys.contains(12))) * step;
        translation[2] += (double(keys.contains(13)) - double(keys.contains(1))) * step;
        yaw += (double(keys.contains(123)) - double(keys.contains(124))) * dt * 1.2;
        pitch += (double(keys.contains(126)) - double(keys.contains(125))) * dt * 1.2;
    }
    void start() {
        pipeline_start = Clock::now(); // Includes camera-medium classification and stream setup.
        auto candidate = home ? authored : pose;
        if (!home) {
            const auto b = basis(candidate);
            if (yaw != 0)
                rotate(candidate, basis(authored).up, yaw);
            if (pitch != 0)
                rotate(candidate, basis(candidate).right, pitch);
            if (latest_raw || !interfaces)
                translate(candidate,
                          add(add(mul(b.right, translation[0]), mul(b.up, translation[1])),
                              mul(b.forward, translation[2])));
        }
        const auto from = origin(pose), to = origin(candidate);
        const bool translated = from.x != to.x || from.y != to.y || from.z != to.z;
        if (translated && interfaces) {
            if (!latest_raw)
                candidate = pose;
            else {
                if (!medium_tracker) {
                    auto built = CameraMediumTracker::build(*latest_raw);
                    if (!built.tracker) {
                        diagnostic = "Camera move rejected: " + built.diagnostic;
                        candidate = pose;
                    } else
                        medium_tracker = std::move(built.tracker);
                }
                if (medium_tracker) {
                    const auto classified = medium_tracker->classify(*latest_raw, to);
                    if (classified.status == GeometryStatus::success)
                        candidate.camera_medium = classified.medium;
                    else {
                        diagnostic = "Camera move rejected: " + classified.diagnostic;
                        candidate = pose;
                    }
                }
            }
        }
        // Always carry the known medium, including rotations and stationary frames.
        if (!candidate.camera_medium)
            candidate.camera_medium = pose.camera_medium.value_or(scene.camera_medium);
        if (home && epoch == UINT64_MAX)
            throw std::runtime_error("Viewer epoch exhausted");
        const auto candidate_epoch = epoch + std::uint64_t(home);
        if (sample == UINT64_MAX)
            throw std::runtime_error("Viewer sample index exhausted");
        auto result = renderer->submit(
            {.sample_index = sample, .epoch = candidate_epoch, .camera = candidate});
        if (result.busy)
            return; // A completion lease may still be finishing its callback.
        if (!result.frame) {
            halt(result.diagnostic);
            return;
        }
        epoch = candidate_epoch;
        home = false;
        yaw = pitch = 0;
        translation = {};
        ++sample;
        submitted_pose = candidate;
        pipeline_mode = options.reconstruction;
        pending_raw = std::move(result.frame);
    }
    double fps() const {
        if (completed_times.size() < 2)
            return latest_pipeline_seconds > 0 ? 1 / latest_pipeline_seconds : 0;
        const double seconds =
            std::chrono::duration<double>(completed_times.back() - completed_times.front()).count();
        return seconds > 0 ? double(completed_times.size() - 1) / seconds : 0;
    }
    void title() {
        const auto mode = latest_image ? latest_image->poll().mode : options.reconstruction;
        NSString *details = @"";
        if (latest_image) {
            const auto info = latest_image->poll();
            const auto &c = info.counters;
            const std::uint64_t approximations =
                std::uint64_t(c.mix_fallback) + c.measured_fallback + c.fourier_fallback +
                c.layered_approximation + c.transmission_approximation + c.normal_approximation +
                c.media_approximation;
            details = [NSString
                localizedStringWithFormat:
                    NSLocalizedString(
                        @" | Guide approximations %llu%@ | Input <0/>65504 %u/%u",
                        @"Viewer guide approximations and observed input RGB range counts"),
                    static_cast<unsigned long long>(approximations),
                    info.defocus_approximation ? @"+DOF" : @"", c.color_negative, c.color_high];
        }
        if (paused)
            details = [details
                stringByAppendingString:NSLocalizedString(@" | PAUSED", @"Viewer paused status")];
        if (!diagnostic.empty())
            details =
                [details stringByAppendingFormat:NSLocalizedString(@" | Status: %@",
                                                                   @"Viewer technical diagnostic"),
                                                 native(diagnostic)];
        NSString *mode_title =
            mode == ReconstructionMode::metalfx ? @"MetalFX"
            : mode == ReconstructionMode::temporal
                ? NSLocalizedString(@"Signed Temporal", @"Signed temporal RGB reconstruction mode")
                : NSLocalizedString(@"Raw RGB", @"Unfiltered linear RGB preview mode");
        window.title = [NSString
            localizedStringWithFormat:
                NSLocalizedString(
                    @"rypath %ux%u | %@/M%u | %@ | %.1f rendered fps | EV %.1f%@",
                    @"Viewer resolution, estimator, mutation count, mode, frame rate and exposure"),
                options.width, options.height, native(estimator_name()),
                options.estimator == FrameEstimator::area_replay ? options.mutation_steps : 0,
                mode_title, fps(), ev, details];
    }
    void prepare_display(id<MTLDevice> device) {
        std::string error;
        auto library = metal::load_library(device, {}, error);
        if (library == nil)
            throw std::runtime_error(error);
        auto vertex = metal::load_function(library, @"rypath_viewer_vertex", error);
        auto fragment = metal::load_function(library, @"rypath_viewer_fragment", error);
        if (vertex == nil || fragment == nil)
            throw std::runtime_error(error);
        auto descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        NSError *failure = nil;
        display_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&failure];
        if (display_pipeline == nil)
            throw std::runtime_error(failure ? failure.localizedDescription.UTF8String
                                             : "Display pipeline creation failed");
        display_queue = [device newCommandQueue];
        if (display_queue == nil)
            throw std::runtime_error("Display queue creation failed");
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        const auto srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        if (!srgb)
            throw std::runtime_error("Display sRGB color space creation failed");
        layer.colorspace = srgb;
        CGColorSpaceRelease(srgb);
        layer.framebufferOnly = YES;
        layer.displaySyncEnabled = YES;
        layer.allowsNextDrawableTimeout = YES;
    }
    void present() {
        if (!latest_image || !redraw || presentation->pending.load() || !window.isVisible ||
            window.isMiniaturized || !(window.occlusionState & NSWindowOcclusionStateVisible))
            return;
        auto acquired = MetalReconstructedFrameAccess::acquire(*latest_image);
        if (!acquired.lease)
            throw std::runtime_error(acquired.diagnostic);
        if (acquired.lease->output_texture().pixelFormat == MTLPixelFormatRGBA32Float &&
            !acquired.lease->device().supports32BitFloatFiltering)
            throw std::runtime_error("Signed temporal display requires float32 texture filtering");
        if (display_pipeline == nil)
            prepare_display(acquired.lease->device());
        const auto backing = [view convertRectToBacking:view.bounds].size;
        if (backing.width < 1 || backing.height < 1)
            return;
        layer.contentsScale = window.backingScaleFactor;
        layer.drawableSize = backing;
        auto drawable = [layer nextDrawable];
        if (drawable == nil)
            return;
        auto descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        descriptor.colorAttachments[0].texture = drawable.texture;
        descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        auto command = [display_queue commandBuffer];
        auto encoder = [command renderCommandEncoderWithDescriptor:descriptor];
        if (command == nil || encoder == nil)
            throw std::runtime_error("Display encoder creation failed");
        const double scale =
            std::min(backing.width / options.width, backing.height / options.height);
        const double width = options.width * scale, height = options.height * scale;
        [encoder setViewport:MTLViewport{(backing.width - width) / 2, (backing.height - height) / 2,
                                         width, height, 0, 1}];
        [encoder setRenderPipelineState:display_pipeline];
        [encoder setFragmentTexture:acquired.lease->output_texture() atIndex:0];
        const float exposure = float(ev);
        [encoder setFragmentBytes:&exposure length:sizeof(exposure) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        const auto error = acquired.lease->retain_until_completed(command);
        if (!error.empty())
            throw std::runtime_error(error);
        [command presentDrawable:drawable];
        const auto retained = presentation;
        retained->pending.store(true);
        [command addCompletedHandler:^(id<MTLCommandBuffer> completed_command) {
          if (completed_command.status == MTLCommandBufferStatusError) {
              std::lock_guard lock(retained->mutex);
              retained->error = completed_command.error
                                    ? completed_command.error.localizedDescription.UTF8String
                                    : "Display GPU command failed";
          }
          retained->pending.store(false);
        }];
        [command commit];
        redraw = false;
    }
    void capture() {
        if (!latest_raw || !latest_image)
            return;
        capture_requested = false;
        const auto raw = latest_raw->capture();
        const auto image = latest_image->capture();
        if (raw.status != GeometryStatus::success || image.status != GeometryStatus::success)
            throw std::runtime_error("Capture failed: " + raw.diagnostic + " " + image.diagnostic);
        std::error_code error;
        std::filesystem::create_directories(options.capture_directory, error);
        if (error)
            throw std::runtime_error("Could not create capture directory: " + error.message());
        std::filesystem::path directory;
        const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            directory = options.capture_directory /
                        ("capture-" + std::to_string(stamp) + "-" + std::to_string(::getpid()) +
                         "-" + std::to_string(capture_serial++));
            if (std::filesystem::create_directory(directory, error))
                break;
            if (error || attempt == 99)
                throw std::runtime_error("Could not reserve a fresh capture directory");
        }
        const auto info = latest_image->poll();
        std::vector<std::pair<std::string, std::string>> metadata{
            {"rypath.output", "raw-unexposed-unclamped"},
            {"rypath.scene", options.demo ? "demo" : options.scene.string()},
            {"rypath.sample_index", std::to_string(info.source.sample_index)},
            {"rypath.epoch", std::to_string(info.source.epoch)},
            {"rypath.seed", std::to_string(options.seed)},
            {"rypath.estimator", estimator_name()},
            {"rypath.hero.requested",
             options.hero_wavelengths
                 ? std::string(hero_wavelength_sampling_name(*options.hero_wavelengths))
                 : "default"},
            {"rypath.hero.effective",
             scene.spectral_transport == SpectralTransport::hero
                 ? std::string(hero_wavelength_sampling_name(scene.hero_wavelengths))
                 : "not-applicable"},
            {"rypath.resource_profile",
             std::string(resource_profile_name(options.resource_profile))},
            {"rypath.variance_policy",
             info.mode == ReconstructionMode::temporal
                 ? (options.variance_policy == SpatialVariancePolicy::conservative ? "conservative"
                    : options.variance_policy == SpatialVariancePolicy::center     ? "center"
                                                                                   : "paired")
                 : "not-applicable"},
            {"rypath.area.requested_domain", frame_area_path_domain_name(options.area_path_domain)},
            {"rypath.area.domain", options.estimator == FrameEstimator::area_replay
                                       ? frame_area_path_domain_name(info.source.area_path_domain)
                                       : "not-applicable"},
            {"rypath.area.replay",
             options.estimator == FrameEstimator::area_replay &&
                     frame_area_contribution_family(info.source.area_path_domain)
                 ? frame_contribution_replay_name(info.source.contribution_replay)
                 : "not-applicable"},
            {"rypath.area.spatial_mode", frame_spatial_mode_name(info.source.spatial_mode)},
            {"rypath.area.forward_atlas", std::to_string(info.source.stochastic_forward_atlas)},
            {"rypath.area.canonical_atlas", std::to_string(info.source.stochastic_canonical_atlas)},
            {"rypath.area.requested_cap", std::to_string(options.enhanced_history_cap)},
            {"rypath.area.history_cap_policy",
             frame_history_cap_policy_name(info.source.history_cap_policy)},
            {"rypath.area.maximum_history_cap", std::to_string(info.source.enhanced_history_cap)},
            {"rypath.area.history_cap",
             info.source.history_cap_policy == FrameHistoryCapPolicy::duplication
                 ? "adaptive"
                 : std::to_string(info.source.enhanced_history_cap)},
            {"rypath.area.canonical_law", std::to_string(info.source.canonical_transport_version)},
            {"rypath.area.discovery_law", std::to_string(info.source.fresh_discovery_version)},
            {"rypath.area.requested_mutation", frame_mutation_policy_name(options.mutation_policy)},
            {"rypath.area.requested_stencil", frame_area_stencil_name(options.area_stencil)},
            {"rypath.area.mutation_policy",
             options.estimator == FrameEstimator::area_replay
                 ? frame_mutation_policy_name(info.source.mutation_policy)
                 : "not-applicable"},
            {"rypath.area.stencil", options.estimator == FrameEstimator::area_replay
                                        ? frame_area_stencil_name(info.source.area_stencil)
                                        : "not-applicable"},
            {"rypath.area.stencil_offsets",
             std::to_string(info.source.area_stencil_offsets[0]) + "," +
                 std::to_string(info.source.area_stencil_offsets[1]) + "," +
                 std::to_string(info.source.area_stencil_offsets[2]) + "," +
                 std::to_string(info.source.area_stencil_offsets[3])},
            {"rypath.area.contract",
             std::to_string(options.estimator == FrameEstimator::area_replay ? area_wire::contract
                                                                             : 0)},
            {"rypath.area.output", options.estimator == FrameEstimator::area_replay
                                       ? "conditional-mean"
                                       : "not-applicable"},
            {"rypath.area.target", options.estimator != FrameEstimator::area_replay ? "none"
                                   : info.source.area_path_domain == FrameAreaPathDomain::enhanced
                                       ? "quarter-l1-hybrid-scaled-zero-v4"
                                       : "quarter-l1-v1"},
            {"rypath.area.source_mis",
             options.estimator != FrameEstimator::area_replay ? "none"
             : info.source.area_path_domain == FrameAreaPathDomain::enhanced
                 ? "enhanced-hybrid-confidence-scaled-mis-v4"
                 : "source-target-mis-v1"},
            {"rypath.area.temporal_reuse",
             options.estimator != FrameEstimator::area_replay                       ? "0"
             : info.source.history_cap_policy == FrameHistoryCapPolicy::duplication ? "adaptive"
             : info.source.area_path_domain == FrameAreaPathDomain::enhanced
                 ? std::to_string(info.source.enhanced_history_cap)
                 : "20"},
            {"rypath.mutations", std::to_string(options.estimator == FrameEstimator::area_replay
                                                    ? options.mutation_steps
                                                    : 0)},
            {"rypath.fresh_per_pixel", "1"},
            {"rypath.evaluations", std::to_string(info.source.completed_evaluations)},
            {"rypath.camera_medium", std::to_string(pose.camera_medium.value_or(no_medium))},
            {"rypath.display_ev", std::to_string(ev)},
            {"rypath.film_exposure", std::to_string(scene.exposure)}};
        for (const auto &[name, value] : metadata) {
            (void)value;
            if (name.size() > 31)
                throw std::runtime_error("Capture metadata name exceeds 31 bytes");
        }
        auto film = make_film(raw.width, raw.height, scene.model, 1);
        for (std::size_t i = 0; i < raw.raw_xyz.size(); ++i) {
            for (unsigned c = 0; c < 3; ++c)
                film.pixels[i].sum[c] = raw.raw_xyz[i][c];
            film.pixels[i].count = 1;
        }
        auto failure = write_exr(film, directory / "raw.exr", ExrPrecision::float32, metadata);
        if (!failure.empty())
            throw std::runtime_error(failure);
        const char *image_name = "raw-rgb.exr";
        const char *reconstruction_name = "raw-rgb";
        const char *format = "RGBA16Float";
        switch (info.mode) {
        case ReconstructionMode::temporal:
            metadata[0].second = "temporal-denoised-linear-rgb";
            image_name = "temporal.exr";
            reconstruction_name = "temporal";
            format = "RGBA32Float";
            break;
        case ReconstructionMode::metalfx:
            metadata[0].second = "metalfx-denoised-linear-rgb";
            image_name = "denoised.exr";
            reconstruction_name = "metalfx";
            break;
        case ReconstructionMode::raw_rgb:
            metadata[0].second = "raw-rgb-preview-clamped";
            break;
        }
        metadata.emplace_back("rypath.reconstruction", reconstruction_name);
        metadata.emplace_back("rypath.presentation_format", format);
        failure = write_rgb(directory / image_name, image, metadata);
        if (!failure.empty())
            throw std::runtime_error(failure);
        diagnostic = "Saved " + directory.string();
        std::cout << diagnostic << '\n';
    }
    void tick() {
        if (closing)
            return;
        try {
            const auto now = Clock::now();
            input(std::clamp(std::chrono::duration<double>(now - previous_tick).count(), 0., .1));
            previous_tick = now;
            {
                std::lock_guard lock(presentation->mutex);
                if (!presentation->error.empty())
                    throw std::runtime_error(presentation->error);
            }
            if (pending_image) {
                const auto info = pending_image->poll();
                if (info.status == ReconstructionStatus::failed) {
                    halt(info.diagnostic);
                    return;
                }
                if (info.status == ReconstructionStatus::complete) {
                    latest_image = std::move(pending_image);
                    latest_raw = std::move(pending_raw);
                    pose = submitted_pose;
                    latest_pipeline_seconds =
                        std::chrono::duration<double>(Clock::now() - pipeline_start).count();
                    ++completed;
                    completed_times.push_back(Clock::now());
                    if (completed_times.size() > 32)
                        completed_times.pop_front();
                    redraw = true;
                }
            } else if (pending_raw) {
                const auto info = pending_raw->poll();
                if (info.status == FrameStatus::failed) {
                    halt(info.diagnostic);
                    return;
                }
                if (info.status == FrameStatus::complete) {
                    if (!reconstruction || !stream_mode || *stream_mode != pipeline_mode) {
                        auto built = build_reconstruction_stream(
                            *pending_raw,
                            viewer::reconstruction_configuration(options, pipeline_mode));
                        if (!built.stream) {
                            halt(built.diagnostic);
                            return;
                        }
                        reconstruction = std::move(built.stream);
                        stream_mode = pipeline_mode;
                    }
                    auto result = reconstruction->submit(*pending_raw, reset_submission);
                    if (!result.busy) {
                        if (!result.frame) {
                            halt(result.diagnostic);
                            return;
                        }
                        pending_image = std::move(result.frame);
                        reset_submission = false;
                    }
                }
            }
            if (capture_requested && !pending_raw && !pending_image && latest_image)
                capture();
            present();
            title();
            if (options.frames && completed >= options.frames) {
                if (!presentation->pending.load()) {
                    std::cout << "Viewer completed " << completed << " pipeline frames; " << fps()
                              << " rendered fps\n";
                    close();
                }
                return;
            }
            if (!paused && !pending_raw && !pending_image)
                start();
        } catch (const std::exception &error) {
            halt(error.what());
            title();
        }
    }
};

@interface RypathViewerView : NSView {
  @public
    ViewerState *viewer;
}
@end
@implementation RypathViewerView
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)keyDown:(NSEvent *)event {
    if (!viewer)
        return;
    if ((event.modifierFlags & NSEventModifierFlagCommand) && event.keyCode == 1) {
        viewer->capture_requested = true;
        return;
    }
    if (!event.isARepeat) {
        if (event.keyCode == 53) {
            [self.window performClose:nil];
            return;
        }
        if (event.keyCode == 115) {
            viewer->reset();
            return;
        }
        if (event.keyCode == 15) {
            viewer->toggle();
            return;
        }
    }
    const auto text = event.charactersIgnoringModifiers;
    if ([text isEqualToString:@"+"] || [text isEqualToString:@"="]) {
        viewer->exposure(.5);
        return;
    }
    if ([text isEqualToString:@"-"]) {
        viewer->exposure(-.5);
        return;
    }
    viewer->keys.insert(event.keyCode);
}
- (void)keyUp:(NSEvent *)event {
    if (viewer)
        viewer->keys.erase(event.keyCode);
}
- (void)flagsChanged:(NSEvent *)event {
    if (viewer)
        viewer->fast = (event.modifierFlags & NSEventModifierFlagShift) != 0;
}
- (void)mouseDown:(NSEvent *)event {
    (void)event;
    [self.window makeFirstResponder:self];
}
- (void)mouseDragged:(NSEvent *)event {
    if (viewer) {
        viewer->yaw -= event.deltaX * .004;
        viewer->pitch -= event.deltaY * .004;
    }
}
- (void)rightMouseDragged:(NSEvent *)event {
    [self mouseDragged:event];
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (viewer)
        viewer->redraw = true;
}
@end

@interface RypathViewerDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
  @public
    ViewerState *viewer;
}
@end
@implementation RypathViewerDelegate
- (void)tick:(NSTimer *)timer {
    if (!viewer || viewer->closing) {
        [timer invalidate];
        return;
    }
    viewer->tick();
    if (viewer->closing)
        [timer invalidate];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return NO;
}
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    if (viewer)
        viewer->close();
}
- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    if (viewer) {
        viewer->keys.clear();
        viewer->fast = false;
    }
}
- (void)capture:(id)sender {
    (void)sender;
    if (viewer)
        viewer->capture_requested = true;
}
- (void)reset:(id)sender {
    (void)sender;
    if (viewer)
        viewer->reset();
}
- (void)toggle:(id)sender {
    (void)sender;
    if (viewer)
        viewer->toggle();
}
- (void)quit:(id)sender {
    (void)sender;
    if (viewer)
        viewer->close();
}
- (void)controls:(id)sender {
    (void)sender;
    auto alert = [NSAlert new];
    alert.messageText = NSLocalizedString(@"rypath controls", @"Viewer controls dialog title");
    alert.informativeText = NSLocalizedString(
        @"WASD / QE: move\nShift: move faster\nDrag mouse / arrows: rotate\nHome: authored "
        @"pose\nR: raw RGB / selected denoiser\n+ / −: display exposure EV\nCmd+S: fresh raw + "
        @"reconstructed "
        @"FLOAT EXRs\nEsc: close\n\nOne fresh path per pixel per completed render frame. The title "
        @"reports completed pipeline FPS; display redraws are not render frames.",
        @"Viewer keyboard and mouse controls, with pipeline FPS explanation");
    [alert runModal];
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            viewer::Options options;
            if (argc == 1) {
                options.demo = true; // Finder launches the self-contained demonstration.
            } else if (const auto error = viewer::parse_options(argc, argv, options);
                       !error.empty()) {
                std::cerr << error << '\n' << viewer::usage();
                return 1;
            }
            if (options.help) {
                std::cout << viewer::usage();
                return 0;
            }
            if (options.capture_directory.empty()) {
                const auto pictures =
                    [[NSFileManager defaultManager] URLsForDirectory:NSPicturesDirectory
                                                           inDomains:NSUserDomainMask]
                        .firstObject;
                const auto path = pictures ? pictures.fileSystemRepresentation : nullptr;
                if (!path)
                    throw std::runtime_error("Could not resolve the Pictures capture directory");
                options.capture_directory = std::filesystem::path(path) / "rypath";
            }
            ViewerState state(options, load_scene(options));
            auto app = [NSApplication sharedApplication];
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            auto delegate = [RypathViewerDelegate new];
            delegate->viewer = &state;
            app.delegate = delegate;
            const double width = std::clamp(double(options.width), 640., 1100.);
            const double height = std::clamp(width * options.height / options.width, 360., 750.);
            auto window = [[NSWindow alloc]
                initWithContentRect:NSMakeRect(0, 0, width, height)
                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                    NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                            backing:NSBackingStoreBuffered
                              defer:NO];
            window.delegate = delegate;
            window.releasedWhenClosed = NO;
            window.minSize = NSMakeSize(480, 300);
            window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
            auto view = [[RypathViewerView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
            view->viewer = &state;
            view.wantsLayer = YES;
            auto layer = [CAMetalLayer layer];
            view.layer = layer;
            window.contentView = view;
            state.window = window;
            state.view = view;
            state.layer = layer;
            auto menu = [NSMenu new];
            auto app_item = [NSMenuItem new];
            [menu addItem:app_item];
            auto application_menu = [NSMenu new];
            app_item.submenu = application_menu;
            auto quit = [[NSMenuItem alloc]
                initWithTitle:NSLocalizedString(@"Quit rypath", @"Quit viewer menu item")
                       action:@selector(quit:)
                keyEquivalent:@"q"];
            quit.target = delegate;
            [application_menu addItem:quit];
            auto actions = [NSMenuItem new];
            [menu addItem:actions];
            auto actions_menu = [[NSMenu alloc]
                initWithTitle:NSLocalizedString(@"Actions", @"Viewer actions menu")];
            actions.submenu = actions_menu;
            for (NSMenuItem *item in @[
                     [[NSMenuItem alloc]
                         initWithTitle:NSLocalizedString(@"Save Fresh Capture Pair",
                                                         @"Save raw and reconstructed EXR files")
                                action:@selector(capture:)
                         keyEquivalent:@"s"],
                     [[NSMenuItem alloc]
                         initWithTitle:NSLocalizedString(
                                           @"Reset Authored Camera",
                                           @"Restore the scene's authored camera pose")
                                action:@selector(reset:)
                         keyEquivalent:@""],
                     [[NSMenuItem alloc]
                         initWithTitle:NSLocalizedString(@"Toggle Reconstruction",
                                                         @"Switch between raw RGB and MetalFX")
                                action:@selector(toggle:)
                         keyEquivalent:@"r"],
                     [[NSMenuItem alloc]
                         initWithTitle:NSLocalizedString(@"Controls", @"Show viewer controls")
                                action:@selector(controls:)
                         keyEquivalent:@"?"]
                 ]) {
                item.target = delegate;
                [actions_menu addItem:item];
            }
            auto view_item = [NSMenuItem new];
            [menu addItem:view_item];
            auto view_menu =
                [[NSMenu alloc] initWithTitle:NSLocalizedString(@"View", @"Viewer display menu")];
            view_item.submenu = view_menu;
            auto fullscreen = [[NSMenuItem alloc]
                initWithTitle:NSLocalizedString(@"Toggle Fullscreen", @"Enter or leave fullscreen")
                       action:@selector(toggleFullScreen:)
                keyEquivalent:@"f"];
            fullscreen.keyEquivalentModifierMask =
                NSEventModifierFlagCommand | NSEventModifierFlagControl;
            fullscreen.target = window;
            [view_menu addItem:fullscreen];
            app.mainMenu = menu;
            [window center];
            [window makeKeyAndOrderFront:nil];
            [window makeFirstResponder:view];
            [app finishLaunching];
            [app activate];
            auto timer = [NSTimer timerWithTimeInterval:1. / 120
                                                 target:delegate
                                               selector:@selector(tick:)
                                               userInfo:nil
                                                repeats:YES];
            timer.tolerance = .001;
            [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
            state.title();
            [app run];
            [timer invalidate];
            state.closing = true;
            view->viewer = nullptr;
            delegate->viewer = nullptr;
            window.delegate = nil;
            app.delegate = nil;
            return state.exit_code;
        } catch (const std::exception &error) {
            std::cerr << "rypath viewer: " << error.what() << '\n';
            return 1;
        }
    }
}
