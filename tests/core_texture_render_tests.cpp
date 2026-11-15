// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace rypath;
using std::string_view;

constexpr std::uint64_t seed = 0x5dc218d043956ab9ULL;
constexpr std::uint32_t trials = 8192;
constexpr string_view options = R"(Camera "perspective" "float fov" 8
Film "rgb" "integer xresolution" 1 "integer yresolution" 1
PixelFilter "box"
Sampler "independent"
Integrator "path"
WorldBegin
)";
constexpr string_view receiver = R"(Shape "trianglemesh"
  "point3 P" [-2 -2 2 2 -2 2 2 2 2 -2 2 2]
  "point2 uv" [0 0 1 0 1 1 0 1]
  "integer indices" [0 1 2 0 2 3]
)";
constexpr string_view light = R"(Material "diffuse" "rgb reflectance" [0 0 0]
AreaLightSource "diffuse" "float scale" 100 "bool twosided" true
Shape "trianglemesh"
  "point3 P" [1.2 -0.6 1 2.2 -0.6 1 2.2 0.6 1 1.2 0.6 1]
  "point2 uv" [0 0 1 0 1 1 0 1]
  "integer indices" [0 1 2 0 2 3]
)";
constexpr string_view shadow_blocker = R"(AreaLightSource ""
Shape "trianglemesh"
  "point3 P" [0.4 -1 1.5 1.3 -1 1.5 1.3 1 1.5 0.4 1 1.5]
  "integer indices" [0 1 2 0 2 3]
)";
constexpr string_view sky = R"(LightSource "infinite" "float scale" 100
)";

void require(bool value, string_view message) {
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}

RenderScene load(const fs::path &path, string_view body, unsigned depth,
                 string_view scene_options = options) {
    std::ofstream input(path);
    input << scene_options << body;
    input.close();
    require(input.good(), "Could not write integrated PBRT test scene");
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
    if (!translated.scene) {
        for (const auto &issue : translated.diagnostics) {
            std::cerr << pbrt::format_diagnostic(issue);
        }
        throw std::runtime_error("Could not translate integrated PBRT test scene");
    }
    translated.scene->max_depth = depth;
    return std::move(*translated.scene);
}

std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto renderer = build_metal_renderer(scene);
    require(renderer.status == GeometryStatus::success && renderer.scene, renderer.diagnostic);
    return std::move(renderer.scene);
}

std::vector<RenderSample> samples(MetalRenderScene &renderer, unsigned count, unsigned begin = 0,
                                  unsigned width = 0) {
    const RenderRequest request{.width = 1,
                                .height = 1,
                                .seed = seed,
                                .pixel_count = 1,
                                .sample_begin = begin,
                                .sample_count = count,
                                .total_samples = trials,
                                .threadgroup_width = width};
    auto result = renderer.sample(request);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == count && result.guard_count > 0, "Incomplete Metal samples");
    for (unsigned i = 0; i < count; ++i) {
        require(result.samples[i].sample == begin + i && result.samples[i].pixel == 0 &&
                    std::isfinite(result.samples[i].radiance),
                "Metal partition did not retain absolute sample identity");
    }
    return std::move(result.samples);
}

std::vector<RenderSample> trace(const fs::path &path, string_view source, unsigned depth,
                                unsigned count = trials) {
    return samples(*build(load(path, source, depth)), count);
}

void equivalent(std::span<const RenderSample> left, std::span<const RenderSample> right,
                string_view scene, double tolerance = 0) {
    require(left.size() == right.size(), "Expected paired samples");
    for (std::size_t i = 0; i < left.size(); ++i) {
        require(left[i].sample == right[i].sample && left[i].wavelength == right[i].wavelength &&
                    left[i].random_words == right[i].random_words &&
                    left[i].scatterings == right[i].scatterings &&
                    std::abs(left[i].radiance - right[i].radiance) <= tolerance,
                scene);
    }
}

double total(std::span<const RenderSample> paths) {
    return std::accumulate(paths.begin(), paths.end(), 0.0,
                           [](double sum, const auto &path) { return sum + path.radiance; });
}

void check_mean(std::span<const RenderSample> observed, std::span<const RenderSample> expected,
                double factor, string_view label) {
    require(observed.size() == expected.size() && observed.size() > 1, "Expected paired means");
    double sum = 0;
    double squares = 0;
    for (std::size_t i = 0; i < observed.size(); ++i) {
        const double delta = observed[i].radiance - factor * expected[i].radiance;
        sum += delta;
        squares += delta * delta;
    }
    const double n = double(observed.size());
    const double mean = sum / n;
    const double se = std::sqrt(std::max(0.0, (squares - sum * mean) / (n * (n - 1))));
    std::cout << label << " residual " << mean << ", SE " << se << '\n';
    require(std::abs(mean) <= 6 * se + 3e-5, label);
}

void graph_controls(const fs::path &path) {
    constexpr unsigned count = 1024;
    struct Comparison {
        string_view pattern;
        string_view control;
        string_view message;
    };
    constexpr std::array cases{
        Comparison{.pattern = R"(Texture "blend" "spectrum" "mix" "rgb tex1" [0.25 0.25 0.25]
  "rgb tex2" [0.5 0.5 0.5]
Material "diffuse" "texture reflectance" "blend"
)",
                   .control = R"(Material "diffuse" "rgb reflectance" [0.375 0.375 0.375]
)",
                   .message = "A constant procedural graph must match legacy reflectance"},
        Comparison{
            .pattern =
                R"(Texture "gray" "spectrum" "imagemap" "string filename" "assets/gray-pattern.png"
  "string mapping" "planar" "vector3 v1" [0.375 0 0] "vector3 v2" [0 0.25 0]
  "float udelta" 0.875 "float vdelta" 0.375 "string filter" "point"
Material "diffuse" "texture reflectance" "gray"
)",
            .control =
                R"(Texture "gray" "spectrum" "imagemap" "string filename" "assets/gray-pattern.png"
  "float uscale" 1.5 "float udelta" 0.125 "float vdelta" -0.125 "string filter" "point"
Material "diffuse" "texture reflectance" "gray"
)",
            .message = "A world-planar point image map must match corresponding legacy UVs"},
        Comparison{
            .pattern =
                R"(Texture "white" "spectrum" "imagemap" "string filename" "assets/white.png" "float scale" 2
Texture "blend" "spectrum" "mix" "texture tex1" "white" "rgb tex2" [0 0 0] "float amount" 0.75
Material "diffuse" "texture reflectance" "blend"
)",
            .control = R"(Material "diffuse" "rgb reflectance" [0.25 0.25 0.25]
)",
            .message = "Image albedo is clamped before graph composition"},
    };
    for (const auto &test : cases) {
        const auto source = std::string(sky) + std::string(test.pattern) + std::string(receiver);
        const auto truth = std::string(sky) + std::string(test.control) + std::string(receiver);
        equivalent(trace(path, source, 1, count), trace(path, truth, 1, count), test.message);
    }
}

void primary_alpha(const fs::path &path) {
    const auto material = R"(Material "diffuse" "rgb reflectance" [0 0 0]
)";
    std::string scene = std::string(sky) + material + std::string(receiver);
    const auto clear = trace(path, scene + R"("float alpha" 0
)",
                             0);
    const auto opaque = trace(path, scene + R"("float alpha" 1
)",
                              0);
    require(total(clear) > 0 && total(opaque) == 0, "Binary camera alpha endpoints");
    auto fractional = build(load(path, scene + R"("float alpha" 0.35
)",
                                 0));
    const auto whole = samples(*fractional, trials);
    for (std::size_t i = 0; i < whole.size(); ++i) {
        require(whole[i].radiance == 0 || whole[i].radiance == clear[i].radiance,
                "Fractional coverage must choose transparent or opaque transport");
    }
    const auto accepted =
        std::ranges::count_if(whole, [](const RenderSample &s) { return s.radiance > 0; });
    const double expected = trials * 0.65;
    require(std::abs(double(accepted) - expected) <= 6 * std::sqrt(expected * 0.35),
            "Fractional camera acceptance departs from its binomial tolerance");
    unsigned begin = 0;
    for (const unsigned count : {1337u, 2987u, trials - 4324}) {
        const auto split = samples(*fractional, count, begin, begin == 0 ? 32 : 64);
        equivalent(split, std::span(whole).subspan(begin, count),
                   "Alpha path changes with dispatch partition or threadgroup width");
        begin += count;
    }
}

void spatial_instance_alpha(const fs::path &path) {
    constexpr string_view camera = R"(LookAt 0.5 0.5 -2 0.5 0.5 1 0 1 0
Camera "perspective" "float fov" 60
Film "rgb" "integer xresolution" 64 "integer yresolution" 64
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 32
Integrator "path" "integer maxdepth" 0
WorldBegin
)";
    auto scene = build(load(path, R"(LightSource "infinite" "rgb L" [1 1 1]
Texture "mask" "float" "checkerboard" "integer dimension" 3 "float tex1" 1 "float tex2" 0
ObjectBegin "square"
    Material "diffuse" "rgb reflectance" [0 0 0]
    Shape "trianglemesh" "point3 P" [0.1 0.1 0.25 0.9 0.1 0.25 0.9 0.9 0.25 0.1 0.9 0.25]
        "integer indices" [0 1 2 0 2 3] "texture alpha" "mask"
ObjectEnd
ObjectInstance "square"
Translate 1 0 0
ObjectInstance "square"
)",
                            0, camera));
    for (unsigned x : {32u, 44u, 56u}) {
        const RenderRequest request{.width = 64,
                                    .height = 64,
                                    .seed = seed,
                                    .pixel_begin = 32 * 64 + x,
                                    .pixel_count = 1,
                                    .sample_count = 32,
                                    .total_samples = 32};
        const auto traced = scene->sample(request);
        require(traced.status == GeometryStatus::success && traced.samples.size() == 32,
                traced.diagnostic);
        for (const auto &sample : traced.samples) {
            require(x == 44 ? sample.radiance > 0 : sample.radiance == 0,
                    "Instance placement must preserve definition-space procedural alpha");
        }
    }
}

std::string lit_scene(string_view detail = {}) {
    return R"(Material "diffuse" "rgb reflectance" [0.75 0.75 0.75]
)" + std::string(detail) +
           std::string(receiver) + std::string(light);
}

void shadows_and_emitters(const fs::path &path, std::span<const RenderSample> clear) {
    const auto scene = lit_scene();
    const auto shadow = [&](string_view alpha) {
        return trace(path, scene + std::string(shadow_blocker) + std::string(alpha), 1);
    };
    const auto transparent = shadow(R"("float alpha" 0
)");
    equivalent(transparent, clear, "Transparent blocker must match the clear shadow query");
    require(total(shadow(R"("float alpha" 1
)")) == 0,
            "Opaque shadow blocker must cover the entire light aperture");
    const auto blocked = shadow(R"("float alpha" 0.35
)");
    for (std::size_t i = 0; i < clear.size(); ++i) {
        require(blocked[i].radiance <= clear[i].radiance,
                "Shadow coverage increased path radiance");
    }
    check_mean(blocked, clear, 0.65, "Fractional shadow path mean");

    const auto invisible = trace(path, scene + R"("float alpha" 0
)",
                                 1);
    require(total(invisible) > 0, "Invisible constant emitter retains full direct estimate");
    check_mean(invisible, clear, 1, "Invisible emitter and ordinary light mean");
    const auto absent =
        trace(path, R"(Texture "zero" "float" "imagemap" "string filename" "assets/zero.png"
  "string filter" "point"
)" + scene + R"("texture alpha" "zero"
)",
              1);
    require(total(absent) == 0, "An all-zero point image does not emit");
}

void detail_controls(const fs::path &path, std::span<const RenderSample> clear) {
    constexpr unsigned count = 2048;
    const auto test = [&](string_view fields, string_view textures = {}) {
        return trace(path, std::string(textures) + lit_scene(fields), 1, count);
    };
    equivalent(test(R"("float displacement" 0
)"),
               clear.first(count), "Zero bump changes ordinary diffuse transport", 1e-6);
    equivalent(test(R"("string normalmap" "assets/normal-flat.png"
)"),
               clear.first(count), "Flat native normal map changes diffuse transport", 1e-6);
    const auto tilted = test(R"("string normalmap" "assets/normal-tilted.png"
)");
    require(total(tilted) > 1.2 * total(clear.first(count)),
            "Tilt toward the lamp must increase light");
    const auto bumped = test(R"("texture displacement" "ux"
)",
                             R"(Texture "ux" "float" "bilerp" "float v00" 0 "float v01" 0
  "float v10" 1.5 "float v11" 1.5
)");
    require(total(bumped) > 1.2 * total(clear.first(count)),
            "The controlled displacement gradient must tilt shading toward the lamp");
    equivalent(test(R"("string normalmap" "assets/normal-tilted.png" "float displacement" 100
)"),
               tilted, "An authored normal map takes precedence over displacement");
}

void combined_consumers(const fs::path &path) {
    const auto body = std::string(R"(
LightSource "infinite" "string filename" "assets/environment.png" "float scale" 10
Texture "color" "spectrum" "mix" "rgb tex1" [.2 .6 .3] "rgb tex2" [.7 .1 .4] "float amount" .4
Texture "bump" "float" "bilerp" "float v00" 0 "float v01" 0 "float v10" .3 "float v11" .3
Texture "visible" "float" "imagemap" "string filename" "assets/white.png" "string filter" "point"
Material "diffuse" "texture reflectance" "color" "texture displacement" "bump"
  "string normalmap" "assets/normal-tilted.png"
)") + std::string(receiver) +
                      R"( "float alpha" .5
Material "diffuse" "rgb reflectance" [0 0 0]
AreaLightSource "diffuse" "string filename" "assets/emission.png" "float scale" 100 "bool twosided" true
Shape "trianglemesh" "point3 P" [1.2 -.6 1 2.2 -.6 1 2.2 .6 1 1.2 .6 1]
  "integer indices" [0 1 2 0 2 3] "point2 uv" [0 0 1 0 1 1 0 1] "texture alpha" "visible"
)";
    auto scene = load(path, body, 2);
    const auto check_partitions = [&] {
        auto renderer = build(scene);
        const auto whole = samples(*renderer, 257);
        auto partitioned = samples(*renderer, 129, 0, 1);
        const auto tail = samples(*renderer, 128, 129, 1);
        partitioned.insert(partitioned.end(), tail.begin(), tail.end());
        equivalent(whole, partitioned,
                   "Combined texture consumers changed across batch/group partitions");
        require(total(whole) > 0, "Combined image lights must contribute radiance");
    };
    check_partitions();
    const auto replace = [&](std::uint32_t index, std::array<float, 3> rgb) {
        DecodedImage source{.width = 1,
                            .height = 1,
                            .rgb = {},
                            .sample_format = ImageSampleFormat::float32,
                            .file_format = ImageFileFormat::exr,
                            .floating_rgb = {rgb.begin(), rgb.end()}};
        const auto &previous = *scene.image_textures[index].image;
        auto image = previous.native_base
                         ? build_image_base(source, ImageEncoding::linear, previous.wrap)
                         : build_image_pyramid(source, ImageEncoding::linear, previous.wrap);
        require(image.value.has_value(), image.diagnostic);
        scene.image_textures[index].image = std::make_shared<ImagePyramid>(std::move(*image.value));
    };
    replace(scene.environment_image, {100000.125f, 80000.25f, 70000.5f});
    scene.environment_image_scale *= 1e-5f;
    for (auto &emitter : scene.area_emissions) {
        if (emitter.image_texture != UINT32_MAX) {
            replace(emitter.image_texture, {70000.5f, 80000.25f, 100000.125f});
            emitter.image_scale *= 1e-5f;
        }
    }
    for (const auto &material : scene.materials) {
        if (material.normal_map != UINT32_MAX) {
            replace(material.normal_map, {.2f, .5f, 1.f});
        }
    }
    check_partitions();

    // A finite HDR source can decode and upload but define an invalid normal.
    // The failed dispatch must not commit even a prefix of its sample batch.
    for (const auto &material : scene.materials) {
        if (material.normal_map != UINT32_MAX) {
            replace(material.normal_map, {.5f, .5f, .5f});
        }
    }
    const auto failed = render_progressive(
        scene, {.width = 1, .height = 1, .samples = 64, .batch_size = 64, .seed = seed});
    require(failed.status == GeometryStatus::failure && failed.film.pixels.size() == 1 &&
                failed.film.pixels.front().count == 0,
            "Invalid FLOAT normal map committed a partial or substituted sample batch");
}

void hdr_scalar_failure(const fs::path &path) {
    const auto body = std::string(R"(Texture "height" "float" "imagemap"
  "string filename" "assets/white.png" "string filter" "bilinear"
  "float uscale" 0 "float vscale" 0 "float udelta" .25 "float vdelta" .5
  "bool invert" true
)") + lit_scene(R"("texture displacement" "height"
)");
    for (const float amplitude : {1.2e38f, 1e38f}) {
        auto scene = load(path, body, 1);
        require(scene.image_textures.size() == 1, "Expected one scalar image");
        DecodedImage source{
            .width = 2,
            .height = 1,
            .rgb = {},
            .sample_format = ImageSampleFormat::float32,
            .file_format = ImageFileFormat::exr,
            .floating_rgb = {amplitude, amplitude, amplitude, -amplitude, -amplitude, -amplitude}};
        auto pyramid = build_image_pyramid(source, ImageEncoding::linear, ImageWrap::clamp);
        if (!pyramid.value)
            throw std::runtime_error(pyramid.diagnostic);
        scene.image_textures.front().image =
            std::make_shared<ImagePyramid>(std::move(*pyramid.value));
        // Cover overflow both in RGB mean conversion and after authored scale.
        scene.image_textures.front().scale = amplitude > 1e38f ? 0 : 4;
        const auto failed = render_progressive(
            scene, {.width = 1, .height = 1, .samples = 8, .batch_size = 8, .seed = seed});
        require(failed.status == GeometryStatus::failure && failed.film.pixels.size() == 1 &&
                    failed.film.pixels.front().count == 0,
                "Scalar inversion hid HDR overflow or committed a partial sample batch");
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 3, "Expected scratch and core texture fixture directories");
        bool no_device = false;
        bool no_ray_tracing = false;
        for (int i = 3; i < argc; ++i) {
            const string_view option(argv[i]);
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown Metal core texture test option");
            no_device |= option == "--allow-no-device";
            no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        const fs::path scratch(argv[1]), fixtures(argv[2]);
        fs::create_directories(scratch / "assets");
        for (const auto name : {"white.png", "zero.png", "gray-pattern.png", "normal-flat.png",
                                "normal-tilted.png", "environment.png", "emission.png"}) {
            fs::copy_file(fixtures / "assets" / name, scratch / "assets" / name,
                          fs::copy_options::overwrite_existing);
        }
        const auto path = scratch / "core.pbrt";
        auto baseline = build_metal_renderer(load(path, lit_scene(), 1));
        if ((baseline.status == GeometryStatus::no_device && no_device) ||
            (baseline.status == GeometryStatus::no_ray_tracing && no_ray_tracing)) {
            std::cout << "SKIP Metal core textures. " << baseline.diagnostic << '\n';
            return 77;
        }
        require(baseline.status == GeometryStatus::success && baseline.scene, baseline.diagnostic);
        const auto clear = samples(*baseline.scene, trials);
        require(total(clear) > 0, "Unmasked scene must establish positive directional light");
        graph_controls(path);
        primary_alpha(path);
        spatial_instance_alpha(path);
        shadows_and_emitters(path, clear);
        detail_controls(path, clear);
        combined_consumers(path);
        hdr_scalar_failure(path);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Core texture render: " << error.what() << '\n';
        return 1;
    }
}
