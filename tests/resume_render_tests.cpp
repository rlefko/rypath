// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "render_driver.hpp"

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
bool exact(const Film &a, const Film &b) {
    if (a.width != b.width || a.height != b.height || a.model != b.model ||
        a.exposure != b.exposure || a.pixels.size() != b.pixels.size())
        return false;
    for (std::size_t p = 0; p < a.pixels.size(); ++p) {
        if (a.pixels[p].count != b.pixels[p].count)
            return false;
        for (std::size_t c = 0; c < 3; ++c)
            if (std::bit_cast<std::uint64_t>(a.pixels[p].sum[c]) !=
                    std::bit_cast<std::uint64_t>(b.pixels[p].sum[c]) ||
                std::bit_cast<std::uint64_t>(a.pixels[p].correction[c]) !=
                    std::bit_cast<std::uint64_t>(b.pixels[p].correction[c]))
                return false;
    }
    return true;
}
struct Synthetic {
    int builds = 0;
    int calls = 0;
    bool cancel = false;
    int fail_call = 0;
    bool throw_sample = false;
    bool fail_build = false;
    std::uint64_t target = 0;
    std::uint64_t first = UINT64_MAX;
    detail::RenderSampleSource source() {
        return {[this] {
                    ++builds;
                    RenderBuildResult built;
                    built.status = fail_build ? GeometryStatus::failure : GeometryStatus::success;
                    built.diagnostic = fail_build ? "injected build failure" : "";
                    built.device.name = "synthetic";
                    return built;
                },
                [this](const RenderRequest &request) {
                    ++calls;
                    first = std::min(first, request.sample_begin);
                    require(request.total_samples == target, "target budget changed");
                    cancel = true;
                    if (throw_sample)
                        throw std::runtime_error("injected sample exception");
                    RenderSamplesResult tile;
                    const bool fail = calls == fail_call;
                    tile.status = fail ? GeometryStatus::failure : GeometryStatus::success;
                    tile.diagnostic = fail ? "injected sample failure" : "";
                    for (std::uint64_t s = request.sample_begin;
                         s < request.sample_begin + request.sample_count; ++s)
                        for (auto p = request.pixel_begin;
                             p < request.pixel_begin + request.pixel_count; ++p)
                            tile.samples.push_back({p, s, 400.f + float((s + p) % 350),
                                                    float(1 + (s * 13 + p) % 99) / 17});
                    return tile;
                }};
    }
};
void portable() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    RenderSettings settings{2, 2, 7, 3, 17};
    Synthetic all;
    all.target = settings.samples;
    auto whole = detail::drive_progressive(scene, settings, {}, all.source());
    require(whole.completion == RenderCompletion::complete, whole.diagnostic);
    for (const auto stop : {0u, 3u, 6u, 7u}) {
        Synthetic prefix;
        prefix.target = settings.samples;
        std::uint64_t completed = 0;
        RenderControl control;
        control.cancelled = [&] { return completed >= stop; };
        control.progress = [&](auto count) { completed = count; };
        auto partial = detail::drive_progressive(scene, settings, control, prefix.source());
        require(partial.status == GeometryStatus::success, partial.diagnostic);
        require(partial.film.pixels.front().count == stop, "wrong cancellation boundary");
        Synthetic rest;
        rest.target = settings.samples;
        RenderControl resume;
        resume.initial_film = &partial.film;
        int checkpoints = 0;
        resume.committed = [&](const Film &, const MetalDeviceInfo &) {
            ++checkpoints;
            return std::string{};
        };
        auto resumed = detail::drive_progressive(scene, settings, resume, rest.source());
        require(resumed.completion == RenderCompletion::complete && exact(whole.film, resumed.film),
                "resume changed raw Film bits");
        require(checkpoints > 0, "missing checkpoint callback");
        require(stop == 7 ? rest.builds == 0 : rest.first == stop, "wrong resume start");
    }
    // Cancellation raised by the sample provider cannot interrupt a multi-tile batch.
    settings.width = 4097;
    Synthetic during;
    during.target = settings.samples;
    RenderControl control;
    control.cancelled = [&] { return during.cancel; };
    auto partial = detail::drive_progressive(scene, settings, control, during.source());
    require(partial.completion == RenderCompletion::cancelled && during.calls > 1,
            "cancel during batch did not finish all tiles");
    for (const auto &pixel : partial.film.pixels)
        require(pixel.count == 3, "cancel selectively dropped samples");
    for (const auto fault : {0, 1, 2}) {
        Synthetic source;
        source.target = settings.samples;
        source.fail_call = fault == 0 ? 2 : 0;
        source.throw_sample = fault == 1;
        source.fail_build = fault == 2;
        control.cancelled = [&] { return source.cancel; };
        auto failed = detail::drive_progressive(scene, settings, control, source.source());
        require(
            failed.completion == RenderCompletion::failed &&
                failed.status == GeometryStatus::failure &&
                (fault == 2 ? failed.film.pixels.empty() : failed.film.pixels.front().count == 0),
            "error lost precedence or committed partial batch");
    }
    // Fresh unavailable jobs preserve the original empty-Film API contract;
    // resumed jobs retain their exact committed snapshot for recovery.
    for (const auto unavailable :
         {GeometryStatus::not_built, GeometryStatus::no_device, GeometryStatus::no_ray_tracing}) {
        detail::RenderSampleSource source;
        source.prepare = [=] {
            RenderBuildResult built;
            built.status = unavailable;
            return built;
        };
        auto fresh = detail::drive_progressive(scene, settings, {}, source);
        require(fresh.status == unavailable && fresh.film.pixels.empty() &&
                    fresh.completion == RenderCompletion::failed,
                "fresh unavailable render retained a Film");
        RenderControl restored;
        restored.initial_film = &partial.film;
        auto unavailable_resume = detail::drive_progressive(scene, settings, restored, source);
        require(unavailable_resume.status == unavailable &&
                    exact(unavailable_resume.film, partial.film),
                "unavailable resumed render lost its committed Film");
    }
    // Completion wins over cancellation raised during the final batch.
    settings = {2, 2, 3, 3, 17};
    Synthetic final;
    final.target = settings.samples;
    control = {};
    control.cancelled = [&] { return final.cancel; };
    auto finished = detail::drive_progressive(scene, settings, control, final.source());
    require(finished.completion == RenderCompletion::complete, "cancellation beat completion");
    settings = {2, 2, 7, 3, 17};
    auto initial = make_film(2, 2, scene.model);
    for (int invalid = 0; invalid < 9; ++invalid) {
        auto film = initial;
        switch (invalid) {
        case 0:
            film.width = 1;
            break;
        case 1:
            film.pixels.pop_back();
            break;
        case 2:
            film.model = RenderModel::demo;
            break;
        case 3:
            film.pixels[0].count = 1;
            break;
        case 4:
            for (auto &p : film.pixels)
                p.count = 8;
            break;
        case 5:
            film.pixels[0].sum[0] = 1;
            break;
        case 6:
            film.pixels[0].correction[0] = std::numeric_limits<double>::infinity();
            break;
        case 7:
            film.pixels[0].sum[0] = std::numeric_limits<double>::quiet_NaN();
            break;
        case 8:
            for (auto &p : film.pixels)
                p.count = 1;
            film.pixels[0].sum[0] = std::numeric_limits<double>::max();
            film.pixels[0].correction[0] = std::numeric_limits<double>::max();
            break;
        default:
            throw std::runtime_error("unknown invalid Film case");
        }
        Synthetic source;
        control = {};
        control.initial_film = &film;
        auto failed = detail::drive_progressive(scene, settings, control, source.source());
        require(failed.completion == RenderCompletion::failed && source.builds == 0,
                "invalid Film reached GPU setup");
    }
    Synthetic invalid_sampler;
    auto invalid_settings = settings;
    invalid_settings.sampler.kind = static_cast<SamplerKind>(99);
    auto invalid_range =
        detail::drive_progressive(scene, invalid_settings, {}, invalid_sampler.source());
    require(invalid_range.completion == RenderCompletion::failed && invalid_sampler.builds == 0,
            "invalid sampler reached GPU setup");
    Synthetic before;
    control = {};
    control.cancelled = [] { return true; };
    int saved = 0;
    control.committed = [&](const Film &film, const MetalDeviceInfo &device) {
        require(film.pixels.front().count == 0 && device.name.empty(), "pre-work checkpoint");
        ++saved;
        return std::string{};
    };
    auto cancelled = detail::drive_progressive(scene, settings, control, before.source());
    require(cancelled.completion == RenderCompletion::cancelled && before.builds == 0 && saved == 1,
            "pre-work cancellation failed");
    control.committed = [](const Film &, const MetalDeviceInfo &) {
        return "pre-work persistence failure";
    };
    auto prework_failure = detail::drive_progressive(scene, settings, control, before.source());
    require(prework_failure.completion == RenderCompletion::failed,
            "pre-work persistence failure lost to cancellation");
    Synthetic initial_failure;
    control = {};
    control.committed = [&](const Film &, const MetalDeviceInfo &device) {
        require(device.name == "synthetic", "initial callback ran before GPU setup");
        return "initial checkpoint failure";
    };
    auto initial_failed =
        detail::drive_progressive(scene, settings, control, initial_failure.source());
    require(initial_failed.completion == RenderCompletion::failed && initial_failure.calls == 0,
            "initial checkpoint failure still sampled");
    Synthetic persist;
    persist.target = settings.samples;
    control = {};
    control.committed = [](const Film &film, const MetalDeviceInfo &) {
        return film.pixels.front().count ? "injected persistence failure" : "";
    };
    auto failed = detail::drive_progressive(scene, settings, control, persist.source());
    require(failed.completion == RenderCompletion::failed && failed.film.pixels.front().count == 3,
            "persistence failure discarded committed batch");
    scene.exposure = 2;
    auto complete = whole.film;
    control = {};
    control.initial_film = &complete;
    control.cancelled = [] {
        throw std::runtime_error("completed render polled cancellation");
        return true;
    };
    int complete_saved = 0;
    control.committed = [&](const Film &, const MetalDeviceInfo &device) {
        require(device.name.empty(), "completed resume fabricated a GPU device");
        ++complete_saved;
        return std::string{};
    };
    auto rebased = detail::drive_progressive(scene, settings, control, before.source());
    complete.exposure = 2;
    require(rebased.completion == RenderCompletion::complete && exact(complete, rebased.film) &&
                complete_saved == 1 && before.builds == 0,
            "completed resume or exposure restoration failed");
    std::cout
        << "Portable resume: exact raw bits, atomic cancellation and failure precedence passed\n";
}
int native() {
    const auto probe = build_metal_renderer(make_demo_scene());
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
    if (probe.status == GeometryStatus::no_device) {
        std::cout << "SKIP resume GPU: " << probe.diagnostic << '\n';
        return 77;
    }
#endif
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
    if (probe.status == GeometryStatus::no_ray_tracing) {
        std::cout << "SKIP resume GPU: " << probe.diagnostic << '\n';
        return 77;
    }
#endif
    require(probe.status == GeometryStatus::success, probe.diagnostic);
    const auto check_resume = [](const RenderScene &scene) {
        for (const auto sampler : {SamplerKind::independent, SamplerKind::sobol}) {
            RenderSettings settings{8, 8, 17, 6, 37};
            settings.sampler.kind = sampler;
            auto whole = render_progressive(scene, settings);
            require(whole.completion == RenderCompletion::complete, whole.diagnostic);
            if (!scene.point_lights.empty() || !scene.spot_lights.empty() ||
                !scene.distant_lights.empty() || !scene.goniometric_lights.empty() ||
                !scene.projection_lights.empty()) {
                bool positive = false;
                for (const auto &pixel : whole.film.pixels)
                    positive = positive || pixel.sum[1] + pixel.correction[1] > 0;
                require(positive, "light resume fixture has no positive signal");
            }
            std::uint64_t count = 0;
            RenderControl control;
            control.progress = [&](auto n) { count = n; };
            control.cancelled = [&] { return count >= 6; };
            auto partial = render_progressive(scene, settings, control);
            require(partial.completion == RenderCompletion::cancelled, partial.diagnostic);
            require(count == 6 && partial.film.pixels.front().count == 6,
                    "native cancellation did not commit the expected point in the sequence");
            control = {};
            control.initial_film = &partial.film;
            settings.batch_size = 4;
            auto resumed = render_progressive(scene, settings, control);
            require(resumed.completion == RenderCompletion::complete &&
                        exact(whole.film, resumed.film),
                    "native uninterrupted/resumed Film bits differ");
        }
    };
    for (const auto fixture :
         {"image-secondary.pbrt", "media-reference/isotropic.pbrt", "point-lights/mixed.pbrt",
          "point-lights/volume.pbrt", "spot-lights/mixed.pbrt", "spot-lights/volume.pbrt",
          "goniometric-lights/mixed-lighting.pbrt", "goniometric-lights/bounded-volume.pbrt",
          "goniometric-lights/geometry-free-volume.pbrt", "projection-lights/mixed-lighting.pbrt",
          "projection-lights/bounded-volume.pbrt", "projection-lights/geometry-free-volume.pbrt"}) {
        std::cout << "Resume fixture: " << fixture << std::endl;
        auto path =
            std::filesystem::path(RYPATH_SOURCE_DIR) / "tests/fixtures/pbrt-render" / fixture;
        auto checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
        if (!translated.scene)
            throw std::runtime_error("native fixture translation failed");
        if (std::string_view(fixture).starts_with("point-lights/"))
            require(!translated.scene->point_lights.empty(), "point fixture lost its light");
        if (std::string_view(fixture).starts_with("spot-lights/"))
            require(!translated.scene->spot_lights.empty(), "spot fixture lost its light");
        if (std::string_view(fixture).starts_with("goniometric-lights/"))
            require(!translated.scene->goniometric_lights.empty(),
                    "goniometric fixture lost its light");
        if (std::string_view(fixture).starts_with("projection-lights/"))
            require(!translated.scene->projection_lights.empty(),
                    "projection fixture lost its light");
        check_resume(*translated.scene);
    }
    // Empty geometry takes a distinct kernel. A scattering camera medium must
    // retain point contributions through cancellation and a changed batch size.
    RenderScene empty;
    empty.model = RenderModel::pbrt;
    empty.raster_camera = RasterCamera{{}, {-.1f, -.1f, 1}, {.2f, 0, 0}, {0, .2f, 0}};
    empty.volume_transport = true;
    empty.camera_medium = 0;
    empty.max_depth = 3;
    HomogeneousMedium fog;
    fog.sigma_a.fill(.3f);
    fog.sigma_s.fill(.7f);
    fog.g = .65f;
    empty.media.push_back(fog);
    PointLight light;
    light.position = {1, 0, 0};
    light.intensity.fill(1);
    empty.point_lights.push_back(light);
    check_resume(empty);
    empty.point_lights.clear();
    SpotLight spot;
    spot.position = light.position;
    spot.intensity = light.intensity;
    spot.cos_falloff_end = -1;
    spot.cos_falloff_start = .5f;
    empty.spot_lights.push_back(spot);
    check_resume(empty);
    // Distant volume visibility needs the finite scene-radius policy. Off-axis
    // geometry exercises the geometry kernel without blocking the camera rays.
    empty.spot_lights.clear();
    GeometryMesh off_axis;
    off_axis.positions = {{2, 0, 1}, {3, 0, 1}, {2, 1, 1}};
    off_axis.triangles = {{0, 1, 2}};
    empty.geometry.meshes.push_back(std::move(off_axis));
    empty.geometry.instances.push_back({0, {}});
    SpectralMaterial distant_geometry;
    distant_geometry.kind = SpectralMaterialKind::constant;
    distant_geometry.constant = .5f;
    empty.materials.push_back(distant_geometry);
    empty.instance_materials.push_back(0);
    DistantLight distant;
    distant.direction = {0, 0, -1};
    distant.radiance.fill(1);
    empty.distant_lights.push_back(distant);
    empty.distant_scene_radius = 4;
    check_resume(empty);
    std::cout << "Native resume: independent/Sobol textured, volume and all positional/distant raw "
                 "Film bits "
                 "match\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    try {
        require(argc == 2, "expected --portable or --native");
        if (std::string_view(argv[1]) == "--native")
            return native();
        require(std::string_view(argv[1]) == "--portable", "unknown test mode");
        portable();
    } catch (const std::exception &error) {
        std::cerr << "Resume render test: " << error.what() << '\n';
        return 1;
    }
}
