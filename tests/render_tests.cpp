// SPDX-License-Identifier: GPL-3.0-only

#include "image_output.hpp"
#include "metal_render.hpp"
#include "render_reference.hpp"
#include "render_transport_tests.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace rypath;
using namespace rypath::test;
using XYZ = std::array<double, 3>;

constexpr auto max_word = std::numeric_limits<std::uint32_t>::max();
constexpr auto max_sample = std::numeric_limits<std::uint64_t>::max();
constexpr float nan = std::numeric_limits<float>::quiet_NaN();
constexpr float inf = std::numeric_limits<float>::infinity();

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}

void near(double actual, double expected, double tolerance, std::string_view label) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        throw std::runtime_error(std::string{label} + " expected " + std::to_string(expected) +
                                 ", observed " + std::to_string(actual) + ", tolerance " +
                                 std::to_string(tolerance));
    }
}

SpectralMaterial constant_material(float reflectance) {
    SpectralMaterial material;
    material.reflectance.fill(reflectance);
    return material;
}

RenderScene floor_scene() {
    RenderScene scene;
    scene.geometry.meshes = {
        {{{-8, 0, -8}, {8, 0, -8}, {8, 0, 8}, {-8, 0, 8}}, {{0, 1, 2}, {0, 2, 3}}}};
    scene.geometry.instances = {{0, {}}};
    scene.instance_materials = {0};
    scene.materials = {{{0.10f, 0.13f, 0.22f, 0.39f, 0.53f, 0.68f, 0.73f, 0.66f}}};
    scene.camera = {{0, 2, 0}, {0, 0, 0}, {0, 0, 1}, 30};
    return scene;
}

void cpu_sampler() {
    using Words = std::array<std::uint32_t, 4>;
    // Random123 v1.14.0 tests/kat_vectors, Philox4x32 with ten rounds.
    require(sample_words(0, 0, 0, 0) == Words{0x6627e8d5, 0xe169c58d, 0xbc57ac4c, 0x9b00dbd8},
            "Philox zero known-answer mismatch");
    require(sample_words(max_sample, max_word, max_sample, max_word) ==
                Words{0x408f276d, 0x41c83b0e, 0xa20bc7c6, 0x6d5451fd},
            "Philox all-ones known-answer mismatch");
    require(sample_words(0x299f31d0a4093822ULL, 0x243f6a88, 0x13198a2e85a308d3ULL, 0x03707344) ==
                Words{0xd16cfe09, 0x94fdcceb, 0x5001e420, 0x24126ea1},
            "Philox nonuniform known-answer mismatch");
    require(sample_uniform(0) == 0 && sample_uniform(255) == 0 && sample_uniform(256) == 0x1p-24f &&
                sample_uniform(max_word) == 1 - 0x1p-24f,
            "Philox uniform endpoints or top-bit conversion");

    // Low-order distribution checks use a fixed stream and predeclared absolute
    // tolerances, with moments known from the continuous sampling transforms.
    constexpr std::uint64_t paths = 65536;
    double sphere_cosine_mean = 0;
    double hemisphere_cosine_mean = 0;
    double survival_mean = 0;
    for (std::uint64_t i = 0; i < paths; ++i) {
        const auto light = sample_words(0xf3908aa2c3119d04ULL, 0, i, 2);
        const auto bsdf = sample_words(0xf3908aa2c3119d04ULL, 0, i, 1);
        sphere_cosine_mean += (1 - 2 * static_cast<double>(sample_uniform(light[0]))) / paths;
        hemisphere_cosine_mean += std::sqrt(1 - double{sample_uniform(bsdf[0])}) / paths;
        survival_mean += (bsdf[2] < 0xc0000000u ? 1 / 0.75 : 0) / paths;
    }
    near(sphere_cosine_mean, 0, 0.014, "Uniform-sphere cosine mean");
    near(hemisphere_cosine_mean, 2.0 / 3, 0.006, "Cosine-weighted hemisphere cosine mean");
    near(survival_mean, 1, 0.014, "Compensated roulette mean");
}

void cpu_sensor_and_display() {
    const double normalization = cie_y_integral();
    near(normalization, 106.8569149167669, 1e-10, "CIE Y normalization");
    require(wavelength_xyz(359.9) == XYZ{} && wavelength_xyz(830.1) == XYZ{} &&
                wavelength_xyz(nan) == XYZ{},
            "CIE out-of-range wavelengths should be zero");
    const auto xyz = wavelength_xyz(479);
    constexpr XYZ cie_row_479{0.1042979, 0.1334528, 0.8566193};
    for (std::size_t c = 0; c < xyz.size(); ++c) {
        near(xyz[c] * normalization / 470, cie_row_479[c], 2e-12, "Published CIE row 479");
        near(wavelength_xyz(479.5)[c], (xyz[c] + wavelength_xyz(480)[c]) / 2, 2e-14,
             "CIE linear interpolation");
    }
    require(wavelength_xyz(360)[2] > 0 && wavelength_xyz(830)[0] > 0, "CIE tabulated endpoints");

    const auto material = floor_scene().materials.front();
    for (std::size_t i = 0; i < spectral_knots.size(); ++i) {
        near(spectral_reflectance(material, spectral_knots[i]), material.reflectance[i], 1e-14,
             "Spectral reflectance knot");
    }
    near(spectral_reflectance(material, 495),
         (double{material.reflectance[2]} + material.reflectance[3]) / 2, 1e-14,
         "Spectral reflectance interpolation");
    require(spectral_reflectance(material, 350) == 0 &&
                spectral_reflectance(material, std::numeric_limits<double>::infinity()) == 0,
            "Spectral reflectance outside range");

    using RGB = std::array<std::uint8_t, 3>;
    require(display_rgb({0, 0, 0}) == RGB{0, 0, 0}, "sRGB black");
    require(display_rgb({0.95047, 1, 1.08883}) == RGB{255, 255, 255}, "D65 sRGB white");
    require(display_rgb({0.4124564, 0.2126729, 0.0193339}) == RGB{255, 0, 0}, "sRGB red");
    require(display_rgb({0.95047 * 0.18, 0.18, 1.08883 * 0.18}) == RGB{118, 118, 118},
            "sRGB 18 percent grey");
    require(display_rgb({5, 0, 0})[1] == 0, "sRGB negative channel clipping");
}

void cpu_validation() {
    require(validate_render_settings({}).empty(), "Default render settings");
    require(!validate_render_settings({.width = 0}).empty(), "Zero render dimension accepted");
    require(!validate_render_settings({.width = max_word, .height = 2}).empty(),
            "Pixel index overflow accepted");
    require(!validate_render_settings({.samples = 0}).empty(), "Zero sample count accepted");
    require(!validate_render_settings({.batch_size = 0}).empty(), "Zero batch size accepted");
    require(!validate_render_settings({.width = 2, .height = 1, .samples = max_sample}).empty(),
            "Total path count overflow accepted");
    require(validate_render_settings({.width = 1, .height = 1, .samples = max_sample}).empty(),
            "Maximum representable total path count rejected");
    require(validate_render_settings({.samples = 3, .batch_size = 8}).empty(),
            "Large batch should render a complete tail");
    require(validate_render_scene(make_demo_scene()).empty(), "Invalid demo scene");
    require(validate_render_scene({}).empty(), "Invalid empty environment scene");

    auto scene = floor_scene();
    scene.geometry.meshes[0].triangles[0][0] = max_word;
    require(!validate_render_scene(scene).empty(), "Bad triangle index accepted");
    scene = floor_scene();
    scene.instance_materials.clear();
    require(!validate_render_scene(scene).empty(), "Missing instance material accepted");
    scene.instance_materials = {max_word};
    require(!validate_render_scene(scene).empty(), "Bad material index accepted");
    for (const auto value : {nan, inf, -0.01f, 0.75001f}) {
        scene = floor_scene();
        scene.materials[0].reflectance[3] = value;
        require(!validate_render_scene(scene).empty(), "Invalid spectral reflectance accepted");
    }
    scene = floor_scene();
    scene.camera.origin.y = nan;
    require(!validate_render_scene(scene).empty(), "Nonfinite camera accepted");
    scene = floor_scene();
    scene.camera.target = scene.camera.origin;
    require(!validate_render_scene(scene).empty(), "Coincident camera origin/target accepted");
    scene = floor_scene();
    scene.camera.up = {0, 1, 0};
    require(!validate_render_scene(scene).empty(), "Parallel camera forward/up accepted");
    scene = floor_scene();
    scene.camera.vertical_fov_degrees = 180;
    require(!validate_render_scene(scene).empty(), "Degenerate camera FOV accepted");
}

void cpu_pbrt_material_payloads() {
    using Kind = SpectralMaterialKind;
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{{}, {-1, 1, 1}, {2, 0, 0}, {0, -2, 0}};
    scene.materials.resize(1);
    auto &material = scene.materials.front();
    const auto valid = [&] { return validate_render_scene(scene).empty(); };
    constexpr float largest = std::numeric_limits<float>::max();

    material = {.kind = Kind::constant, .constant = 1};
    require(valid() && spectral_reflectance(material, 612.75) == 1,
            "PBRT constant white was rejected or changed");
    material.constant = 0;
    require(valid() && spectral_reflectance(material, 612.75) == 0,
            "PBRT constant black was rejected or changed");
    for (const auto value : {nan, inf, -0.01f, 1.01f}) {
        material.constant = value;
        require(!valid(), "Invalid constant reflectance accepted");
    }
    material = {.kind = Kind::rgb_sigmoid, .coefficients = {0, 0, largest}};
    require(valid() && spectral_reflectance(material, 612.75) == 1,
            "Finite positive polynomial failed to saturate to white");
    material.coefficients[2] = -largest;
    require(valid() && spectral_reflectance(material, 612.75) == 0,
            "Finite negative polynomial failed to saturate to black");
    for (const auto coefficients :
         {std::array{0.0f, 0.0f, nan}, std::array{0.0f, inf, 0.0f}, std::array{largest, 0.0f, 0.0f},
          std::array{0.0f, largest, 0.0f}, std::array{-1e33f, 1.2e36f, 0.0f}}) {
        material.coefficients = coefficients;
        require(!valid(), "Nonfinite or overflowing RGB polynomial accepted");
    }
    material = {.knots = {{360, 0}, {830, 1}}, .kind = Kind::rgb_sigmoid};
    require(!valid(), "RGB material accepted a sampled payload");
    material.kind = Kind::constant;
    require(!valid(), "Constant material accepted a sampled payload");
    material.kind = Kind::sampled;
    require(valid(), "PBRT sampled material with two knots rejected");
    material.constant = 0.5f;
    require(!valid(), "Sampled material accepted a constant payload");
    // Deliberately construct an unknown tag to exercise input rejection.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    material = {.kind = static_cast<Kind>(-1)};
    require(!valid(), "Unknown spectral material kind accepted");
}

void cpu_film() {
    require(make_film(0, 1).pixels.empty(), "Invalid film dimensions allocated storage");
    auto film = make_film(2, 1);
    const auto empty = film;
    const std::array samples{RenderSample{1, 0, 555, 1}, RenderSample{0, 0, 555, 1e20f},
                             RenderSample{0, 1, 555, 1}, RenderSample{1, 1, 555, 3}};
    FilmBatch batch(film, 2);
    require(batch.add(samples).empty(), "Valid out-of-order pixels rejected");
    require(film == empty, "Pending samples escaped into film");
    require(batch.commit(film).empty(), "Complete film batch failed to commit");
    require(film.pixels[0].count == 2 && film.pixels[1].count == 2, "Film sample counts");
    const auto weight = wavelength_xyz(555);
    near(film.pixels[0].correction[1], weight[1], 1e-14, "Small lost term recovered");
    near(film_mean(film.pixels[1])[1], 2 * weight[1], 1e-14, "Mean divides by sample count");
    const auto saved = film;
    require(!batch.commit(film).empty() && film == saved, "Batch committed twice");

    FilmBatch incomplete(film, 1);
    const std::array partial{RenderSample{0, 2, 555, 1}};
    require(incomplete.add(partial).empty(), "Partial film batch rejected early");
    require(!incomplete.commit(film).empty() && film == saved, "Incomplete film committed");
    const std::array remaining{RenderSample{1, 2, 555, 1}};
    require(!incomplete.add(remaining).empty() && !incomplete.commit(film).empty() && film == saved,
            "Incomplete commit failure was not sticky");

    const std::array corrupt{RenderSample{0, 2, nan, 1},  RenderSample{1, 2, 555, nan},
                             RenderSample{1, 2, 555, -1}, RenderSample{0, 2, 831, 1},
                             RenderSample{0, 2, 555, 1},  RenderSample{1, 3, 555, 1},
                             RenderSample{2, 2, 555, 1}};
    for (const auto &bad : corrupt) {
        FilmBatch invalid(film, 1);
        require(invalid.add(partial).empty(), "Failed to stage valid prefix");
        require(!invalid.add(std::span{&bad, 1}).empty() && !invalid.add(remaining).empty() &&
                    !invalid.commit(film).empty() && film == saved,
                "Corrupt film batch was accepted or committed");
    }

    auto high = make_film(1, 1);
    high.pixels[0].count = max_sample - 1;
    FilmBatch last(high, 1);
    const std::array end{RenderSample{0, max_sample - 1, 830, 1}};
    require(last.add(end).empty() && last.commit(high).empty() &&
                high.pixels[0].count == max_sample,
            "Last representable film count rejected");
    const auto final_count = high;
    FilmBatch overflow(high, 1);
    require(!overflow.commit(high).empty() && high == final_count, "Film sample count overflow");
    FilmBatch zero(film, 0);
    require(!zero.commit(film).empty() && film == saved, "Empty increment committed");
}

void cpu_reference() {
    const auto sky = analytic_xyz(nullptr);
    near(sky.mean[1], 1, 2e-14, "Unit equal-energy Y");
    auto floor = floor_scene();
    const auto floor_moments = analytic_xyz(&floor.materials[0]);
    for (std::size_t c = 0; c < sky.mean.size(); ++c) {
        require(sky.variance[c] > 0 && floor_moments.variance[c] > 0, "Positive spectral variance");
        require(floor_moments.mean[c] < 0.75 * sky.mean[c], "Floor energy conservation");
    }
    floor.materials[0] = constant_material(0.5f);
    const auto grey = analytic_xyz(&floor.materials[0]);
    for (std::size_t c = 0; c < sky.mean.size(); ++c) {
        near(grey.mean[c], sky.mean[c] / 2, 2e-14, "Lambertian furnace identity");
        require(grey.variance[c] > sky.variance[c] / 4, "Independent MIS arms add variance");
    }
    const auto sky_paths =
        reference_image({}, {.width = 2, .height = 2, .samples = 256, .seed = 0xdde1});
    const auto floor_paths =
        reference_image(floor, {.width = 2, .height = 2, .samples = 256, .seed = 0xdde1});
    require(sky_paths.max_scatterings == 0 && floor_paths.max_scatterings == 1,
            "Analytic CPU geometry must hit floor once and sky zero times");
    for (std::size_t c = 0; c < sky.mean.size(); ++c) {
        near(floor_paths.mean[c], 0.5 * sky_paths.mean[c], 1e-14,
             "Independent cosine estimator in a constant environment");
    }
}

void run_cpu() {
    cpu_sampler();
    cpu_sensor_and_display();
    cpu_validation();
    cpu_pbrt_material_payloads();
    cpu_film();
    cpu_reference();
}

std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto result = build_metal_renderer(scene);
    require(result.status == GeometryStatus::success && result.scene, result.diagnostic);
    return std::move(result.scene);
}

RenderSamplesResult native_samples(MetalRenderScene &scene, const RenderRequest &request) {
    auto result = scene.sample(request);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() ==
                static_cast<std::size_t>(request.pixel_count) * request.sample_count,
            "Native sample count mismatch");
    require(result.guard_count > 0, "Native sample dispatch checked no output guards");
    for (std::size_t i = 0; i < result.samples.size(); ++i) {
        const auto &sample = result.samples[i];
        const auto pixel =
            static_cast<std::uint32_t>(request.pixel_begin + i % request.pixel_count);
        const auto id = request.sample_begin + i / request.pixel_count;
        require(sample.pixel == pixel && sample.sample == id, "Native sample identity mismatch");
        const auto words = sample_words(request.seed, pixel, id, 0);
        require(sample.random_words == words, "Native Philox block differs from CPU");
        near(sample.wavelength, 360 + 470 * double{sample_uniform(words[2])}, 7e-5,
             "Native wavelength disagrees with Philox");
    }
    return result;
}

Film native_film(MetalRenderScene &scene, const RenderRequest &request,
                 std::uint32_t *max_scatterings = nullptr) {
    const auto result = native_samples(scene, request);
    auto film = make_film(request.width, request.height);
    FilmBatch batch(film, request.sample_count);
    const auto add = batch.add(result.samples);
    require(add.empty(), add);
    const auto commit = batch.commit(film);
    require(commit.empty(), commit);
    if (max_scatterings != nullptr) {
        for (const auto &sample : result.samples) {
            *max_scatterings = std::max(*max_scatterings, sample.scatterings);
        }
    }
    return film;
}

XYZ image_mean(const Film &film) {
    XYZ mean{};
    for (const auto &pixel : film.pixels) {
        const auto xyz = film_mean(pixel);
        for (std::size_t c = 0; c < mean.size(); ++c) {
            mean[c] += xyz[c] / static_cast<double>(film.pixels.size());
        }
    }
    return mean;
}

double numerical_floor(double expected) { return 2e-5 * std::max(1.0, std::abs(expected)); }

void native_analytic(MetalRenderScene &sky, MetalRenderScene &floor,
                     const SpectralMaterial &reflectance) {
    constexpr std::uint32_t paths = 32768;
    const RenderRequest request{.width = 1,
                                .height = 1,
                                .seed = 0x9e314fd9c628b435ULL,
                                .pixel_count = 1,
                                .sample_count = paths};
    const auto verify = [&](std::string_view label, MetalRenderScene &scene,
                            const SpectralMaterial *material) {
        std::uint32_t max_scatterings = 0;
        const auto xyz = image_mean(native_film(scene, request, &max_scatterings));
        require(max_scatterings == (material == nullptr ? 0 : 1),
                std::string{label} + " maximum scatterings was " + std::to_string(max_scatterings));
        const auto expected = analytic_xyz(material);
        for (std::size_t c = 0; c < xyz.size(); ++c) {
            const double se = std::sqrt(expected.variance[c] / paths);
            near(xyz[c], expected.mean[c], 6 * se + numerical_floor(expected.mean[c]), label);
            std::cout << label << " channel " << c << " observed " << xyz[c] << ", expected "
                      << expected.mean[c] << ", analytic SE " << se << '\n';
        }
    };
    verify("Sky", sky, nullptr);
    verify("Unobstructed diffuse floor", floor, &reflectance);
}

struct Replicates {
    std::array<XYZ, 16> values{};

    XYZ mean() const {
        XYZ result{};
        for (const auto &value : values) {
            for (std::size_t c = 0; c < result.size(); ++c) {
                result[c] += value[c] / static_cast<double>(values.size());
            }
        }
        return result;
    }

    XYZ mean_variance() const {
        const auto averages = mean();
        XYZ result{};
        for (const auto &value : values) {
            for (std::size_t c = 0; c < result.size(); ++c) {
                const auto delta = value[c] - averages[c];
                result[c] +=
                    delta * delta / static_cast<double>(values.size() * (values.size() - 1));
            }
        }
        return result;
    }
};

void native_multibounce(MetalRenderScene &native, const RenderScene &scene) {
    Replicates gpu;
    Replicates cpu;
    std::uint32_t gpu_max = 0;
    std::uint32_t cpu_max = 0;
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 4;
    constexpr std::uint32_t samples = 128;
    for (std::size_t replicate = 0; replicate < gpu.values.size(); ++replicate) {
        const RenderRequest request{.width = width,
                                    .height = height,
                                    .seed = 0x71ac6a05eb690500ULL + replicate,
                                    .pixel_count = width * height,
                                    .sample_count = samples};
        gpu.values[replicate] = image_mean(native_film(native, request, &gpu_max));
        const auto oracle = reference_image(scene, {.width = width,
                                                    .height = height,
                                                    .samples = samples,
                                                    .seed = 0x56da2309ab9f4000ULL + replicate});
        cpu.values[replicate] = oracle.mean;
        cpu_max = std::max(cpu_max, oracle.max_scatterings);
    }
    require(gpu_max >= 4 && cpu_max >= 4, "Multibounce fixture did not reach a fourth hit");
    std::cout << "Multibounce maximum scatterings GPU " << gpu_max << ", CPU " << cpu_max << '\n';
    const auto actual = gpu.mean();
    const auto expected = cpu.mean();
    const auto gpu_var = gpu.mean_variance();
    const auto cpu_var = cpu.mean_variance();
    for (std::size_t c = 0; c < actual.size(); ++c) {
        const double se = std::sqrt(gpu_var[c] + cpu_var[c]);
        near(actual[c], expected[c], 5 * se + numerical_floor(expected[c]), "Multibounce XYZ");
        std::cout << "Multibounce channel " << c << " GPU " << actual[c] << ", CPU " << expected[c]
                  << ", combined SE " << se << '\n';
    }
}

void native_convergence(MetalRenderScene &floor, const SpectralMaterial &reflectance) {
    const auto expected = analytic_xyz(&reflectance).mean;
    constexpr std::uint32_t width = 8;
    constexpr std::uint32_t height = 8;
    constexpr std::array sample_counts{16u, 64u};
    std::array<double, 2> errors{};
    for (std::uint64_t seed = 0; seed < 16; ++seed) {
        for (std::size_t budget = 0; budget < sample_counts.size(); ++budget) {
            const RenderRequest request{.width = width,
                                        .height = height,
                                        .seed = 0x39c62fd15af97e00ULL + seed,
                                        .pixel_count = width * height,
                                        .sample_count = sample_counts[budget]};
            const auto film = native_film(floor, request);
            for (const auto &pixel : film.pixels) {
                const auto xyz = film_mean(pixel);
                for (std::size_t c = 0; c < xyz.size(); ++c) {
                    const double delta = xyz[c] - expected[c];
                    errors[budget] += delta * delta;
                }
            }
        }
    }
    const double ratio = std::sqrt(errors[1] / errors[0]);
    std::cout << "16 vs 64 spp pooled convergence RMSE ratio " << ratio << '\n';
    require(ratio >= 0.35 && ratio <= 0.70, "Pooled convergence ratio outside [0.35, 0.70]");
}

void native_progressive(MetalRenderScene &demo) {
    constexpr std::uint32_t width = 17;
    constexpr std::uint32_t height = 13;
    constexpr std::uint32_t pixels = width * height;
    constexpr std::uint32_t sample_count = 32;
    constexpr std::uint64_t seed = 0x6b1357dc198fcd31ULL;
    RenderRequest request{.width = width,
                          .height = height,
                          .seed = seed,
                          .pixel_count = pixels,
                          .sample_count = sample_count};
    const auto whole = native_film(demo, request);
    auto split = make_film(width, height);
    std::uint64_t offset = 0;
    constexpr std::uint32_t head = 97;
    for (const auto count : {1u, 7u, 24u}) {
        FilmBatch batch(split, count);
        request.sample_begin = offset;
        request.sample_count = count;
        request.threadgroup_width = 7;
        request.pixel_begin = head;
        request.pixel_count = pixels - head;
        const auto tail = native_samples(demo, request);
        const auto tail_diagnostic = batch.add(tail.samples);
        require(tail_diagnostic.empty(), tail_diagnostic);
        request.threadgroup_width = 13;
        request.pixel_begin = 0;
        request.pixel_count = head;
        const auto beginning = native_samples(demo, request);
        const auto head_diagnostic = batch.add(beginning.samples);
        require(head_diagnostic.empty(), head_diagnostic);
        const auto commit = batch.commit(split);
        require(commit.empty(), commit);
        offset += count;
    }
    require(whole == split, "Progressive partition, tile reorder, or group width changed raw film");

    std::vector<std::uint64_t> progress;
    const RenderSettings settings{width, height, sample_count, 7, seed};
    const auto result = render_progressive(make_demo_scene(), settings,
                                           [&](std::uint64_t done) { progress.push_back(done); });
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(progress == std::vector<std::uint64_t>{7, 14, 21, 28, 32}, "Tail progress accounting");
    require(result.film == whole, "Driver tail batch changed raw film");

    request.sample_begin = (std::uint64_t{1} << 32) - 1;
    request.sample_count = 3;
    request.threadgroup_width = 13;
    request.pixel_count = 17;
    const auto high_indices = native_samples(demo, request);
    require(high_indices.samples.back().sample == (std::uint64_t{1} << 32) + 1,
            "Native sample counter carry failed");
}

void native_driver_capacity() {
    // Exercise the driver's 4096-pixel tiling and 65,536-path dispatch limit
    // in the same batch, including a partial pixel tile and sample tile.
    const RenderScene sky;
    RenderSettings settings{.width = 4099, .height = 1, .samples = 17, .batch_size = 17};
    const auto whole = render_progressive(sky, settings);
    require(whole.status == GeometryStatus::success, whole.diagnostic);
    require(whole.film.pixels.size() == settings.width &&
                std::ranges::all_of(
                    whole.film.pixels,
                    [&](const FilmPixel &pixel) { return pixel.count == settings.samples; }),
            "Driver omitted paths when splitting a large batch");
    settings.batch_size = 8;
    const auto split = render_progressive(sky, settings);
    require(split.status == GeometryStatus::success, split.diagnostic);
    require(split.film == whole.film, "Native dispatch capacity changed raw sample accumulation");
}

void native_camera_and_empty_instance(MetalRenderScene &floor) {
    RenderScene quadrant;
    quadrant.geometry.meshes = {
        {{{0, 0, 0}, {3, 0, 0}, {3, 3, 0}, {0, 3, 0}}, {{0, 1, 2}, {0, 2, 3}}}};
    quadrant.geometry.instances = {{0, {}}};
    quadrant.instance_materials = {0};
    quadrant.materials = {constant_material(0)};
    quadrant.camera = {{0, 0, 2}, {0, 0, 0}, {0, 1, 0}, 45};
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 4;
    const RenderRequest camera_request{.width = width,
                                       .height = height,
                                       .seed = 0x256bd4c9943268a7ULL,
                                       .pixel_count = width * height,
                                       .sample_count = 8};
    auto camera_scene = build(quadrant);
    for (const auto &sample : native_samples(*camera_scene, camera_request).samples) {
        const bool top_right =
            sample.pixel % width >= width / 2 && sample.pixel / width < height / 2;
        require(sample.radiance == (top_right ? 0.0f : 1.0f) && sample.scatterings == 0,
                "Black upper-right quadrant was mirrored, flipped, or scattered");
    }

    auto sparse = floor_scene();
    sparse.geometry.meshes.emplace_back();
    sparse.geometry.instances.insert(sparse.geometry.instances.begin(), {1, {}});
    sparse.materials.push_back(constant_material(0));
    sparse.instance_materials.insert(sparse.instance_materials.begin(), 1);
    auto sparse_floor = build(sparse);
    require(native_film(*sparse_floor, camera_request) == native_film(floor, camera_request),
            "Leading empty instance changed active floor shading");
}

RenderScene add_parallel_blocker(RenderScene scene, GeometryTransform transform, float height,
                                 const SpectralMaterial &material) {
    transform.values[7] += height;
    scene.geometry.instances.push_back({0, transform});
    scene.instance_materials.push_back(1);
    scene.materials.push_back(material);
    return scene;
}

void native_spawn_and_failures(MetalRenderScene &floor) {
    constexpr RenderRequest ray{.width = 1,
                                .height = 1,
                                .seed = 0x6a540294bd881bb9ULL,
                                .pixel_count = 1,
                                .sample_count = 32,
                                .threadgroup_width = 7};
    const auto unobstructed = native_samples(floor, ray);
    require(unobstructed.guard_count == 4, "Rounded dispatch did not check all padded lanes");
    require(std::ranges::all_of(unobstructed.samples,
                                [](const RenderSample &sample) {
                                    return sample.scatterings == 1 && sample.radiance > 0;
                                }),
            "An isolated floor path missed, self-hit, or lost its unoccluded continuation");

    const auto reflectance = floor_scene().materials.front();
    for (const auto &sample : unobstructed.samples) {
        const auto light = sample_words(ray.seed, sample.pixel, sample.sample, 2);
        const auto bsdf = sample_words(ray.seed, sample.pixel, sample.sample, 1);
        const double z = 1 - 2 * double{sample_uniform(light[0])};
        const double phi = 2 * std::numbers::pi * sample_uniform(light[1]);
        const double ratio = 4 * std::max(0.0, std::sqrt(1 - z * z) * std::sin(phi));
        const double ratio_squared = 16 * (1 - double{sample_uniform(bsdf[0])});
        const double expected = spectral_reflectance(reflectance, sample.wavelength) *
                                (ratio / (1 + ratio * ratio) + ratio_squared / (1 + ratio_squared));
        near(sample.radiance, expected, 2e-6, "Per-path floor MIS");
    }

    const GeometryTransform mirrored{{-1.5f, 0.4f, 0.2f, 2, 0, 0.7f, 0, 3, 0, 0, 1.3f, -4}};
    auto transformed = floor_scene();
    transformed.geometry.instances[0].transform = mirrored;
    transformed.camera = {{2, 3.001f, -4}, {2, 3, -4}, {0, 0, 1}, 20};
    auto mirrored_floor = build(transformed);
    const auto same = native_samples(*mirrored_floor, ray);
    for (std::size_t i = 0; i < same.samples.size(); ++i) {
        require(same.samples[i].scatterings == 1, "Mirrored transformed floor self-hit");
        near(same.samples[i].radiance, unobstructed.samples[i].radiance, 2e-6,
             "Mirrored transformed floor radiance");
    }
    auto tilted = mirrored;
    tilted.values[4] = 0.15f;
    tilted.values[6] = 0.25f;
    transformed.geometry.instances[0].transform = tilted;
    auto blocked_floor =
        build(add_parallel_blocker(transformed, tilted, 0.002f, constant_material(0)));
    auto one_scattering = ray;
    one_scattering.emergency_limit = 1;
    const auto blocked = native_samples(*blocked_floor, one_scattering);
    require(std::ranges::all_of(blocked.samples,
                                [](const RenderSample &sample) {
                                    return sample.scatterings == 1 && sample.radiance == 0;
                                }),
            "Nearby black blocker was skipped by a shadow or continuation ray");

    auto cavity = build(add_parallel_blocker(floor_scene(), {}, 3, constant_material(0.75f)));
    auto fail = ray;
    fail.emergency_limit = 1;
    const auto stopped = cavity->sample(fail);
    require(stopped.status == GeometryStatus::failure && stopped.samples.empty() &&
                stopped.diagnostic.find("emergency") != std::string::npos,
            "Emergency depth returned partial or successful paths");

    const auto invalid = [&](RenderRequest request) {
        const auto result = floor.sample(request);
        require(result.status == GeometryStatus::failure && !result.diagnostic.empty() &&
                    result.samples.empty(),
                "Invalid native request yielded sample output");
    };
    fail = ray;
    fail.sample_begin = max_sample;
    invalid(fail);
    fail = ray;
    fail.pixel_begin = 1;
    invalid(fail);
    fail = ray;
    fail.sample_count = render_tile_capacity + 1;
    invalid(fail);
    fail = ray;
    fail.threadgroup_width = max_word;
    invalid(fail);
    fail = ray;
    fail.emergency_limit = 0;
    invalid(fail);
    require(native_samples(*cavity, ray).samples.size() == ray.sample_count,
            "Renderer did not recover from emergency request");
    require(native_samples(floor, ray).samples.size() == ray.sample_count,
            "Renderer did not recover from invalid requests");

    auto moved = std::move(*mirrored_floor);
    const auto moved_from = mirrored_floor->sample(ray);
    require(moved_from.status == GeometryStatus::failure && moved_from.samples.empty(),
            "Moved-from renderer returned samples");
    native_samples(moved, ray);
}

int run_native(bool allow_no_device, bool allow_no_ray_tracing) {
    auto demo = build_metal_renderer(make_demo_scene());
    if ((demo.status == GeometryStatus::no_device && allow_no_device) ||
        (demo.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
        std::cout << "SKIP Metal render. " << demo.diagnostic << '\n';
        return 77;
    }
    require(demo.status == GeometryStatus::success && demo.scene, demo.diagnostic);

    auto sky = build({});
    auto floor = build(floor_scene());
    const auto reflectance = floor_scene().materials.front();
    native_analytic(*sky, *floor, reflectance);
    native_multibounce(*demo.scene, make_demo_scene());
    native_convergence(*floor, reflectance);
    native_progressive(*demo.scene);
    native_driver_capacity();
    native_spawn_and_failures(*floor);
    native_camera_and_empty_instance(*floor);
    check_render_transport();
    return 0;
}

void run_stub() {
    const auto built = build_metal_renderer(make_demo_scene());
    require(built.status == GeometryStatus::not_built && !built.scene && !built.diagnostic.empty(),
            "Metal render stub gave incorrect availability");
    const auto rendered = render_progressive(make_demo_scene(), {});
    require(rendered.status == GeometryStatus::not_built && rendered.film.pixels.empty() &&
                !rendered.diagnostic.empty(),
            "Progressive render stub gave incorrect availability or returned a film");
    const auto png = write_png(make_film(1, 1), {});
    require(png.find("not built") != std::string::npos, "PNG stub availability diagnostic");
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 2, "Expected render test mode cpu, metal, or stub");
        const std::string_view mode{argv[1]};
        if (mode == "metal") {
            bool allow_no_device = false;
            bool allow_no_ray_tracing = false;
            for (int i = 2; i < argc; ++i) {
                const std::string_view arg{argv[i]};
                if (arg == "--allow-no-device") {
                    allow_no_device = true;
                } else if (arg == "--allow-no-ray-tracing") {
                    allow_no_ray_tracing = true;
                } else {
                    throw std::runtime_error("Unexpected render test option");
                }
            }
            return run_native(allow_no_device, allow_no_ray_tracing);
        }
        require(argc == 2, "Unexpected portable render test options");
        if (mode == "cpu") {
            run_cpu();
        } else if (mode == "stub") {
            run_stub();
        } else {
            throw std::runtime_error("Unknown render test mode");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Render test failure. " << error.what() << '\n';
        return 1;
    }
}
