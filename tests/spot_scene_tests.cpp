// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "rgb_spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
constexpr auto options = "Camera \"perspective\"\nFilm \"rgb\"\nPixelFilter \"box\"\nSampler "
                         "\"independent\"\nIntegrator \"path\"\nWorldBegin\n";
constexpr auto triangle = "Material \"diffuse\"\nShape \"trianglemesh\" \"point3 P\" "
                          "[0 0 2 1 0 2 0 1 2]\n";
fs::path write(const fs::path &dir, const std::string &source) {
    const auto path = dir / "scene.pbrt";
    std::ofstream(path) << options << source << triangle;
    return path;
}
pbrt::RenderTranslation translate(const fs::path &dir, const std::string &source) {
    const auto path = write(dir, source);
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto result = pbrt::translate(checked, pbrt::Location{path.string()});
    require(result.scene.has_value(), "spot translation failed");
    return result;
}
void close(double actual, double expected, const std::string &message) {
    require(std::abs(actual - expected) <= 2e-6 * std::max(1., std::abs(expected)), message);
}
} // namespace
int main() try {
    const auto dir = fs::temp_directory_path() / ("rypath-spot-scene-" + std::to_string(getpid()));
    try {
        fs::create_directories(dir);
        const auto base = translate(dir, "LightSource \"spot\"\n");
        require(base.scene->spot_lights.size() == 1, "one spot default");
        const auto &spot = base.scene->spot_lights.front();
        require(spot.position.x == 0 && spot.position.y == 0 && spot.position.z == 0,
                "default origin");
        const auto d65 = pbrt_d65();
        const auto norm = pbrt_y_inner_product(d65);
        for (std::size_t i = 0; i < d65.size(); ++i)
            close(spot.intensity[i], d65[i] / norm, "default normalized illuminant");
        for (const auto power : {"0", "-3"}) {
            const auto result =
                translate(dir, std::string("LightSource \"spot\" \"float scale\" [2] ") +
                                   "\"float power\" [" + power + "]\n");
            for (std::size_t i = 0; i < d65.size(); ++i)
                close(result.scene->spot_lights.front().intensity[i], 2 * spot.intensity[i],
                      "power and intensity units");
        }
        const auto powered = translate(dir, "LightSource \"spot\" \"float scale\" [2] "
                                            "\"float power\" [10]\n");
        const auto &powered_light = powered.scene->spot_lights[0];
        const double divisor =
            2 * std::numbers::pi *
            ((1. - powered_light.cos_falloff_start) +
             (double(powered_light.cos_falloff_start) - powered_light.cos_falloff_end) / 2);
        for (std::size_t i = 0; i < d65.size(); ++i)
            close(powered_light.intensity[i], 20 * spot.intensity[i] / divisor,
                  "positive power uses independent smooth cone integral");
        close(spot.cos_falloff_start, std::cos(25 * std::numbers::pi / 180), "default inner cone");
        close(spot.cos_falloff_end, std::cos(30 * std::numbers::pi / 180), "default outer cone");
        auto transformed = translate(dir, "Translate 3 4 5\nScale 2 3 4\nLightSource \"spot\" "
                                          "\"point3 from\" [1 2 3]\n");
        const auto position = transformed.scene->spot_lights.front().position;
        require(position.x == 5 && position.y == 10 && position.z == 17,
                "from transformed in world coordinates");
        const auto direction_cosine = [](const SpotLight &light, std::array<double, 3> world) {
            std::array<double, 3> local{};
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t col = 0; col < 3; ++col)
                    local[row] += light.light_from_world[row * 3 + col] * world[col];
            return local[2] / std::hypot(local[0], local[1], local[2]);
        };
        auto aimed = translate(dir, "Scale 2 3 4\nLightSource \"spot\" "
                                    "\"point3 from\" [1 2 3] \"point3 to\" [2 4 6]\n");
        close(direction_cosine(aimed.scene->spot_lights[0], {2, 6, 12}), 1,
              "composed from-to and nonuniform affine aim");
        close(direction_cosine(aimed.scene->spot_lights[0], {0, 0, 4}), 3 / std::sqrt(14.),
              "affine inverse preserves light-space angle");
        auto reflected = translate(dir, "Scale -2 3 4\nLightSource \"spot\" "
                                        "\"point3 to\" [1 0 0]\n");
        close(direction_cosine(reflected.scene->spot_lights[0], {-2, 0, 0}), 1,
              "negative determinant retains forward direction");
        auto tiny = translate(dir, "Scale 1e-30 1e-30 1e-30\nLightSource \"spot\"\n");
        close(direction_cosine(tiny.scene->spot_lights[0], {0, 0, 1}), 1,
              "tiny uniform transform has representable common-scaled inverse");
        for (const auto *scale : {"1e-300", "1e300"}) {
            auto extreme =
                translate(dir, std::string("AttributeBegin\nScale ") + scale + " " + scale + " " +
                                   scale + "\nLightSource \"spot\"\nAttributeEnd\n");
            close(direction_cosine(extreme.scene->spot_lights[0], {0, 0, 1}), 1,
                  "extreme uniform affine map normalized before inversion");
        }
        auto sheared = translate(dir, "AttributeBegin\n"
                                      "Transform [1 0 0 0 2 1 0 0 0 0 1 0 0 0 0 1]\n"
                                      "LightSource \"spot\"\nAttributeEnd\n");
        close(direction_cosine(sheared.scene->spot_lights[0], {2, 1, 1}), 1 / std::sqrt(2.),
              "shear is inverted before evaluating cone angle");
        auto sampled = translate(dir, "LightSource \"spot\" \"spectrum I\" [360 1 830 2]\n");
        std::array<float, 471> independent_shape{};
        for (std::size_t i = 0; i < independent_shape.size(); ++i)
            independent_shape[i] = static_cast<float>(1. + double(i) / 470);
        const double sampled_y = pbrt_y_inner_product(independent_shape);
        for (std::size_t i = 0; i < independent_shape.size(); ++i)
            close(sampled.scene->spot_lights[0].intensity[i], independent_shape[i] / sampled_y,
                  "independent sampled spectral shape and photometric normalization");
        auto hard = translate(dir, "LightSource \"spot\" \"float conedeltaangle\" [0]\n");
        require(hard.scene->spot_lights[0].cos_falloff_start ==
                    hard.scene->spot_lights[0].cos_falloff_end,
                "zero feather packs a hard cone");
        auto full = translate(dir, "LightSource \"spot\" \"float coneangle\" [180] "
                                   "\"float conedeltaangle\" [180]\n");
        require(full.scene->spot_lights[0].cos_falloff_start == 1 &&
                    full.scene->spot_lights[0].cos_falloff_end == -1,
                "full cone endpoints");
        auto scoped = translate(dir, "Attribute \"light\" \"float scale\" [2]\nAttributeBegin\n"
                                     "Translate 7 0 0\nLightSource \"spot\"\nAttributeEnd\n"
                                     "LightSource \"spot\" \"float scale\" [3]\n"
                                     "ObjectBegin \"o\"\nLightSource \"spot\"\nObjectEnd\n"
                                     "ObjectInstance \"o\"\nObjectInstance \"o\"\n"
                                     "LightSource \"infinite\"\n");
        require(scoped.scene->spot_lights.size() == 3, "object light remains global");
        require(scoped.scene->spot_lights[0].position.x == 7 &&
                    scoped.scene->spot_lights[1].position.x == 0,
                "transform scope restored");
        close(scoped.scene->spot_lights[0].intensity[200], 2 * spot.intensity[200],
              "inherited light scale");
        close(scoped.scene->spot_lights[1].intensity[200], 3 * spot.intensity[200],
              "explicit scale overrides inherited");
        for (const auto &spectrum :
             {"\"rgb I\" [.3 .7 .1]", "\"blackbody I\" [3000]", "\"spectrum I\" [360 1 830 1]",
              "\"spectrum I\" [\"stdillum-D65\"]"}) {
            const auto source = std::string("LightSource \"spot\" ") + spectrum + "\n";
            auto result = translate(dir, source);
            require(result.scene->spot_lights.front().intensity[200] > 0,
                    "supported intensity spectral form");
        }
        std::ofstream(dir / "intensity.spd") << "360 1\n830 2\n";
        auto path = write(dir, "Attribute \"light\" \"spectrum I\" [\"intensity.spd\"]\n"
                               "LightSource \"spot\"\n");
        auto checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        auto owned = checked;
        checked = {};
        fs::remove(dir / "intensity.spd");
        auto result = pbrt::translate(owned, pbrt::Location{path.string()});
        require(result.scene && result.scene->spot_lights[0].intensity[200] > 0,
                "copied checked scene owns decoded inherited file");
        const auto zero = translate(dir, "LightSource \"spot\" \"rgb I\" [0 0 0]\n");
        require(std::ranges::all_of(zero.scene->spot_lights[0].intensity,
                                    [](float value) { return value == 0; }),
                "zero RGB intensity remains black");
        std::ofstream(dir / "malformed.spd") << "360 nope\n";
        path = write(dir, "LightSource \"spot\" \"spectrum I\" [\"malformed.spd\"]\n");
        require(pbrt::check(path).exit_code() == 1, "malformed spot spectrum file rejected");
        std::ofstream(dir / "light.pbrt") << "LightSource \"spot\" \"point3 from\" [2 0 0]\n";
        const auto included = translate(dir, "Translate 4 0 0\nInclude \"light.pbrt\"\n");
        require(included.scene->spot_lights[0].position.x == 6,
                "included spot observes shared world state");
        std::string many;
        for (unsigned i = 0; i < max_positional_lights; ++i)
            many += "LightSource \"spot\"\n";
        require(translate(dir, many).scene->spot_lights.size() == max_positional_lights,
                "64 lights");
        path = write(dir, many + "LightSource \"spot\"\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 1 && !checked.document.diagnostics.empty() &&
                    checked.document.diagnostics.back().location.line > 6,
                "65th light source-located resource error");
        for (const auto &invalid :
             {"\"float I\" [1]", "\"point3 from\" [1 2]", "\"float scale\" [-1]",
              "\"rgb I\" [-1 0 0]", "\"string filename\" [\"x.png\"]",
              "\"spectrum I\" [\"absent.spd\"]", "\"float power\" [1 2]"}) {
            path = write(dir, std::string("LightSource \"spot\" ") + invalid + "\n");
            require(pbrt::check(path).exit_code() == 1, "malformed spot rejected");
        }
        path = write(dir, "LightSource \"spot\" \"spectrum I\" [360 -1 830 -1]\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        require(!pbrt::translate(checked, pbrt::Location{path.string()}).scene,
                "negative spectral intensity rejected at translation");
        path = write(dir,
                     "LightSource \"infinite\"\nLightSource \"spot\"\nLightSource \"infinite\"\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 0 &&
                    !pbrt::translate(checked, pbrt::Location{path.string()}).scene,
                "multiple infinite lights remain unsupported");
        path = write(dir, "MediumInterface \"\" \"missing\"\nLightSource \"spot\"\n"
                          "MediumInterface \"\" \"\"\n");
        require(pbrt::check(path).exit_code() == 1, "undefined endpoint medium rejected");
        result = translate(dir, "MakeNamedMedium \"fog\" \"string type\" [\"homogeneous\"]\n"
                                "MediumInterface \"\" \"fog\"\nLightSource \"spot\"\n"
                                "MediumInterface \"\" \"\"\n");
        require(!result.scene->volume_transport, "endpoint medium does not force volpath");
        checked = pbrt::check(dir / "scene.pbrt");
        require(checked.scene && checked.scene->lights[0].outside_medium == "fog",
                "spot captures declaration outside medium");
        path = write(dir, "LightSource \"spot\"\n");
        checked = pbrt::check(path);
        pbrt::Limits limits;
        limits.total_values = 470;
        require(!pbrt::translate(checked, pbrt::Location{path.string()}, limits).scene,
                "dense spot spectrum charges translated value budget");
        for (const auto &invalid :
             {"\"float coneangle\" [0]", "\"float coneangle\" [181]",
              "\"float conedeltaangle\" [-1]", "\"float conedeltaangle\" [31]",
              "\"point3 to\" [0 0 0]", "\"float coneangle\" []"}) {
            path = write(dir, std::string("LightSource \"spot\" ") + invalid + "\n");
            checked = pbrt::check(path);
            require(checked.exit_code() == 1 && !checked.document.diagnostics.empty() &&
                        checked.document.diagnostics[0].location.line > 6,
                    "invalid effective spot parameters rejected with source location");
        }
        for (const auto &invalid : {"\"point3 from\" [1e-40 0 0]",
                                    "\"float coneangle\" [0.00001] \"float conedeltaangle\" [0]",
                                    "\"float conedeltaangle\" [1e-10]"}) {
            path = write(dir, std::string("LightSource \"spot\" ") + invalid + "\n");
            checked = pbrt::check(path);
            require(checked.exit_code() == 0, pbrt::report(checked));
            auto rejected = pbrt::translate(checked, pbrt::Location{path.string()});
            require(!rejected.scene && !rejected.diagnostics.empty() &&
                        rejected.diagnostics[0].location.line > 6,
                    "collapsed packed spot cone produces source-located diagnostic");
        }
        path = write(dir, "LightSource \"spot\" \"point3 to\" [1e300 1e-300 1]\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        require(!pbrt::translate(checked, pbrt::Location{path.string()}).scene,
                "nonzero aiming component lost during normalization is rejected");
        path = write(dir, "AttributeBegin\nScale 1 1 1e-40\nLightSource \"spot\"\nAttributeEnd\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        require(!pbrt::translate(checked, pbrt::Location{path.string()}).scene,
                "affine inverse that needs subnormal packed coefficients is rejected");
        path = write(
            dir, "AttributeBegin\nTransform [1 1 0 0 1 1.0000000000000002 0 0 0 0 1 0 0 0 0 1]\n"
                 "LightSource \"spot\"\nAttributeEnd\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        require(!pbrt::translate(checked, pbrt::Location{path.string()}).scene,
                "nonsingular affine inverse collapsing after float cancellation is rejected");
        std::string mixed;
        for (unsigned i = 0; i < max_positional_lights / 2; ++i)
            mixed += "LightSource \"point\" \"rgb I\" [0 0 0]\nLightSource \"spot\"\n";
        auto mix = translate(dir, mixed);
        require(mix.scene->point_lights.size() + mix.scene->spot_lights.size() ==
                    max_positional_lights,
                "zero lights count toward combined limit");
        path = write(dir, mixed + "LightSource \"point\"\n");
        require(pbrt::check(path).exit_code() == 1, "mixed 65th positional light rejected");
        fs::remove_all(dir);
    } catch (...) {
        fs::remove_all(dir);
        throw;
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
