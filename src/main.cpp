// SPDX-License-Identifier: GPL-3.0-only

#include "gltf.hpp"
#include "image_output.hpp"
#include "metal_backend.hpp"
#include "metal_render.hpp"
#include "obj.hpp"
#include "pbrt.hpp"
#include "pbrt_render.hpp"
#include "render_job.hpp"
#include "render_timings.hpp"
#include "resource_limits.hpp"
#include "stl.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <csignal>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace {

std::string_view status_name(rypath::MetalStatus status) {
    switch (status) {
    case rypath::MetalStatus::success:
        return "success";
    case rypath::MetalStatus::not_built:
        return "not_built";
    case rypath::MetalStatus::no_device:
        return "no_device";
    case rypath::MetalStatus::failure:
        return "failure";
    }
    return "failure";
}

const char *yes_no(bool value) { return value ? "yes" : "no"; }

int report_metal(bool smoke_test) {
    const auto result = smoke_test ? rypath::run_metal_smoke_test() : rypath::query_metal_device();
    std::cout << "Metal status: " << status_name(result.status) << '\n';
    if (result.status == rypath::MetalStatus::failure) {
        std::cerr << "rypath: " << result.diagnostic << '\n';
        return 1;
    }
    if (result.status != rypath::MetalStatus::success) {
        std::cout << "Reason: " << result.diagnostic << '\n';
        return smoke_test ? 2 : 0;
    }

    const auto &device = result.device;
    std::cout << "Device selection: system default\n"
              << "Device: " << device.name << '\n'
              << "Registry ID: " << device.registry_id << '\n'
              << "Architecture: " << device.architecture << '\n'
              << "Unified memory: " << yes_no(device.unified_memory) << '\n'
              << "Ray tracing API (compute): " << yes_no(device.compute_ray_tracing) << '\n'
              << "Ray tracing API (render): " << yes_no(device.render_ray_tracing) << '\n'
              << "Hardware ray tracing execution: not verified\n";
    if (smoke_test) {
        std::cout << "Dispatches verified: " << result.dispatch_count << '\n'
                  << "Elements verified: " << result.element_count << '\n'
                  << "Guard elements verified: " << result.guard_count << '\n';
    }
    return 0;
}

int argument_error() {
    std::cerr << "rypath: unsupported arguments\n"
                 "Try 'rypath --help' for usage.\n";
    return 1;
}

int render_error(std::string_view diagnostic, int status = 1) {
    std::cerr << "rypath: " << diagnostic << '\n';
    return status;
}

struct ObjCommand {
    std::filesystem::path source;
    std::filesystem::path output;
};
int import_obj(const ObjCommand &command) {
    const auto loaded = rypath::obj::load(command.source);
    std::cout << rypath::obj::report(loaded);
    if (loaded.exit_code() != 0 || !loaded.asset)
        return loaded.exit_code() == 0 ? 1 : loaded.exit_code();
    auto conversion = rypath::obj::prepare_materials(*loaded.asset);
    // Loader warnings belong to the published provenance as well as preflight output.
    conversion.diagnostics.insert(conversion.diagnostics.begin(), loaded.diagnostics.begin(),
                                  loaded.diagnostics.end());
    auto status = conversion.exit_code();
    if (status == 0 && !command.output.empty())
        status = rypath::obj::publish(*loaded.asset, conversion, command.output);
    for (std::size_t i = loaded.diagnostics.size(); i < conversion.diagnostics.size(); ++i)
        std::cout << rypath::pbrt::format_diagnostic(conversion.diagnostics[i]) << '\n';
    std::cout << "OBJ conversion: " << (status == 0 ? "supported" : "rejected") << '\n';
    if (status == 0 && !command.output.empty())
        std::cout << "Wrote OBJ bundle: " << command.output.string() << '\n';
    return status;
}

int import_gltf(int argc, char *argv[]) {
    const bool convert = std::string_view(argv[1]) == "--convert-gltf";
    if (argc < 3 || std::string_view(argv[2]).empty() ||
        std::string_view(argv[2]).starts_with("--"))
        return argument_error();
    rypath::gltf::Options options;
    std::filesystem::path output;
    bool output_seen = false;
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc || std::string_view(argv[i + 1]).empty() ||
            std::string_view(argv[i + 1]).starts_with("--"))
            return argument_error();
        const std::string_view key(argv[i]), value(argv[i + 1]);
        if (key == "--output" && convert && !output_seen) {
            output = value;
            output_seen = true;
        } else if (key == "--gltf-scene" && !options.scene) {
            std::size_t index = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return argument_error();
            options.scene = index;
        } else
            return argument_error();
    }
    if (convert && !output_seen)
        return argument_error();
    const auto loaded = rypath::gltf::load(argv[2], options);
    std::cout << rypath::gltf::report(loaded);
    if (loaded.exit_code() != 0 || !loaded.asset)
        return loaded.exit_code() == 0 ? 1 : loaded.exit_code();
    auto conversion = rypath::gltf::prepare(*loaded.asset, loaded.diagnostics);
    auto status = conversion.exit_code();
    if (status == 0 && convert)
        status = rypath::gltf::publish(*loaded.asset, conversion, output);
    for (std::size_t i = loaded.diagnostics.size(); i < conversion.diagnostics.size(); ++i)
        std::cout << rypath::pbrt::format_diagnostic(conversion.diagnostics[i]) << '\n';
    std::cout << "glTF conversion: " << (status == 0 ? "supported" : "rejected") << '\n';
    if (status == 0 && convert)
        std::cout << "Wrote glTF bundle: " << output.string() << '\n';
    return status;
}

int import_stl(int argc, char *argv[]) {
    const bool convert = std::string_view{argv[1]} == "--convert-stl";
    if (argc < 3 || std::string_view{argv[2]}.empty() ||
        std::string_view{argv[2]}.starts_with("--"))
        return argument_error();
    rypath::stl::Options options;
    std::filesystem::path output;
    bool colors_seen = false, output_seen = false;
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc || std::string_view{argv[i + 1]}.empty() ||
            std::string_view{argv[i + 1]}.starts_with("--"))
            return argument_error();
        const std::string_view key{argv[i]}, value{argv[i + 1]};
        if (key == "--output" && convert && !output_seen) {
            output = value;
            output_seen = true;
        } else if (key == "--stl-colors" && !colors_seen) {
            if (value == "auto")
                options.colors = rypath::stl::ColorMode::automatic;
            else if (value == "viscam")
                options.colors = rypath::stl::ColorMode::viscam;
            else if (value == "magics")
                options.colors = rypath::stl::ColorMode::magics;
            else
                return argument_error();
            colors_seen = true;
        } else {
            return argument_error();
        }
    }
    if (convert && !output_seen)
        return argument_error();
    const auto loaded = rypath::stl::load(argv[2], options);
    std::cout << rypath::stl::report(loaded);
    if (loaded.exit_code() != 0 || !loaded.asset)
        return loaded.exit_code() == 0 ? 1 : loaded.exit_code();
    auto conversion = rypath::stl::prepare(*loaded.asset, loaded.diagnostics);
    auto status = conversion.exit_code();
    if (status == 0 && convert)
        status = rypath::stl::publish(*loaded.asset, conversion, output);
    for (std::size_t i = loaded.diagnostics.size(); i < conversion.diagnostics.size(); ++i)
        std::cout << rypath::pbrt::format_diagnostic(conversion.diagnostics[i]) << '\n';
    std::cout << "STL conversion: " << (status == 0 ? "supported" : "rejected") << '\n';
    if (status == 0 && convert)
        std::cout << "Wrote STL bundle: " << output.string() << '\n';
    return status;
}

template <typename Integer> bool parse_integer(std::string_view value, Integer &result) {
    const auto *end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

int finish_render(const rypath::RenderResult &result, const rypath::RenderSettings &settings,
                  const std::filesystem::path &output, rypath::ExrPrecision precision,
                  std::span<const std::pair<std::string, std::string>> metadata) {
    if (!result.device.name.empty()) {
        std::cout << "Device: " << result.device.name << '\n'
                  << "Registry ID: " << result.device.registry_id << '\n'
                  << "Architecture: " << result.device.architecture << '\n';
    }
    const auto unavailable = [&](std::string_view status) {
        std::cout << "Render status: " << status << '\n';
        return render_error(result.diagnostic, 2);
    };
    switch (result.status) {
    case rypath::GeometryStatus::not_built:
        return unavailable("not_built");
    case rypath::GeometryStatus::no_device:
        return unavailable("no_device");
    case rypath::GeometryStatus::no_ray_tracing:
        return unavailable("no_ray_tracing");
    case rypath::GeometryStatus::failure:
        return render_error(result.diagnostic);
    case rypath::GeometryStatus::success:
        break;
    }
    const bool exr = output.extension() == ".exr";
    const auto diagnostic = exr ? rypath::write_exr(result.film, output, precision, metadata)
                                : rypath::write_png(result.film, output);
    if (!diagnostic.empty()) {
        return render_error(diagnostic);
    }
    const auto total =
        static_cast<std::uint64_t>(settings.width) * settings.height * settings.samples;
    std::cout << "Total samples: " << total << '\n'
              << "Wrote " << (exr ? "EXR" : "PNG") << ": " << output.string() << '\n';
    return 0;
}

using Metadata = std::vector<std::pair<std::string, std::string>>;

int finish_job(const rypath::RenderJobResult &job, const rypath::RenderSettings &settings,
               const std::filesystem::path &output, rypath::ExrPrecision precision,
               Metadata metadata) {
    if (job.render.completion == rypath::RenderCompletion::cancelled) {
        std::cout << "Render status: cancelled\n"
                  << "Completed samples per pixel: " << job.render.film.pixels.front().count << '/'
                  << settings.samples << '\n';
        return job.signal == SIGTERM ? 143 : 130;
    }
    if (!job.identity.empty()) {
        metadata.emplace_back("rypath.checkpointIdentity", job.identity);
        metadata.emplace_back("rypath.checkpointProvenance",
                              rypath::checkpoint_provenance(job.segments));
    }
    return finish_render(job.render, settings, output, precision, metadata);
}

// Returns false for an option owned by the surrounding render command.
bool checkpoint_option(std::string_view option, std::string_view value,
                       rypath::CheckpointOptions &options, std::string &diagnostic) {
    if (option == "--checkpoint" || option == "--resume") {
        if (value.empty())
            diagnostic = std::string{option} + " requires a nonempty path";
        (option == "--checkpoint" ? options.output : options.resume) = value;
    } else if (option == "--checkpoint-interval") {
        options.interval_supplied = true;
        if (!parse_integer(value, options.interval_seconds) || options.interval_seconds == 0)
            diagnostic = "--checkpoint-interval must be a positive unsigned integer";
    } else {
        return false;
    }
    return true;
}

std::string surface_model_description(const rypath::RenderScene &scene) {
    std::set<std::string> models;
    for (const auto &m : scene.materials) {
        using K = rypath::ScatteringKind;
        if (m.scattering == K::coated_diffuse || m.scattering == K::coated_conductor) {
            models.insert(std::string(m.scattering == K::coated_diffuse ? "coateddiffuse"
                                                                        : "coatedconductor") +
                          ": PBRT finite internal-depth approximation; maxdepth=" +
                          std::to_string(m.surface.max_depth) +
                          "; nsamples=" + std::to_string(m.surface.samples));
        } else if (m.scattering == K::fourier) {
            models.insert(
                "fourier: SCATFUN v1; achromatic or angular RGBUnbounded spectral reconstruction");
        }
    }
    std::string result;
    for (const auto &model : models) {
        if (!result.empty())
            result += " | ";
        result += model;
    }
    return result;
}

std::string render_sampler_description(const rypath::RenderSettings &settings,
                                       const rypath::RenderScene &scene) {
    const bool packet = scene.spectral_transport == rypath::SpectralTransport::packet;
    auto description = rypath::sampler_description(settings.sampler,
                                                   !packet && rypath::scene_has_dielectrics(scene),
                                                   !packet && scene.perspective_lens.has_value());
    if (packet)
        description +=
            "; packet fixed dimension allocation [0,920); keyed material-local Philox streams; "
            "independent coating hash/PCG domains";
    return description;
}

Metadata render_metadata(const rypath::RenderSettings &settings, std::string scene,
                         std::string profile, const rypath::RenderScene &render_scene,
                         std::string overrides) {
    std::ostringstream exposure;
    exposure << std::setprecision(std::numeric_limits<double>::max_digits10)
             << render_scene.exposure;
    const auto seed = settings.sampler.kind != rypath::SamplerKind::independent
                          ? std::to_string(rypath::sobol_signed_seed(settings.seed))
                          : std::to_string(settings.seed);
    Metadata metadata{{"rypath.version", RYPATH_VERSION},
                      {"rypath.profile", std::move(profile)},
                      {"rypath.scene", std::move(scene)},
                      {"rypath.sampler", render_sampler_description(settings, render_scene)},
                      {"rypath.seed", seed},
                      {"rypath.materialSeed", std::to_string(settings.material_seed)},
                      {"rypath.lightSampler",
                       render_scene.integrator == rypath::IntegratorKind::bdpt         ? "power"
                       : render_scene.light_sampler == rypath::LightSamplerKind::bvh   ? "bvh"
                       : render_scene.light_sampler == rypath::LightSamplerKind::power ? "power"
                                                                                       : "uniform"},
                      {"rypath.samplesPerPixel", std::to_string(settings.samples)},
                      {"rypath.batchSize", std::to_string(settings.batch_size)},
                      {"rypath.exposure", exposure.str()},
                      {"rypath.maxDepth", std::to_string(render_scene.max_depth)},
                      {"rypath.overrides", std::move(overrides)}};
    if (render_scene.perspective_lens) {
        std::ostringstream camera;
        camera.imbue(std::locale::classic());
        camera << std::setprecision(std::numeric_limits<float>::max_digits10)
               << "perspective thin lens; radius=" << render_scene.perspective_lens->radius
               << "; focal distance=" << render_scene.perspective_lens->focal_distance
               << "; normalized aperture; unit ray weight";
        if (render_scene.spectral_transport == rypath::SpectralTransport::packet)
            camera << "; fixed packet header block 1";
        metadata.emplace_back("rypath.camera", camera.str());
    }
    if (const auto description = surface_model_description(render_scene); !description.empty())
        metadata.emplace_back("rypath.materialModel", description);
    if (std::any_of(render_scene.materials.begin(), render_scene.materials.end(),
                    [](const auto &m) {
                        return m.scattering >= rypath::ScatteringKind::thin_dielectric &&
                               m.scattering != rypath::ScatteringKind::null_interface;
                    }))
        metadata.emplace_back(
            "rypath.materialSampling",
            render_scene.spectral_transport == rypath::SpectralTransport::packet
                ? "role-separated material-local Philox; deterministic keyed local walks; "
                  "stable-ID mix"
                : "material-local-philox-v1; deterministic keyed local walks; stable-ID mix");
    if (render_scene.spectral_transport == rypath::SpectralTransport::packet) {
        metadata.emplace_back("rypath.integrator",
                              render_scene.integrator == rypath::IntegratorKind::bdpt
                                  ? "bdpt; vacuum surface transport; balance MIS; camera splats"
                              : render_scene.integrator == rypath::IntegratorKind::volpath
                                  ? "volpath; vacuum surface transport; power MIS"
                                  : "path; vacuum surface transport; power MIS");
        metadata.emplace_back(
            "rypath.spectralTransport",
            "four shared-geometry visible wavelengths; secondary termination for dispersion");
        metadata.emplace_back(
            "rypath.packetSampling",
            "fixed dimension allocation [0,920); keyed material-local Philox streams");
        if (std::isfinite(render_scene.max_component_value)) {
            metadata.emplace_back("rypath.filmApproximation",
                                  "exposed CIE sensor XYZ max-component clamp before filtering; "
                                  "each camera sample and each splat clamped separately");
            metadata.emplace_back("rypath.maxComponentValue",
                                  std::to_string(render_scene.max_component_value));
        }
    } else if (render_scene.volume_transport) {
        metadata.emplace_back("rypath.integrator",
                              "volpath; scalar homogeneous spectral transport");
        metadata.emplace_back(
            "rypath.mediumSampling",
            "medium-local-philox-v1; hybrid with QMC samplers; independent segment counters");
        metadata.emplace_back("rypath.mediumModel",
                              "PBRT dense homogeneous spectra; analytic segment emission and "
                              "shadow transmittance; HG g clamped to [-0.99,0.99]");
    }
    return metadata;
}

const char *output_description(const std::filesystem::path &path, rypath::ExrPrecision precision) {
    if (path.extension() != ".exr")
        return "PNG; display sRGB; no FP16 intermediate";
    return precision == rypath::ExrPrecision::half ? "EXR; linear sRGB; HALF"
                                                   : "EXR; linear sRGB; FLOAT";
}

int render_demo(int argc, char *argv[]) {
    rypath::RenderSettings settings;
    rypath::CheckpointOptions checkpoints;
    std::filesystem::path output;
    std::set<std::string_view> seen;
    for (int index = 2; index < argc; index += 2) {
        const std::string_view option = argv[index];
        if (!seen.insert(option).second) {
            return render_error("duplicate option " + std::string{option});
        }
        if (index + 1 == argc || std::string_view{argv[index + 1]}.starts_with("--")) {
            return render_error("missing value for " + std::string{option});
        }
        const std::string_view value = argv[index + 1];
        bool valid = true;
        std::string diagnostic;
        if (checkpoint_option(option, value, checkpoints, diagnostic)) {
            if (!diagnostic.empty())
                return render_error(diagnostic);
        } else if (option == "--output") {
            output = value;
        } else if (option == "--width") {
            valid = parse_integer(value, settings.width);
        } else if (option == "--height") {
            valid = parse_integer(value, settings.height);
        } else if (option == "--samples") {
            valid = parse_integer(value, settings.samples);
        } else if (option == "--batch-size") {
            valid = parse_integer(value, settings.batch_size);
        } else if (option == "--seed") {
            valid = parse_integer(value, settings.seed);
        } else {
            return argument_error();
        }
        if (!valid) {
            return render_error("invalid unsigned integer for " + std::string{option});
        }
    }
    if (output.empty()) {
        return render_error("--render-demo requires --output PATH");
    }
    if (const auto diagnostic = rypath::validate_render_settings(settings); !diagnostic.empty()) {
        return render_error(diagnostic);
    }
    if (const auto diagnostic = rypath::validate_checkpoint_paths(checkpoints, output, {});
        !diagnostic.empty())
        return render_error(diagnostic);

    std::cout << "rypath " RYPATH_VERSION "\n"
              << "Scene: demo-v1\n"
              << "Sampler: philox4x32-10-v1\n"
              << "Color: CIE 1931 2-degree XYZ / sRGB\n"
              << "Resolution: " << settings.width << 'x' << settings.height << '\n'
              << "Samples per pixel: " << settings.samples << '\n'
              << "Batch size: " << settings.batch_size << '\n'
              << "Seed: " << settings.seed << '\n'
              << std::flush;
    const auto scene = rypath::make_demo_scene();
    const auto metadata = render_metadata(settings, "demo-v1", "demo-v1", scene, "output path");
    std::cout << "Output: " << output.string() << " ("
              << output_description(output, rypath::ExrPrecision::float32) << ")\n";
    const auto job = rypath::run_render_job(scene, settings, checkpoints, output, {},
                                            RYPATH_BUILD_DESCRIPTION, [&](std::uint64_t count) {
                                                std::cout
                                                    << "Committed samples per pixel: " << count
                                                    << '/' << settings.samples << std::endl;
                                            });
    return finish_job(job, settings, output, rypath::ExrPrecision::float32, metadata);
}

int render_pbrt(int argc, char *argv[], rypath::RenderTimings *timings = nullptr,
                rypath::RenderTimingsOutput *timing_output = nullptr) {
    using Phase = rypath::RenderTimingPhase;
    const auto finish = [&](int code) {
        if (timings)
            timings->begin_destruction();
        return code;
    };
    const auto fail = [&](std::string_view message, int code = 1) {
        if (timings)
            timings->failure(std::string(message));
        return finish(render_error(message, code));
    };
    if (argc < 3 || std::string_view{argv[2]}.starts_with("--")) {
        return finish(argument_error());
    }
    const std::filesystem::path entry = argv[2];
    rypath::CheckpointOptions checkpoints;
    std::filesystem::path output, raw_output, timing_path;
    std::uint64_t batch_size = 8;
    std::int32_t material_seed = 0;
    auto resource_profile = rypath::ResourceProfile::standard;
    std::set<std::string_view> seen;
    for (int index = 3; index < argc; index += 2) {
        const std::string_view option = argv[index];
        if (!seen.insert(option).second) {
            return fail("duplicate option " + std::string{option});
        }
        if (index + 1 == argc || std::string_view{argv[index + 1]}.starts_with("--")) {
            return fail("missing value for " + std::string{option});
        }
        std::string diagnostic;
        if (checkpoint_option(option, argv[index + 1], checkpoints, diagnostic)) {
            if (!diagnostic.empty())
                return fail(diagnostic);
        } else if (option == "--output") {
            output = argv[index + 1];
        } else if (option == "--raw-output") {
            raw_output = argv[index + 1];
        } else if (option == "--timings") {
            timing_path = argv[index + 1];
        } else if (option == "--material-seed") {
            if (!parse_integer(std::string_view{argv[index + 1]}, material_seed))
                return fail("--material-seed must be a signed 32-bit integer");
        } else if (option == "--resource-profile") {
            const auto parsed = rypath::parse_resource_profile(argv[index + 1]);
            if (!parsed)
                return fail("--resource-profile requires standard or large");
            resource_profile = *parsed;
        } else if (option == "--batch-size") {
            if (!parse_integer(std::string_view{argv[index + 1]}, batch_size) || batch_size == 0) {
                return fail("--batch-size must be a positive unsigned integer");
            }
        } else {
            return finish(argument_error());
        }
    }
    if (output.empty()) {
        return fail("--render-pbrt requires --output IMAGE.{png,exr}");
    }
    if (output.extension() != ".png" && output.extension() != ".exr") {
        return fail("--render-pbrt output must have the .png or .exr extension");
    }
    if (const auto diagnostic = rypath::validate_checkpoint_paths(checkpoints, output, {});
        !diagnostic.empty())
        return fail(diagnostic);
    if (!raw_output.empty() && raw_output.extension() != ".exr")
        return fail("--raw-output requires a .exr path");
    if (timings && timing_path.empty())
        return fail("--timings requires a fresh nonempty output path");
    const auto reserve_timings = [&](const std::vector<std::filesystem::path> &inputs) {
        if (!timings)
            return std::string{};
        auto protected_paths = inputs;
        for (const auto &path : {output, raw_output, checkpoints.output, checkpoints.resume})
            if (!path.empty())
                protected_paths.push_back(path);
        return timing_output->reserve(timing_path, protected_paths);
    };
    const auto resources = rypath::resource_limits(resource_profile);
    rypath::pbrt::RenderTranslation translated;
    std::vector<std::filesystem::path> inputs{entry};
    {
        rypath::RenderTimingScope preflight(timings, Phase::preflight_overhead);
        const auto checked = rypath::render_timed(
            timings, Phase::pbrt_check, [&] { return rypath::pbrt::check(entry, resources.pbrt); });
        if (checked.exit_code() != 0) {
            if (timings) {
                for (const auto &dependency : checked.document.dependencies)
                    inputs.push_back(dependency.resolved_path);
                const auto timing_error = reserve_timings(inputs);
                if (!timing_error.empty())
                    std::cerr << "Timings: " << timing_error << '\n';
                timings->failure("PBRT preflight failed; see diagnostic output");
            }
            std::cout << rypath::pbrt::report(checked);
            return finish(checked.exit_code());
        }
        for (const auto &dependency : checked.document.dependencies)
            inputs.push_back(dependency.resolved_path);
        if (const auto timing_error = reserve_timings(inputs); !timing_error.empty())
            return fail(timing_error);
        translated = rypath::render_timed(timings, Phase::pbrt_translate, [&] {
            return rypath::pbrt::translate(checked, rypath::pbrt::Location{entry.string()},
                                           resources.pbrt);
        });
        if (timings)
            timings->transition(Phase::document_cleanup);
    }
    rypath::RenderTimingScope post_translation(timings, Phase::post_translation);
    for (const auto &diagnostic : translated.diagnostics) {
        std::cerr << rypath::pbrt::format_diagnostic(diagnostic);
    }
    if (translated.exit_code() != 0) {
        std::cout << "Rendering was not attempted.\n";
        if (timings)
            timings->failure("PBRT translation failed; see diagnostic output");
        return finish(translated.exit_code());
    }
    auto owned_scene = std::move(translated.scene);
    if (!owned_scene.has_value()) {
        return fail("PBRT translation returned no render scene");
    }
    const auto &scene = owned_scene.value();
    if (!raw_output.empty()) {
        auto protected_paths = inputs;
        protected_paths.push_back(output);
        if (const auto diagnostic =
                rypath::validate_checkpoint_paths(checkpoints, raw_output, protected_paths);
            !diagnostic.empty())
            return fail(diagnostic);
        rypath::CheckpointOptions raw_guard;
        raw_guard.output = raw_output;
        if (const auto diagnostic = rypath::validate_checkpoint_paths(raw_guard, output, inputs);
            !diagnostic.empty())
            return fail("raw output must not alias the primary output or scene inputs");
    }
    translated.settings.batch_size = batch_size;
    translated.settings.material_seed = material_seed;
    const auto &settings = translated.settings;
    if (const auto diagnostic = rypath::validate_render_settings(settings); !diagnostic.empty()) {
        return fail(diagnostic);
    }
    const auto precision =
        translated.save_fp16 ? rypath::ExrPrecision::half : rypath::ExrPrecision::float32;
    const bool qmc = settings.sampler.kind != rypath::SamplerKind::independent;
    const auto seed = qmc ? std::to_string(rypath::sobol_signed_seed(settings.seed))
                          : std::to_string(settings.seed);
    std::cout << "rypath " RYPATH_VERSION "\n"
              << "Scene: " << entry.string() << '\n'
              << "Render profile: pbrt-spectral-v1\n"
              << "Resource profile: " << rypath::resource_profile_name(resource_profile) << '\n'
              << "Sampler: " << render_sampler_description(settings, scene) << '\n'
              << "Light sampling: "
              << (scene.integrator == rypath::IntegratorKind::bdpt ||
                          scene.light_sampler == rypath::LightSamplerKind::power
                      ? "power"
                  : scene.light_sampler == rypath::LightSamplerKind::bvh ? "bvh"
                                                                         : "uniform")
              << "; matched forward and reverse probabilities\n"
              << "Color: calibrated CIE 1931 XYZ / sRGB\n"
              << "Resolution: " << settings.width << 'x' << settings.height << '\n'
              << "Samples per pixel: " << settings.samples << '\n'
              << "Batch size: " << settings.batch_size << '\n'
              << "Seed: " << seed << '\n'
              << "Material seed: " << settings.material_seed << '\n'
              << "Finite path maxdepth: " << scene.max_depth << '\n'
              << "Output override: " << output.string() << " (Film.filename overridden; "
              << output_description(output, precision)
              << (output.extension() == ".png" ? "; Film.savefp16 overridden"
                                               : "; Film.savefp16 honored")
              << ")\n";
    if (const auto description = surface_model_description(scene); !description.empty())
        std::cout << "Surface model: " << description << '\n';
    if (scene.volume_transport && scene.spectral_transport == rypath::SpectralTransport::scalar)
        std::cout
            << "Medium sampling: medium-local-philox-v1; hybrid with QMC samplers; independent "
               "segment counters\n";
    for (const auto &item : translated.provenance) {
        std::cout << item << '\n';
    }
    std::cout << std::flush;
    if (timings) {
        timings->provenance("scene", entry.string());
        timings->provenance("width", std::to_string(settings.width));
        timings->provenance("height", std::to_string(settings.height));
        timings->provenance("samples", std::to_string(settings.samples));
        timings->provenance("batch", std::to_string(settings.batch_size));
        timings->provenance("seed", std::to_string(settings.seed));
        timings->provenance("material_seed", std::to_string(settings.material_seed));
        timings->provenance("sampler", render_sampler_description(settings, scene));
    }
    post_translation.close();
    const auto job = rypath::render_timed(timings, Phase::render_job, [&] {
        return rypath::run_render_job(
            scene, settings, checkpoints, output, inputs, RYPATH_BUILD_DESCRIPTION,
            [&](std::uint64_t count) {
                std::cout << "Committed samples per pixel: " << count << '/' << settings.samples
                          << std::endl;
            },
            timings);
    });
    if (timings) {
        timings->provenance("device", job.render.device.name);
        timings->provenance("architecture", job.render.device.architecture);
        timings->provenance("registry_id", std::to_string(job.render.device.registry_id));
        timings->provenance("identity", job.identity);
        if (job.render.completion == rypath::RenderCompletion::cancelled)
            timings->cancelled();
        if (!job.render.diagnostic.empty())
            timings->failure(job.render.diagnostic);
    }
    rypath::RenderTimingScope output_metadata(timings, Phase::output_metadata);
    auto metadata =
        render_metadata(settings, entry.string(), "pbrt-spectral-v1", scene,
                        output.extension() == ".exr" ? "Film.filename; batch size"
                                                     : "Film.filename; Film.savefp16; batch size");
    std::string translation_provenance;
    for (const auto &item : translated.provenance) {
        if (!translation_provenance.empty())
            translation_provenance += '\n';
        translation_provenance += item;
    }
    metadata.emplace_back("rypath.sceneTranslation", std::move(translation_provenance));
    output_metadata.close();
    const int status = rypath::render_timed(timings, Phase::primary_output, [&] {
        return finish_job(job, settings, output, precision, metadata);
    });
    if (timings && status != 0 && job.render.status == rypath::GeometryStatus::success &&
        job.render.completion == rypath::RenderCompletion::complete)
        timings->failure("Primary image output failed; see diagnostic output");
    if (status == 0)
        std::cout << "Preparation seconds: " << job.render.preparation_seconds << '\n'
                  << "Pipeline seconds: " << job.render.pipeline_seconds << '\n'
                  << "Render and accumulation seconds: " << job.render.render_seconds << '\n';
    if (status != 0 || raw_output.empty())
        return finish(status);
    auto raw = rypath::render_timed(timings, Phase::raw_film_copy, [&] { return job.render.film; });
    rypath::RenderTimingScope raw_metadata(timings, Phase::output_metadata);
    raw.compatibility_output = false;
    raw.exposure = 1;
    std::erase_if(metadata, [](const auto &item) {
        return item.first == "rypath.filmApproximation" ||
               item.first == "rypath.maxComponentValue" || item.first == "rypath.exposure";
    });
    metadata.emplace_back("rypath.exposure", "1");
    metadata.emplace_back("rypath.filmOutput", "unclamped unexposed raw Monte Carlo estimate; "
                                               "camera reconstruction plus normalized splats");
    if (!job.identity.empty()) {
        metadata.emplace_back("rypath.checkpointIdentity", job.identity);
        metadata.emplace_back("rypath.checkpointProvenance",
                              rypath::checkpoint_provenance(job.segments));
    }
    raw_metadata.close();
    if (const auto diagnostic = rypath::render_timed(
            timings, Phase::raw_output,
            [&] {
                return rypath::write_exr(raw, raw_output, rypath::ExrPrecision::float32, metadata);
            });
        !diagnostic.empty())
        return fail(diagnostic);
    std::cout << "Raw output: " << raw_output.string() << " (EXR FLOAT; unexposed; unclamped)\n";
    return finish(0);
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc == 1 || (argc == 2 && std::string_view{argv[1]} == "--help")) {
        std::cout << "Usage: rypath [--help | --version | --metal-info | --metal-smoke-test]\n"
                     "       rypath --check-pbrt SCENE [--resource-profile standard|large]\n"
                     "       rypath --check-obj MODEL.obj\n"
                     "       rypath --convert-obj MODEL.obj --output DIRECTORY\n"
                     "       rypath --check-gltf MODEL [--gltf-scene INDEX]\n"
                     "       rypath --convert-gltf MODEL --output DIRECTORY [--gltf-scene INDEX]\n"
                     "       rypath --check-stl MODEL.stl [--stl-colors auto|viscam|magics]\n"
                     "       rypath --convert-stl MODEL.stl --output DIRECTORY\n"
                     "              [--stl-colors auto|viscam|magics]\n"
                     "       rypath --render-pbrt SCENE --output IMAGE.{png,exr} [--batch-size 8]\n"
                     "           [--resource-profile standard|large] [--raw-output PATH.exr] "
                     "[--timings PATH.json]\n"
                     "           [--material-seed 0]\n"
                     "       rypath --render-demo --output PATH [--width 256] [--height 192]\n"
                     "              [--samples 128] [--batch-size 8] [--seed 1]\n"
                     "       Render options: [--checkpoint PATH] [--resume PATH]\n"
                     "                       [--checkpoint-interval SECONDS (default: 60)]\n";
        return 0;
    }
    try {
        if (argc > 1 && (std::string_view{argv[1]} == "--check-gltf" ||
                         std::string_view{argv[1]} == "--convert-gltf"))
            return import_gltf(argc, argv);
        if (argc > 1 && (std::string_view{argv[1]} == "--check-stl" ||
                         std::string_view{argv[1]} == "--convert-stl"))
            return import_stl(argc, argv);
        if (argc == 3 && std::string_view{argv[1]} == "--check-obj" &&
            !std::string_view{argv[2]}.empty() && !std::string_view{argv[2]}.starts_with("--"))
            return import_obj({.source = argv[2], .output = {}});
        if (argc == 5 && std::string_view{argv[1]} == "--convert-obj" &&
            !std::string_view{argv[2]}.empty() && !std::string_view{argv[2]}.starts_with("--") &&
            std::string_view{argv[3]} == "--output" && !std::string_view{argv[4]}.empty() &&
            !std::string_view{argv[4]}.starts_with("--"))
            return import_obj({.source = argv[2], .output = argv[4]});
        if (argc > 1 && std::string_view{argv[1]} == "--render-pbrt") {
            bool capture_timings = false;
            for (int i = 3; i < argc; i += 2)
                capture_timings = capture_timings || std::string_view{argv[i]} == "--timings";
            if (!capture_timings)
                return render_pbrt(argc, argv);
            rypath::RenderTimings timings;
            rypath::RenderTimingsOutput report;
            int status = 1;
            {
                rypath::RenderTimingScope command(&timings, rypath::RenderTimingPhase::command);
                try {
                    status = render_pbrt(argc, argv, &timings, &report);
                } catch (const std::exception &error) {
                    timings.failure(error.what());
                    status = render_error(error.what());
                }
            }
            timings.finish();
            if (report.reserved()) {
                try {
                    if (const auto diagnostic =
                            report.publish(timings, status, RYPATH_BUILD_DESCRIPTION);
                        !diagnostic.empty()) {
                        std::cerr << "Timings: " << diagnostic << '\n';
                        if (status == 0)
                            status = 1;
                    }
                } catch (const std::exception &error) {
                    std::cerr << "Timings: " << error.what() << '\n';
                    if (status == 0)
                        status = 1;
                }
            } else if (status == 0) {
                status = render_error("timings output was not safely reserved");
            }
            if (!timings.valid()) {
                std::cerr << "Timings: collector accounting failed\n";
                if (status == 0)
                    status = 1;
            }
            return status;
        }
        if ((argc == 3 || argc == 5) && std::string_view{argv[1]} == "--check-pbrt" &&
            !std::string_view{argv[2]}.starts_with("--")) {
            auto profile = rypath::ResourceProfile::standard;
            if (argc == 5) {
                const auto parsed = rypath::parse_resource_profile(argv[4]);
                if (std::string_view{argv[3]} != "--resource-profile" || !parsed)
                    return argument_error();
                profile = *parsed;
            }
            const auto resources = rypath::resource_limits(profile);
            if (argc == 5)
                std::cout << "Resource profile: " << rypath::resource_profile_name(profile) << '\n';
            const auto result = rypath::pbrt::check(argv[2], resources.pbrt);
            std::cout << rypath::pbrt::report(result);
            return result.exit_code();
        }
        if (argc > 1 && std::string_view{argv[1]} == "--render-demo") {
            return render_demo(argc, argv);
        }
        if (argc == 2) {
            if (std::string_view{argv[1]} == "--metal-info") {
                return report_metal(false);
            }
            if (std::string_view{argv[1]} == "--metal-smoke-test") {
                return report_metal(true);
            }
            if (std::string_view{argv[1]} == "--version") {
                std::cout << "rypath " RYPATH_VERSION "\n";
                return 0;
            }
        }
    } catch (const std::exception &error) {
        return render_error(error.what());
    }
    return argument_error();
}
