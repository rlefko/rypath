// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"
#include "rgb_spectrum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace rypath;
using Rgb = std::array<float, 3>;

constexpr std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
constexpr std::array<float, 2> fixed_uv{0.43f, 0.61f};
constexpr std::array<std::array<std::uint8_t, 3>, 4> srgb_pixels{
    {{210, 20, 50}, {25, 220, 70}, {30, 80, 235}, {230, 150, 15}}};
constexpr std::uint64_t seed = 0x913b6a1f7957c251ULL;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

RenderScene plane() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{{0, 0, 0}, {-0.5f, 0.5f, 1}, {1, 0, 0}, {0, -1, 0}};
    scene.max_depth = 1;
    scene.environment.fill(1.0f);
    scene.materials = {{.kind = SpectralMaterialKind::constant, .constant = 1.0f}};
    scene.instance_materials = {0};
    scene.geometry.meshes = {{
        .positions = {{-4, -4, 2}, {4, -4, 2}, {-4, 4, 2}, {4, 4, 2}},
        .triangles = {{0, 1, 2}, {2, 1, 3}},
        .uvs = {fixed_uv, fixed_uv, fixed_uv, fixed_uv},
    }};
    scene.geometry.instances = {{.transform = {}}};
    return scene;
}

DecodedImage color_pixels() {
    DecodedImage image{2, 2, {}};
    for (const auto &pixel : srgb_pixels) {
        image.rgb.insert(image.rgb.end(), pixel.begin(), pixel.end());
    }
    return image;
}

DecodedImage varying_pixels() {
    DecodedImage image{8, 8, {}};
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            image.rgb.insert(image.rgb.end(), {static_cast<std::uint8_t>(23 * x + 3 * y),
                                               static_cast<std::uint8_t>(7 * x + 20 * y),
                                               static_cast<std::uint8_t>(250 - 12 * x - 9 * y)});
        }
    }
    return image;
}

RenderScene textured_plane(const DecodedImage &pixels, ImageEncoding encoding,
                           bool varying_coordinates, float uv_scale) {
    auto scene = plane();
    auto &mesh = scene.geometry.meshes.front();
    if (varying_coordinates) {
        mesh.uvs = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    }
    scene.instance_texture_frames.resize(1);
    const auto diagnostic = prepare_texture_triangle_frames(mesh, identity, identity,
                                                            scene.instance_texture_frames.front());
    require(diagnostic.empty(), diagnostic);
    auto pyramid = build_image_pyramid(pixels, encoding, ImageWrap::repeat);
    if (!pyramid.value) {
        throw std::runtime_error(pyramid.diagnostic);
    }
    scene.image_textures.push_back(
        {.image = std::make_shared<ImagePyramid>(std::move(pyramid.value.value())),
         .uscale = uv_scale,
         .vscale = uv_scale});
    scene.materials.front() = {.kind = SpectralMaterialKind::image};
    return scene;
}

RgbSpectrum reconstruct(const Rgb &rgb) {
    const auto result = reconstruct_srgb(rgb);
    if (!result) {
        throw std::runtime_error("Could not reconstruct linear sRGB control");
    }
    return result.value();
}

RenderScene control(const Rgb &rgb) {
    const auto spectrum = reconstruct(rgb);
    auto scene = plane();
    scene.materials.front() = {.kind = spectrum.is_constant ? SpectralMaterialKind::constant
                                                            : SpectralMaterialKind::rgb_sigmoid,
                               .constant = spectrum.constant,
                               .coefficients = spectrum.coefficients};
    return scene;
}

std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    return std::move(built.scene);
}

RenderSamplesResult sample(MetalRenderScene &scene, const RenderRequest &request) {
    auto result = scene.sample(request);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == std::size_t{request.pixel_count} * request.sample_count &&
                result.guard_count > 0,
            "Wrong image sample count or missing output guard coverage");
    return result;
}

float decode_srgb(double sample) {
    return static_cast<float>(sample <= 0.04045 ? sample / 12.92
                                                : std::pow((sample + 0.055) / 1.055, 2.4));
}

void same_path(const RenderSample &observed, const RenderSample &expected) {
    require(observed.pixel == expected.pixel && observed.sample == expected.sample &&
                observed.wavelength == expected.wavelength && observed.scatterings == 1 &&
                expected.scatterings == 1 && observed.random_words == expected.random_words,
            "Image and control must shade the same hit and wavelength");
}

void bilinear(MetalRenderScene &textured) {
    constexpr RenderRequest request{.width = 8,
                                    .height = 8,
                                    .seed = seed,
                                    .pixel_count = 64,
                                    .sample_count = 128,
                                    .total_samples = 128,
                                    .threadgroup_width = 32};
    const auto no_budget =
        textured.sample({.width = 1, .height = 1, .pixel_count = 1, .sample_count = 1});
    require(no_budget.status == GeometryStatus::failure && no_budget.samples.empty() &&
                no_budget.guard_count == 0 &&
                no_budget.diagnostic.find("budget") != std::string::npos,
            "Image render dispatched without a fixed sample budget");

    // The 2x2 lookup converts normalized authored UVs to texel centers, with
    // file rows starting at the top. Derivatives stay below the 1x1 threshold.
    const double dx = 2.0 * fixed_uv[0] - 0.5;
    const double dy = 2.0 * (1.0 - fixed_uv[1]) - 0.5;
    const std::array weights{(1 - dx) * (1 - dy), dx * (1 - dy), (1 - dx) * dy, dx * dy};
    std::array<double, 3> linear{};
    std::array<double, 3> encoded{};
    std::array<RgbSpectrum, 4> texel_spectra{};
    for (std::size_t i = 0; i < weights.size(); ++i) {
        Rgb texel{};
        for (std::size_t c = 0; c < texel.size(); ++c) {
            const double byte = srgb_pixels[i][c] / 255.0;
            texel[c] = decode_srgb(byte);
            linear[c] += weights[i] * texel[c];
            encoded[c] += weights[i] * byte;
        }
        texel_spectra[i] = reconstruct(texel);
    }
    const Rgb expected_rgb{static_cast<float>(linear[0]), static_cast<float>(linear[1]),
                           static_cast<float>(linear[2])};
    const Rgb wrong_encoded_rgb{decode_srgb(encoded[0]), decode_srgb(encoded[1]),
                                decode_srgb(encoded[2])};
    const auto native = sample(textured, request);
    const auto expected = sample(*build(control(expected_rgb)), request);
    const auto wrong_encoded = sample(*build(control(wrong_encoded_rgb)), request);
    const auto whites = sample(*build(plane()), request);
    double max_error = 0;
    double max_encoded_error = 0;
    double max_spectrum_error = 0;
    for (std::size_t i = 0; i < native.samples.size(); ++i) {
        const auto &actual = native.samples[i];
        const auto &reference = expected.samples[i];
        const auto &white = whites.samples[i];
        same_path(actual, reference);
        const double error = std::abs(double{actual.radiance} - reference.radiance);
        max_error = std::max(max_error, error);
        require(white.radiance > 0 && error <= 1e-7 + 3e-5 * white.radiance,
                "Bilinear image radiance disagrees with the same-seed linear RGB control");
        max_encoded_error =
            std::max(max_encoded_error,
                     std::abs(double{reference.radiance} - wrong_encoded.samples[i].radiance));
        double spectral_blend = 0;
        for (std::size_t tap = 0; tap < weights.size(); ++tap) {
            spectral_blend += weights[tap] * evaluate_srgb(texel_spectra[tap], actual.wavelength);
        }
        max_spectrum_error = std::max(
            max_spectrum_error, std::abs(reference.radiance - white.radiance * spectral_blend));
    }
    require(max_encoded_error > 0.005 && max_spectrum_error > 0.005,
            "Bilinear fixture does not distinguish filtering in encoded RGB or wavelength space");
    std::cout << "Image plane maximum path error " << max_error << ", encoded-blend difference "
              << max_encoded_error << ", spectral-blend difference " << max_spectrum_error << '\n';
}

void coarsest_mip() {
    constexpr RenderRequest request{.width = 8,
                                    .height = 8,
                                    .seed = seed,
                                    .pixel_count = 64,
                                    .sample_count = 33,
                                    .total_samples = 33};
    // The mean 8-bit channels of varying_pixels round to (91, 95, 177).
    constexpr Rgb coarse{91 / 255.0f, 95 / 255.0f, 177 / 255.0f};
    const auto expected = sample(*build(control(coarse)), request);
    double max_error = 0;
    for (float scale : {1024.0f, 3e38f}) {
        auto scene = textured_plane(varying_pixels(), ImageEncoding::linear, true, scale);
        if (scale > 1024.0f) {
            // The mapped coordinates overflow binary32. Coarsest-mip selection
            // depends only on the footprint and never needs these coordinates.
            scene.image_textures.front().udelta = scale;
            scene.image_textures.front().vdelta = scale;
        }
        const auto actual = sample(*build(scene), request);
        for (std::size_t i = 0; i < actual.samples.size(); ++i) {
            same_path(actual.samples[i], expected.samples[i]);
            max_error = std::max(max_error, std::abs(double{actual.samples[i].radiance} -
                                                     expected.samples[i].radiance));
        }
    }
    require(max_error < 3e-5, "Large primary footprints did not select the coarsest mip");
    std::cout << "Coarsest mip maximum path error " << max_error << '\n';
}

Film progressive(const RenderScene &scene, const RenderSettings &settings) {
    auto result = render_progressive(scene, settings);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(
        result.film.pixels.size() == std::size_t{settings.width} * settings.height &&
            std::ranges::all_of(result.film.pixels,
                                [&](const auto &pixel) { return pixel.count == settings.samples; }),
        "Progressive image render lost pixel or sample counts");
    return std::move(result.film);
}

void batch_partition() {
    const auto scene = textured_plane(varying_pixels(), ImageEncoding::linear, true, 40);
    // 2,048 pixels times 65 samples exceeds the 65,536-path dispatch limit.
    RenderSettings settings{
        .width = 64, .height = 32, .samples = 65, .batch_size = 65, .seed = seed};
    const auto full = progressive(scene, settings);
    settings.batch_size = 7;
    require(full == progressive(scene, settings),
            "Batch partition changed raw image-textured film");
    settings.batch_size = 1;
    require(full == progressive(scene, settings),
            "Single-sample batches changed raw image-textured film");

    auto renderer = build(scene);
    RenderRequest request{.width = 64,
                          .height = 32,
                          .seed = seed,
                          .pixel_count = 2048,
                          .sample_count = 1,
                          .total_samples = 65};
    const auto fine = sample(*renderer, request);
    request.total_samples = 1;
    const auto coarse = sample(*renderer, request);
    require(std::ranges::any_of(fine.samples,
                                [&](const auto &s) {
                                    return std::abs(s.radiance - coarse.samples[s.pixel].radiance) >
                                           1e-3f;
                                }),
            "Batch fixture does not exercise sample-budget-dependent LOD");
}

void translated_footprint() {
    auto scene = textured_plane(varying_pixels(), ImageEncoding::linear, true, 8);
    constexpr RenderRequest request{.width = 8,
                                    .height = 8,
                                    .seed = seed,
                                    .pixel_count = 64,
                                    .sample_count = 33,
                                    .total_samples = 33};
    const auto centered = sample(*build(scene), request);
    if (!scene.raster_camera) {
        throw std::runtime_error("Translated footprint requires a raster camera");
    }
    scene.raster_camera->origin.x = 1e8f;
    scene.geometry.instances.front().transform.values[3] = 1e8f;
    const auto translated = sample(*build(scene), request);
    double maximum = 0;
    for (std::size_t i = 0; i < centered.samples.size(); ++i) {
        same_path(translated.samples[i], centered.samples[i]);
        maximum = std::max(maximum, std::abs(double{translated.samples[i].radiance} -
                                             centered.samples[i].radiance));
    }
    require(maximum < 3e-5, "Camera-relative translation changed image texture footprints");
    std::cout << "Translated footprint maximum path error " << maximum << '\n';
}

void validation() {
    const auto valid = textured_plane(color_pixels(), ImageEncoding::srgb, false, 1);
    auto invalid = [](const RenderScene &scene, std::string_view diagnostic) {
        const auto result = build_metal_renderer(scene);
        require(result.status == GeometryStatus::failure && !result.scene &&
                    result.diagnostic.find(diagnostic) != std::string::npos,
                "Invalid image texture scene reached the renderer");
    };
    auto wrong_index = valid;
    wrong_index.materials.front().image_texture = 1;
    invalid(wrong_index, "invalid texture");

    auto wrong_layout = valid;
    auto pyramid = std::make_shared<ImagePyramid>(*valid.image_textures.front().image);
    pyramid->levels.back().offset += 1;
    wrong_layout.image_textures.front().image = std::move(pyramid);
    invalid(wrong_layout, "mip layout");

    auto wrong_frames = valid;
    wrong_frames.instance_texture_frames.front().pop_back();
    invalid(wrong_frames, "triangle frame");

    auto wrong_camera = valid;
    wrong_camera.texture_camera.camera_from_world.fill(0);
    invalid(wrong_camera, "inverse pair");
    wrong_camera = valid;
    wrong_camera.texture_camera.world_from_camera[0] = 2;
    invalid(wrong_camera, "inverse pair");

    auto demo = make_demo_scene();
    demo.image_textures.emplace_back();
    invalid(demo, "Image textures require PBRT");
}

} // namespace

int main(int argc, char **argv) {
    try {
        bool allow_no_device = false;
        bool allow_no_ray_tracing = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option(argv[i]);
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown image render test option");
            allow_no_device |= option == "--allow-no-device";
            allow_no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        // Texture source and CPU pyramid die before the first dispatch.
        const auto native =
            build_metal_renderer(textured_plane(color_pixels(), ImageEncoding::srgb, false, 1));
        if ((native.status == GeometryStatus::no_device && allow_no_device) ||
            (native.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
            std::cout << "SKIP Metal image render. " << native.diagnostic << '\n';
            return 77;
        }
        require(native.status == GeometryStatus::success && native.scene, native.diagnostic);
        bilinear(*native.scene);
        coarsest_mip();
        batch_partition();
        translated_footprint();
        validation();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
