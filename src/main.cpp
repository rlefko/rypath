// SPDX-License-Identifier: GPL-3.0-only

#include "image_output.hpp"
#include "metal_backend.hpp"
#include "metal_render.hpp"
#include "pbrt.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <charconv>
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

template <typename Integer> bool parse_unsigned(std::string_view value, Integer &result) {
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

Metadata render_metadata(const rypath::RenderSettings &settings, std::string scene,
                         std::string profile, const rypath::RenderScene &render_scene,
                         std::string overrides) {
    std::ostringstream exposure;
    exposure << std::setprecision(std::numeric_limits<double>::max_digits10)
             << render_scene.exposure;
    const auto seed = settings.sampler.kind == rypath::SamplerKind::sobol
                          ? std::to_string(rypath::sobol_signed_seed(settings.seed))
                          : std::to_string(settings.seed);
    Metadata metadata{{"rypath.version", RYPATH_VERSION},
                      {"rypath.profile", std::move(profile)},
                      {"rypath.scene", std::move(scene)},
                      {"rypath.sampler",
                       rypath::sampler_description(settings.sampler,
                                                   rypath::scene_has_dielectrics(render_scene))},
                      {"rypath.seed", seed},
                      {"rypath.samplesPerPixel", std::to_string(settings.samples)},
                      {"rypath.batchSize", std::to_string(settings.batch_size)},
                      {"rypath.exposure", exposure.str()},
                      {"rypath.maxDepth", std::to_string(render_scene.max_depth)},
                      {"rypath.overrides", std::move(overrides)}};
    if (const auto description = surface_model_description(render_scene); !description.empty())
        metadata.emplace_back("rypath.materialModel", description);
    if (std::any_of(
            render_scene.materials.begin(), render_scene.materials.end(),
            [](const auto &m) { return m.scattering >= rypath::ScatteringKind::thin_dielectric; }))
        metadata.emplace_back(
            "rypath.materialSampling",
            "material-local-philox-v1; deterministic keyed local walks; stable-ID mix");
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
        if (option == "--output") {
            output = value;
        } else if (option == "--width") {
            valid = parse_unsigned(value, settings.width);
        } else if (option == "--height") {
            valid = parse_unsigned(value, settings.height);
        } else if (option == "--samples") {
            valid = parse_unsigned(value, settings.samples);
        } else if (option == "--batch-size") {
            valid = parse_unsigned(value, settings.batch_size);
        } else if (option == "--seed") {
            valid = parse_unsigned(value, settings.seed);
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
    const auto result = rypath::render_progressive(scene, settings, [&](std::uint64_t count) {
        std::cout << "Committed samples per pixel: " << count << '/' << settings.samples
                  << std::endl;
    });
    return finish_render(result, settings, output, rypath::ExrPrecision::float32, metadata);
}

int render_pbrt(int argc, char *argv[]) {
    if (argc < 3 || std::string_view{argv[2]}.starts_with("--")) {
        return argument_error();
    }
    const std::filesystem::path entry = argv[2];
    std::filesystem::path output;
    std::uint64_t batch_size = 8;
    std::set<std::string_view> seen;
    for (int index = 3; index < argc; index += 2) {
        const std::string_view option = argv[index];
        if (!seen.insert(option).second) {
            return render_error("duplicate option " + std::string{option});
        }
        if (index + 1 == argc || std::string_view{argv[index + 1]}.starts_with("--")) {
            return render_error("missing value for " + std::string{option});
        }
        if (option == "--output") {
            output = argv[index + 1];
        } else if (option == "--batch-size") {
            if (!parse_unsigned(std::string_view{argv[index + 1]}, batch_size) || batch_size == 0) {
                return render_error("--batch-size must be a positive unsigned integer");
            }
        } else {
            return argument_error();
        }
    }
    if (output.empty()) {
        return render_error("--render-pbrt requires --output IMAGE.{png,exr}");
    }
    if (output.extension() != ".png" && output.extension() != ".exr") {
        return render_error("--render-pbrt output must have the .png or .exr extension");
    }
    rypath::pbrt::RenderTranslation translated;
    {
        const auto checked = rypath::pbrt::check(entry);
        if (checked.exit_code() != 0) {
            std::cout << rypath::pbrt::report(checked);
            return checked.exit_code();
        }
        translated = rypath::pbrt::translate(checked, rypath::pbrt::Location{entry.string()});
    }
    for (const auto &diagnostic : translated.diagnostics) {
        std::cerr << rypath::pbrt::format_diagnostic(diagnostic);
    }
    if (translated.exit_code() != 0) {
        std::cout << "Rendering was not attempted.\n";
        return translated.exit_code();
    }
    auto owned_scene = std::move(translated.scene);
    if (!owned_scene.has_value()) {
        return render_error("PBRT translation returned no render scene");
    }
    const auto &scene = owned_scene.value();
    translated.settings.batch_size = batch_size;
    const auto &settings = translated.settings;
    if (const auto diagnostic = rypath::validate_render_settings(settings); !diagnostic.empty()) {
        return render_error(diagnostic);
    }
    const auto precision =
        translated.save_fp16 ? rypath::ExrPrecision::half : rypath::ExrPrecision::float32;
    const bool sobol = settings.sampler.kind == rypath::SamplerKind::sobol;
    const auto seed = sobol ? std::to_string(rypath::sobol_signed_seed(settings.seed))
                            : std::to_string(settings.seed);
    std::cout
        << "rypath " RYPATH_VERSION "\n"
        << "Scene: " << entry.string() << '\n'
        << "Render profile: pbrt-spectral-v1\n"
        << "Sampler: "
        << rypath::sampler_description(settings.sampler, rypath::scene_has_dielectrics(scene))
        << '\n'
        << "Light sampling: separate environment and uniform triangle area (differs from PBRT)\n"
        << "Color: calibrated CIE 1931 XYZ / sRGB\n"
        << "Resolution: " << settings.width << 'x' << settings.height << '\n'
        << "Samples per pixel: " << settings.samples << '\n'
        << "Batch size: " << settings.batch_size << '\n'
        << "Seed: " << seed << '\n'
        << "Finite path maxdepth: " << scene.max_depth << '\n'
        << "Output override: " << output.string() << " (Film.filename overridden; "
        << output_description(output, precision)
        << (output.extension() == ".png" ? "; Film.savefp16 overridden" : "; Film.savefp16 honored")
        << ")\n";
    if (const auto description = surface_model_description(scene); !description.empty())
        std::cout << "Surface model: " << description << '\n';
    for (const auto &item : translated.provenance) {
        std::cout << item << '\n';
    }
    std::cout << std::flush;
    const auto result = rypath::render_progressive(scene, settings, [&](std::uint64_t count) {
        std::cout << "Committed samples per pixel: " << count << '/' << settings.samples
                  << std::endl;
    });
    const auto metadata =
        render_metadata(settings, entry.string(), "pbrt-spectral-v1", scene,
                        output.extension() == ".exr" ? "Film.filename; batch size"
                                                     : "Film.filename; Film.savefp16; batch size");
    return finish_render(result, settings, output, precision, metadata);
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc == 1 || (argc == 2 && std::string_view{argv[1]} == "--help")) {
        std::cout << "Usage: rypath [--help | --version | --metal-info | --metal-smoke-test]\n"
                     "       rypath --check-pbrt SCENE\n"
                     "       rypath --render-pbrt SCENE --output IMAGE.{png,exr} [--batch-size 8]\n"
                     "       rypath --render-demo --output PATH [--width 256] [--height 192]\n"
                     "              [--samples 128] [--batch-size 8] [--seed 1]\n";
        return 0;
    }
    try {
        if (argc > 1 && std::string_view{argv[1]} == "--render-pbrt") {
            return render_pbrt(argc, argv);
        }
        if (argc == 3 && std::string_view{argv[1]} == "--check-pbrt" &&
            !std::string_view{argv[2]}.starts_with("--")) {
            const auto result = rypath::pbrt::check(argv[2]);
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
