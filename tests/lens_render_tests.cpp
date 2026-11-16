// SPDX-License-Identifier: GPL-3.0-only
#include "metal_render.hpp"
#include "pbrt_render.hpp"
#if defined(RYPATH_ENABLE_LENS_PROBE)
#include "lens_probe.hpp"
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {
using namespace rypath;
void require(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
void near(double actual, double expected, double tolerance, const std::string &why) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
            why + ": actual " + std::to_string(actual) + ", expected " + std::to_string(expected));
}
bool exact(const Film &a, const Film &b) {
    if (a.pixels.size() != b.pixels.size())
        return false;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        if (a.pixels[i].count != b.pixels[i].count)
            return false;
        for (unsigned c = 0; c < 3; ++c)
            if (std::bit_cast<std::uint64_t>(a.pixels[i].sum[c]) !=
                    std::bit_cast<std::uint64_t>(b.pixels[i].sum[c]) ||
                std::bit_cast<std::uint64_t>(a.pixels[i].correction[c]) !=
                    std::bit_cast<std::uint64_t>(b.pixels[i].correction[c]))
                return false;
    }
    return true;
}
RenderScene empty_scene() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.volume_transport = true;
    scene.raster_camera = RasterCamera{{}, {-1, 1, 1}, {2, 0, 0}, {0, -2, 0}};
    scene.perspective_lens = PerspectiveLens{.3f, 4, {1, 0, 0}, {0, -1, 0}};
    scene.environment.fill(1);
    scene.max_depth = 3;
    return scene;
}
void transport() {
    for (const auto sampler : {SamplerKind::independent, SamplerKind::sobol})
        for (const auto mode : {SobolRandomization::none, SobolRandomization::permutedigits,
                                SobolRandomization::fastowen, SobolRandomization::owen})
            for (const auto filter : {PixelFilterKind::box, PixelFilterKind::triangle}) {
                if (sampler == SamplerKind::independent && mode != SobolRandomization::fastowen)
                    continue;
                auto scene = empty_scene();
                scene.filter = {filter, filter == PixelFilterKind::box ? .5f : 2.f,
                                filter == PixelFilterKind::box ? .5f : 2.f};
                RenderSettings settings{8, 6, 33, 7, 19};
                settings.sampler.kind = sampler;
                settings.sampler.randomization = mode;
                const auto lens = render_progressive(scene, settings);
                require(lens.completion == RenderCompletion::complete, lens.diagnostic);
                auto pinhole = scene;
                pinhole.perspective_lens.reset();
                const auto control = render_progressive(pinhole, settings);
                require(control.completion == RenderCompletion::complete, control.diagnostic);
                require(
                    exact(lens.film, control.film),
                    "constant environment radiance and wavelength stream independent of aperture");
                std::uint64_t done = 0;
                RenderControl stop;
                stop.progress = [&](auto n) { done = n; };
                stop.cancelled = [&] { return done >= 7; };
                const auto partial = render_progressive(scene, settings, stop);
                require(partial.completion == RenderCompletion::cancelled && done == 7,
                        "lens cancellation boundary");
                stop = {};
                stop.initial_film = &partial.film;
                settings.batch_size = 5;
                const auto resumed = render_progressive(scene, settings, stop);
                require(resumed.completion == RenderCompletion::complete &&
                            exact(lens.film, resumed.film),
                        "lens raw Film exactly resumes with different batch size");
                // Geometry-free scattering uses its own rendering kernel. Aperture
                // displacement must affect finite-source transport, yet partitioning
                // must preserve every absolute sample and accumulation operation.
                scene.environment.fill(0);
                scene.camera_medium = 0;
                HomogeneousMedium medium;
                medium.sigma_a.fill(.2f);
                medium.sigma_s.fill(.8f);
                scene.media.push_back(medium);
                PointLight point;
                point.position = {1, .3f, 2};
                point.intensity.fill(1);
                scene.point_lights.push_back(point);
                settings.samples = 64;
                settings.batch_size = 11;
                const auto volume = render_progressive(scene, settings);
                require(volume.completion == RenderCompletion::complete, volume.diagnostic);
                settings.batch_size = 3;
                const auto partitioned = render_progressive(scene, settings);
                require(partitioned.completion == RenderCompletion::complete &&
                            exact(volume.film, partitioned.film),
                        "geometry-free lens medium exact batch partition");
                double signal = 0;
                for (const auto &pixel : volume.film.pixels)
                    signal += pixel.sum[1] + pixel.correction[1];
                require(signal > 0, "geometry-free medium has positive finite-source signal");
            }
}
void textured_boundary() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("rypath-lens-boundary-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    try {
        const auto image = std::filesystem::path(__FILE__).parent_path() /
                           "fixtures/pbrt-render/image-assets/asymmetric.png";
        const auto make = [&](bool boundary) {
            const auto path = directory / "scene.pbrt";
            std::ofstream out(path);
            out << "LookAt 3 2 -1 3 2 3 0 1 0\nCamera \"perspective\" \"float fov\" [35] "
                   "\"float lensradius\" [.35] \"float focaldistance\" [4]\n"
                   "Film \"rgb\" \"integer xresolution\" [8] \"integer yresolution\" [6]\n"
                   "PixelFilter \"triangle\" \"float xradius\" [1] \"float yradius\" [1]\n"
                   "Sampler \"sobol\"\nIntegrator \"volpath\" \"integer maxdepth\" [1]\n"
                   "WorldBegin\nLightSource \"infinite\" \"spectrum L\" [360 1 830 1]\n";
            if (boundary)
                out << "AttributeBegin\nMaterial \"interface\"\nShape \"trianglemesh\" "
                       "\"point3 P\" [-10 -10 0 20 -10 0 20 20 0 -10 20 0] "
                       "\"integer indices\" [0 1 2 0 2 3]\nAttributeEnd\n";
            out << "Texture \"image\" \"spectrum\" \"imagemap\" \"string filename\" [\""
                << image.string()
                << "\"] \"string filter\" [\"ewa\"]\n"
                   "Material \"diffuse\" \"texture reflectance\" [\"image\"]\n"
                   "Shape \"trianglemesh\" \"point3 P\" [-5 -6 1.4 11 -6 4.6 11 10 4.6 -5 10 1.4] "
                   "\"integer indices\" [0 1 2 0 2 3] \"point2 uv\" [0 0 8 0 8 8 0 8]\n";
            out.close();
            const auto checked = pbrt::check(path);
            require(checked.exit_code() == 0, pbrt::report(checked));
            auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
            require(translated.scene.has_value(), "textured boundary scene translation");
            return std::move(*translated.scene);
        };
        const auto direct = make(false), boundary = make(true);
        for (const auto sampler : {SamplerKind::independent, SamplerKind::sobol}) {
            RenderSettings settings{8, 6, 97, 13, 37};
            settings.sampler.kind = sampler;
            const auto a = render_progressive(direct, settings);
            const auto b = render_progressive(boundary, settings);
            require(a.completion == RenderCompletion::complete, a.diagnostic);
            require(b.completion == RenderCompletion::complete, b.diagnostic);
            double signal = 0;
            for (std::size_t pixel = 0; pixel < a.film.pixels.size(); ++pixel)
                for (unsigned c = 0; c < 3; ++c) {
                    const double expected =
                        a.film.pixels[pixel].sum[c] + a.film.pixels[pixel].correction[c];
                    const double actual =
                        b.film.pixels[pixel].sum[c] + b.film.pixels[pixel].correction[c];
                    near(actual, expected, .003 * std::max(1., std::abs(expected)),
                         "null boundary preserves lens textured differential footprint");
                    signal += expected;
                }
            require(signal > 0, "textured lens control has positive signal");
        }
        std::filesystem::remove_all(directory);
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
}
#if defined(RYPATH_ENABLE_LENS_PROBE)
struct V {
    double x, y, z;
    V operator+(V b) const { return {x + b.x, y + b.y, z + b.z}; }
    V operator-(V b) const { return {x - b.x, y - b.y, z - b.z}; }
    V operator*(double s) const { return {x * s, y * s, z * s}; }
};
double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V normalized(V v) { return v * (1 / std::sqrt(dot(v, v))); }
V vector(render_wire::Point p) { return {p.x, p.y, p.z}; }
void close(V a, V b, double tolerance, const std::string &why) {
    near(a.x, b.x, tolerance, why + " x");
    near(a.y, b.y, tolerance, why + " y");
    near(a.z, b.z, tolerance, why + " z");
}
// Scalar Random123 reference with 64-bit products, independent of MSL mulhi.
std::array<std::uint32_t, 4> philox(std::array<std::uint32_t, 4> c, std::uint32_t lo,
                                    std::uint32_t hi) {
    for (unsigned r = 0; r < 10; ++r) {
        const auto a = std::uint64_t{0xd2511f53u} * c[0];
        const auto b = std::uint64_t{0xcd9e8d57u} * c[2];
        c = {std::uint32_t(b >> 32) ^ c[1] ^ lo, std::uint32_t(b),
             std::uint32_t(a >> 32) ^ c[3] ^ hi, std::uint32_t(a)};
        lo += 0x9e3779b9u;
        hi += 0xbb67ae85u;
    }
    return c;
}
double unit(std::uint32_t word, bool sobol_sampler) {
    return sobol_sampler ? sobol::uniform(word) : double(word >> 8) * 0x1p-24;
}
// Inverse tent CDF, evaluated in double with its direct square-root branches.
double tent(double u, double radius) {
    return radius * (u < .5 ? std::sqrt(2 * u) - 1 : 1 - std::sqrt(2 - 2 * u));
}
V disk(double u, double v) {
    const double a = 2 * u - 1, b = 2 * v - 1;
    if (a == 0 && b == 0)
        return {};
    double r, theta;
    if (std::abs(a) > std::abs(b)) {
        r = a;
        theta = std::numbers::pi / 4 * b / a;
    } else {
        r = b;
        theta = std::numbers::pi / 2 - std::numbers::pi / 4 * a / b;
    }
    return {r * std::cos(theta), r * std::sin(theta), 0};
}
void rays() {
    for (const auto origin_case : {0, 1, 2, 3})
        for (const bool reflected : {false, true})
            for (const bool sobol_sampler : {false, true})
                for (const auto mode : {0u, 1u, 2u, 3u})
                    for (const int filter_case : {0, 1, 2, 3}) {
                        const bool triangle = filter_case == 2;
                        const bool centered = filter_case != 0;
                        if (!sobol_sampler && mode != 0)
                            continue;
                        render_wire::Parameters p{};
                        p.origin = {3, -2, 1};
                        p.forward = {-1, 1, 1};
                        p.right = {2, 0, 0};
                        p.up = {0, -2, 0};
                        p.lens_x = {1, 0, 0};
                        p.lens_y = {0, -1, 0};
                        if (reflected) {
                            p.forward.x *= -1;
                            p.right.x *= -1;
                            p.lens_x.x *= -1;
                        }
                        if (origin_case != 0) {
                            p.origin = {1e7f, 1e7f, 0};
                            if (origin_case >= 2) {
                                const auto rotate = [](render_wire::Point value) {
                                    const float h = std::sqrt(.5f);
                                    return render_wire::Point{h * (value.x - value.y),
                                                              h * (value.x + value.y), value.z};
                                };
                                p.forward = rotate(p.forward);
                                p.right = rotate(p.right);
                                p.up = rotate(p.up);
                                p.lens_x = rotate(p.lens_x);
                                p.lens_y = rotate(p.lens_y);
                            }
                        }
                        if (origin_case == 3) {
                            const auto rotate = [](render_wire::Point value) {
                                const float c = std::sqrt(.75f), s = .5f;
                                return render_wire::Point{c * value.x + s * value.z, value.y,
                                                          -s * value.x + c * value.z};
                            };
                            p.forward = rotate(p.forward);
                            p.right = rotate(p.right);
                            p.up = rotate(p.up);
                            p.lens_x = rotate(p.lens_x);
                            p.lens_y = rotate(p.lens_y);
                        }
                        p.width = 8;
                        p.height = 6;
                        p.pixel_begin = 3;
                        p.pixel_count = 37;
                        p.sample_low = 17;
                        p.seed_low = 19317;
                        p.seed_high = sobol_sampler ? 0 : 0xabcdef12u;
                        p.lane_count = 37 * 97 + 3;
                        p.model = render_wire::pbrt_model;
                        p.sampler_kind = sobol_sampler ? render_wire::sobol_sampler : 0;
                        p.sobol_scale_exponent = sobol_scale_exponent(p.width, p.height);
                        p.randomization = mode;
                        p.filter_kind = triangle ? render_wire::triangle_filter : 0;
                        p.filter_x_radius = filter_case == 3 ? .125f : centered ? 2 : .5f;
                        p.filter_y_radius = filter_case == 3 ? .25f : centered ? 1.25f : .5f;
                        p.lens_radius = origin_case == 0 ? .375f : 1;
                        p.focal_distance = 4;
                        const auto values = lens_gpu_probe(p);
                        double radial_sum = 0;
                        for (unsigned lane = 0; lane < p.lane_count; ++lane) {
                            const auto pixel = p.pixel_begin + lane % p.pixel_count;
                            const auto sample = p.sample_low + lane / p.pixel_count;
                            std::array<std::uint32_t, 4> camera;
                            std::array<std::uint32_t, 2> lens;
                            if (sobol_sampler) {
                                const auto &tables = sobol_tables();
                                const auto index =
                                    sobol::index(tables, p.sobol_scale_exponent, pixel % p.width,
                                                 pixel / p.width, sample);
                                for (unsigned i = 0; i < 4; ++i)
                                    camera[i] = sobol::word(tables, index, i, i < 2 ? 0 : mode,
                                                            i < 2 ? 0 : p.seed_low);
                                for (unsigned i = 0; i < 2; ++i)
                                    lens[i] =
                                        sobol::word(tables, index, 1022 + i, mode, p.seed_low);
                            } else {
                                camera = philox({pixel, sample, 0, 0}, p.seed_low, p.seed_high);
                                const auto words = philox({pixel, sample, 0, 0xc0000000u},
                                                          p.seed_low, p.seed_high);
                                lens = {words[0], words[1]};
                            }
                            const auto &actual = values[lane];
                            require(std::equal(camera.begin(), camera.end(), actual.camera_words),
                                    "lens extension changes core camera/wavelength/roulette words");
                            require(std::equal(lens.begin(), lens.end(), actual.lens_words),
                                    "reserved lens stream words");
                            double u = unit(camera[0], sobol_sampler),
                                   v = unit(camera[1], sobol_sampler);
                            if (sobol_sampler) {
                                u = sobol::camera_uniform(camera[0], p.sobol_scale_exponent,
                                                          pixel % p.width);
                                v = sobol::camera_uniform(camera[1], p.sobol_scale_exponent,
                                                          pixel / p.width);
                            }
                            const double x = (pixel % p.width +
                                              (triangle   ? .5 + tent(u, 2)
                                               : centered ? .5 + (2 * u - 1) * p.filter_x_radius
                                                          : u)) /
                                             p.width;
                            const double y = (pixel / p.width +
                                              (triangle   ? .5 + tent(v, 1.25)
                                               : centered ? .5 + (2 * v - 1) * p.filter_y_radius
                                                          : v)) /
                                             p.height;
                            auto d = vector(p.forward) + vector(p.right) * x + vector(p.up) * y;
                            if (centered) {
                                // Independently round the documented centered raster/FMA
                                // contract before the high-precision geometric oracle.
                                const float ox =
                                    float(triangle ? tent(u, 2) : (2 * u - 1) * p.filter_x_radius);
                                const float oy = float(triangle ? tent(v, 1.25)
                                                                : (2 * v - 1) * p.filter_y_radius);
                                const float fx =
                                    ((float(pixel % p.width) + .5f) + ox) / float(p.width);
                                const float fy =
                                    ((float(pixel / p.width) + .5f) + oy) / float(p.height);
                                d = {std::fma(fy, p.up.x, std::fma(fx, p.right.x, p.forward.x)),
                                     std::fma(fy, p.up.y, std::fma(fx, p.right.y, p.forward.y)),
                                     std::fma(fy, p.up.z, std::fma(fx, p.right.z, p.forward.z))};
                            }
                            const auto aperture =
                                disk(unit(lens[0], sobol_sampler), unit(lens[1], sobol_sampler));
                            const auto ideal =
                                (vector(p.lens_x) * aperture.x + vector(p.lens_y) * aperture.y) *
                                p.lens_radius;
                            const V origin{
                                static_cast<float>(p.origin.x + static_cast<float>(ideal.x)),
                                static_cast<float>(p.origin.y + static_cast<float>(ideal.y)),
                                static_cast<float>(p.origin.z + static_cast<float>(ideal.z))};
                            const auto offset = origin - vector(p.origin);
                            close(vector(actual.lens_offset), offset, 3e-6,
                                  "uniform concentric disk");
                            close(vector(actual.origin), origin, 3e-6, "world aperture origin");
                            const auto focus = vector(p.origin) + d * p.focal_distance;
                            close(vector(actual.direction), normalized(focus - origin), 4e-6,
                                  "analytic focused ray");
                            for (const auto depth : {2., 4., 8.}) {
                                const auto optical = normalized(
                                    vector(p.forward) + vector(p.right) * .5 + vector(p.up) * .5);
                                const auto hit = vector(actual.origin) +
                                                 vector(actual.direction) *
                                                     ((depth - dot(optical, offset)) /
                                                      dot(optical, vector(actual.direction)));
                                const auto to_focus = d * p.focal_distance - offset;
                                const auto expected = vector(p.origin) + offset +
                                                      to_focus * ((depth - dot(optical, offset)) /
                                                                  dot(optical, to_focus));
                                close(hit, expected, 4e-5,
                                      "defocus intersection at camera-axis depth");
                            }
                            // Intersect the primary and its neighbors with a tilted
                            // plane. A helper that reuses pinhole origins or resamples
                            // the disk cannot satisfy these independent intersections.
                            const V normal{.13, -.21, 1};
                            const V plane = vector(p.origin) + V{0, 0, 7};
                            for (unsigned axis = 0; axis < 2; ++axis) {
                                const auto adjacent =
                                    d + vector(axis == 0 ? p.right : p.up) *
                                            (1. / (axis == 0 ? p.width : p.height));
                                const auto expected_direction = normalized(
                                    vector(p.origin) + adjacent * p.focal_distance - origin);
                                const auto direction = vector(axis == 0 ? actual.dx : actual.dy);
                                close(direction, expected_direction, 4e-6,
                                      "same-aperture ray differential");
                                const auto hit = origin + direction * (dot(normal, plane - origin) /
                                                                       dot(normal, direction));
                                const auto expected_hit =
                                    origin + expected_direction * (dot(normal, plane - origin) /
                                                                   dot(normal, expected_direction));
                                close(hit, expected_hit, 8e-5,
                                      "tilted plane differential intersection");
                                const V probe_normal{double(.2f), double(-.1f), 1};
                                const auto primary_direction = normalized(focus - origin);
                                const auto primary_hit =
                                    offset +
                                    primary_direction * ((4 - dot(probe_normal, offset)) /
                                                         dot(probe_normal, primary_direction));
                                const auto neighbor_hit =
                                    offset +
                                    expected_direction * ((4 - dot(probe_normal, offset)) /
                                                          dot(probe_normal, expected_direction));
                                close(vector(axis == 0 ? actual.dpdx : actual.dpdy),
                                      neighbor_hit - primary_hit, 3e-5,
                                      "production lens-relative tilted-plane footprint");
                            }
                            radial_sum += dot(offset, offset) / (p.lens_radius * p.lens_radius);
                        }
                        if (origin_case == 0)
                            near(radial_sum / p.lane_count, .5, .04,
                                 "uniform disk second radial moment");
                    }
}
void high_index_words() {
    for (const bool qmc : {false, true})
        for (unsigned mode = 0; mode < 4; ++mode) {
            if (!qmc && mode != 0)
                continue;
            const auto limit = std::uint64_t{1} << 46; // m=3: 52-2*m sample bits.
            for (const auto begin : {std::uint64_t{UINT32_MAX} - 2, limit - 8}) {
                render_wire::Parameters p{};
                p.width = 8;
                p.height = 6;
                p.pixel_begin = 3;
                p.pixel_count = 5;
                p.lane_count = 38; // Eight samples, including padded output lanes.
                p.sample_low = static_cast<std::uint32_t>(begin);
                p.sample_high = static_cast<std::uint32_t>(begin >> 32);
                p.seed_low = 0x80000000u;
                p.seed_high = qmc ? 0 : 0xfedcba98u;
                p.sampler_kind = qmc ? render_wire::sobol_sampler : 0;
                p.randomization = mode;
                p.sobol_scale_exponent = 3;
                p.model = render_wire::pbrt_model;
                p.forward = {-.5f, .5f, 1};
                p.right = {1, 0, 0};
                p.up = {0, -1, 0};
                p.filter_x_radius = 1.5f;
                p.filter_y_radius = .75f;
                p.lens_radius = .25f;
                p.focal_distance = 4;
                p.lens_x = {1, 0, 0};
                p.lens_y = {0, 1, 0};
                const auto actual = lens_gpu_probe(p);
                for (unsigned lane = 0; lane < p.lane_count; ++lane) {
                    const auto pixel = p.pixel_begin + lane % p.pixel_count;
                    const std::uint64_t sample = begin + lane / p.pixel_count;
                    std::array<std::uint32_t, 4> core;
                    std::array<std::uint32_t, 2> lens;
                    if (qmc) {
                        const auto &tables = sobol_tables();
                        const auto index =
                            sobol::index(tables, 3, pixel % p.width, pixel / p.width, sample);
                        for (unsigned i = 0; i < 4; ++i)
                            core[i] = sobol::word(tables, index, i, i < 2 ? 0 : mode,
                                                  i < 2 ? 0 : p.seed_low);
                        for (unsigned i = 0; i < 2; ++i)
                            lens[i] = sobol::word(tables, index, 1022 + i, mode, p.seed_low);
                    } else {
                        const auto low = static_cast<std::uint32_t>(sample);
                        const auto high = static_cast<std::uint32_t>(sample >> 32);
                        core = philox({pixel, low, high, 0}, p.seed_low, p.seed_high);
                        const auto words =
                            philox({pixel, low, high, 0xc0000000u}, p.seed_low, p.seed_high);
                        lens = {words[0], words[1]};
                    }
                    require(std::equal(core.begin(), core.end(), actual[lane].camera_words) &&
                                std::equal(lens.begin(), lens.end(), actual[lane].lens_words),
                            "64-bit sample carry and upper Sobol sample range preserve lens/core "
                            "words");
                    const double u = qmc ? sobol::camera_uniform(core[0], 3, pixel % p.width)
                                         : unit(core[0], false);
                    const double v = qmc ? sobol::camera_uniform(core[1], 3, pixel / p.width)
                                         : unit(core[1], false);
                    const double x = (pixel % p.width + .5 + (2 * u - 1) * 1.5) / p.width;
                    const double y = (pixel / p.width + .5 + (2 * v - 1) * .75) / p.height;
                    const auto aperture = disk(unit(lens[0], qmc), unit(lens[1], qmc)) * .25;
                    const auto direction = normalized(
                        (vector(p.forward) + vector(p.right) * x + vector(p.up) * y) * 4 -
                        aperture);
                    close(vector(actual[lane].direction), direction, 4e-6,
                          "high-index anisotropic box lens direction");
                }
            }
        }
}
void absorbing_target() {
    for (const auto sampler : {SamplerKind::independent, SamplerKind::sobol})
        for (const auto filter : {PixelFilterKind::box, PixelFilterKind::triangle}) {
            auto scene = empty_scene();
            scene.environment.fill(0);
            scene.raster_camera = RasterCamera{{}, {-.4f, .4f, 1}, {.8f, 0, 0}, {0, -.8f, 0}};
            scene.perspective_lens = PerspectiveLens{.75f, 2, {1, 0, 0}, {0, 1, 0}};
            scene.filter = {filter, .5f, .5f};
            scene.max_depth = 0;
            scene.camera_medium = 0;
            HomogeneousMedium absorber;
            absorber.sigma_a.fill(.4f);
            scene.media.push_back(absorber);
            GeometryMesh target;
            target.positions = {{-100, -100, 4}, {100, -100, 4}, {100, 100, 4}, {-100, 100, 4}};
            target.triangles = {{0, 1, 2}, {0, 2, 3}};
            scene.geometry.meshes.push_back(target);
            scene.geometry.instances.push_back({0, {}});
            SpectralMaterial black;
            black.kind = SpectralMaterialKind::constant;
            scene.materials.push_back(black);
            scene.instance_materials = {0};
            AreaEmission emission;
            emission.radiance.fill(1);
            emission.two_sided = true;
            scene.area_emissions.push_back(emission);
            scene.instance_emissions = {0};
            auto built = build_metal_renderer(scene);
            require(built.status == GeometryStatus::success, built.diagnostic);
            RenderRequest request{.width = 1,
                                  .height = 1,
                                  .seed = 19317,
                                  .pixel_count = 1,
                                  .sample_count = 4096,
                                  .total_samples = 4096};
            request.sampler.kind = sampler;
            const auto samples = built.scene->sample(request);
            require(samples.status == GeometryStatus::success, samples.diagnostic);
            require(samples.samples.size() == request.sample_count && samples.guard_count > 0,
                    "absorbing target sample accounting");
            double aperture_effect = 0, residual = 0, variance = 0;
            for (const auto &sample : samples.samples) {
                std::array<std::uint32_t, 4> core;
                std::array<std::uint32_t, 2> lens;
                const bool qmc = sampler == SamplerKind::sobol;
                if (qmc) {
                    const auto &tables = sobol_tables();
                    const auto index = sobol::index(tables, 0, 0, 0, sample.sample);
                    for (unsigned i = 0; i < 4; ++i)
                        core[i] = sobol::word(tables, index, i, i < 2 ? 0 : sobol::fastowen,
                                              i < 2 ? 0 : static_cast<unsigned>(request.seed));
                    for (unsigned i = 0; i < 2; ++i)
                        lens[i] = sobol::word(tables, index, 1022 + i, sobol::fastowen,
                                              static_cast<unsigned>(request.seed));
                } else {
                    core = philox({0, static_cast<unsigned>(sample.sample), 0, 0},
                                  static_cast<unsigned>(request.seed), 0);
                    const auto words =
                        philox({0, static_cast<unsigned>(sample.sample), 0, 0xc0000000u},
                               static_cast<unsigned>(request.seed), 0);
                    lens = {words[0], words[1]};
                }
                const double u = unit(core[0], qmc), v = unit(core[1], qmc);
                const double x = filter == PixelFilterKind::triangle ? .5 + tent(u, .5) : u;
                const double y = filter == PixelFilterKind::triangle ? .5 + tent(v, .5) : v;
                const V pinhole{double(-.4f) + double(.8f) * x, double(.4f) - double(.8f) * y, 1};
                const auto offset = disk(unit(lens[0], qmc), unit(lens[1], qmc)) * .75;
                const auto direction = normalized(pinhole * 2 - offset);
                const double distance = 4 / direction.z;
                const double expected = std::exp(-double(.4f) * distance);
                // The production medium estimator samples analog absorption:
                // survival is Bernoulli, with mean exp(-sigma * ray_length).
                // Independently reproduce its disjoint free-flight integer word
                // and compare the event to this analytic camera-to-plane length.
                const auto flight_words =
                    philox({0, static_cast<unsigned>(sample.sample),
                            static_cast<unsigned>(sample.sample >> 32), 0x20000000u},
                           static_cast<unsigned>(request.seed), 0x6d656431u);
                const double flight = -std::log1p(-unit(flight_words[0], false)) / double(.4f);
                const double survival = flight < distance ? 0 : 1;
                near(sample.radiance, survival, 0,
                     "analog absorption event uses analytic lens camera-to-emitter distance");
                residual += sample.radiance - expected;
                variance += expected * (1 - expected);
                aperture_effect +=
                    std::abs(expected - std::exp(-double(.4f) * (4 / normalized(pinhole).z)));
            }
            near(residual / request.sample_count, 0,
                 6 * std::sqrt(variance) / request.sample_count + 1e-4,
                 "analog lens absorption mean agrees with Beer-Lambert expectation");
            require(aperture_effect / request.sample_count > .001,
                    "Beer control detects physically significant aperture path-length change");
        }
}
#endif
} // namespace
int main(int argc, char **argv) try {
    bool no_device = false, no_rt = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        require(flag == "--allow-no-device" || flag == "--allow-no-ray-tracing",
                "unknown lens option");
        no_device |= flag == "--allow-no-device";
        no_rt |= flag == "--allow-no-ray-tracing";
    }
    const auto capability = build_metal_renderer(empty_scene());
    if ((no_device && capability.status == GeometryStatus::no_device) ||
        (no_rt && capability.status == GeometryStatus::no_ray_tracing)) {
        std::cout << "SKIP Metal lenses: " << capability.diagnostic << '\n';
        return 77;
    }
    require(capability.status == GeometryStatus::success, capability.diagnostic);
#if defined(RYPATH_ENABLE_LENS_PROBE)
    rays();
    high_index_words();
    absorbing_target();
#endif
    transport();
    textured_boundary();
    std::cout
        << "PASS lens ray geometry, differentials, constant radiometry, volume and exact resume\n";
} catch (const std::exception &error) {
    std::cerr << "Lens render: " << error.what() << '\n';
    return 1;
}
