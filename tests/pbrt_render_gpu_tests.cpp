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

SpectralMoments quadrature(const Curve &light, double scale, const Curve &reflectance) {
    const auto &cie = detail::cie_xyz;
    double normalization = 0;
    for (std::size_t i = 0; i < cie.size(); ++i) {
        normalization += value_at(light, 360.0 + static_cast<double>(i)) * cie[i][1];
    }
    const auto angular = reflectance.empty() ? AngularMoments{} : diffuse_angular_moments();
    SpectralMoments moments;
    constexpr int subintervals = 128;
    constexpr double width = 1.0 / subintervals;
    for (int nm = 360; nm < 830; ++nm) {
        for (int step = 0; step < subintervals; ++step) {
            const double wavelength = nm + (step + 0.5) * width;
            const auto index =
                static_cast<std::size_t>(nm - 360) + (step >= subintervals / 2 ? 1u : 0u);
            const double rho = reflectance.empty() ? 1 : value_at(reflectance, wavelength);
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

        auto sky = load(directory / "background.pbrt");
        require(sky.max_depth == 0, "Background fixture must exercise depth zero");
        analytic(*build(sky), quadrature({{360, 0}, {439, 0}, {460, 1}, {481, 0}, {830, 0}}, 2, {}),
                 0x66f59a71768bba49ULL, "background", 0);
        analytic(*native.scene,
                 quadrature({{360, 0.25}, {485, 0.8}, {650, 1}, {830, 0.25}}, 1,
                            {{360, 0.1}, {476, 0.8}, {503.5, 0.2}, {575, 0.9}, {830, 0.3}}),
                 0x495d9cb540c04df3ULL, "diffuse", 1);

        constexpr float widest = std::numeric_limits<float>::max();
        auto diffuse = load(directory / "diffuse.pbrt");
        diffuse.materials.at(diffuse.instance_materials.front()).knots = {{-widest, 0.25f},
                                                                          {widest, 0.75f}};
        analytic(*build(diffuse),
                 quadrature({{360, 0.25}, {485, 0.8}, {650, 1}, {830, 0.25}}, 1,
                            {{360, 0.5}, {830, 0.5}}),
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
