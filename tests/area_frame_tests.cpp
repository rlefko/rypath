// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace rypath;
void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
#if RYPATH_TEST_AREA_METAL
constexpr std::uint64_t high_sample = (1ull << 32) + 29;
FrameConfig config(FrameEstimator estimator = FrameEstimator::area_replay) {
    FrameConfig result;
    result.width = 7;
    result.height = 3;
    result.seed = 19317;
    result.material_seed = -11;
    result.estimator = estimator;
    return result;
}
RenderScene sky(bool packet = false, float radiance = 1) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = packet ? SpectralTransport::packet : SpectralTransport::scalar;
    scene.raster_camera = RasterCamera{{}, {-.3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
    scene.environment.fill(radiance);
    scene.max_depth = 4;
    return scene;
}
FrameCamera camera(const RenderScene &scene) {
    return {.camera = scene.camera,
            .camera_medium = scene.camera_medium,
            .raster_camera = scene.raster_camera,
            .perspective_lens = scene.perspective_lens,
            .texture_camera = scene.texture_camera};
}
void plane(RenderScene &scene, float z, bool positive, std::uint32_t material = 0,
           MediumInterface medium = {}, std::uint32_t emission = no_area_emission) {
    GeometryMesh mesh;
    mesh.positions = {{-16, -16, z}, {16, -16, z}, {16, 16, z}, {-16, 16, z}};
    mesh.triangles = positive ? std::vector<std::array<unsigned, 3>>{{0, 1, 2}, {0, 2, 3}}
                              : std::vector<std::array<unsigned, 3>>{{0, 2, 1}, {0, 3, 2}};
    const auto index = static_cast<unsigned>(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({index, {}});
    scene.instance_materials.push_back(material);
    scene.instance_media.push_back(medium);
    scene.instance_emissions.push_back(emission);
    scene.instance_surface_transforms.emplace_back();
}
RenderScene emitter(bool packet = false, bool glass_lens = false) {
    auto scene = sky(packet, 0);
    SpectralMaterial black;
    black.kind = SpectralMaterialKind::constant;
    scene.materials.push_back(black);
    AreaEmission emission;
    emission.radiance.fill(.7f);
    emission.two_sided = true;
    scene.area_emissions.push_back(emission);
    plane(scene, 5, true, 0, {}, 0);
    if (glass_lens) {
        SpectralMaterial glass;
        glass.kind = SpectralMaterialKind::constant;
        glass.scattering = ScatteringKind::dielectric;
        glass.eta.knots = {{360, 1.65f}, {500, 1.52f}, {830, 1.45f}};
        glass.eta.extend_endpoints = true;
        scene.materials.push_back(glass);
        plane(scene, 2, false, 1);
        plane(scene, 2.25f, true, 1);
        scene.max_depth = 6;
        scene.perspective_lens = PerspectiveLens{.1f, 4, {1, 0, 0}, {0, 1, 0}};
        // Dielectric evaluation consumes local derivative frames even without
        // an image texture. Keep an empty row for non-consuming placements.
        scene.instance_texture_frames.resize(scene.geometry.instances.size());
        for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
            if (!instance_uses_texture_context(scene, i))
                continue;
            const auto &surface = scene.instance_surface_transforms[i];
            const auto diagnostic = prepare_texture_triangle_frames(
                scene.geometry.meshes[scene.geometry.instances[i].mesh_index], surface.shape,
                surface.placement, scene.instance_texture_frames[i]);
            require(diagnostic.empty(), "Dispersive lens fixture frames: " + diagnostic);
        }
    }
    return scene;
}
RenderScene fog(bool boundaries) {
    auto scene = sky();
    scene.volume_transport = true;
    scene.integrator = IntegratorKind::volpath;
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
        for (unsigned slab = 0; slab < 2; ++slab) {
            plane(scene, 1 + float(slab), false, 0, {0, no_medium});
            plane(scene, 1.5f + float(slab), true, 0, {0, no_medium});
        }
    } else
        scene.camera_medium = 0;
    return scene;
}
RenderScene diffuse_lighting(bool indirect) {
    auto scene = sky(true, 0);
    SpectralMaterial receiver;
    receiver.kind = SpectralMaterialKind::constant;
    receiver.constant = .6f;
    scene.materials.push_back(receiver);
    plane(scene, 3, false);
    PointLight light;
    light.position = {.75f, .5f, 1};
    light.intensity.fill(8);
    scene.point_lights.push_back(light);
    scene.max_depth = indirect ? 4 : 1;
    if (indirect) {
        SpectralMaterial back;
        back.kind = SpectralMaterialKind::constant;
        back.constant = .35f;
        scene.materials.push_back(back);
        plane(scene, -1, true, 1);
    }
    return scene;
}
RenderScene film_lens_edge() {
    auto scene = emitter(true);
    scene.perspective_lens = PerspectiveLens{.3f, 4, {1, 0, 0}, {0, 1, 0}};
    scene.filter = {PixelFilterKind::gaussian, 1.5f, 1.5f, .5f};
    plane(scene, 2, false);
    for (auto &point : scene.geometry.meshes.back().positions)
        if (point.x < 0)
            point.x = 0;
    return scene;
}
std::unique_ptr<FrameSession> build(const RenderScene &scene, const FrameConfig &settings) {
    const auto diagnostic = validate_render_scene(scene);
    require(diagnostic.empty(), "Area frame fixture: " + diagnostic);
    auto result = build_frame_session(scene, settings);
    require(result.status == GeometryStatus::success && result.session, result.diagnostic);
    return std::move(result.session);
}
std::unique_ptr<Frame> submit(FrameSession &session, std::uint64_t sample = high_sample,
                              std::uint64_t epoch = 7, std::optional<FrameCamera> snapshot = {}) {
    auto result =
        session.submit({.sample_index = sample, .epoch = epoch, .camera = std::move(snapshot)});
    require(result.status == GeometryStatus::success && result.frame && !result.busy,
            result.diagnostic);
    return std::move(result.frame);
}
struct Completed {
    FrameInfo info;
    FrameCapture image;
};
Completed complete(Frame &frame, const FrameConfig &settings, bool history) {
    const auto info = frame.wait();
    const auto pixels = std::uint64_t(settings.width) * settings.height;
    require(info.status == FrameStatus::complete && info.completed_samples == pixels &&
                info.command_count > 0,
            info.diagnostic.empty() ? "Area frame must finish every destination pixel"
                                    : info.diagnostic);
    require(info.fresh_candidates == pixels,
            "Exactly one fresh candidate per destination, including black paths");
    if (settings.estimator == FrameEstimator::area_replay) {
        require(info.history_used == history && info.temporal_candidates == (history ? 2u : 1u) &&
                    info.spatial_candidates == 5 &&
                    info.completed_evaluations == pixels * (history ? 6 : 5),
                "Area full-replay evaluation and per-pixel pool accounting");
        require(info.history_generation > 0, "Completed area history publication generation");
    } else
        require(!info.history_used && info.completed_evaluations == pixels,
                "Independent baseline evaluates one fresh path per pixel");
    auto image = frame.capture();
    require(image.status == GeometryStatus::success && image.width == settings.width &&
                image.height == settings.height && image.raw_xyz.size() == pixels,
            image.diagnostic);
    for (const auto &xyz : image.raw_xyz)
        for (const auto value : xyz)
            require(std::isfinite(value), "No nonfinite published area XYZ");
    require(frame.poll().status == FrameStatus::complete &&
                frame.wait().area_serial == info.area_serial,
            "Repeated wait/poll preserves immutable completion identity");
    return {info, std::move(image)};
}
std::array<double, 3> mean(const FrameCapture &image) {
    std::array<double, 3> result{};
    for (const auto &xyz : image.raw_xyz)
        for (unsigned c = 0; c < 3; ++c)
            result[c] += xyz[c];
    for (auto &value : result)
        value /= static_cast<double>(image.raw_xyz.size());
    return result;
}
void paired(const FrameCapture &actual, const FrameCapture &expected) {
    require(actual.raw_xyz.size() == expected.raw_xyz.size(), "Paired area dimensions");
    // Same seed, accepted serial and replay/decision payloads. Queue partitions
    // may change only float arithmetic, not the estimator's chosen paths.
    constexpr double relative = 3e-5, absolute = 3e-6;
    for (std::size_t i = 0; i < actual.raw_xyz.size(); ++i)
        for (unsigned c = 0; c < 3; ++c)
            require(std::abs(double(actual.raw_xyz[i][c]) - expected.raw_xyz[i][c]) <=
                        absolute + relative * std::abs(expected.raw_xyz[i][c]),
                    "Queue partition changed paired area estimate");
}
void independent_oracle() {
    const auto scene = emitter(true, true);
    const auto settings = config(FrameEstimator::independent);
    auto session = build(scene, settings);
    auto frame = submit(*session);
    const auto output = complete(*frame, settings, false);
    auto direct =
        build_metal_renderer(scene, {}, {.intersection_profile = settings.intersection_profile});
    require(direct.status == GeometryStatus::success && direct.scene, direct.diagnostic);
    const auto samples = direct.scene->sample({.width = settings.width,
                                               .height = settings.height,
                                               .seed = settings.seed,
                                               .material_seed = settings.material_seed,
                                               .pixel_count = settings.width * settings.height,
                                               .sample_begin = high_sample,
                                               .sample_count = 1,
                                               .total_samples = high_sample + 1});
    require(samples.status == GeometryStatus::success &&
                samples.packet_samples.size() == output.image.raw_xyz.size(),
            samples.diagnostic);
    FrameCapture expected;
    expected.raw_xyz.resize(output.image.raw_xyz.size());
    for (std::size_t i = 0; i < expected.raw_xyz.size(); ++i) {
        const auto &sample = samples.packet_samples[i];
        require(sample.pixel == i && sample.sample == high_sample && sample.splat_count == 0,
                "Independent direct-MC owner identity");
        const auto xyz = packet_sensor_xyz(sample, sample.radiance);
        for (unsigned c = 0; c < 3; ++c)
            expected.raw_xyz[i][c] = static_cast<float>(xyz[c]);
    }
    paired(output.image, expected);
}
void black_and_identity() {
    auto settings = config();
    settings.capacity = 1;
    settings.quantum = 1;
    auto session = build(sky(false, 0), settings);
    auto first = submit(*session);
    std::unique_ptr<Frame> raced_second;
    if (first->poll().status == FrameStatus::pending) {
        auto attempt = session->submit({.sample_index = high_sample, .epoch = 7});
        if (attempt.busy)
            require(!attempt.frame && attempt.status == GeometryStatus::failure,
                    "Pending area frame reports busy without consuming a frame");
        else {
            require(first->poll().status == FrameStatus::complete &&
                        attempt.status == GeometryStatus::success && attempt.frame,
                    "A second area submission can succeed only after preceding publication");
            raced_second = std::move(attempt.frame);
        }
    }
    const auto a = complete(*first, settings, false);
    auto second = raced_second ? std::move(raced_second) : submit(*session);
    const auto b = complete(*second, settings, true);
    require(a.info.sample_index == high_sample && b.info.sample_index == high_sample &&
                b.info.area_serial == a.info.area_serial + 1 &&
                b.info.history_generation == a.info.history_generation + 1,
            "Repeated high sample index uses a unique internal submission serial");
    require(session->submit({.sample_index = high_sample, .epoch = 7}).busy,
            "Two retained area frames still occupy both completion leases");
    for (const auto *image : {&a.image, &b.image})
        for (const auto &xyz : image->raw_xyz)
            require(xyz == std::array<float, 3>{},
                    "Black candidates count without NaN or fabricated energy");
    first.reset();
    auto reset = submit(*session, high_sample, 8);
    const auto c = complete(*reset, settings, false);
    require(c.info.area_serial == b.info.area_serial + 1 &&
                c.info.history_generation == b.info.history_generation + 1,
            "Epoch reset discards imports without resetting global publication identity");
    second.reset();
    auto orphan = submit(*session, high_sample, 8);
    session.reset();
    const auto d = complete(*orphan, settings, true);
    require(d.info.area_serial == c.info.area_serial + 1,
            "Accepted area replay survives immediate session destruction");
    paired(reset->capture(), c.image);
}
void camera_replay() {
    const auto scene = emitter();
    const auto settings = config();
    auto session = build(scene, settings);
    auto first = submit(*session);
    const auto original = complete(*first, settings, false);
    require(mean(original.image)[1] > 0, "Visible constant emitter camera control");
    auto changed = scene;
    if (!changed.raster_camera)
        throw std::runtime_error("Emitter fixture requires a raster camera");
    changed.raster_camera->origin.x += 100;
    auto missed = submit(*session, high_sample, 7, camera(changed));
    const auto black = complete(*missed, settings, true);
    for (const auto &xyz : black.image.raw_xyz)
        require(xyz == std::array<float, 3>{},
                "Imported payloads must be reevaluated in current camera; old XYZ cannot leak");
    paired(first->capture(), original.image);
    first.reset();
    auto restored = submit(*session, high_sample, 7, camera(scene));
    require(mean(complete(*restored, settings, true).image)[1] > 0,
            "Restoring emitter camera retraces paths after black history");
}
void scheduling(const RenderScene &scene) {
    auto narrow = config();
    narrow.capacity = 1;
    narrow.quantum = 1;
    auto wide = config();
    wide.capacity = 17;
    wide.quantum = 8;
    wide.group_width = 64;
    auto a = build(scene, narrow), b = build(scene, wide);
    for (unsigned ordinal = 0; ordinal < 3; ++ordinal) {
        const auto epoch = ordinal < 2 ? 13u : 14u;
        std::optional<FrameCamera> snapshot;
        if (scene.perspective_lens) {
            auto lens_snapshot = camera(scene);
            if (!lens_snapshot.perspective_lens)
                throw std::runtime_error("Lens fixture requires a lens camera snapshot");
            lens_snapshot.perspective_lens->radius = ordinal == 1 ? .2f : .1f;
            lens_snapshot.perspective_lens->focal_distance = ordinal == 1 ? 3.5f : 4.f;
            snapshot = std::move(lens_snapshot);
        }
        auto first = submit(*a, high_sample, epoch, snapshot),
             second = submit(*b, high_sample, epoch, snapshot);
        const bool history = ordinal == 1;
        const auto x = complete(*first, narrow, history), y = complete(*second, wide, history);
        require(x.info.area_serial == y.info.area_serial &&
                    x.info.history_generation == y.info.history_generation,
                "Scheduling cannot change replay/decision serials or publication generations");
        require(x.info.command_count > 1, "Capacity-one area replay must resume across commands");
        paired(x.image, y.image);
    }
}
void failure_preserves_history() {
    auto scene = sky();
    scene.volume_transport = true;
    scene.integrator = IntegratorKind::volpath;
    scene.media.resize(1);
    scene.media[0].sigma_a.fill(.25f);
    scene.media[0].emission.fill(std::numeric_limits<float>::max());
    const auto settings = config();
    auto session = build(scene, settings);
    auto first = submit(*session);
    const auto old = complete(*first, settings, false);
    auto invalid_camera = camera(scene);
    invalid_camera.camera_medium = 1;
    const auto rejected =
        session->submit({.sample_index = high_sample, .epoch = 7, .camera = invalid_camera});
    require(rejected.status == GeometryStatus::failure && !rejected.frame && !rejected.busy,
            "Out-of-range medium rejects before accepting a serial");
    auto active = camera(scene);
    active.camera_medium = 0;
    auto failed = submit(*session, high_sample, 7, active);
    const auto info = failed->wait();
    require(info.status == FrameStatus::failed && !info.diagnostic.empty() &&
                info.area_serial == old.info.area_serial + 1 &&
                info.history_generation == old.info.history_generation,
            "Accepted asynchronous failure consumes serial but preserves old committed history");
    const auto partial = failed->capture();
    require(partial.status == GeometryStatus::failure && partial.raw_xyz.empty(),
            "Failed area replay cannot publish partial capture");
    failed.reset();
    auto recovered = submit(*session, high_sample, 7, camera(scene));
    const auto good = complete(*recovered, settings, true);
    require(good.info.area_serial == info.area_serial + 1 &&
                good.info.history_generation == old.info.history_generation + 1,
            "Recovery imports the last successful history and advances identity exactly once");
    paired(first->capture(), old.image);
    first.reset();
    auto cut_failure = submit(*session, high_sample, 8, active);
    const auto cut_info = cut_failure->wait();
    require(cut_info.status == FrameStatus::failed && !cut_info.history_used &&
                cut_info.history_generation == good.info.history_generation &&
                cut_info.area_serial == good.info.area_serial + 1,
            "Failed new epoch invalidates eligibility without publishing history");
    require(cut_failure->capture().raw_xyz.empty(), "Failed epoch reset has no partial capture");
    cut_failure.reset();
    auto old_epoch = submit(*session, high_sample, 7, camera(scene));
    const auto cold = complete(*old_epoch, settings, false);
    require(cold.info.area_serial == cut_info.area_serial + 1 &&
                cold.info.history_generation == good.info.history_generation + 1,
            "Returning to an earlier epoch must not resurrect discarded history");
}
std::array<double, 3> analytic_constant(float radiance) {
    std::array<double, 3> xyz{};
    for (unsigned wavelength = 360; wavelength <= 830; ++wavelength) {
        const auto cie = pbrt_cie_xyz(wavelength);
        const auto measure = (wavelength == 360 || wavelength == 830) ? .5 : 1.;
        for (unsigned c = 0; c < 3; ++c)
            xyz[c] += radiance * measure * cie[c];
    }
    return xyz;
}
void mean_gate(const std::vector<double> &residuals, double scale, std::string_view label) {
    double sum = 0, square = 0;
    for (const auto residual : residuals) {
        sum += residual;
        square += residual * residual;
    }
    const auto n = static_cast<double>(residuals.size());
    const auto average = sum / n;
    const auto variance = std::max(0., (square - sum * average) / (n - 1));
    // Predeclared six-SE smoke gate across 16 independent session seeds. Pixels
    // and history frames within one seed are correlated and are never counted
    // as independent replicates. This is not a convergence or variance study.
    const auto tolerance = 6 * std::sqrt(variance / n) + 1e-4 * std::abs(scale) + .002;
    std::cout << "AREA_GATE_JSON {\"label\":" << std::quoted(std::string(label))
              << ",\"replicates\":" << residuals.size()
              << ",\"mean_residual\":" << std::setprecision(17) << average
              << ",\"standard_error\":" << std::sqrt(variance / n) << ",\"tolerance\":" << tolerance
              << ",\"pass\":"
              << (std::isfinite(average) && std::abs(average) <= tolerance ? "true" : "false")
              << "}\n";
    require(std::isfinite(average) && std::abs(average) <= tolerance,
            std::string(label) + " mean residual=" + std::to_string(average) +
                " tolerance=" + std::to_string(tolerance));
}
void statistical_means(const RenderScene &scene, float radiance) {
    std::array<std::vector<double>, 3> area_residual, independent_residual, paired_residual;
    const auto expected = analytic_constant(radiance);
    for (unsigned seed = 0; seed < 16; ++seed) {
        auto area = config();
        area.width = 8;
        area.height = 8;
        area.seed = 0x3c1a9675ULL + 0x9e3779b9ULL * seed;
        auto baseline = area;
        baseline.estimator = FrameEstimator::independent;
        auto a = build(scene, area), b = build(scene, baseline);
        auto warmup = submit(*a);
        complete(*warmup, area, false);
        warmup.reset();
        auto reused = submit(*a, high_sample + 1);
        auto x = mean(complete(*reused, area, true).image);
        reused.reset();
        auto third = submit(*a, high_sample + 2);
        const auto z = mean(complete(*third, area, true).image);
        for (unsigned c = 0; c < 3; ++c)
            x[c] = (x[c] + z[c]) / 2;
        auto fresh = submit(*b, high_sample + 1);
        const auto y = mean(complete(*fresh, baseline, false).image);
        for (unsigned c = 0; c < 3; ++c) {
            area_residual[c].push_back(x[c] - expected[c]);
            independent_residual[c].push_back(y[c] - expected[c]);
            paired_residual[c].push_back(x[c] - y[c]);
        }
    }
    for (unsigned c = 0; c < 3; ++c) {
        mean_gate(area_residual[c], expected[c], "Area replay versus analytic constant radiance");
        mean_gate(independent_residual[c], expected[c],
                  "Independent MC versus analytic constant radiance");
        mean_gate(paired_residual[c], expected[c], "Area replay versus independent MC");
    }
}
struct ReferenceMean {
    std::array<double, 3> xyz{};
    std::array<double, 8> column_y{};
};
ReferenceMean capture_mean(const FrameCapture &image) {
    ReferenceMean result;
    require(image.width == 8 && image.height == 8, "Mean-oracle dimensions");
    result.xyz = mean(image);
    for (std::size_t pixel = 0; pixel < image.raw_xyz.size(); ++pixel)
        result.column_y[pixel % 8] += image.raw_xyz[pixel][1] / 8.;
    return result;
}
ReferenceMean raw_mean(MetalRenderScene &source, const RenderScene &scene, std::uint64_t seed) {
    ReferenceMean result;
    // Independent raw MC uses eight ordinary absolute samples per pixel. All
    // these fixtures have no image textures, so the legacy footprint budget is inert.
    for (unsigned sample = 0; sample < 8; ++sample) {
        const auto samples = source.sample({.width = 8,
                                            .height = 8,
                                            .seed = seed,
                                            .material_seed = -11,
                                            .pixel_count = 64,
                                            .sample_begin = high_sample + 101 + sample,
                                            .sample_count = 1,
                                            .total_samples = high_sample + 109});
        require(samples.status == GeometryStatus::success, samples.diagnostic);
        const bool packet = scene.spectral_transport == SpectralTransport::packet;
        require(packet ? samples.packet_samples.size() == 64 && samples.samples.empty()
                       : samples.samples.size() == 64 && samples.packet_samples.empty(),
                "Raw reference owns every pixel exactly once per absolute sample");
        for (unsigned pixel = 0; pixel < 64; ++pixel) {
            std::array<double, 3> xyz{};
            if (packet) {
                const auto &owner = samples.packet_samples[pixel];
                require(owner.pixel == pixel && owner.sample == high_sample + 101 + sample &&
                            owner.splat_count == 0,
                        "Raw packet reference identity");
                xyz = packet_sensor_xyz(owner, owner.radiance);
            } else {
                const auto &owner = samples.samples[pixel];
                require(owner.pixel == pixel && owner.sample == high_sample + 101 + sample,
                        "Raw scalar reference identity");
                xyz = pbrt_cie_xyz(owner.wavelength);
                for (auto &value : xyz)
                    value *= owner.radiance * 470.;
            }
            for (unsigned c = 0; c < 3; ++c) {
                require(std::isfinite(xyz[c]), "Raw reference finite sensor contribution");
                result.xyz[c] += xyz[c] / (64 * 8.);
            }
            result.column_y[pixel % 8] += xyz[1] / (8 * 8.);
        }
    }
    return result;
}
std::unique_ptr<MetalRenderScene> raw_source(const RenderScene &scene) {
    const auto diagnostic = validate_render_scene(scene);
    require(diagnostic.empty(), "Raw mean fixture: " + diagnostic);
    auto built =
        build_metal_renderer(scene, {}, {.intersection_profile = IntersectionProfile::native});
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    return std::move(built.scene);
}
template <typename Values> void diagnostic_array(const Values &values) {
    std::cout << '[';
    bool first = true;
    for (const auto value : values) {
        if (!first)
            std::cout << ',';
        std::cout << value;
        first = false;
    }
    std::cout << ']';
}
enum class MeanControl { none, point_nee, indirect_bounce, film_lens_edge };
void independent_means(const RenderScene &scene, std::string_view label,
                       MeanControl mode = MeanControl::none) {
    const bool nee = mode == MeanControl::point_nee;
    const bool indirect = mode == MeanControl::indirect_bounce;
    const bool edge = mode == MeanControl::film_lens_edge;
    auto reference = raw_source(scene);
    std::unique_ptr<MetalRenderScene> control;
    RenderScene control_scene;
    if (indirect) {
        control_scene = scene;
        control_scene.max_depth = 1;
        control = raw_source(control_scene);
    } else if (nee) {
        control_scene = scene;
        control_scene.point_lights.clear();
        control = raw_source(control_scene);
    }
    std::array<std::vector<double>, 3> residual, analytic_area, analytic_raw;
    std::vector<double> edge_residual;
    std::array<double, 3> reference_scale{};
    const auto edge_analytic = analytic_constant(.35f);
    for (unsigned seed = 0; seed < 16; ++seed) {
        auto settings = config();
        settings.width = settings.height = 8;
        settings.seed = 0x173af251ULL + 0x9e3779b97f4a7c15ULL * seed;
        // Disjoint seed families and separate native owners: no replay kernel
        // supplies either the reference values or its error estimate.
        const auto reference_seed = 0xc7b61a93ULL + 0xd1b54a32d192ed03ULL * seed;
        const auto raw = raw_mean(*reference, scene, reference_seed);
        if (control && (indirect || seed == 0)) {
            const auto ablated = raw_mean(*control, control_scene, reference_seed);
            if (indirect)
                require(raw.xyz[1] > ablated.xyz[1],
                        "Lit bounce witness: deeper raw transport adds positive indirect energy");
            else
                require(
                    ablated.xyz == std::array<double, 3>{} && raw.xyz[1] > 0,
                    "Point NEE witness: positive light response and exact black light-off control");
        }
        auto session = build(scene, settings);
        auto cold = submit(*session);
        const auto cold_output = complete(*cold, settings, false);
        const auto cold_info = cold_output.info;
        const auto cold_mean = capture_mean(cold_output.image);
        cold.reset();
        auto warm = submit(*session, high_sample + 1);
        const auto first = complete(*warm, settings, true);
        const auto x = capture_mean(first.image);
        warm.reset();
        auto again = submit(*session, high_sample + 2);
        const auto second = complete(*again, settings, true);
        const auto y = capture_mean(second.image);
        require(first.info.history_generation == cold_info.history_generation + 1 &&
                    second.info.history_generation == first.info.history_generation + 1,
                "Mean study executes consecutive cold/warm/warm history publications");
        for (unsigned c = 0; c < 3; ++c) {
            const auto area = (x.xyz[c] + y.xyz[c]) / 2;
            residual[c].push_back(area - raw.xyz[c]);
            reference_scale[c] += raw.xyz[c] / 16;
            if (edge) {
                analytic_area[c].push_back(area - edge_analytic[c]);
                analytic_raw[c].push_back(raw.xyz[c] - edge_analytic[c]);
            }
        }
        std::cout << "AREA_MEAN_JSON {\"case\":" << std::quoted(std::string(label))
                  << ",\"seed_index\":" << seed << ",\"area_seed\":" << settings.seed
                  << ",\"reference_seed\":" << reference_seed
                  << ",\"target_scale\":" << std::setprecision(17) << settings.target_scale
                  << ",\"cold\":";
        diagnostic_array(cold_mean.xyz);
        std::cout << ",\"warm1\":";
        diagnostic_array(x.xyz);
        std::cout << ",\"warm2\":";
        diagnostic_array(y.xyz);
        std::cout << ",\"raw\":";
        diagnostic_array(raw.xyz);
        if (edge) {
            std::cout << ",\"analytic\":";
            diagnostic_array(edge_analytic);
            std::cout << ",\"columns_cold_y\":";
            diagnostic_array(cold_mean.column_y);
            std::cout << ",\"columns_warm1_y\":";
            diagnostic_array(x.column_y);
            std::cout << ",\"columns_warm2_y\":";
            diagnostic_array(y.column_y);
            std::cout << ",\"columns_raw_y\":";
            diagnostic_array(raw.column_y);
        }
        std::cout << "}\n";
        if (edge) {
            require(raw.column_y[0] > 0 && raw.column_y[7] == 0 && raw.column_y[4] > 0,
                    "Gaussian/DOF edge witness includes lit, black and partially visible columns");
            edge_residual.push_back((x.column_y[4] + y.column_y[4]) / 2 - raw.column_y[4]);
        }
    }
    for (unsigned c = 0; c < 3; ++c) {
        mean_gate(residual[c], reference_scale[c],
                  std::string(label) + " area versus independent raw MC channel " +
                      std::to_string(c));
        if (edge) {
            mean_gate(analytic_area[c], edge_analytic[c],
                      "DOF/Gaussian area analytic half-emitter mean");
            mean_gate(analytic_raw[c], edge_analytic[c],
                      "DOF/Gaussian raw analytic half-emitter mean");
        }
    }
    if (edge)
        mean_gate(edge_residual, reference_scale[1], "DOF/Gaussian right-center column indicator");
}
void rejected_configs() {
    for (const float floor :
         {0.f, -1.f, std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::denorm_min()}) {
        auto settings = config();
        settings.target_scale = floor;
        const auto result = build_frame_session(sky(), settings);
        require(result.status == GeometryStatus::failure && !result.session &&
                    !result.diagnostic.empty(),
                "Invalid area support floor rejected");
    }
    auto settings = config();
    settings.execution = FrameExecution::megakernel;
    const auto result = build_frame_session(sky(), settings);
    require(result.status == GeometryStatus::failure && !result.session,
            "Area replay requires wavefront execution");
}
#endif
} // namespace
int main() {
    try {
#if RYPATH_TEST_AREA_METAL
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
        require(device.device.compute_ray_tracing, "Area replay tests require compute ray tracing");
        rejected_configs();
        independent_oracle();
        black_and_identity();
        camera_replay();
        scheduling(emitter(true, true));
        scheduling(fog(false));
        scheduling(fog(true));
        failure_preserves_history();
        statistical_means(sky(false), 1);
        statistical_means(emitter(true), .7f);
        independent_means(diffuse_lighting(false), "Point NEE", MeanControl::point_nee);
        independent_means(diffuse_lighting(true), "Lit diffuse indirect bounce",
                          MeanControl::indirect_bounce);
        // Keep the original punctual-light volume fixtures in scheduling().
        // Statistical volume gates use bounded medium-source/emission scenes;
        // point-light near-source singularities need separate tail qualification.
        auto bounded_fog = fog(false);
        bounded_fog.point_lights.clear();
        independent_means(bounded_fog, "Geometry-free bounded scattering/emission");
        auto bounded_slabs = fog(true);
        bounded_slabs.point_lights.clear();
        independent_means(bounded_slabs, "Bounded nonzero null-boundary slabs");
        independent_means(film_lens_edge(), "Gaussian film and defocus edge",
                          MeanControl::film_lens_edge);
#else
        const auto result = build_frame_session(
            {}, {.width = 1, .height = 1, .estimator = FrameEstimator::area_replay});
        require(result.status == GeometryStatus::not_built && !result.session &&
                    !result.diagnostic.empty(),
                "Portable area-replay availability contract");
#endif
        std::cout << "Area replay frame validation passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
