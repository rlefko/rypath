// SPDX-License-Identifier: GPL-3.0-only

#include "ewa_reference.hpp"
#include "metal_render.hpp"
#include "rgb_spectrum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

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

DecodedImage gray16(std::initializer_list<std::uint16_t> samples) {
    DecodedImage image{static_cast<std::uint32_t>(samples.size()), 1, {}, 16};
    for (const auto value : samples) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            image.rgb.push_back(static_cast<std::uint8_t>(value >> 8));
            image.rgb.push_back(static_cast<std::uint8_t>(value));
        }
    }
    return image;
}

RenderScene textured_plane(const DecodedImage &pixels, ImageEncoding encoding,
                           bool varying_coordinates, float uv_scale,
                           ImageWrap wrap = ImageWrap::repeat) {
    auto scene = plane();
    auto &mesh = scene.geometry.meshes.front();
    if (varying_coordinates) {
        mesh.uvs = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    }
    scene.instance_texture_frames.resize(1);
    const auto diagnostic = prepare_texture_triangle_frames(mesh, identity, identity,
                                                            scene.instance_texture_frames.front());
    require(diagnostic.empty(), diagnostic);
    auto pyramid = build_image_pyramid(pixels, encoding, wrap);
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

SpectralMaterial rgb_material(const Rgb &rgb) {
    const auto spectrum = reconstruct(rgb);
    return {.kind = spectrum.is_constant ? SpectralMaterialKind::constant
                                         : SpectralMaterialKind::rgb_sigmoid,
            .constant = spectrum.constant,
            .coefficients = spectrum.coefficients};
}

RenderScene control(const Rgb &rgb) {
    auto scene = plane();
    scene.materials.front() = rgb_material(rgb);
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

void same_path(const RenderSample &observed, const RenderSample &expected,
               unsigned scatterings = 1) {
    require(observed.pixel == expected.pixel && observed.sample == expected.sample &&
                observed.wavelength == expected.wavelength && observed.scatterings == scatterings &&
                expected.scatterings == scatterings &&
                observed.random_words == expected.random_words,
            "Image and control must shade the same hit and wavelength");
}

void indexed_rgb8_equivalence() {
    // Project-authored 4x2, two-bit indexed PNG with rows [0, 1, 2, 2] and
    // [3, 0, 1, 0]. The first two palette entries have alpha 0 and 127.
    constexpr auto source = std::to_array<std::uint8_t>({
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x02, 0x03, 0x00, 0x00,
        0x00, 0x02, 0xc6, 0x95, 0xf0, 0x00, 0x00, 0x00, 0x0c, 0x50, 0x4c, 0x54, 0x45, 0xd2,
        0x14, 0x32, 0x19, 0xdc, 0x46, 0x1e, 0x50, 0xeb, 0xe6, 0x96, 0x0f, 0x9b, 0x37, 0x58,
        0x95, 0x00, 0x00, 0x00, 0x02, 0x74, 0x52, 0x4e, 0x53, 0x00, 0x7f, 0xb6, 0x29, 0xa1,
        0x95, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x90, 0x62,
        0x38, 0x02, 0x00, 0x01, 0x16, 0x00, 0xdf, 0xb9, 0x43, 0x15, 0xc1, 0x00, 0x00, 0x00,
        0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    });
    const DecodedImage rgb8{4, 2, {210, 20,  50, 25,  220, 70, 30, 80,  235, 30,  80, 235,
                                   230, 150, 15, 210, 20,  50, 25, 220, 70,  210, 20, 50}};
    const auto decoded = decode_png(source);
    if (!decoded.value) {
        throw std::runtime_error(decoded.diagnostic);
    }
    const auto &indexed = decoded.value.value();

    struct Case {
        ImageFilter filter;
        ImageWrap wrap;
        float scale;
        bool patch;
    };
    constexpr std::array cases{
        Case{ImageFilter::point, ImageWrap::black, 4, false},
        Case{ImageFilter::bilinear, ImageWrap::clamp, 4, true},
        Case{ImageFilter::trilinear, ImageWrap::repeat, 24, false},
        Case{ImageFilter::ewa, ImageWrap::repeat, 48, true},
    };
    constexpr RenderRequest request{.width = 8,
                                    .height = 8,
                                    .seed = seed,
                                    .pixel_count = 64,
                                    .sample_count = 16,
                                    .total_samples = 16};
    for (const auto encoding : {ImageEncoding::srgb, ImageEncoding::linear}) {
        for (const auto &test : cases) {
            const auto render = [&](const DecodedImage &pixels) {
                auto scene = textured_plane(pixels, encoding, true, test.scale, test.wrap);
                auto &texture = scene.image_textures.front();
                texture.filter = test.filter;
                texture.vscale *= 1.5f;
                texture.udelta = 0.3f - texture.uscale / 2;
                texture.vdelta = 0.4f - texture.vscale / 2;
                if (test.patch) {
                    auto &mesh = scene.geometry.meshes.front();
                    mesh.triangles.clear();
                    mesh.patches = {{0, 1, 2, 3}};
                    scene.instance_texture_frames.front().clear();
                }
                return sample(*build(scene), request);
            };
            const auto expected = render(rgb8);
            const auto actual = render(indexed);
            bool has_signal = false;
            for (std::size_t i = 0; i < actual.samples.size(); ++i) {
                const auto &a = actual.samples[i];
                const auto &e = expected.samples[i];
                same_path(a, e, e.scatterings);
                require(a.radiance == e.radiance, "Indexed and expanded RGB8 radiance must match");
                has_signal |= e.scatterings == 1 && e.radiance > 0;
            }
            require(has_signal, "Indexed equivalence must exercise textured scattering");
        }
    }
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

void trilinear() {
    // Each source 2x2 block is uniform. The 2x2 mip keeps these colors
    // exactly, and the sRGB encoding of their linear mean is (138,153,135).
    constexpr std::array<std::array<std::uint8_t, 3>, 4> blocks{
        {{245, 110, 120}, {75, 230, 70}, {45, 45, 220}, {30, 150, 25}}};
    constexpr std::array<std::uint8_t, 3> mean_bytes{138, 153, 135};
    DecodedImage image{4, 4, {}};
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            const auto &texel = blocks[2 * (y / 2) + x / 2];
            image.rgb.insert(image.rgb.end(), texel.begin(), texel.end());
        }
    }

    constexpr RenderRequest request{.width = 1,
                                    .height = 1,
                                    .seed = seed,
                                    .pixel_count = 1,
                                    .sample_count = 512,
                                    .total_samples = 512};
    constexpr float scale = 1.65f;
    // At the pixel center the camera ray is (0,0,1) and the normalized
    // differential rays are (1,0,1)/sqrt(2) and (0,-1,1)/sqrt(2).
    // At z=2 their scaled offset is 2/(7*sqrt(2)+1) world units. The plane
    // spans eight world units per UV unit, so map to width 2^(-1/2) and LOD 1.5.
    const float uv_scale = static_cast<float>(14 + std::sqrt(2.0));
    auto scene = textured_plane(image, ImageEncoding::srgb, true, uv_scale, ImageWrap::black);
    // Keep the tiny camera footprint inside one triangle, away from the shared diagonal.
    scene.geometry.instances.front().transform.values[3] = 2;
    scene.filter = {PixelFilterKind::triangle, 0x1p-22f, 0x1p-22f};
    auto &bilinear_texture = scene.image_textures.front();
    bilinear_texture.udelta = 0.25f - 0.25f * uv_scale;
    bilinear_texture.vdelta = 0.75f - 0.5f * uv_scale;
    bilinear_texture.scale = scale;
    bilinear_texture.invert = true;
    auto trilinear_texture = bilinear_texture;
    trilinear_texture.filter = ImageFilter::trilinear;
    // The shader must read descriptor 1 even though both descriptors share mip storage.
    scene.image_textures.push_back(std::move(trilinear_texture));

    Rgb blended_rgb{}, bilinear_rgb{}, clamped_endpoints_rgb{};
    const auto transform = [](float value) { return std::clamp(1 - scale * value, 0.f, 1.f); };
    for (std::size_t c = 0; c < blended_rgb.size(); ++c) {
        // At (s,t)=(1/4,1/4), the lower mip hits its first texel center;
        // black addressing leaves 9/16 of the upper 1x1 mip's value.
        const float lower = decode_srgb(blocks.front()[c] / 255.0);
        const float upper = (9.f / 16) * decode_srgb(mean_bytes[c] / 255.0);
        blended_rgb[c] = transform((lower + upper) * 0.5f);
        bilinear_rgb[c] = transform(lower);
        clamped_endpoints_rgb[c] = (transform(lower) + transform(upper)) * 0.5f;
    }

    for (bool patch : {false, true}) {
        if (patch) {
            auto &mesh = scene.geometry.meshes.front();
            mesh.triangles.clear();
            mesh.patches = {{0, 1, 2, 3}};
            scene.instance_texture_frames.front().clear();
        }
        scene.materials.front().image_texture = 0;
        const auto bilinear = sample(*build(scene), request);
        scene.materials.front().image_texture = 1;
        const auto actual = sample(*build(scene), request);
        auto uniform = scene;
        uniform.image_textures.clear();
        uniform.instance_texture_frames.clear();
        uniform.materials.front() = rgb_material(blended_rgb);
        const auto expected = sample(*build(uniform), request);
        uniform.materials.front() = rgb_material(bilinear_rgb);
        const auto lower = sample(*build(uniform), request);
        uniform.materials.front() = rgb_material(clamped_endpoints_rgb);
        const auto wrong_order = sample(*build(uniform), request);
        double max_error = 0, max_bilinear_error = 0, mip_difference = 0, order_difference = 0;
        for (std::size_t i = 0; i < actual.samples.size(); ++i) {
            const auto &a = actual.samples[i];
            const auto &e = expected.samples[i];
            const auto &b = bilinear.samples[i];
            same_path(a, e);
            same_path(b, lower.samples[i]);
            max_error = std::max(max_error, std::abs(double{a.radiance} - e.radiance));
            max_bilinear_error = std::max(max_bilinear_error,
                                          std::abs(double{b.radiance} - lower.samples[i].radiance));
            mip_difference = std::max(mip_difference, std::abs(double{a.radiance} - b.radiance));
            order_difference = std::max(
                order_difference, std::abs(double{a.radiance} - wrong_order.samples[i].radiance));
        }
        require(max_error < 3e-5 && max_bilinear_error < 3e-5,
                "Filtered image radiance disagrees with the same-seed RGB controls");
        require(mip_difference > 0.02 && order_difference > 0.02,
                "Trilinear fixture does not distinguish mip blending or premature clamping");
        std::cout << (patch ? "Patch" : "Triangle") << " trilinear maximum path error " << max_error
                  << ", bilinear error " << max_bilinear_error << ", mip difference "
                  << mip_difference << ", clamp-order difference " << order_difference << '\n';
    }
}

void point() {
    // Fixed UV (0.43,0.61) maps to file coordinates (0.43,0.39):
    // both round(2*st-0.5) indices are zero, away from every boundary.
    auto scene = textured_plane(color_pixels(), ImageEncoding::srgb, false, 1);
    constexpr float scale = 1.65f;
    auto &texture = scene.image_textures.front();
    texture.scale = scale;
    texture.invert = true;
    auto point_texture = texture;
    point_texture.filter = ImageFilter::point;
    scene.image_textures.push_back(std::move(point_texture));
    Rgb expected_rgb{};
    for (std::size_t c = 0; c < expected_rgb.size(); ++c) {
        expected_rgb[c] =
            std::clamp(1 - scale * decode_srgb(srgb_pixels.front()[c] / 255.0), 0.f, 1.f);
    }
    constexpr RenderRequest request{.width = 8,
                                    .height = 8,
                                    .seed = seed,
                                    .pixel_count = 64,
                                    .sample_count = 128,
                                    .total_samples = 128};
    for (bool patch : {false, true}) {
        if (patch) {
            auto &mesh = scene.geometry.meshes.front();
            mesh.triangles.clear();
            mesh.patches = {{0, 1, 2, 3}};
            scene.instance_texture_frames.front().clear();
        }
        scene.materials.front().image_texture = 0;
        const auto bilinear = sample(*build(scene), request);
        // Both descriptors share storage; point must survive packing at index 1.
        scene.materials.front().image_texture = 1;
        const auto actual = sample(*build(scene), request);
        auto uniform = scene;
        uniform.image_textures.clear();
        uniform.instance_texture_frames.clear();
        uniform.materials.front() = rgb_material(expected_rgb);
        const auto expected = sample(*build(uniform), request);
        uniform.materials.front() = rgb_material({1, 1, 1});
        const auto white = sample(*build(uniform), request);
        double max_error = 0, filter_difference = 0;
        for (std::size_t i = 0; i < actual.samples.size(); ++i) {
            const auto &a = actual.samples[i];
            same_path(a, expected.samples[i]);
            same_path(a, white.samples[i]);
            same_path(a, bilinear.samples[i]);
            const double error = std::abs(double{a.radiance} - expected.samples[i].radiance);
            require(error <= 1e-7 + 3e-5 * white.samples[i].radiance,
                    "Point lookup disagrees with the independent RGB reflectance control");
            max_error = std::max(max_error, error);
            filter_difference = std::max(
                filter_difference, std::abs(double{a.radiance} - bilinear.samples[i].radiance));
        }
        require(filter_difference > 0.02, "Point control does not distinguish bilinear lookup");
        std::cout << (patch ? "Patch" : "Triangle") << " point maximum path error " << max_error
                  << ", bilinear difference " << filter_difference << '\n';
    }
}

void ewa() {
    DecodedImage pixels{8, 8, {}};
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            pixels.rgb.insert(pixels.rgb.end(),
                              {static_cast<std::uint8_t>(y == 2 || y == 3 ? 240 : 20),
                               static_cast<std::uint8_t>(x == 2 || x == 3 ? 180 : 40),
                               static_cast<std::uint8_t>(50 * ((x + 2 * y) % 5))});
        }
    }
    auto scene = textured_plane(pixels, ImageEncoding::srgb, true, 48);
    scene.geometry.instances.front().transform.values[3] = 2;
    // Every float camera sample rounds to the pixel center. Analytic primary
    // differentials are independent of the randomly sampled path wavelength.
    scene.filter = {PixelFilterKind::triangle, 0x1p-27f, 0x1p-27f};
    auto &texture = scene.image_textures.front();
    texture.vscale = 2;
    texture.udelta = -11.71875f;
    texture.vdelta = -0.421875f;
    texture.scale = 1.2f;
    texture.invert = true;
    auto elliptical = texture;
    elliptical.filter = ImageFilter::ewa;
    elliptical.max_anisotropy = 8;
    scene.image_textures.push_back(elliptical);
    auto limited = elliptical;
    limited.max_anisotropy = 4;
    scene.image_textures.push_back(limited);

    // At the center ray (0,0,1), the two offset normalized pinhole rays
    // intersect z=2 after their 1/8 differential scaling at these offsets.
    const double dp = 2 / (7 * std::sqrt(2.) + 1);
    const auto rgb =
        test::ewa_reference(*elliptical.image, 0.28125, 0.421875, 6 * dp, 0, 0, -0.25 * dp, 8);
    Rgb expected_rgb{};
    for (std::size_t c = 0; c < 3; ++c) {
        expected_rgb[c] = static_cast<float>(std::clamp(1 - 1.2 * rgb[c], 0., 1.));
    }
    constexpr RenderRequest request{.width = 1,
                                    .height = 1,
                                    .seed = seed,
                                    .pixel_count = 1,
                                    .sample_count = 512,
                                    .total_samples = 512};
    for (bool patch : {false, true}) {
        if (patch) {
            auto &mesh = scene.geometry.meshes.front();
            mesh.triangles.clear();
            mesh.patches = {{0, 1, 2, 3}};
            scene.instance_texture_frames.front().clear();
        }
        scene.materials.front().image_texture = 1;
        const auto actual = sample(*build(scene), request);
        scene.materials.front().image_texture = 2;
        const auto capped = sample(*build(scene), request);
        scene.materials.front().image_texture = 0;
        scene.image_textures.front().filter = ImageFilter::trilinear;
        const auto isotropic = sample(*build(scene), request);
        auto uniform = scene;
        uniform.image_textures.clear();
        uniform.instance_texture_frames.clear();
        uniform.materials.front() = rgb_material(expected_rgb);
        const auto expected = sample(*build(uniform), request);
        uniform.materials.front() = rgb_material({1, 1, 1});
        const auto white = sample(*build(uniform), request);
        double error = 0, filter_difference = 0, cap_difference = 0;
        for (std::size_t i = 0; i < actual.samples.size(); ++i) {
            const auto &a = actual.samples[i];
            same_path(a, expected.samples[i]);
            same_path(a, white.samples[i]);
            same_path(a, capped.samples[i]);
            same_path(a, isotropic.samples[i]);
            const double delta = std::abs(double{a.radiance} - expected.samples[i].radiance);
            require(delta <= 1e-7 + 3e-5 * white.samples[i].radiance,
                    "EWA transport disagrees with the independent RGB control");
            error = std::max(error, delta);
            filter_difference = std::max(
                filter_difference, std::abs(double{a.radiance} - isotropic.samples[i].radiance));
            cap_difference =
                std::max(cap_difference, std::abs(double{a.radiance} - capped.samples[i].radiance));
        }
        require(filter_difference > 0.02 && cap_difference > 0.02,
                "EWA transport must distinguish isotropic filtering and anisotropy limits");
        std::cout << (patch ? "Patch" : "Triangle") << " EWA maximum path error " << error
                  << ", trilinear difference " << filter_difference << ", anisotropy difference "
                  << cap_difference << '\n';
    }
}

void add_secondary_front(RenderScene &scene) {
    // Camera rays see the white front plane at z=2. Only an indirect ray
    // can hit the textured panel behind the camera.
    scene.max_depth = 2;
    auto &panel = scene.geometry.meshes.front();
    for (auto &position : panel.positions) {
        position.z = -1;
    }
    const auto front = plane();
    scene.geometry.meshes.push_back(front.geometry.meshes.front());
    scene.geometry.instances.push_back({1, {}});
    scene.materials.push_back(front.materials.front());
    scene.instance_materials.push_back(1);
    scene.instance_texture_frames.emplace_back();
}

void filtered_secondary(ImageFilter filter) {
    auto scene = textured_plane(varying_pixels(), ImageEncoding::srgb, true, 24);
    if (filter == ImageFilter::ewa) {
        scene.image_textures.front().vscale = 3;
    }
    add_secondary_front(scene);

    constexpr RenderRequest request{.width = 8,
                                    .height = 8,
                                    .seed = seed,
                                    .pixel_count = 64,
                                    .sample_count = 33,
                                    .total_samples = 33};
    const auto bilinear = sample(*build(scene), request);
    scene.image_textures.front().filter = filter;
    const auto filtered = sample(*build(scene), request);
    double difference = 0;
    std::size_t secondary_paths = 0, changed_secondary_paths = 0;
    for (std::size_t i = 0; i < bilinear.samples.size(); ++i) {
        const auto &b = bilinear.samples[i];
        const auto &t = filtered.samples[i];
        same_path(b, t, b.scatterings);
        const double delta = std::abs(double{b.radiance} - t.radiance);
        if (b.scatterings == 2) {
            ++secondary_paths;
            changed_secondary_paths += delta > 0.001;
            difference = std::max(difference, delta);
        } else {
            require(b.scatterings == 1 && delta == 0,
                    "A path without a secondary surface hit changed its radiance");
        }
    }
    require(secondary_paths > 100 && changed_secondary_paths > 100 && difference > 0.01,
            "Secondary texture fixture did not distinguish the selected filter");
    std::cout << "Secondary "
              << (filter == ImageFilter::point ? "point"
                  : filter == ImageFilter::ewa ? "EWA"
                                               : "trilinear")
              << " changed " << changed_secondary_paths << " of " << secondary_paths
              << " two-hit paths by more than 0.001, maximum " << difference << '\n';
}

void half_mip_overshoot() {
    auto scene = textured_plane(gray16({0, 65535, 65535}), ImageEncoding::linear, false, 0,
                                ImageWrap::clamp);
    auto &texture = scene.image_textures.front();
    texture.udelta = 0.625f;
    texture.vdelta = 0.5f;
    texture.scale = 0.5f;
    // Pinned PBRT's clamped 3-to-4 resize stores 0x3c53 at x=2 before
    // texture scaling. Clamping the mip or lookup first would give 0.5.
    constexpr float expected_gray = 0x1.14cp-1f;
    constexpr RenderRequest request{.width = 1,
                                    .height = 1,
                                    .seed = seed,
                                    .pixel_count = 1,
                                    .sample_count = 128,
                                    .total_samples = 128};
    for (const bool patch : {false, true}) {
        if (patch) {
            auto &mesh = scene.geometry.meshes.front();
            mesh.triangles.clear();
            mesh.patches = {{0, 1, 2, 3}};
            scene.instance_texture_frames.front().clear();
        }
        auto uniform = scene;
        uniform.image_textures.clear();
        uniform.instance_texture_frames.clear();
        uniform.materials.front() = rgb_material({expected_gray, expected_gray, expected_gray});
        const auto expected = sample(*build(uniform), request);
        uniform.materials.front() = rgb_material({0.5f, 0.5f, 0.5f});
        const auto clipped = sample(*build(uniform), request);
        double max_error = 0, clipping_difference = 0;
        for (const auto filter : {ImageFilter::point, ImageFilter::bilinear, ImageFilter::trilinear,
                                  ImageFilter::ewa}) {
            texture.filter = filter;
            const auto actual = sample(*build(scene), request);
            for (std::size_t i = 0; i < actual.samples.size(); ++i) {
                const auto &a = actual.samples[i];
                same_path(a, expected.samples[i]);
                same_path(a, clipped.samples[i]);
                max_error = std::max(max_error,
                                     std::abs(double{a.radiance} - expected.samples[i].radiance));
                clipping_difference =
                    std::max(clipping_difference,
                             std::abs(double{a.radiance} - clipped.samples[i].radiance));
            }
        }
        require(max_error < 3e-5 && clipping_difference > 0.02,
                "Scaled half mip did not preserve resize overshoot");
        std::cout << (patch ? "Patch" : "Triangle") << " half mip maximum path error " << max_error
                  << ", early-clipping difference " << clipping_difference << '\n';
    }
}

void mixed_texture_secondary() {
    auto scene = textured_plane(gray16({0x9abc}), ImageEncoding::linear, false, 1);
    add_secondary_front(scene);
    const auto front = textured_plane({1, 1, {128, 128, 128}}, ImageEncoding::linear, false, 1);
    scene.image_textures.insert(scene.image_textures.begin(), front.image_textures.front());
    scene.image_textures.back().filter = ImageFilter::ewa;
    scene.materials = {{.kind = SpectralMaterialKind::image, .image_texture = 1},
                       {.kind = SpectralMaterialKind::image, .image_texture = 0}};
    scene.instance_texture_frames.back() = front.instance_texture_frames.front();

    // The front gets the one-word encoded8 pyramid. The indirect panel uses a
    // half pyramid starting at the next word, with 0x9abc / 65535 rounded to half.
    auto uniform = scene;
    uniform.image_textures.clear();
    uniform.instance_texture_frames.clear();
    uniform.materials = {{.kind = SpectralMaterialKind::constant, .constant = 0x1.358p-1f},
                         {.kind = SpectralMaterialKind::constant, .constant = 128 / 255.0f}};
    constexpr RenderRequest request{.width = 8,
                                    .height = 8,
                                    .seed = seed,
                                    .pixel_count = 64,
                                    .sample_count = 33,
                                    .total_samples = 33};
    const auto actual = sample(*build(scene), request);
    const auto expected = sample(*build(uniform), request);
    double max_error = 0;
    std::size_t indirect = 0;
    for (std::size_t i = 0; i < actual.samples.size(); ++i) {
        const auto &a = actual.samples[i];
        same_path(a, expected.samples[i], a.scatterings);
        indirect += a.scatterings == 2;
        max_error =
            std::max(max_error, std::abs(double{a.radiance} - expected.samples[i].radiance));
    }
    require(indirect > 100 && indirect < actual.samples.size() && max_error < 3e-5,
            "Mixed-format packed textures disagreed with the secondary-hit control");
    std::cout << "Mixed textures maximum path error " << max_error << " across " << indirect
              << " two-hit paths\n";
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

void batch_partition(const DecodedImage &pixels) {
    auto scene = textured_plane(pixels, ImageEncoding::linear, true, 40);
    for (const auto filter :
         {ImageFilter::bilinear, ImageFilter::trilinear, ImageFilter::point, ImageFilter::ewa}) {
        scene.image_textures.front().filter = filter;
        const float mapping = filter == ImageFilter::trilinear ? 160 : 40;
        scene.image_textures.front().uscale = mapping;
        scene.image_textures.front().vscale = mapping;
        if (filter == ImageFilter::ewa) {
            scene.image_textures.front().uscale = 160;
            scene.image_textures.front().vscale = 12;
        }
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
        if (filter == ImageFilter::trilinear || filter == ImageFilter::ewa) {
            auto control_scene = scene;
            control_scene.image_textures.front().filter = ImageFilter::bilinear;
            const auto single_level = sample(*build(control_scene), request);
            require(std::ranges::any_of(
                        fine.samples,
                        [&](const auto &s) {
                            return std::abs(s.radiance - single_level.samples[s.pixel].radiance) >
                                   1e-3f;
                        }),
                    "Filtered batch fixture must differ from bilinear at its fixed budget");
        }
        request.total_samples = 1;
        const auto coarse = sample(*renderer, request);
        require(std::ranges::any_of(fine.samples,
                                    [&](const auto &s) {
                                        return std::abs(s.radiance -
                                                        coarse.samples[s.pixel].radiance) > 1e-3f;
                                    }),
                "Batch fixture does not exercise sample-budget-dependent LOD");
    }
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

    auto wrong_layout = textured_plane(varying_pixels(), ImageEncoding::linear, true, 40);
    wrong_layout.image_textures.front().filter = ImageFilter::trilinear;
    auto pyramid = std::make_shared<ImagePyramid>(*wrong_layout.image_textures.front().image);
    pyramid->levels.back().offset += 1;
    wrong_layout.image_textures.front().image = std::move(pyramid);
    invalid(wrong_layout, "mip layout");

    auto invalid_half = textured_plane(gray16({0x9abc}), ImageEncoding::linear, false, 1);
    const auto valid_half = *invalid_half.image_textures.front().image;
    for (const auto &words :
         {std::vector<std::uint32_t>{0}, std::vector<std::uint32_t>{0x7c00, 0},
          std::vector<std::uint32_t>{0x7e000000, 0}, std::vector<std::uint32_t>{0, 0xbc00},
          std::vector<std::uint32_t>{0, 0x10000}}) {
        auto malformed = std::make_shared<ImagePyramid>(valid_half);
        malformed->texels = words;
        invalid_half.image_textures.front().image = std::move(malformed);
        invalid(invalid_half, words.size() == 1 ? "mip layout" : "half image texel");
    }

    auto wrong_filter = valid;
    auto invalid_descriptor = wrong_filter.image_textures.front();
    // Inject a non-enumerator to exercise the native boundary's rejection path.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    invalid_descriptor.filter = static_cast<ImageFilter>(99);
    wrong_filter.image_textures.push_back(std::move(invalid_descriptor));
    wrong_filter.materials.front().image_texture = 1;
    invalid(wrong_filter, "image texture filter");

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

    auto overflow = valid;
    overflow.filter = {PixelFilterKind::triangle, 0x1p-27f, 0x1p-27f};
    auto &texture = overflow.image_textures.front();
    texture.filter = ImageFilter::ewa;
    texture.uscale = 3e38f;
    texture.udelta = 3e38f;
    texture.vscale = 0;
    // The exact-center footprint has a zero minor axis: it needs base lookup
    // and cannot use the center-independent wide-footprint shortcut.
    const auto failed = render_progressive(
        overflow, {.width = 1, .height = 1, .samples = 8, .batch_size = 8, .seed = seed});
    require(failed.status == GeometryStatus::failure &&
                failed.diagnostic.find("arithmetic") != std::string::npos &&
                failed.film.pixels.size() == 1 && failed.film.pixels.front().count == 0,
            "EWA arithmetic failure committed a partial or substituted sample batch");
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
        trilinear();
        point();
        ewa();
        indexed_rgb8_equivalence();
        filtered_secondary(ImageFilter::trilinear);
        filtered_secondary(ImageFilter::point);
        filtered_secondary(ImageFilter::ewa);
        coarsest_mip();
        half_mip_overshoot();
        mixed_texture_secondary();
        auto pixels = varying_pixels();
        batch_partition(pixels);
        const auto encoded8 = std::exchange(pixels.rgb, {});
        pixels.bit_depth = 16;
        for (const auto value : encoded8) {
            pixels.rgb.insert(pixels.rgb.end(), {value, value}); // Expand byte v to v * 257.
        }
        batch_partition(pixels);
        translated_footprint();
        validation();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
