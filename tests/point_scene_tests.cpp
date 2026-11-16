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
    require(result.scene.has_value(), "point translation failed");
    return result;
}
void close(double actual, double expected, const std::string &message) {
    require(std::abs(actual - expected) <= 2e-6 * std::max(1., std::abs(expected)), message);
}
} // namespace
int main() try {
    const auto dir = fs::temp_directory_path() / ("rypath-point-scene-" + std::to_string(getpid()));
    try {
        fs::create_directories(dir);
        const auto base = translate(dir, "LightSource \"point\"\n");
        require(base.scene->point_lights.size() == 1, "one point default");
        const auto &point = base.scene->point_lights.front();
        require(point.position.x == 0 && point.position.y == 0 && point.position.z == 0,
                "default origin");
        const auto d65 = pbrt_d65();
        const auto norm = pbrt_y_inner_product(d65);
        for (std::size_t i = 0; i < d65.size(); ++i)
            close(point.intensity[i], d65[i] / norm, "default normalized illuminant");
        for (const auto power : {"0", "-3", "12.566370614359172"}) {
            const auto result =
                translate(dir, std::string("LightSource \"point\" \"float scale\" [2] ") +
                                   "\"float power\" [" + power + "]\n");
            for (std::size_t i = 0; i < d65.size(); ++i)
                close(result.scene->point_lights.front().intensity[i], 2 * point.intensity[i],
                      "power and intensity units");
        }
        const auto powered = translate(dir, "LightSource \"point\" \"float scale\" [2] "
                                            "\"float power\" [25.132741228718345]\n");
        for (std::size_t i = 0; i < d65.size(); ++i)
            close(powered.scene->point_lights[0].intensity[i], 4 * point.intensity[i],
                  "positive power multiplies scale by power over four pi");
        auto transformed = translate(dir, "Translate 3 4 5\nScale 2 3 4\nLightSource \"point\" "
                                          "\"point3 from\" [1 2 3]\n");
        const auto position = transformed.scene->point_lights.front().position;
        require(position.x == 5 && position.y == 10 && position.z == 17,
                "from transformed in world coordinates");
        auto scoped = translate(dir, "Attribute \"light\" \"float scale\" [2]\nAttributeBegin\n"
                                     "Translate 7 0 0\nLightSource \"point\"\nAttributeEnd\n"
                                     "LightSource \"point\" \"float scale\" [3]\n"
                                     "ObjectBegin \"o\"\nLightSource \"point\"\nObjectEnd\n"
                                     "ObjectInstance \"o\"\nObjectInstance \"o\"\n"
                                     "LightSource \"infinite\"\n");
        require(scoped.scene->point_lights.size() == 3, "object light remains global");
        require(scoped.scene->point_lights[0].position.x == 7 &&
                    scoped.scene->point_lights[1].position.x == 0,
                "transform scope restored");
        close(scoped.scene->point_lights[0].intensity[200], 2 * point.intensity[200],
              "inherited light scale");
        close(scoped.scene->point_lights[1].intensity[200], 3 * point.intensity[200],
              "explicit scale overrides inherited");
        for (const auto &spectrum :
             {"\"rgb I\" [.3 .7 .1]", "\"blackbody I\" [3000]", "\"spectrum I\" [360 1 830 1]",
              "\"spectrum I\" [\"stdillum-D65\"]"}) {
            const auto source = std::string("LightSource \"point\" ") + spectrum + "\n";
            auto result = translate(dir, source);
            require(result.scene->point_lights.front().intensity[200] > 0,
                    "supported intensity spectral form");
        }
        std::ofstream(dir / "intensity.spd") << "360 1\n830 2\n";
        auto path = write(dir, "Attribute \"light\" \"spectrum I\" [\"intensity.spd\"]\n"
                               "LightSource \"point\"\n");
        auto checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        auto owned = checked;
        checked = {};
        fs::remove(dir / "intensity.spd");
        auto result = pbrt::translate(owned, pbrt::Location{path.string()});
        require(result.scene && result.scene->point_lights[0].intensity[200] > 0,
                "copied checked scene owns decoded inherited file");
        const auto zero = translate(dir, "LightSource \"point\" \"rgb I\" [0 0 0]\n");
        require(std::ranges::all_of(zero.scene->point_lights[0].intensity,
                                    [](float value) { return value == 0; }),
                "zero RGB intensity remains black");
        std::ofstream(dir / "malformed.spd") << "360 nope\n";
        path = write(dir, "LightSource \"point\" \"spectrum I\" [\"malformed.spd\"]\n");
        require(pbrt::check(path).exit_code() == 1, "malformed point spectrum file rejected");
        std::ofstream(dir / "light.pbrt") << "LightSource \"point\" \"point3 from\" [2 0 0]\n";
        const auto included = translate(dir, "Translate 4 0 0\nInclude \"light.pbrt\"\n");
        require(included.scene->point_lights[0].position.x == 6,
                "included point observes shared world state");
        std::string many;
        for (unsigned i = 0; i < max_point_lights; ++i)
            many += "LightSource \"point\"\n";
        require(translate(dir, many).scene->point_lights.size() == max_point_lights, "64 lights");
        path = write(dir, many + "LightSource \"point\"\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 1 && !checked.document.diagnostics.empty() &&
                    checked.document.diagnostics.back().location.line > 6,
                "65th light source-located resource error");
        for (const auto &invalid :
             {"\"float I\" [1]", "\"point3 from\" [1 2]", "\"float scale\" [-1]",
              "\"rgb I\" [-1 0 0]", "\"string filename\" [\"x.png\"]",
              "\"spectrum I\" [\"absent.spd\"]", "\"float power\" [1 2]"}) {
            path = write(dir, std::string("LightSource \"point\" ") + invalid + "\n");
            require(pbrt::check(path).exit_code() == 1, "malformed point rejected");
        }
        path = write(dir, "LightSource \"point\" \"spectrum I\" [360 -1 830 -1]\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        require(!pbrt::translate(checked, pbrt::Location{path.string()}).scene,
                "negative spectral intensity rejected at translation");
        path = write(dir,
                     "LightSource \"infinite\"\nLightSource \"point\"\nLightSource \"infinite\"\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 0 &&
                    !pbrt::translate(checked, pbrt::Location{path.string()}).scene,
                "multiple infinite lights remain unsupported");
        path = write(dir, "MediumInterface \"\" \"missing\"\nLightSource \"point\"\n"
                          "MediumInterface \"\" \"\"\n");
        require(pbrt::check(path).exit_code() == 1, "undefined endpoint medium rejected");
        result = translate(dir, "MakeNamedMedium \"fog\" \"string type\" [\"homogeneous\"]\n"
                                "MediumInterface \"\" \"fog\"\nLightSource \"point\"\n"
                                "MediumInterface \"\" \"\"\n");
        require(!result.scene->volume_transport, "endpoint medium does not force volpath");
        checked = pbrt::check(dir / "scene.pbrt");
        require(checked.scene && checked.scene->lights[0].outside_medium == "fog",
                "point captures declaration outside medium");
        path = write(dir, "LightSource \"point\"\n");
        checked = pbrt::check(path);
        pbrt::Limits limits;
        limits.total_values = 470;
        require(!pbrt::translate(checked, pbrt::Location{path.string()}, limits).scene,
                "dense point spectrum charges translated value budget");
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
