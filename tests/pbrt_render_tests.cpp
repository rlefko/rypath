// SPDX-License-Identifier: GPL-3.0-only

#include "pbrt_render.hpp"
#include "rgb_spectrum.hpp"

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
                defaults.settings.samples == 4 && defaults.settings.seed == 0 &&
                defaults.settings.sampler.kind == SamplerKind::independent,
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

void sobol_translation(const fs::path &scratch) {
    const auto source =
        replaced(basic, R"(Sampler "independent")", R"(Sampler "sobol")") + std::string(triangle);
    const auto defaults = accepted(write(scratch, "sobol-defaults.pbrt", source));
    require(defaults.settings.samples == 16 && defaults.settings.seed == 0 &&
                defaults.settings.sampler.kind == SamplerKind::sobol &&
                defaults.settings.sampler.randomization == SobolRandomization::fastowen,
            "PBRT Sobol defaults");

    constexpr std::array modes{
        std::pair{"none", SobolRandomization::none},
        std::pair{"permutedigits", SobolRandomization::permutedigits},
        std::pair{"fastowen", SobolRandomization::fastowen},
        std::pair{"owen", SobolRandomization::owen},
    };
    for (const auto &[name, mode] : modes) {
        const auto scene = replaced(
            source, R"(Sampler "sobol")",
            std::string(R"(Sampler "sobol" "integer seed" -2147483648 "integer pixelsamples" 3
    "string randomization" ")") +
                name + '"');
        const auto path = write(scratch, "sobol-mode.pbrt", scene);
        const auto result = translation(path);
        require(result.exit_code() == 0 && result.scene && result.settings.samples == 3 &&
                    sobol_signed_seed(result.settings.seed) ==
                        std::numeric_limits<std::int32_t>::min() &&
                    result.settings.sampler.randomization == mode,
                "Sobol randomization, signed seed, and exact requested sample count");
        require(result.diagnostics.size() == 1 &&
                    result.diagnostics.front().severity == Severity::warning &&
                    result.diagnostics.front().location.line == 4 &&
                    *result.diagnostics.front().location.file == path.string(),
                "Non-power-of-two Sobol request emits a source-located warning");
    }

    const auto depth_limit =
        replaced(source, R"(Integrator "path")", R"(Integrator "path" "integer maxdepth" 127)");
    require(accepted(write(scratch, "sobol-depth-limit.pbrt", depth_limit)).scene.max_depth == 127,
            "Sobol depth limit is inclusive");
    const auto depth = translation(write(
        scratch, "sobol-depth.pbrt", replaced(depth_limit, "maxdepth\" 127", "maxdepth\" 128")));
    require(depth.exit_code() == 2 && !depth.scene && depth.diagnostics.size() == 1 &&
                depth.diagnostics.front().location.line == 5 &&
                depth.diagnostics.front().message.find("127") != std::string::npos,
            "Sobol depth overflow is diagnosed at the integrator");

    const auto wide_film = replaced(source, R"(Film "rgb")", R"(Film "rgb" "integer yresolution" 1
    "integer xresolution" 33554432)");
    const auto last =
        replaced(wide_film, R"(Sampler "sobol")", R"(Sampler "sobol" "integer pixelsamples" 4)");
    require(accepted(write(scratch, "sobol-range-limit.pbrt", last)).settings.samples == 4,
            "Sobol range accepts the last representable sample");
    const auto extent =
        translation(write(scratch, "sobol-film.pbrt", replaced(last, "33554432", "33554433")));
    require(extent.exit_code() == 1 && !extent.scene && extent.diagnostics.size() == 1 &&
                extent.diagnostics.front().location.line == 3,
            "Sobol exponent overflow is diagnosed at the larger film resolution");
    const auto range = translation(
        write(scratch, "sobol-range.pbrt", replaced(last, "pixelsamples\" 4", "pixelsamples\" 8")));
    require(range.exit_code() == 1 && !range.scene && range.diagnostics.size() == 1 &&
                range.diagnostics.front().location.line == 5,
            "Sobol index overflow is diagnosed at the sampler");
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
                              "ColorSpace \"rec2020\"\n"
                              "  Material \"diffuse\" \"rgb reflectance\" [0.5 0.5 0.5]\n" +
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

void triangle_filter_translation(const fs::path &scratch) {
    const auto with_filter = [&](std::string_view filter) {
        return replaced(basic, R"(PixelFilter "box")", filter) + std::string(triangle);
    };
    const auto defaults = accepted(
        write(scratch, "triangle-filter-defaults.pbrt", with_filter(R"(PixelFilter "triangle")")));
    require(defaults.scene.filter.kind == PixelFilterKind::triangle &&
                defaults.scene.filter.x_radius == 2 && defaults.scene.filter.y_radius == 2,
            "Triangle filter uses PBRT's per-axis defaults");
    const auto one_axis =
        accepted(write(scratch, "triangle-filter-one-axis.pbrt",
                       with_filter(R"(PixelFilter "triangle" "float yradius" 1)")));
    require(one_axis.scene.filter.x_radius == 2 && one_axis.scene.filter.y_radius == 1,
            "The omitted triangle axis uses its own default");
    const auto precedence =
        accepted(write(scratch, "triangle-filter-precedence.pbrt", with_filter(R"(
PixelFilter "gaussian"
PixelFilter "triangle" "float xradius" 1e50 "float xradius" 0.25 "float yradius" 3
)")));
    require(precedence.scene.filter.x_radius == 0.25f && precedence.scene.filter.y_radius == 3,
            "Effective filter and last typed radius win");

    for (const auto value : {"1e50", "1e-40", "1e-50"}) {
        const auto path =
            write(scratch, "invalid-triangle-radius.pbrt",
                  with_filter(std::string(R"(PixelFilter "triangle" "float xradius" )") + value));
        const auto result = translation(path);
        require(result.exit_code() == 1 && !result.scene && result.diagnostics.size() == 1 &&
                    result.diagnostics.front().location.line == 3 &&
                    result.diagnostics.front().location.column == 24,
                "Narrowed triangle radius errors identify the authored parameter");
    }
    const auto nested = write(scratch, "triangle-filter-include.pbrt",
                              "PixelFilter \"triangle\"\n    \"float yradius\" 1e-40\n");
    const auto included =
        translation(write(scratch, "triangle-filter-entry.pbrt",
                          with_filter(R"(Include "triangle-filter-include.pbrt")")));
    require(included.exit_code() == 1 && included.diagnostics.size() == 1 &&
                included.diagnostics.front().location.line == 2 &&
                included.diagnostics.front().location.column == 5 &&
                fs::path(*included.diagnostics.front().location.file) == nested,
            "Triangle radius narrowing preserves the included source location");

    const auto camera = R"(Rotate 45 0 0 1
Camera "perspective" "float screenwindow" [0 3e38 0 3e38])";
    const auto one_pixel = [](const std::string &source) {
        return replaced(source, R"(Film "rgb")",
                        R"(Film "rgb" "integer xresolution" 1 "integer yresolution" 1)");
    };
    const auto narrow = one_pixel(replaced(
        with_filter(R"(PixelFilter "triangle" "float xradius" .0625 "float yradius" .0625)"),
        R"(Camera "perspective")", camera));
    require(
        accepted(write(scratch, "triangle-narrow-support.pbrt", narrow)).scene.filter.x_radius ==
            0.0625f,
        "Narrow triangle support can avoid overflowing unsampled film corners");
    const auto wide = one_pixel(
        replaced(with_filter(R"(PixelFilter "triangle")"), R"(Camera "perspective")", camera));
    const auto overflow = translation(write(scratch, "triangle-support-overflow.pbrt", wide));
    require(overflow.exit_code() == 1 && !overflow.scene && overflow.diagnostics.size() == 1 &&
                overflow.diagnostics.front().location.line == 4 &&
                overflow.diagnostics.front().message.find("support") != std::string::npos,
            "Triangle support overflow identifies the filter declaration");

    const PixelFilter filter{.kind = PixelFilterKind::triangle, .x_radius = 1, .y_radius = 2};
    const RasterCamera ordinary{
        .origin = {}, .corner = {-1, 1, 1}, .dx = {2, 0, 0}, .dy = {0, -2, 0}};
    require(validate_raster_support(ordinary, filter, 16'777'217, 1).empty(),
            "Large width uses the same rounded pixel centers and divisor as Metal");
    auto nearly_flat = ordinary;
    nearly_flat.corner.z = std::numeric_limits<float>::min();
    require(!validate_raster_support(nearly_flat, filter, 8, 8).empty(),
            "Ray-plane bound rejects a direction that may be flushed to zero");
    const RasterCamera toward_zero{.origin = {},
                                   .corner = {-0.5f, -0.5f, 1.f},
                                   .dx = {1.f, 0.f, 0x1.fffffep-2f},
                                   .dy = {0.f, 1.f, -0x1.3ffffep+1f}};
    constexpr auto min_radius = std::numeric_limits<float>::min();
    const PixelFilter center{
        .kind = PixelFilterKind::triangle, .x_radius = min_radius, .y_radius = min_radius};
    // At the center sample, x and y are both 0.5. The exact inner z is
    // slightly below 1.25. Toward zero it rounds down, then the outer FMA cancels it.
    require(!validate_raster_support(toward_zero, center, 1, 1).empty(),
            "Ray-plane bound rejects a direction that vanishes under round-toward-zero");
    auto dangerous = ordinary;
    dangerous.corner = {2e38f, 0, 1};
    dangerous.dx = {2e38f, 0, 0};
    dangerous.dy = {-1e38f, 1, 0};
    const PixelFilter cancellation{
        .kind = PixelFilterKind::triangle, .x_radius = 0.25f, .y_radius = 0.125f};
    require(!validate_raster_support(dangerous, cancellation, 1, 1).empty(),
            "The inner FMA can overflow even when every final direction would fit");
    const RasterCamera dimension_sensitive{
        .origin = {}, .corner = {0, 0, 1}, .dx = {1e38f, 0, 0}, .dy = {0, 1, 0}};
    const PixelFilter wider{.kind = PixelFilterKind::triangle, .x_radius = 3, .y_radius = 1};
    require(!validate_raster_support(dimension_sensitive, wider, 1, 1).empty() &&
                validate_raster_support(dimension_sensitive, wider, 2, 1).empty(),
            "The same pixel-space support can be unsafe at one film width and safe at another");
    auto demo = make_demo_scene();
    demo.filter = filter;
    require(!validate_render_scene(demo).empty(), "Demo accepts only its original box filter");
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

void rgb_material_context(const fs::path &scratch) {
    const auto path = write(scratch, "rgb-materials.pbrt", std::string(basic) + R"(
AttributeBegin
    ColorSpace "rec2020"
    Attribute "material" "rgb reflectance" [0.75 0.125 0.25]
    ColorSpace "srgb"
    MakeNamedMaterial "tinted" "string type" "diffuse"
AttributeEnd
ObjectBegin "pair"
    ColorSpace "dci-p3"
    NamedMaterial "tinted"
)" + std::string(triangle) + R"(
    Material "diffuse" "spectrum reflectance" [360 0.25 830 0.25]
)" + std::string(triangle) + R"(
ObjectEnd
ColorSpace "aces2065-1"
ObjectInstance "pair"
ObjectInstance "pair"
AttributeBegin
    Attribute "material" "rgb reflectance" [0.2 0.3 0.4]
    ColorSpace "srgb"
    Material "diffuse" "rgb reflectance" [0.75 0.125 0.25]
        "spectrum reflectance" [360 0.9 830 0.9]
        "rgb reflectance" [1 1 1]
)" + std::string(triangle) + R"(
    Material "diffuse" "rgb reflectance" [0.75 0.125 0.25]
)" + std::string(triangle) + R"(
    Material "diffuse" "rgb reflectance" [0 0 0]
)" + std::string(triangle) + "AttributeEnd\n");
    const auto translated = accepted(path);
    const auto &scene = translated.scene;
    require(scene.instance_materials.size() == 7, "RGB material instance count");
    require(scene.instance_materials[3] == scene.instance_materials[5] &&
                scene.instance_materials[4] == scene.instance_materials[6],
            "Object placements must preserve definition-time material bindings");
    const auto &white = scene.materials.at(scene.instance_materials[0]);
    const auto &explicit_rgb = scene.materials.at(scene.instance_materials[1]);
    const auto &black = scene.materials.at(scene.instance_materials[2]);
    const auto &inherited = scene.materials.at(scene.instance_materials[3]);
    const auto &numeric = scene.materials.at(scene.instance_materials[4]);
    require(inherited.kind == SpectralMaterialKind::rgb_sigmoid && inherited.knots.empty() &&
                inherited.coefficients == explicit_rgb.coefficients,
            "Diffuse RGB inheritance must use the material dictionary's sRGB color space");
    require(numeric.kind == SpectralMaterialKind::sampled &&
                white.kind == SpectralMaterialKind::constant &&
                black.kind == SpectralMaterialKind::constant && white.knots.empty() &&
                black.knots.empty(),
            "Sampled spectrum and exact RGB grayscale representations");
    near(spectral_reflectance(numeric, 550.25), 0.25, 0, "Numeric SPD in non-sRGB scope");
    near(spectral_reflectance(white, 550.25), 1, 0,
         "Last explicit parameter wins over inherited RGB");
    near(spectral_reflectance(black, 550.25), 0, 0, "Exact RGB black reflectance");
    require(spectral_reflectance(inherited, 640.25) > spectral_reflectance(inherited, 510.25),
            "Named RGB coefficients must remain usable after preflight data is destroyed");

    const auto checked = check(path);
    if (!checked.scene) {
        throw std::runtime_error("Expected a checked RGB scene");
    }
    require(checked.scene->named_materials.size() == 1, "Expected the retained RGB descriptor");
    const auto &descriptor = checked.scene.value().named_materials.front();
    const auto before = descriptor.effective_parameter("reflectance");
    require(before.parameter && before.color_space == "rec2020" &&
                descriptor.color_space == "srgb" && before.parameter->values[1].text == "0.125",
            "Expected distinct inherited color space and original RGB lexeme");
    const auto result = translate(checked, Location{path.string()});
    require(result.scene &&
                descriptor.effective_parameter("reflectance").parameter == before.parameter &&
                before.parameter->values[1].text == "0.125",
            "Translation must preserve the authored RGB record");
}

void rgb_light_context(const fs::path &scratch) {
    const auto environment = [&](std::string_view body) {
        return accepted(write(scratch, "rgb-environment.pbrt",
                              std::string(basic) + std::string(triangle) + std::string(body)))
            .scene.environment;
    };
    const auto d65 = environment(R"(LightSource "infinite")");
    near(pbrt_y_inner_product(d65), 1, 2e-7, "Default D65 luminance");
    require(d65[90] > d65[340], "Default light has D65 spectral shape");
    require(environment(R"(LightSource "infinite" "rgb L" [1 1 1])") == d65,
            "Unit RGB white and omitted L produce the same D65 samples");
    require(environment(R"(LightSource "infinite" "rgb L" [0 0 0])") == std::array<float, 471>{},
            "RGB black must produce exactly zero radiance");
    const auto hdr = environment(R"(LightSource "infinite" "rgb L" [17 12 4])");
    const auto inherited = environment(R"(AttributeBegin
    Attribute "light" "rgb L" [17 12 4]
    ColorSpace "rec2020"
    LightSource "infinite"
AttributeEnd
)");
    const auto overridden = environment(R"(ColorSpace "rec2020"
Attribute "light" "rgb L" [0.5 0.5 0.5]
ColorSpace "srgb"
LightSource "infinite" "rgb L" [17 12 4]
)");
    require(hdr == inherited && hdr == overridden,
            "Explicit and inherited light RGB must use the effective parameter's color space");
    const auto scaled = environment(R"(LightSource "infinite" "rgb L" [17 12 4]
    "float scale" [2] "float illuminance" [3])");
    const auto d65_scaled =
        environment(R"(LightSource "infinite" "float scale" [2] "float illuminance" [3])");
    const auto tint = reconstruct_srgb({17.f / 34, 12.f / 34, 4.f / 34});
    if (!tint) {
        throw std::runtime_error("Expected sRGB reconstruction for normalized HDR tint");
    }
    for (std::size_t i = 0; i < hdr.size(); ++i) {
        const double expected =
            34. * evaluate_srgb(tint.value(), 360.f + static_cast<float>(i)) * d65[i];
        near(hdr[i], expected, 2e-7, "HDR tint uses base D65 Y normalization");
        near(scaled[i], hdr[i] * 6 / std::numbers::pi, 2e-7, "HDR scale and illuminance");
        near(d65_scaled[i], d65[i] * 6 / std::numbers::pi, 2e-8, "Omitted L scale and illuminance");
    }
    require(pbrt_y_inner_product(hdr) > 1, "HDR tint must retain authored luminance");
    const auto large = environment(R"(LightSource "infinite" "rgb L" [1.7e38 1.7e38 1.7e38])");
    near(pbrt_y_inner_product(large) / 1.7e38, 1, 2e-7, "Large representable RGB scale");
    const auto numeric = environment(R"(LightSource "infinite" "spectrum L" [360 1 830 1])");
    require(environment(R"(ColorSpace "dci-p3"
LightSource "infinite" "spectrum L" [360 1 830 1]
)") == numeric,
            "Numeric infinite-light SPD is independent of ColorSpace");
}

void constant_texture_translation(const fs::path &scratch) {
    const auto translated = [&](std::string_view body) {
        return accepted(write(scratch, "constant-texture.pbrt",
                              std::string(basic) + std::string(body) + std::string(triangle)))
            .scene;
    };
    const auto material = [&](std::string_view body) {
        const auto scene = translated(body);
        return scene.materials.at(scene.instance_materials.front());
    };
    const auto white = material(R"(ColorSpace "rec2020"
Texture "paint" "spectrum" "constant"
Texture "paint" "float" "constant" "float value" [-2]
Texture "unused" "float" "constant"
Material "diffuse" "texture reflectance" "paint"
)");
    require(white.kind == SpectralMaterialKind::constant && white.constant == 1 &&
                white.knots.empty(),
            "Named spectrum texture defaults to one independently of color space and float names");
    near(spectral_reflectance(material("Material \"diffuse\"\n"), 550.25), 0.5, 0,
         "Omitted diffuse reflectance retains its separate half-reflectance default");

    const auto direct = material(R"(Material "diffuse" "rgb reflectance" [0.75 0.125 0.25]
)");
    constexpr std::array rgb_cases{
        R"(Texture "paint" "spectrum" "constant" "rgb value" [0.75 0.125 0.25]
ColorSpace "rec2020"
)",
        R"(Attribute "texture" "rgb value" [0.75 0.125 0.25]
ColorSpace "rec2020"
Texture "paint" "spectrum" "constant"
)",
        R"(ColorSpace "rec2020"
Attribute "texture" "rgb value" [0.1 0.2 0.3]
ColorSpace "srgb"
Texture "paint" "spectrum" "constant" "rgb value" [0.75 0.125 0.25]
)",
        R"(Attribute "texture" "spectrum value" [360 0.1 830 0.1]
Attribute "texture" "rgb value" [0.75 0.125 0.25]
Texture "paint" "spectrum" "constant"
)",
        R"(Texture "paint" "spectrum" "constant"
    "rgb value" [0.1 0.2 0.3] "rgb value" [0.75 0.125 0.25]
)",
    };
    for (const auto *body : rgb_cases) {
        const auto named = material(std::string(body) +
                                    "Material \"diffuse\" \"texture reflectance\" \"paint\"\n");
        require(named.kind == direct.kind && named.coefficients == direct.coefficients &&
                    named.knots.empty(),
                "Texture RGB must use the effective definition value's captured color space");
        for (const double wavelength : {360., 420.25, 550., 610.5, 829.75}) {
            near(spectral_reflectance(named, wavelength), spectral_reflectance(direct, wavelength),
                 0, "Named and direct RGB spectral evaluation after preflight destruction");
        }
    }
    for (const float gray : {0.f, 0.5f, 1.f}) {
        const auto scalar = std::to_string(gray);
        const auto named = material("Texture \"paint\" \"spectrum\" \"constant\" \"rgb value\" [" +
                                    scalar + " " + scalar + " " + scalar +
                                    "]\n"
                                    "Material \"diffuse\" \"texture reflectance\" \"paint\"\n");
        require(named.kind == SpectralMaterialKind::constant && named.constant == gray,
                "Named black, gray and white retain exact constant spectra");
    }

    const auto numeric = material(R"(Material "diffuse" "texture reflectance" "later"
AttributeBegin
ColorSpace "dci-p3"
Texture "later" "spectrum" "constant" "spectrum value" [400 0.25 503.5 0.75 700 0.5]
AttributeEnd
)");
    require(numeric.kind == SpectralMaterialKind::sampled && numeric.knots.size() == 3 &&
                numeric.knots[1].wavelength == 503.5f,
            "Forward texture binding owns fractional numeric knots after scope and preflight "
            "destruction");
    near(spectral_reflectance(numeric, 399.75), 0, 0, "Zero below named spectrum support");
    near(spectral_reflectance(numeric, 700.25), 0, 0, "Zero above named spectrum support");
    near(spectral_reflectance(numeric, 451.75), 0.5, 0, "Named spectral interpolation");

    const auto bound = translated(R"(MakeNamedMaterial "named" "string type" "diffuse"
    "texture reflectance" "shared"
ObjectBegin "part"
NamedMaterial "named"
)" + std::string(triangle) + R"(ObjectEnd
ObjectInstance "part"
ObjectInstance "part"
Material "diffuse" "texture reflectance" "shared"
)" + std::string(triangle) + R"(Texture "shared" "spectrum" "constant" "rgb value" [0.75 0.125 0.25]
Material "diffuse" "texture reflectance" "shared"
)");
    require(bound.instance_materials.size() == 4 && bound.materials.size() == 2 &&
                std::ranges::all_of(
                    bound.instance_materials,
                    [&](auto index) { return index == bound.instance_materials.front(); }),
            "Distinct inline/named declarations and object instances reuse one texture payload");
}

void constant_texture_rejections_and_limits(const fs::path &scratch) {
    const auto rejected = [&](std::string_view body, int status, std::string_view message,
                              std::string_view source_token) {
        const auto source = std::string(basic) + std::string(body) + std::string(triangle);
        const auto path = write(scratch, "invalid-constant-texture.pbrt", source);
        const auto offset = source.find(source_token);
        require(offset != std::string::npos, "Missing expected diagnostic token");
        const auto line = 1 + static_cast<std::size_t>(std::ranges::count(
                                  std::string_view(source).substr(0, offset), '\n'));
        const auto previous_line = source.rfind('\n', offset);
        const auto column =
            previous_line == std::string::npos ? offset + 1 : offset - previous_line;
        const auto result = translation(path);
        require(result.exit_code() == status && !result.scene &&
                    std::ranges::any_of(result.diagnostics,
                                        [&](const Diagnostic &diagnostic) {
                                            return diagnostic.message.find(message) !=
                                                       std::string::npos &&
                                                   fs::path(*diagnostic.location.file) == path &&
                                                   diagnostic.location.line == line &&
                                                   diagnostic.location.column == column;
                                        }),
                "Unused unsupported/invalid texture must fail at the authored value: " +
                    std::string(message));
    };
    rejected(R"(ColorSpace "rec2020"
Texture "unused" "spectrum" "constant" "rgb value" [0.2 0.3 0.4]
)",
             2, "sRGB", "\"rgb value\"");
    rejected(R"(ColorSpace "rec2020"
Attribute "texture" "rgb value" [0.2 0.3 0.4]
ColorSpace "srgb"
Texture "unused" "spectrum" "constant"
)",
             2, "sRGB", "\"rgb value\"");
    rejected(R"(Texture "unused" "spectrum" "constant" "spectrum value" [360 0.2 830 1.1]
)",
             2, "Diffuse reflectance must be within [0, 1]", "1.1");
    rejected(R"(Texture "unused" "spectrum" "constant" "spectrum value" [360 -0.1 830 0.2]
)",
             2, "Diffuse reflectance must be within [0, 1]", "-0.1");
    rejected(R"(Texture "unused" "spectrum" "constant" "spectrum value" [360 0.2 1e40 0.5]
)",
             1, "wavelength overflows", "1e40");
    rejected(R"(Texture "unused" "spectrum" "constant" "spectrum value" [360 0.2 360.00000001 0.5]
)",
             1, "increase in binary32", "360.00000001");
    rejected(R"(Texture "unused" "spectrum" "constant" "blackbody value" [5500]
)",
             2, "two numeric", "\"blackbody value\"");
    rejected(R"(Texture "unused" "spectrum" "constant" "spectrum value" "stdillum-D65"
)",
             2, "two numeric", "\"spectrum value\"");
    rejected(R"(Texture "unused" "spectrum" "constant" "spectrum value" [550 0.5]
)",
             2, "two numeric", "\"spectrum value\"");
    rejected(R"(Texture "unused" "float" "constant" "float value" [1e40]
)",
             1, "overflows binary32", "\"float value\"");

    std::string body = "Texture \"shared\" \"spectrum\" \"constant\" \"spectrum value\" [";
    for (int i = 0; i < 20; ++i) {
        body += std::to_string(400 + i * 10) + " 0.5 ";
    }
    body += "]\nMakeNamedMaterial \"named\" \"string type\" \"diffuse\" "
            "\"texture reflectance\" \"shared\"\nNamedMaterial \"named\"\n";
    body += triangle;
    for (int i = 0; i < 2; ++i) {
        body += "Material \"diffuse\" \"texture reflectance\" \"shared\"\n";
        body += triangle;
    }
    const auto entry = write(scratch, "bounded-texture.pbrt", std::string(basic) + body);
    const auto shared = translation(entry, {.total_values = 40});
    require(shared.exit_code() == 0 && shared.scene && shared.scene->materials.size() == 2 &&
                shared.scene->materials.back().knots.size() == 20 &&
                shared.scene->instance_materials == std::vector<std::uint32_t>{1, 1, 1},
            "One spectral definition is charged and stored once across distinct materials");
    for (const auto &limits : {Limits{.total_values = 39}, Limits{.list_values = 39}}) {
        const auto failed = translation(entry, limits);
        require(failed.exit_code() == 1 && !failed.scene &&
                    std::ranges::any_of(failed.diagnostics,
                                        [&](const Diagnostic &diagnostic) {
                                            return diagnostic.code == "limit-exceeded" &&
                                                   diagnostic.location.line == 7 &&
                                                   diagnostic.location.column == 40;
                                        }),
                "Insufficient named spectrum budget fails at the definition value");
    }
}

void rejects_rgb_values_and_spaces(const fs::path &scratch) {
    const auto rejected = [&](std::string_view body, int status, std::string_view code,
                              std::size_t line, std::size_t column) {
        const auto path = write(scratch, "rejected-rgb.pbrt",
                                std::string(basic) + std::string(body) + std::string(triangle));
        const auto checked = check(path);
        const auto result = translate(checked, Location{path.string()});
        require(result.exit_code() == status && !result.scene,
                "Invalid RGB produced a render scene");
        const auto &diagnostics =
            checked.exit_code() == 0 ? result.diagnostics : checked.document.diagnostics;
        require(std::ranges::any_of(diagnostics,
                                    [&](const Diagnostic &diagnostic) {
                                        return diagnostic.code == code &&
                                               fs::path(*diagnostic.location.file) == path &&
                                               diagnostic.location.line == line &&
                                               diagnostic.location.column == column;
                                    }),
                "RGB rejection must identify the effective authored source");
    };
    rejected(R"(Attribute "material" "rgb reflectance" [0.5 0.5 0.5]
ColorSpace "rec2020"
MakeNamedMaterial "unused" "string type" "diffuse"
)",
             2, "unsupported-render-feature", 7, 22);
    rejected(R"(ColorSpace "dci-p3"
Attribute "light" "rgb L" [1 1 1]
ColorSpace "srgb"
LightSource "infinite"
)",
             2, "unsupported-render-feature", 8, 19);
    rejected("ColorSpace \"aces2065-1\"\nLightSource \"infinite\"\n", 2,
             "unsupported-render-feature", 8, 1);
    rejected("LightSource \"infinite\"\n    \"rgb L\" [1 2e38 3]\n", 1, "invalid-render-value", 8,
             16);
    rejected("ColorSpace \"rec2020\"\nLightSource \"infinite\"\n    \"rgb L\" [1 2e38 3]\n", 1,
             "invalid-render-value", 9, 16);
    rejected("LightSource \"infinite\"\n    \"rgb L\" [1 1e308 3]\n", 1, "invalid-render-value", 8,
             16);
    rejected(
        "LightSource \"infinite\" \"float scale\" 1000\n    \"rgb L\" [1.7e38 1.7e38 1.7e38]\n", 1,
        "invalid-render-value", 8, 5);
    rejected("LightSource \"infinite\"\n    \"rgb L\" [1 -0.1 3]\n", 1, "invalid-parameter", 8, 5);
    rejected("Material \"diffuse\"\n    \"rgb reflectance\" [1 1.01 0]\n", 1, "invalid-parameter",
             8, 5);
    rejected("Material \"diffuse\"\n    \"rgb reflectance\" [1 -0.1 0]\n", 1, "invalid-parameter",
             8, 5);
    rejected("Material \"diffuse\"\n    \"color reflectance\" [1 0 0]\n", 2, "legacy-parameter", 8,
             5);
    rejected("LightSource \"infinite\"\n    \"color L\" [1 0 0]\n", 2, "legacy-parameter", 8, 5);
    rejected("Material \"diffuse\"\n    \"blackbody reflectance\" [5500]\n", 2,
             "unsupported-render-feature", 8, 5);
    rejected("LightSource \"infinite\"\n    \"spectrum L\" \"stdillum-D65\"\n", 2,
             "unsupported-render-feature", 8, 5);
    const auto limited_path = write(scratch, "limited-rgb.pbrt",
                                    std::string(basic) +
                                        "ColorSpace \"rec2020\"\n"
                                        "Material \"diffuse\" \"rgb reflectance\" [0.2 0.4 0.6]\n" +
                                        std::string(triangle));
    const auto limited = translation(limited_path, {.list_values = 2});
    require(limited.exit_code() == 1 && !limited.scene &&
                std::ranges::any_of(limited.diagnostics,
                                    [](const Diagnostic &d) { return d.code == "limit-exceeded"; }),
            "RGB value limits must take precedence over unsupported color spaces");
}

void area_light_translation(const fs::path &scratch) {
    std::string source(basic);
    source += R"(LightSource "infinite" "rgb L" [4 2 1]
Material "diffuse" "rgb reflectance" [0 0 0]
Attribute "light" "rgb L" [4 2 1]
AttributeBegin
ColorSpace "rec2020"
AreaLightSource "diffuse" "bool twosided" true "float power" 0
Scale -2 3 1
)";
    source += triangle;
    source += triangle;
    source += "AttributeEnd\n";
    source += triangle;
    source += "AreaLightSource \"diffuse\" \"float scale\" 2 \"float power\" -3\n";
    source += triangle;
    source += R"(AreaLightSource "diffuse" "spectrum L" [360 2 830 2]
)";
    source += triangle;
    source += "AreaLightSource \"diffuse\" \"rgb L\" [0 0 0]\n";
    const auto entry = write(scratch, "area-light.pbrt", source);
    const auto scene = accepted(entry).scene;
    require(scene.area_emissions.size() == 3 &&
                scene.instance_emissions ==
                    std::vector<std::uint32_t>{0, 0, no_area_emission, 1, 2},
            "Placed shapes share owned emission tables without allocating for unused declarations");
    require(scene.area_emissions[0].two_sided && !scene.area_emissions[1].two_sided &&
                scene.instance_reverse_orientation == std::vector<std::uint32_t>{1, 1, 0, 0, 0} &&
                scene.instance_materials[0] == scene.instance_materials[2],
            "Area sidedness, shape orientation, and reflective material are independent");
    require(scene.area_emissions[0].radiance == scene.environment,
            "Area and infinite RGB L share spectral normalization and honor captured RGB space");
    for (std::size_t i = 0; i < scene.environment.size(); ++i) {
        near(scene.area_emissions[1].radiance[i], 2. * scene.environment[i], 1e-8,
             "Area scale preserves RGB intensity");
    }
    near(pbrt_y_inner_product(scene.area_emissions[2].radiance), 1., 1e-6,
         "Numeric area SPD normalizes by its own Y integral");

    std::string defaults(basic);
    defaults += "AreaLightSource \"diffuse\" \"string filename\" \"\"\n";
    defaults += triangle;
    const auto defaults_entry = write(scratch, "area-defaults.pbrt", defaults);
    const auto default_area = accepted(defaults_entry).scene.area_emissions.at(0);
    near(pbrt_y_inner_product(default_area.radiance), 1., 1e-6,
         "Omitted area L uses D65 normalized to unit Y");
    require(!default_area.two_sided && default_area.radiance.front() > 0,
            "Area defaults are one-sided, nonzero D65");
    Limits limits;
    limits.total_values = 470;
    const auto bounded = translation(defaults_entry, limits);
    require(bounded.exit_code() == 1 && !bounded.scene &&
                bounded.diagnostics.back().code == "limit-exceeded",
            "Placed area emission tables obey the translation value budget");
    const auto unused = translation(write(scratch, "area-unused.pbrt",
                                          std::string(basic) +
                                              "AreaLightSource \"diffuse\"\n"
                                              "AreaLightSource \"\"\n" +
                                              std::string(triangle)),
                                    limits);
    require(unused.exit_code() == 0 && unused.scene && unused.scene->area_emissions.empty() &&
                unused.scene->instance_emissions.empty(),
            "Unused area declarations do not consume the dense table budget");
    const auto invalid_unused =
        translation(write(scratch, "area-invalid-unused.pbrt",
                          std::string(basic) +
                              "AreaLightSource \"diffuse\" \"spectrum L\" [360 0 830 0]\n"
                              "AreaLightSource \"\"\n" +
                              std::string(triangle)),
                    limits);
    require(invalid_unused.exit_code() == 1 && !invalid_unused.scene &&
                invalid_unused.diagnostics.front().location.line == 7 &&
                invalid_unused.diagnostics.front().message.find("CIE Y inner product") !=
                    std::string::npos,
            "Unused declarations still validate numeric spectral normalization");

    const auto positive =
        translation(write(scratch, "area-positive-power.pbrt",
                          std::string(basic) + "AreaLightSource \"diffuse\" \"float power\" 1\n" +
                              std::string(triangle)));
    require(positive.exit_code() == 2 && !positive.scene &&
                positive.diagnostics.front().location.line == 7 &&
                positive.diagnostics.front().message.find("Positive power") != std::string::npos,
            "Positive area power receives a source-located render diagnostic");
    const auto extreme = translation(write(scratch, "area-unrepresentable.pbrt",
                                           std::string(basic) +
                                               "AreaLightSource \"diffuse\"\n"
                                               "Scale 1e30 1e30 1\n" +
                                               std::string(triangle)));
    require(extreme.exit_code() == 1 && !extreme.scene &&
                extreme.diagnostics.front().location.line == 9,
            "Area geometry that overflows in world space fails at the shape before acquisition");
}

// Keep the path arguments in the same order as the test runner's command line.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void cornell_rgb_parameters(const fs::path &scratch, const fs::path &fixtures) {
    const auto path = fixtures.parent_path() / "pbrt" / "cornell-box.pbrt";
    const auto checked = check(path);
    require(checked.exit_code() == 0 && checked.document.syntax_complete &&
                checked.document.diagnostics.empty(),
            "The pinned Cornell scene passes preflight");
    const auto result = accepted(path);
    require(result.settings.width == 1024 && result.settings.height == 1024 &&
                result.settings.samples == 64 && result.settings.seed == 0 &&
                result.settings.sampler.kind == SamplerKind::sobol &&
                result.settings.sampler.randomization == SobolRandomization::fastowen &&
                result.scene.max_depth == 65 && result.scene.geometry.instances.size() == 8 &&
                result.scene.area_emissions.size() == 1,
            "The unmodified Cornell scene translates with authored rendering settings");

    // Exercise the authored colors independently with supported descriptors and geometry.
    unsigned materials = 0;
    unsigned lights = 0;
    for (const auto &directive : checked.document.directives) {
        const auto selected = std::ranges::find(directive.parameters, "rgb", &Parameter::type);
        if (selected == directive.parameters.end()) {
            continue;
        }
        const bool is_light = directive.name == "AreaLightSource";
        require((is_light && selected->name == "L") ||
                    (directive.name == "MakeNamedMaterial" && selected->name == "reflectance"),
                "Unexpected Cornell RGB parameter");
        std::string scene(basic);
        scene += is_light ? "AreaLightSource \"diffuse\"" : "Material \"diffuse\"";
        scene += " \"rgb " + selected->name + "\" [";
        for (const auto &value : selected->values) {
            scene += value.text + ' ';
        }
        scene += "]\n";
        scene += triangle;
        const auto translated = accepted(write(scratch, "cornell-color.pbrt", scene));
        if (is_light) {
            ++lights;
            require(pbrt_y_inner_product(translated.scene.area_emissions.at(0).radiance) > 1,
                    "Cornell emission parameter retains HDR luminance");
        } else {
            ++materials;
            const auto &reflectance =
                translated.scene.materials.at(translated.scene.instance_materials.front());
            require(reflectance.kind != SpectralMaterialKind::sampled,
                    "Cornell diffuse parameter must reconstruct RGB");
        }
    }
    require(materials == 8 && lights == 1, "Cornell RGB parameter coverage");
}

void triangle_attribute_translation(const fs::path &scratch) {
    const auto entry = write(scratch, "triangle-attribute-instance.pbrt", std::string{basic} + R"(
ObjectBegin "mesh"
    Scale 2 3 4
    Shape "trianglemesh" "point3 P" [0 0 2 1 0 2 0 1 2]
        "normal N" [0 0 1 1 0 2 0 1 3] "point2 uv" [-1 2 0 0 2 -1]
ObjectEnd
ObjectInstance "mesh"
Scale -1 1 1
ObjectInstance "mesh"
Translate 3 0 0
ObjectInstance "mesh"
Translate 3 0 0
ObjectInstance "mesh"
)");
    const auto scene = accepted(entry).scene;
    require(scene.geometry.meshes.size() == 1 && scene.geometry.instances.size() == 4 &&
                scene.instance_reverse_orientation == std::vector<std::uint32_t>{0, 1, 1, 1},
            "Shared attributed mesh retains per-instance winding");
    const auto &mesh = scene.geometry.meshes.front();
    point_near(mesh.normals.at(1), {1, 0, 2}, "Retained object-space normal magnitude");
    require(mesh.uvs.at(0) == std::array{-1.0f, 2.0f} && mesh.uvs.at(2) == std::array{2.0f, -1.0f},
            "UVs are retained in vertex order without remapping");

    const auto exact = translation(entry, {.total_values = 36});
    require(exact.exit_code() == 0 && exact.scene,
            "Attributed mesh storage and placement work fit exactly");
    const auto expanded = translation(entry, {.total_values = 35});
    require(expanded.exit_code() == 1 && !expanded.scene && expanded.diagnostics.size() == 1 &&
                expanded.diagnostics.front().code == "limit-exceeded" &&
                expanded.diagnostics.front().location.line == 10 &&
                expanded.diagnostics.front().message ==
                    "Expanded shading normal value limit exceeded",
            "Repeated instance normal processing is bounded at the shape");
    const auto stored = translation(entry, {.total_values = 26});
    require(stored.exit_code() == 1 && !stored.scene && stored.diagnostics.size() == 1 &&
                stored.diagnostics.front().code == "limit-exceeded" &&
                stored.diagnostics.front().message == "Translated geometry value limit exceeded",
            "Retained UVs and normals count against mesh storage limits");

    const auto extreme = translation(write(scratch, "triangle-attribute-extreme.pbrt",
                                           std::string{basic} + std::string{triangle} + R"(
"normal N" [0 0 1e38 0 0 1e-37 0 0 1]
)"));
    require(extreme.exit_code() == 1 && !extreme.scene && extreme.diagnostics.size() == 1 &&
                extreme.diagnostics.front().code == "invalid-render-value" &&
                extreme.diagnostics.front().location.line == 7 &&
                extreme.diagnostics.front().message.find("exceed the GPU normal range") !=
                    std::string::npos,
            "Nonrepresentable normal ratios fail before renderer acquisition at the source shape");

    const auto tangent_emitter = [&](std::string_view light) {
        return translation(write(scratch, "triangle-attribute-emitter.pbrt",
                                 std::string{basic} + std::string{light} +
                                     "\nReverseOrientation\n" + std::string{triangle} +
                                     R"("normal N" [1 0 0 1 0 0 1 0 0])"));
    };
    const auto one_sided = tangent_emitter(R"(AreaLightSource "diffuse")");
    require(one_sided.exit_code() == 2 && !one_sided.scene && one_sided.diagnostics.size() == 1 &&
                one_sided.diagnostics.front().code == "unsupported-render-feature" &&
                one_sided.diagnostics.front().location.line == 9 &&
                one_sided.diagnostics.front().message.find("cannot retain PBRT") !=
                    std::string::npos,
            "Conflicting one-sided emitter normals are diagnosed at the source shape");
    require(tangent_emitter(R"(AreaLightSource "diffuse" "bool twosided" true)").exit_code() == 0,
            "Two-sided emission supports tangent normals under the same geometry");
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

void sobol_cornell_fixtures(const fs::path &fixtures) {
    constexpr std::array modes{
        std::pair{"fastowen", SobolRandomization::fastowen},
        std::pair{"permutedigits", SobolRandomization::permutedigits},
        std::pair{"owen", SobolRandomization::owen},
    };
    for (const auto &[name, mode] : modes) {
        const auto translated =
            accepted(fixtures / (std::string("cornell-box-sobol-") + name + ".pbrt"));
        require(translated.settings.width == 8 && translated.settings.height == 8 &&
                    translated.settings.samples == 4096 && translated.settings.seed == 0 &&
                    translated.settings.sampler.kind == SamplerKind::sobol &&
                    translated.settings.sampler.randomization == mode &&
                    translated.scene.exposure == 1 && translated.scene.max_depth == 65,
                "Sobol Cornell comparison settings");
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 3, "Expected scratch directory and PBRT fixtures");
        const fs::path scratch(argv[1]);
        fs::create_directories(scratch);
        effective_defaults_and_cameras(scratch);
        sobol_translation(scratch);
        material_instances(scratch);
        rejects_unsupported_profile(scratch);
        rejects_camera_corner_overflow(scratch);
        triangle_filter_translation(scratch);
        film_crop_precedence(scratch);
        bounded_spectra(scratch);
        infinite_light_normalization(scratch);
        rgb_material_context(scratch);
        rgb_light_context(scratch);
        constant_texture_translation(scratch);
        constant_texture_rejections_and_limits(scratch);
        area_light_translation(scratch);
        rejects_rgb_values_and_spaces(scratch);
        cornell_rgb_parameters(scratch, argv[2]);
        triangle_attribute_translation(scratch);
        sensor();
        owned_fixtures(argv[2]);
        sobol_cornell_fixtures(argv[2]);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
