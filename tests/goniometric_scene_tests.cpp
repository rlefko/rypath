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
constexpr auto light = "LightSource \"goniometric\" \"string filename\" [\"white.png\"] ";
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
    require(result.scene.has_value(), "goniometric translation failed");
    return result;
}
void close(double actual, double expected, const std::string &message) {
    require(std::abs(actual - expected) <= 2e-6 * std::max(1., std::abs(expected)), message);
}
void translation_failure(const fs::path &dir, const std::string &source) {
    const auto path = write(dir, source);
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    const auto result = pbrt::translate(checked, pbrt::Location{path.string()});
    require(!result.scene && !result.diagnostics.empty(), "translation must fail with diagnostic");
    require(result.diagnostics.front().location.line > 6,
            "translation failure retains declaration or parameter source");
}
} // namespace
int main() try {
    const auto dir =
        fs::temp_directory_path() / ("rypath-goniometric-scene-" + std::to_string(getpid()));
    try {
        fs::create_directories(dir);
        const auto assets =
            fs::path(__FILE__).parent_path() / "fixtures/pbrt-render/core-textures/assets";
        for (const auto name : {"white.png", "zero.png", "gray-pattern.png"})
            fs::copy_file(assets / name, dir / name, fs::copy_options::overwrite_existing);
        const auto base = translate(dir, std::string(light) + "\n");
        require(base.scene->goniometric_lights.size() == 1, "one default goniometric light");
        const auto &gonio = base.scene->goniometric_lights.front();
        require(gonio.position.x == 0 && gonio.position.y == 0 && gonio.position.z == 0,
                "declaration origin");
        require(gonio.light_from_world == std::array<float, 9>{1, 0, 0, 0, 0, 1, 0, 1, 0},
                "PBRT YZ swap");
        require(gonio.image_texture < base.scene->image_textures.size(), "native map retained");
        const auto &image = *base.scene->image_textures[gonio.image_texture].image;
        require(image.native_base && image.source_channels == 1, "prepared scalar native map");
        const auto d65 = pbrt_d65();
        const auto norm = pbrt_y_inner_product(d65);
        for (std::size_t i = 0; i < d65.size(); ++i)
            close(gonio.intensity[i], d65[i] / norm, "default photometric spectrum");
        for (const auto power : {"0", "-3", "-1e300"}) {
            const auto result = translate(dir, std::string(light) + "\"float scale\" [2] " +
                                                   "\"float power\" [" + power + "]\n");
            close(result.scene->goniometric_lights[0].intensity[200], 2 * gonio.intensity[200],
                  "finite nonpositive power is inert");
        }
        const auto powered =
            translate(dir, std::string(light) + "\"float scale\" [2] \"float power\" [10]\n");
        close(powered.scene->goniometric_lights[0].intensity[200],
              20 * gonio.intensity[200] / (4 * std::numbers::pi), "white map power integral");
        const auto pattern = translate(dir, "LightSource \"goniometric\" \"string filename\" "
                                            "[\"gray-pattern.png\"] \"float power\" [10]\n");
        const auto &map = *pattern.scene->image_textures[0].image;
        double sum = 0;
        for (std::uint32_t y = 0; y < map.levels[0].height; ++y)
            for (std::uint32_t x = 0; x < map.levels[0].width; ++x)
                sum += (*image_base_texel_linear_rgb(map, x, y))[0];
        const double mean = sum / (double(map.levels[0].width) * map.levels[0].height);
        close(pattern.scene->goniometric_lights[0].intensity[200],
              10 * gonio.intensity[200] / (4 * std::numbers::pi * mean), "prepared map mean power");
        const auto moved =
            translate(dir, "Translate 3 4 5\nScale -1 1 1\n" + std::string(light) + "\n");
        const auto &reflected = moved.scene->goniometric_lights[0];
        require(reflected.position.x == 3 && reflected.position.y == 4 && reflected.position.z == 5,
                "transformed declaration origin");
        require(reflected.light_from_world == std::array<float, 9>{-1, 0, 0, 0, 0, 1, 0, 1, 0},
                "reflection preserves actual inverse");
        const auto near = translate(dir, "AttributeBegin\nScale 1.000003 1 1\n" +
                                             std::string(light) + "\nAttributeEnd\n");
        require(near.scene->goniometric_lights[0].light_from_world[0] == float(1 / 1.000003),
                "near tolerance uses inverse rather than transpose or common rescaling");
        const auto shear =
            translate(dir, "AttributeBegin\nTransform [1 0 0 0 .000003 1 0 0 0 0 1 0 0 0 0 1]\n" +
                               std::string(light) + "\nAttributeEnd\n");
        require(shear.scene->goniometric_lights[0].light_from_world[1] == float(-.000003),
                "accepted near-orthonormal inverse retains shear sign");
        for (const auto &transform :
             {"Scale 2 2 2", "Scale 1.00001 1 1", "Transform [1 0 0 0 .01 1 0 0 0 0 1 0 0 0 0 1]",
              "Translate 1e300 0 0", "Translate 1e-300 0 0",
              "Transform [1 0 0 0 1e-300 1 0 0 0 0 1 0 0 0 0 1]"})
            translation_failure(dir, "AttributeBegin\n" + std::string(transform) + "\n" + light +
                                         "\nAttributeEnd\n");
        auto scoped = translate(dir, "Attribute \"light\" \"float scale\" [2]\nAttributeBegin\n"
                                     "Translate 7 0 0\n" +
                                         std::string(light) + "\nAttributeEnd\n" + light +
                                         "\"float scale\" [3]\n" + light + "\n");
        require(scoped.scene->goniometric_lights[0].position.x == 7 &&
                    scoped.scene->goniometric_lights[1].position.x == 0,
                "scoped transform");
        close(scoped.scene->goniometric_lights[0].intensity[200], 2 * gonio.intensity[200],
              "inherited scale");
        close(scoped.scene->goniometric_lights[1].intensity[200], 3 * gonio.intensity[200],
              "explicit precedence");
        require(scoped.scene->image_textures.size() == 1 &&
                    scoped.scene->goniometric_lights[0].image_texture ==
                        scoped.scene->goniometric_lights[1].image_texture,
                "shared prepared image cache alias");
        for (const auto &spectrum :
             {"\"rgb I\" [.3 .7 .1]", "\"blackbody I\" [3000]", "\"spectrum I\" [360 1 830 2]",
              "\"spectrum I\" [\"stdillum-D65\"]"})
            require(translate(dir, std::string(light) + spectrum + "\n")
                            .scene->goniometric_lights[0]
                            .intensity[200] > 0,
                    "all supported intensity forms");
        std::ofstream(dir / "intensity.spd") << "360 1\n830 2\n";
        auto path = write(dir, "Attribute \"light\" \"spectrum I\" [\"intensity.spd\"]\n" +
                                   std::string(light) + "\n");
        auto checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        auto owned = checked;
        checked = {};
        fs::remove(dir / "intensity.spd");
        auto result = pbrt::translate(owned, pbrt::Location{path.string()});
        require(result.scene && result.scene->goniometric_lights[0].intensity[200] > 0,
                "checked scene owns inherited file spectrum");
        auto black =
            translate(dir, "LightSource \"goniometric\" \"string filename\" [\"zero.png\"]\n");
        require(black.scene->goniometric_lights.size() == 1,
                "black map without positive power supported");
        translation_failure(
            dir,
            "LightSource \"goniometric\" \"string filename\" [\"zero.png\"] \"float power\" [1]\n");
        translation_failure(dir, std::string(light) + "\"float power\" [1e300]\n");
        translation_failure(dir, std::string(light) + "\"float scale\" [1e-38]\n");
        const auto zero_intensity = translate(dir, std::string(light) + "\"rgb I\" [0 0 0]\n");
        require(std::ranges::all_of(zero_intensity.scene->goniometric_lights[0].intensity,
                                    [](float value) { return value == 0; }),
                "zero intensity remains exactly black");
        translation_failure(dir, std::string(light) + "\"spectrum I\" [360 -1 830 -1]\n");
        for (const auto &invalid :
             {"LightSource \"goniometric\"\n",
              "LightSource \"goniometric\" \"string filename\" [\"\"]\n",
              "LightSource \"goniometric\" \"string filename\" [\"absent.png\"]\n"}) {
            checked = pbrt::check(write(dir, invalid));
            require(checked.exit_code() == 1 && !checked.document.diagnostics.empty() &&
                        checked.document.diagnostics.front().location.line > 6,
                    "required map source-located failure");
        }
        std::string mixed;
        for (unsigned i = 0; i < max_positional_lights; ++i)
            mixed += i % 3 == 0   ? std::string(light) + "\n"
                     : i % 3 == 1 ? "LightSource \"point\"\n"
                                  : "LightSource \"spot\"\n";
        const auto many = translate(dir, mixed);
        require(many.scene->goniometric_lights.size() + many.scene->point_lights.size() +
                        many.scene->spot_lights.size() ==
                    max_positional_lights,
                "combined positional budget");
        for (const auto extra : {"LightSource \"point\"\n", "LightSource \"spot\"\n", light})
            require(pbrt::check(write(dir, mixed + extra + "\n")).exit_code() == 1,
                    "combined 65th light rejected");
        path = write(dir, std::string(light) + "\n");
        checked = pbrt::check(path);
        pbrt::Limits limits;
        limits.total_values = 470;
        require(!pbrt::translate(checked, pbrt::Location{path.string()}, limits).scene,
                "dense spectrum value budget");
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
