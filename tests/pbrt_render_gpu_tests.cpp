// SPDX-License-Identifier: GPL-3.0-only

#include "cie_xyz_data.hpp"
#include "metal_render.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace rypath;
using XYZ = std::array<double, 3>;
using Curve = std::vector<std::pair<double, double>>;
constexpr std::uint32_t analytic_paths = 32768;
const Curve diffuse_light{{360, 0.25}, {485, 0.8}, {650, 1}, {830, 0.25}};

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}

RenderScene load(const std::filesystem::path &file) {
    const auto checked = pbrt::check(file);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, pbrt::Location{file.string()});
    if (!translated.scene || translated.exit_code() != 0) {
        for (const auto &diagnostic : translated.diagnostics) {
            std::cerr << *diagnostic.location.file << ':' << diagnostic.location.line << ": "
                      << diagnostic.message << '\n';
        }
        throw std::runtime_error("Could not translate " + file.string());
    }
    return std::move(translated.scene.value());
}

double value_at(const Curve &curve, double wavelength) {
    const auto upper = std::ranges::upper_bound(curve, wavelength, {}, &Curve::value_type::first);
    if (upper == curve.end()) {
        return curve.back().second;
    }
    const auto [hi, yhi] = *upper;
    const auto [lo, ylo] = *std::prev(upper);
    return ylo + (yhi - ylo) * ((wavelength - lo) / (hi - lo));
}

struct AngularMoments {
    double mean = 1;
    double second_moment = 1;
};

AngularMoments diffuse_angular_moments() {
    // Uniform-sphere NEE and independent cosine-hemisphere BSDF draws.
    // Integrate each arm's first and second moments in cos(theta).
    constexpr int steps = 16384;
    double nee_mean = 0;
    double nee_second = 0;
    double bsdf_mean = 0;
    double bsdf_second = 0;
    for (int i = 0; i < steps; ++i) {
        const double c = (i + 0.5) / steps;
        const double ratio = 4 * c;
        const double nee = ratio / (1 + ratio * ratio);
        const double bsdf = ratio * ratio / (1 + ratio * ratio);
        nee_mean += 0.5 * nee / steps;
        nee_second += 0.5 * nee * nee / steps;
        bsdf_mean += 2 * c * bsdf / steps;
        bsdf_second += 2 * c * bsdf * bsdf / steps;
    }
    return {nee_mean + bsdf_mean, nee_second + bsdf_second + 2 * nee_mean * bsdf_mean};
}

struct SpectralMoments {
    XYZ mean{};
    XYZ second_moment{};
};

SpectralMoments spectral_quadrature(const Curve &light, double scale, const auto &reflectance,
                                    AngularMoments angular) {
    const auto &cie = detail::cie_xyz;
    double normalization = 0;
    for (std::size_t i = 0; i < cie.size(); ++i) {
        normalization += value_at(light, 360.0 + static_cast<double>(i)) * cie[i][1];
    }
    SpectralMoments moments;
    constexpr int subintervals = 128;
    constexpr double width = 1.0 / subintervals;
    for (int nm = 360; nm < 830; ++nm) {
        for (int step = 0; step < subintervals; ++step) {
            const double wavelength = nm + (step + 0.5) * width;
            const auto index =
                static_cast<std::size_t>(nm - 360) + (step >= subintervals / 2 ? 1u : 0u);
            const double rho = reflectance(wavelength);
            const double radiance =
                scale * value_at(light, 360.0 + static_cast<double>(index)) * rho / normalization;
            for (std::size_t c = 0; c < moments.mean.size(); ++c) {
                const double density = radiance * cie[index][c];
                moments.mean[c] += width * density * angular.mean;
                moments.second_moment[c] += width * 470 * density * density * angular.second_moment;
            }
        }
    }
    return moments;
}

SpectralMoments quadrature(const Curve &light, double scale, const Curve &reflectance) {
    if (reflectance.empty()) {
        return spectral_quadrature(light, scale, [](double) { return 1.0; }, {});
    }
    return spectral_quadrature(
        light, scale, [&](double wavelength) { return value_at(reflectance, wavelength); },
        diffuse_angular_moments());
}

RenderSamplesResult trace(MetalRenderScene &scene, std::uint32_t paths, std::uint64_t seed,
                          std::uint32_t emergency = render_emergency_limit) {
    auto result = scene.sample({.width = 1,
                                .height = 1,
                                .seed = seed,
                                .pixel_count = 1,
                                .sample_count = paths,
                                .threadgroup_width = 32,
                                .emergency_limit = emergency});
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == paths && result.guard_count > 0,
            "PBRT path count or shader output guard coverage");
    for (std::uint32_t i = 0; i < paths; ++i) {
        const auto &sample = result.samples[i];
        require(sample.pixel == 0 && sample.sample == i, "PBRT sample identity");
    }
    return result;
}

std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    return std::move(built.scene);
}

void analytic(MetalRenderScene &scene, const SpectralMoments &moments, std::uint64_t seed,
              std::string_view name, std::uint32_t scatterings) {
    const auto sampled = trace(scene, analytic_paths, seed);
    require(std::ranges::all_of(
                sampled.samples,
                [scatterings](const auto &sample) { return sample.scatterings == scatterings; }),
            "Unexpected hit or miss in the analytic PBRT fixture");
    auto film = make_film(1, 1, RenderModel::pbrt);
    FilmBatch batch(film, analytic_paths);
    require(batch.add(sampled.samples).empty() && batch.commit(film).empty(),
            "PBRT analytic samples did not accumulate");
    const auto observed = film_mean(film.pixels.front());
    for (std::size_t c = 0; c < observed.size(); ++c) {
        const double expected = moments.mean[c];
        const double variance = std::max(0.0, moments.second_moment[c] - expected * expected);
        const double standard_error = std::sqrt(variance / analytic_paths);
        const double limit = 6 * standard_error + 2e-5 * std::max(1.0, std::abs(expected));
        std::cout << name << " XYZ[" << c << "] expected=" << expected
                  << " observed=" << observed[c] << " analytic_se=" << standard_error
                  << " limit=" << limit << '\n';
        require(std::abs(observed[c] - expected) <= limit, "PBRT analytic spectral mean mismatch");
    }
}

Film progressive(const RenderScene &scene, const RenderSettings &settings) {
    auto rendered = render_progressive(scene, settings);
    require(rendered.status == GeometryStatus::success, rendered.diagnostic);
    require(rendered.film.model == RenderModel::pbrt && rendered.film.exposure == scene.exposure,
            "Progressive rendering lost PBRT film metadata");
    require(std::ranges::all_of(
                rendered.film.pixels,
                [&](const FilmPixel &pixel) { return pixel.count == settings.samples; }),
            "Progressive PBRT rendering lost sample counts");
    return std::move(rendered.film);
}

void constant_texture_equivalence(const std::filesystem::path &directory, std::string_view stem) {
    const auto direct_fixture = std::string(stem) + ".pbrt";
    const auto texture_fixture = "texture-" + direct_fixture;
    const auto textured = load(directory / texture_fixture);
    auto direct = load(directory / direct_fixture);
    require(textured.instance_materials.size() == 1 && direct.instance_materials.size() == 1,
            "Expected one material binding in each constant texture comparison scene");
    const auto &actual = textured.materials.at(textured.instance_materials.front());
    const auto &expected = direct.materials.at(direct.instance_materials.front());
    const auto knots_equal = std::ranges::equal(
        actual.knots, expected.knots, [](const SpectralKnot &a, const SpectralKnot &b) {
            return a.wavelength == b.wavelength && a.value == b.value;
        });
    require(actual.kind == expected.kind && actual.constant == expected.constant &&
                actual.coefficients == expected.coefficients && knots_equal,
            texture_fixture + ": translated reflectance payload differs");

    constexpr RenderSettings settings{
        .width = 8, .height = 8, .samples = 64, .batch_size = 32, .seed = 0x1975bd6a6ea321cfULL};
    const auto direct_film = progressive(direct, settings);
    const auto textured_film = progressive(textured, settings);
    require(direct_film == textured_film, texture_fixture + ": same-seed raw film differs");

    direct.materials.at(direct.instance_materials.front()) = {
        .kind = SpectralMaterialKind::constant, .constant = 0};
    require(direct_film != progressive(direct, settings),
            texture_fixture + ": no pixel received reflected light");
}

// Coefficients of linear sRGB (1, 0, 0), from the pinned upstream PBRT table.
constexpr std::array red_coefficients{0x1.5d5e1cp-11f, -0x1.524982p-1f, 0x1.3b3c8p+7f};

double red_oracle(double wavelength) {
    const auto [a, b, c] = red_coefficients;
    const double p = a * wavelength * wavelength + b * wavelength + c;
    return 0.5 + 0.5 * p / std::sqrt(1 + p * p);
}

RenderScene with_material(RenderScene scene, SpectralMaterial material) {
    scene.materials.at(scene.instance_materials.front()) = std::move(material);
    return scene;
}

void rgb_materials(const RenderScene &plane) {
    using Kind = SpectralMaterialKind;
    constexpr SpectralMaterial white{.kind = Kind::constant, .constant = 1};
    constexpr SpectralMaterial black{.kind = Kind::constant};
    constexpr SpectralMaterial red{.kind = Kind::rgb_sigmoid, .coefficients = red_coefficients};
    constexpr float largest = std::numeric_limits<float>::max();
    constexpr SpectralMaterial large{.kind = Kind::rgb_sigmoid, .coefficients = {0, 0, largest}};
    constexpr std::uint64_t seed = 0xc1348c175ee1275cULL;
    const auto whites = trace(*build(with_material(plane, white)), analytic_paths, seed);
    const auto blacks = trace(*build(with_material(plane, black)), analytic_paths, seed);
    const auto saturated = trace(*build(with_material(plane, large)), analytic_paths, seed);
    // The source scene is destroyed before either dispatch.
    auto native = build(with_material(plane, red));
    const auto colors = trace(*native, analytic_paths, seed);
    bool distinguishes_dense = false;
    double max_error = 0;
    for (std::size_t i = 0; i < colors.samples.size(); ++i) {
        const auto &w = whites.samples[i];
        const auto &c = colors.samples[i];
        const auto &b = blacks.samples[i];
        require(w.scatterings == 1 && c.scatterings == 1 && w.radiance > 0 &&
                    w.wavelength == c.wavelength && w.random_words == c.random_words,
                "RGB plane must shade the same hit and wavelength as white");
        require(b.scatterings == 0 && b.radiance == 0 && b.wavelength == w.wavelength,
                "Constant black plane returned light");
        require(saturated.samples[i].radiance == w.radiance &&
                    saturated.samples[i].scatterings == 1,
                "Large finite RGB polynomial failed to saturate to white");
        const double expected = w.radiance * red_oracle(c.wavelength);
        const double bound = 1e-7 + 2e-5 * std::abs(w.radiance);
        const double error = std::abs(c.radiance - expected);
        require(error <= bound, "RGB radiance disagrees with the same-seed white plane");
        max_error = std::max(max_error, error);
        const double nm = std::floor(c.wavelength);
        const double dense = std::lerp(red_oracle(nm), red_oracle(nm + 1), c.wavelength - nm);
        distinguishes_dense |= std::abs(c.radiance - w.radiance * dense) > bound;
    }
    require(distinguishes_dense, "RGB samples did not distinguish a one-nanometer interpolation");
    std::cout << "RGB plane maximum per-path radiance error " << max_error << '\n';
    analytic(*native, spectral_quadrature(diffuse_light, 1, red_oracle, diffuse_angular_moments()),
             0x58e193a00927f17aULL, "RGB diffuse", 1);

    auto mixed = with_material(plane, red);
    mixed.materials.push_back(plane.materials.at(plane.instance_materials.front()));
    mixed.geometry.meshes.push_back(plane.geometry.meshes.front());
    for (auto &point : mixed.geometry.meshes[0].positions) {
        point.x = std::min(point.x, 0.0f);
    }
    for (auto &point : mixed.geometry.meshes[1].positions) {
        point.x = std::max(point.x, 0.0f);
    }
    auto second_instance = plane.geometry.instances.front();
    second_instance.mesh_index = 1;
    mixed.geometry.instances.push_back(second_instance);
    mixed.instance_materials.push_back(static_cast<std::uint32_t>(mixed.materials.size() - 1));
    if (!plane.instance_reverse_orientation.empty()) {
        mixed.instance_reverse_orientation.push_back(plane.instance_reverse_orientation.front());
    }
    auto settings =
        RenderSettings{.width = 2, .height = 1, .samples = 33, .batch_size = 33, .seed = seed};
    const auto whole = progressive(mixed, settings);
    const auto all_red = progressive(with_material(plane, red), settings);
    const auto all_sampled = progressive(plane, settings);
    require(whole.pixels[0] == all_red.pixels[0] && whole.pixels[1] == all_sampled.pixels[1] &&
                whole.pixels[0] != all_sampled.pixels[0] && whole.pixels[1] != all_red.pixels[1],
            "Mixed material instances did not select the expected sampled or RGB payload");
    settings.batch_size = 7;
    require(whole == progressive(mixed, settings),
            "Mixed RGB and sampled batch partition changed film");

    for (const auto &invalid : {
             SpectralMaterial{.kind = static_cast<Kind>(-1)},
             SpectralMaterial{.kind = Kind::constant, .constant = 2},
             SpectralMaterial{.kind = Kind::rgb_sigmoid, .coefficients = {largest, 0, 0}},
             SpectralMaterial{.kind = Kind::rgb_sigmoid,
                              .coefficients = {0, 0, std::numeric_limits<float>::infinity()}},
         }) {
        const auto rejected = build_metal_renderer(with_material(plane, invalid));
        require(rejected.status == GeometryStatus::failure && !rejected.scene &&
                    !rejected.diagnostic.empty(),
                "Invalid material payload acquired a native renderer");
    }
    auto extreme = with_material(plane, white);
    extreme.environment.fill(largest);
    const auto overflow = build(extreme)->sample({.width = 1,
                                                  .height = 1,
                                                  .seed = seed,
                                                  .pixel_count = 1,
                                                  .sample_count = 128,
                                                  .threadgroup_width = 32});
    require(overflow.status == GeometryStatus::failure && overflow.samples.empty() &&
                overflow.diagnostic.find("arithmetic") != std::string::npos,
            "Nonfinite shader result returned successful or partial samples");
}

void depth_and_batch(RenderScene room) {
    constexpr std::uint64_t seed = 0xedfe734261b2a1e9ULL;
    require(room.max_depth == 5, "PBRT path default maxdepth is five");
    auto settings =
        RenderSettings{.width = 5, .height = 7, .samples = 64, .batch_size = 64, .seed = seed};
    room.exposure = 2;
    const auto whole = progressive(room, settings);
    settings.batch_size = 7;
    require(whole == progressive(room, settings), "Batch partition changed PBRT raw film");
    settings.batch_size = 1;
    require(whole == progressive(room, settings), "Single-sample batches changed PBRT raw film");

    const auto five = trace(*build(room), analytic_paths, seed);
    require(std::ranges::any_of(five.samples, [](const auto &s) { return s.scatterings == 5; }),
            "Fixture did not reach the PBRT default depth limit");
    room.max_depth = 2;
    const auto two = trace(*build(room), analytic_paths, seed);
    room.max_depth = 1;
    const auto one = trace(*build(room), analytic_paths, seed);
    room.max_depth = 0;
    const auto zero = trace(*build(room), analytic_paths, seed);

    bool depth_two_adds_light = false;
    bool default_depth_adds_light = false;
    for (std::size_t i = 0; i < analytic_paths; ++i) {
        const auto &a = zero.samples[i];
        const auto &b = one.samples[i];
        const auto &c = two.samples[i];
        const auto &d = five.samples[i];
        require(a.scatterings == 0 && b.scatterings <= 1 && c.scatterings <= 2 &&
                    d.scatterings <= 5,
                "PBRT path sampled beyond maxdepth");
        if (b.scatterings != 0) {
            require(a.radiance == 0, "Primary hit at maxdepth zero received lighting");
        } else {
            require(a.radiance == b.radiance && b.radiance == c.radiance &&
                        c.radiance == d.radiance,
                    "Primary escape changed with PBRT maxdepth");
        }
        require(a.radiance <= b.radiance && b.radiance <= c.radiance && c.radiance <= d.radiance,
                "PBRT depth limit discarded earlier light");
        depth_two_adds_light |= c.radiance > b.radiance;
        default_depth_adds_light |= d.radiance > c.radiance;
    }
    require(depth_two_adds_light && default_depth_adds_light,
            "PBRT multibounce fixture did not exercise later illumination");

    room.max_depth = 1;
    const auto at_cap = trace(*build(room), analytic_paths, seed, 1);
    for (std::size_t i = 0; i < analytic_paths; ++i) {
        require(at_cap.samples[i].radiance == one.samples[i].radiance,
                "PBRT cutoff at maxdepth must take precedence over a coincident emergency cap");
    }
    room.max_depth = render_emergency_limit;
    auto capped = build(room);
    const auto emergency = capped->sample({.width = 1,
                                           .height = 1,
                                           .seed = seed,
                                           .pixel_count = 1,
                                           .sample_count = analytic_paths,
                                           .emergency_limit = 1});
    require(emergency.status == GeometryStatus::failure && emergency.samples.empty() &&
                emergency.diagnostic.find("emergency") != std::string::npos,
            "PBRT emergency cap must fail without publishing partial samples");
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 2, "Expected PBRT fixture directory");
        const std::filesystem::path directory(argv[1]);
        bool allow_no_device = false;
        bool allow_no_ray_tracing = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view option(argv[i]);
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown PBRT Metal test option");
            allow_no_device |= option == "--allow-no-device";
            allow_no_ray_tracing |= option == "--allow-no-ray-tracing";
        }

        // The temporary scene and its spectral storage are gone before sample().
        auto native = build_metal_renderer(load(directory / "diffuse.pbrt"));
        if ((native.status == GeometryStatus::no_device && allow_no_device) ||
            (native.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
            std::cout << "SKIP Metal PBRT render. " << native.diagnostic << '\n';
            return 77;
        }
        require(native.status == GeometryStatus::success && native.scene, native.diagnostic);

        constant_texture_equivalence(directory, "diffuse");
        constant_texture_equivalence(directory, "ply-external");

        auto sky = load(directory / "background.pbrt");
        require(sky.max_depth == 0, "Background fixture must exercise depth zero");
        analytic(*build(sky), quadrature({{360, 0}, {439, 0}, {460, 1}, {481, 0}, {830, 0}}, 2, {}),
                 0x66f59a71768bba49ULL, "background", 0);
        analytic(*native.scene,
                 quadrature(diffuse_light, 1,
                            {{360, 0.1}, {476, 0.8}, {503.5, 0.2}, {575, 0.9}, {830, 0.3}}),
                 0x495d9cb540c04df3ULL, "diffuse", 1);

        constexpr float widest = std::numeric_limits<float>::max();
        auto diffuse = load(directory / "diffuse.pbrt");
        rgb_materials(diffuse);
        diffuse.materials.at(diffuse.instance_materials.front()).knots = {{-widest, 0.25f},
                                                                          {widest, 0.75f}};
        analytic(*build(diffuse), quadrature(diffuse_light, 1, {{360, 0.5}, {830, 0.5}}),
                 0x7959a5d008b72260ULL, "wide diffuse SPD", 1);

        diffuse.environment.fill(0);
        auto black =
            progressive(diffuse, {.width = 3, .height = 5, .samples = 19, .batch_size = 7});
        require(std::ranges::all_of(black.pixels,
                                    [&](const auto &p) {
                                        return p.sum == XYZ{} && p.correction == XYZ{} &&
                                               linear_rgb(black, p) == XYZ{};
                                    }),
                "A PBRT scene without a light must render exact black");

        depth_and_batch(load(directory / "multibounce.pbrt"));
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
