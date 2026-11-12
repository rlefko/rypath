// SPDX-License-Identifier: GPL-3.0-only

#include "pbrt_render.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {

using namespace rypath;
using namespace rypath::pbrt;
namespace fs = std::filesystem;
using XYZ = std::array<double, 3>;

constexpr std::string_view basic = R"(Camera "perspective"
Film "rgb"
PixelFilter "box"
Sampler "independent"
Integrator "path"
WorldBegin
)";
constexpr std::string_view triangle = R"(Shape "trianglemesh" "point3 P" [0 0 2 1 0 2 0 1 2]
    "integer indices" [0 1 2]
)";

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}

void near(double actual, double expected, double tolerance, std::string_view name) {
    require(std::abs(actual - expected) <= tolerance, std::string(name) + " expected " +
                                                          std::to_string(expected) + ", observed " +
                                                          std::to_string(actual));
}

void point_near(GeometryPoint actual, GeometryPoint expected, std::string_view name) {
    near(actual.x, expected.x, 2e-6, name);
    near(actual.y, expected.y, 2e-6, name);
    near(actual.z, expected.z, 2e-6, name);
}

// Fixture paths precede the PBRT source text at each call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
fs::path write(const fs::path &directory, std::string_view name, std::string_view source) {
    auto file = directory / name;
    std::ofstream out(file);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << source;
    out.close();
    return file;
}

RenderTranslation translation(const fs::path &entry, const Limits &limits = {}) {
    const auto checked = check(entry);
    require(checked.exit_code() == 0, report(checked));
    return translate(checked, Location{entry.string()}, limits);
}

struct AcceptedScene {
    RenderScene scene;
    RenderSettings settings;
};

AcceptedScene accepted(const fs::path &entry) {
    auto result = translation(entry);
    if (!result.scene || result.exit_code() != 0) {
        for (const auto &diagnostic : result.diagnostics) {
            std::cerr << *diagnostic.location.file << ':' << diagnostic.location.line << ':'
                      << diagnostic.location.column << ": " << diagnostic.message << '\n';
        }
        throw std::runtime_error("Expected supported render profile for " + entry.string());
    }
    require(validate_render_scene(*result.scene).empty(), "Invalid translated renderer scene");
    require(validate_render_settings(result.settings).empty(),
            "Invalid translated render settings");
    return {std::move(result.scene.value()), result.settings};
}

const RasterCamera &raster_camera(const RenderScene &scene) {
    if (!scene.raster_camera) {
        throw std::runtime_error("Missing PBRT raster camera");
    }
    return scene.raster_camera.value();
}

// Follow std::string::replace ordering of the source, old text, and new text.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::string replaced(std::string_view source, std::string_view from, std::string_view to) {
    std::string result(source);
    const auto offset = result.find(from);
    require(offset != std::string::npos, "Missing template token");
    return result.replace(offset, from.size(), to);
}

void effective_defaults_and_cameras(const fs::path &scratch) {
    // accepted() destroys both the preflight result and entry Location before returning.
    const auto defaults =
        accepted(write(scratch, "defaults.pbrt", std::string(basic) + std::string(triangle)));
    const auto &scene = defaults.scene;
    require(scene.model == RenderModel::pbrt && scene.max_depth == 5 && scene.exposure == 1,
            "PBRT model, path maxdepth or sensor exposure default");
    require(defaults.settings.width == 1280 && defaults.settings.height == 720 &&
                defaults.settings.samples == 4 && defaults.settings.seed == 0,
            "PBRT film/sampler defaults");
    const auto &camera = raster_camera(scene);
    point_near(camera.origin, {}, "Default camera position");
    point_near(camera.corner, {-16.f / 9, 1, 1}, "Landscape raster corner");
    point_near(camera.dx, {32.f / 9, 0, 0}, "Landscape x raster step");
    point_near(camera.dy, {0, -2, 0}, "Landscape y raster step");
    require(scene.environment == std::array<float, 471>{}, "No light must produce zero radiance");
    require(scene.geometry.instances.size() == 1 && scene.instance_materials.size() == 1,
            "Default material binding or geometry was lost");
    near(spectral_reflectance(scene.materials.at(scene.instance_materials[0]), 539), 0.5, 1e-12,
         "Default diffuse spectrum");

    const auto portrait =
        accepted(write(scratch, "portrait.pbrt",
                       replaced(basic, R"(Film "rgb")",
                                R"(Film "rgb" "integer xresolution" 4 "integer yresolution" 8)") +
                           std::string(triangle)));
    point_near(raster_camera(portrait.scene).corner, {-1, 2, 1}, "Portrait raster corner");
    point_near(raster_camera(portrait.scene).dx, {2, 0, 0}, "Portrait x raster step");
    point_near(raster_camera(portrait.scene).dy, {0, -4, 0}, "Portrait y raster step");

    const auto shifted = accepted(write(scratch, "shifted.pbrt", R"(Camera "orthographic"
LookAt 1 2 3 2 2 3 0 1 0
Camera "perspective" "float fov" [60] "float fov" [90]
    "float screenwindow" [0.5 2.5 -2 3]
    "float shutteropen" [0.25] "float shutterclose" [0.75]
Film "rgb" "integer xresolution" [4] "integer yresolution" [9] "float iso" [400]
PixelFilter "box"
Sampler "independent" "integer pixelsamples" [3] "integer seed" [27]
Integrator "path" "integer maxdepth" [0]
WorldBegin
)" + std::string(triangle)));
    require(shifted.settings.samples == 3 && shifted.settings.seed == 27 &&
                shifted.scene.max_depth == 0 && shifted.scene.exposure == 2,
            "Explicit PBRT sampler, maxdepth or exposure");
    point_near(raster_camera(shifted.scene).origin, {1, 2, 3}, "LookAt world-space origin");
    point_near(raster_camera(shifted.scene).corner, {1, 3, -0.5}, "Shifted LookAt raster corner");
    point_near(raster_camera(shifted.scene).dx, {0, 0, -2}, "Shifted LookAt raster x");
    point_near(raster_camera(shifted.scene).dy, {0, -5, 0}, "Shifted LookAt raster y");

    const auto reflected =
        accepted(write(scratch, "reflected.pbrt",
                       "Scale -1 1 1\n" +
                           replaced(basic, R"(Camera "perspective")",
                                    R"(Camera "perspective" "float frameaspectratio" [2])") +
                           std::string(triangle)));
    point_near(raster_camera(reflected.scene).corner, {2, 1, 1}, "Reflected raster corner");
    point_near(raster_camera(reflected.scene).dx, {-4, 0, 0}, "Reflected raster x");

    const auto maximum = accepted(write(
        scratch, "maximum-depth.pbrt",
        replaced(basic, R"(Integrator "path")", R"(Integrator "path" "integer maxdepth" [1024])") +
            std::string(triangle)));
    require(maximum.scene.max_depth == 1024, "PBRT accepts the authored depth limit of 1024");
}

void material_instances(const fs::path &scratch) {
    std::string content(basic);
    content += R"(MakeNamedMaterial "spiky" "string type" "diffuse"
    "spectrum reflectance" [440 0 451.5 1 470 0]
Attribute "material" "spectrum reflectance" [360 0.25 830 0.25]
ObjectBegin "pair"
    NamedMaterial "spiky"
    ReverseOrientation
)";
    content += triangle;
    content += R"(    AttributeBegin
        ReverseOrientation
        Scale -1 2 1
        Material "diffuse"
)";
    content += triangle;
    content += R"(    AttributeEnd
ObjectEnd
AttributeBegin
    Translate 3 0 0
    ObjectInstance "pair"
AttributeEnd
AttributeBegin
    Translate -3 0 0
    Scale -1 1 1
    ObjectInstance "pair"
AttributeEnd
Material "diffuse" "spectrum reflectance" [360 0.75 830 0.75]
    "spectrum reflectance" [360 0.5 830 0.5]
)";
    content += triangle;
    const auto translated = accepted(write(scratch, "bindings.pbrt", content));
    const auto &scene = translated.scene;
    require(
        scene.geometry.instances.size() == 5 && scene.geometry.meshes.size() == 3 &&
            scene.instance_materials.size() == 5 && scene.instance_reverse_orientation.size() == 5,
        "PBRT instances must preserve two placements of both object shapes and one direct shape");

    unsigned direct = 0;
    unsigned spiky = 0;
    unsigned inherited = 0;
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        const auto &transform = scene.geometry.instances[i].transform.values;
        const auto &material = scene.materials.at(scene.instance_materials[i]);
        const bool reverse = scene.instance_reverse_orientation[i] != 0;
        const double rho = spectral_reflectance(material, 451.5);
        if (transform[3] == 0) {
            ++direct;
            near(rho, 0.5, 1e-12, "Latest explicit diffuse overrides inherited value");
            require(!reverse, "Object scope leaked ReverseOrientation");
        } else if (transform[5] == 1) {
            ++spiky;
            require(material.knots.size() == 3, "Numeric SPD was resampled onto the demo grid");
            near(rho, 1, 1e-12, "Off-grid spectral peak");
            near(spectral_reflectance(material, 445.75), 0.5, 1e-12, "Linear spectral segment");
            near(spectral_reflectance(material, 439.999), 0, 0,
                 "Spectral support below first knot");
            near(spectral_reflectance(material, 470.001), 0, 0, "Spectral support above last knot");
            require(reverse == (transform[3] > 0), "Named shape normal orientation and placement");
        } else {
            ++inherited;
            near(rho, 0.25, 1e-12, "Inherited inline diffuse");
            require(transform[5] == 2 && transform[0] == (transform[3] > 0 ? -1.0f : 1.0f),
                    "Shape and placement affine transform composition");
            require(reverse == (transform[3] > 0), "Inline shape normal orientation and placement");
        }
    }
    require(direct == 1 && spiky == 2 && inherited == 2, "Material binding counts");
}

void rejects_unsupported_profile(const fs::path &scratch) {
    struct Rejection {
        std::string_view replace;
        std::string_view with;
        std::size_t line;
    };
    constexpr std::array cases{
        Rejection{R"(Camera "perspective")", "", 1},
        Rejection{R"(Camera "perspective")", R"(Camera "perspective" "float lensradius" [0.1])", 1},
        Rejection{R"(Camera "perspective")", "Scale 2 1 1\nCamera \"perspective\"", 2},
        Rejection{R"(Film "rgb")", R"(Film "rgb" "float whitebalance" [6500])", 2},
        Rejection{R"(Film "rgb")", R"(Film "rgb" "float cropwindow" [0.1 0.8 0.2 0.9])", 2},
        Rejection{R"(PixelFilter "box")", R"(PixelFilter "box" "float xradius" [1])", 3},
        Rejection{R"(Integrator "path")", R"(Integrator "path" "integer maxdepth" [1025])", 5},
        Rejection{R"(Integrator "path")", R"(Integrator "path" "bool regularize" [true])", 5},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto &[from, to, line] = cases[i];
        const auto path = write(scratch, "unsupported-" + std::to_string(i) + ".pbrt",
                                replaced(basic, from, to) + std::string(triangle));
        const auto result = translation(path);
        require(result.exit_code() != 0 && !result.scene && !result.diagnostics.empty(),
                "Unsupported PBRT profile translated successfully");
        const auto &source = result.diagnostics.front().location;
        require(fs::path(*source.file).filename() == path.filename() && source.line == line,
                "Translation diagnostic lost effective source location");
    }

    const auto nested = write(scratch, "unsupported-include.pbrt",
                              "\n  Material \"diffuse\" \"rgb reflectance\" [0.5 0.5 0.5]\n" +
                                  std::string(triangle));
    const auto result =
        translation(write(scratch, "include-entry.pbrt",
                          std::string(basic) + "Include \"unsupported-include.pbrt\"\n"));
    require(result.exit_code() != 0 && !result.scene &&
                std::ranges::any_of(result.diagnostics,
                                    [&](const Diagnostic &d) {
                                        return fs::path(*d.location.file).filename() ==
                                                   nested.filename() &&
                                               d.location.line == 2 && d.location.column > 1;
                                    }),
            "Included unsupported reflectance lost source location");

    const auto malformed = write(scratch, "structural-failure.pbrt",
                                 std::string(basic) + "ObjectInstance \"absent\"\n");
    const auto checked = check(malformed);
    const auto forwarded = translate(checked, Location{malformed.string()});
    require(checked.exit_code() != 0 && forwarded.exit_code() == checked.exit_code() &&
                !forwarded.scene,
            "Translation weakened structural preflight failure");
}

void rejects_camera_corner_overflow(const fs::path &scratch) {
    // The individual raster coefficients fit in float, but their sum at a
    // corner after rotating the screen window exceeds the float range.
    const auto camera = R"(Rotate 45 0 0 1
Camera "perspective"
    "float screenwindow" [0 3e38 0 3e38])";
    const auto path =
        write(scratch, "camera-corner-overflow.pbrt",
              replaced(basic, R"(Camera "perspective")", camera) + std::string(triangle));
    const auto result = translation(path);
    require(result.exit_code() == 1 && !result.scene,
            "Binary32 raster direction overflow must fail translation");
    require(std::ranges::any_of(result.diagnostics,
                                [&](const Diagnostic &diagnostic) {
                                    const auto &location = diagnostic.location;
                                    return diagnostic.severity == Severity::error &&
                                           diagnostic.code == "invalid-render-value" &&
                                           fs::path(*location.file).filename() == path.filename() &&
                                           location.line == 3 && location.column == 5;
                                }),
            "Camera corner overflow must identify the authored screen window");
}

void film_crop_precedence(const fs::path &scratch) {
    const auto full_frame = R"(Film "rgb" "integer xresolution" [8] "integer yresolution" [8]
    "integer pixelbounds" [0 4 0 4]
    "float cropwindow" [0 1 0 1])";
    const auto full =
        accepted(write(scratch, "full-frame-crop.pbrt",
                       replaced(basic, R"(Film "rgb")", full_frame) + std::string(triangle)));
    require(full.settings.width == 8 && full.settings.height == 8,
            "A full cropwindow must override partial pixelbounds");

    const auto partial_frame = R"(Film "rgb" "integer xresolution" [8] "integer yresolution" [8]
    "integer pixelbounds" [0 8 0 8]
    "float cropwindow" [0 0.5 0 0.5])";
    const auto partial =
        translation(write(scratch, "partial-crop.pbrt",
                          replaced(basic, R"(Film "rgb")", partial_frame) + std::string(triangle)));
    require(partial.exit_code() == 2 && !partial.scene,
            "A partial cropwindow must override full pixelbounds");
}

void bounded_spectra(const fs::path &scratch) {
    const auto rejected = [&](std::string_view name, std::string_view body) {
        const auto path = write(scratch, name, std::string(basic) + std::string(body));
        const auto checked = check(path);
        const auto result = translate(checked, Location{path.string()});
        require(result.exit_code() != 0 && !result.scene,
                "Invalid spectrum produced a render scene");
        require(!checked.document.diagnostics.empty() || !result.diagnostics.empty(),
                "Invalid spectrum lost its diagnostic");
    };
    rejected("unordered.pbrt", "Material \"diffuse\" \"spectrum reflectance\" [500 0.2 400 0.3]\n" +
                                   std::string(triangle));
    rejected("over-one.pbrt", "Material \"diffuse\" \"spectrum reflectance\" [360 0.2 830 1.01]\n" +
                                  std::string(triangle));
    rejected("collapsed-knots.pbrt",
             "Material \"diffuse\" \"spectrum reflectance\" [450 0.2 450.0000001 0.3]\n" +
                 std::string(triangle));
    rejected("light-no-spectrum.pbrt", R"(LightSource "infinite")");
    rejected("too-many-lights.pbrt", R"(LightSource "infinite" "spectrum L" [360 1 830 1]
LightSource "infinite" "spectrum L" [360 1 830 1]
)");

    const auto extended = accepted(write(
        scratch, "extended.pbrt",
        std::string(basic) + "Material \"diffuse\" \"spectrum reflectance\" [-20 0.25 830 0.75]\n" +
            std::string(triangle)));
    const auto &scene = extended.scene;
    const auto &material = scene.materials.at(scene.instance_materials.front());
    require(material.knots.front().wavelength == -20,
            "PBRT reflectance must retain finite knots outside visible wavelengths");
    near(spectral_reflectance(material, 405), 0.5, 1e-12,
         "Interpolation across an out-of-band knot");

    const auto fixture =
        write(scratch, "bounded.pbrt",
              std::string(basic) +
                  "Material \"diffuse\" \"spectrum reflectance\" [360 0.4 550 0.5 830 1]\n" +
                  std::string(triangle));
    const auto limited = translation(fixture, {.list_values = 4});
    require(limited.exit_code() != 0 && !limited.scene && !limited.diagnostics.empty(),
            "Render translation did not apply the spectrum allocation limit");
}

void infinite_light_normalization(const fs::path &scratch) {
    const auto source = std::string(basic) + std::string(triangle) +
                        R"(LightSource "infinite" "spectrum L" [360 1 830 1])";
    const auto environment = [&](std::string_view suffix) {
        return accepted(write(scratch, "light-normalization.pbrt", source + std::string(suffix)))
            .scene.environment;
    };
    // Sum of the 471 published y-bar entries, with both endpoints included.
    constexpr double y_sum = 106.8569171011719;
    const auto baseline = environment("");
    const auto lit = environment(R"( "float scale" [2] "float illuminance" [3])");
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        near(baseline[i], 1 / y_sum, 1e-9, "Constant infinite-light Y normalization");
        const double expected = static_cast<double>(baseline[i]) * 6 / std::numbers::pi;
        near(lit[i], expected, 2e-5 * std::max(1., std::abs(expected)),
             "Infinite-light scale and illuminance");
    }
    require(environment(R"( "float illuminance" [0])") == baseline &&
                environment(R"( "float illuminance" [-2])") == baseline,
            "Nonpositive illuminance must preserve the default amplitude");
    require(environment(R"( "float scale" [0] "float illuminance" [3])") ==
                std::array<float, 471>{},
            "Zero light scale with a positive Y denominator must produce exact black");

    const auto zero = translation(write(scratch, "light-no-luminance.pbrt",
                                        replaced(source, "[360 1 830 1]", "[360 0 830 0]")));
    require(zero.exit_code() == 1 && !zero.scene &&
                std::ranges::any_of(zero.diagnostics,
                                    [](const Diagnostic &diagnostic) {
                                        return diagnostic.severity == Severity::error &&
                                               diagnostic.code == "invalid-render-value" &&
                                               diagnostic.message.find("CIE Y inner product") !=
                                                   std::string::npos;
                                    }),
            "A zero-light SPD must fail the positive Y normalization guard");
}

void sensor() {
    constexpr XYZ published_479{0.1042979, 0.1334528, 0.8566193};
    constexpr XYZ published_480{0.09564, 0.13902, 0.8129501};
    require(pbrt_cie_xyz(479.4999) == published_479 && pbrt_cie_xyz(479.5) == published_480 &&
                pbrt_cie_xyz(480.4999) == published_480,
            "PBRT nearest-nanometer observer and half ties");
    require(pbrt_cie_xyz(360)[2] > 0 && pbrt_cie_xyz(830)[0] > 0 &&
                pbrt_cie_xyz(359.999) == XYZ{} && pbrt_cie_xyz(830.001) == XYZ{} &&
                pbrt_cie_xyz(std::numeric_limits<double>::quiet_NaN()) == XYZ{},
            "PBRT observer boundary support");
    std::array<float, 471> spectrum{};
    spectrum[119] = 2;
    spectrum[120] = 3;
    near(pbrt_y_inner_product(spectrum), 2 * published_479[1] + 3 * published_480[1], 1e-14,
         "Discrete PBRT Y inner product");

    auto film = make_film(1, 1, RenderModel::pbrt, 2);
    const std::array samples{RenderSample{0, 0, 479.5f, 0.25f}};
    FilmBatch batch(film, 1);
    require(batch.add(samples).empty() && batch.commit(film).empty(), "PBRT spectral accumulation");
    const auto &pixel = film.pixels.front();
    const auto mean = film_mean(pixel);
    for (std::size_t c = 0; c < mean.size(); ++c) {
        near(mean[c], 470 * 0.25 * published_480[c], 1e-12, "Unexposed PBRT XYZ");
    }
    const auto exposed = linear_rgb(film, pixel);
    film.exposure = 1;
    const auto raw = linear_rgb(film, pixel);
    require(exposed == XYZ{2 * raw[0], 2 * raw[1], 2 * raw[2]} && raw[0] < 0 && raw[2] > 1 &&
                film_mean(pixel) == mean,
            "PBRT linear RGB exposure, signed range, and raw accumulation");
}

void owned_fixtures(const fs::path &fixtures) {
    for (const auto name : {"background.pbrt", "diffuse.pbrt", "multibounce.pbrt"}) {
        const auto translated = accepted(fixtures / name);
        require(translated.settings.width == 8 && translated.settings.height == 8 &&
                    translated.settings.samples == 4096 && translated.settings.seed == 0 &&
                    translated.scene.exposure == 1,
                "Reference fixture sampling or exposure");
        const double expected_y = std::string_view(name) == "background.pbrt" ? 2 : 1;
        near(pbrt_y_inner_product(translated.scene.environment), expected_y, 2e-7,
             "Normalized infinite light luminance");
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 3, "Expected scratch directory and PBRT fixtures");
        const fs::path scratch(argv[1]);
        fs::create_directories(scratch);
        effective_defaults_and_cameras(scratch);
        material_instances(scratch);
        rejects_unsupported_profile(scratch);
        rejects_camera_corner_overflow(scratch);
        film_crop_precedence(scratch);
        bounded_spectra(scratch);
        infinite_light_normalization(scratch);
        sensor();
        owned_fixtures(argv[2]);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
