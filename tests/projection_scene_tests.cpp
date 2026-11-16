// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "rgb_spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
constexpr auto light = "LightSource \"projection\" \"string filename\" [\"white.hdr\"] ";
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
    require(result.scene.has_value(), "projection translation failed: " + source);
    return result;
}
void close(double actual, double expected, const std::string &message) {
    require(std::abs(actual - expected) <= 2e-6 * std::max(1e-8, std::abs(expected)), message);
}
void translation_failure(const fs::path &dir, const std::string &source) {
    const auto path = write(dir, source);
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    const auto result = pbrt::translate(checked, pbrt::Location{path.string()});
    require(!result.scene && !result.diagnostics.empty(), "translation must fail: " + source);
    require(result.diagnostics.front().location.line > 6, "source-located translation failure");
}
void hdr(const fs::path &path, unsigned width, unsigned height, bool black = false) {
    std::ofstream output(path, std::ios::binary);
    output << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << height << " +X " << width << '\n';
    const unsigned char pixel[]{128, 128, 128, 129}; // exact linear RGB 1
    const unsigned char zero[4]{};
    for (unsigned i = 0; i < width * height; ++i)
        output.write(reinterpret_cast<const char *>(black ? zero : pixel), 4);
}
} // namespace
int main() try {
    const auto dir =
        fs::temp_directory_path() / ("rypath-projection-scene-" + std::to_string(getpid()));
    try {
        fs::create_directories(dir);
        hdr(dir / "white.hdr", 3, 2);
        hdr(dir / "black.hdr", 1, 1, true);
        // Owned RGBA PNG: opaque red RGB with zero alpha; alpha does not dim emission.
        constexpr unsigned char rgba_png[]{
            137, 80, 78,  71,  13, 10,  26,  10, 0,   0,   0,   13,  73,  72,  68,  82, 0, 0,
            0,   1,  0,   0,   0,  1,   8,   6,  0,   0,   0,   31,  21,  196, 137, 0,  0, 0,
            13,  73, 68,  65,  84, 120, 156, 99, 248, 207, 192, 192, 0,   0,   4,   1,  1, 0,
            71,  6,  202, 222, 0,  0,   0,   0,  73,  69,  78,  68,  174, 66,  96,  130};
        std::ofstream rgba_output(dir / "rgba.png", std::ios::binary);
        rgba_output.write(reinterpret_cast<const char *>(rgba_png), sizeof(rgba_png));
        rgba_output.close();
        const auto rgba =
            translate(dir, "LightSource \"projection\" \"string filename\" [\"rgba.png\"]\n");
        const auto &rgba_image =
            *rgba.scene->image_textures[rgba.scene->projection_lights[0].image_texture].image;
        require(rgba_image.source_channels == 3 &&
                    image_base_texel_linear_rgb(rgba_image, 0, 0) == std::array<float, 3>{1, 0, 0},
                "RGBA zero alpha preserves red emission");
        const auto base = translate(dir, std::string(light) + "\n");
        require(base.scene->projection_lights.size() == 1, "one default projection light");
        const auto &projection = base.scene->projection_lights.front();
        const auto color_space = translate(dir, "ColorSpace \"aces2065-1\"\n" + std::string(light) +
                                                    "\nColorSpace \"srgb\"\n");
        require(color_space.scene->projection_lights[0].image_scale == projection.image_scale,
                "image metadata color space is independent of active declaration color space");

        require(projection.light_from_world == std::array<float, 9>{1, 0, 0, 0, -1, 0, 0, 0, 1},
                "PBRT Y flip");
        const auto &image = *base.scene->image_textures[projection.image_texture].image;
        require(image.native_base && image.source_channels == 3 && image.levels.size() == 1 &&
                    image.levels[0].width == 3 && image.levels[0].height == 2,
                "native rectangular NPOT RGB image");
        close(projection.screen_half_width, 1.5, "landscape screen");
        close(projection.screen_half_height, 1, "landscape screen height");
        close(projection.image_scale, 1 / pbrt_y_inner_product(pbrt_d65()), "D65 photometry");
        close(projection.inv_tan_half_fov, 1, "default FOV");
        // Independent analytic six-center integral for the 3x2 constant-white map.
        const double side = std::pow(1 + 1 + .25, -1.5);
        const double middle = std::pow(1 + .25, -1.5);
        const double integral = 6 * (4 * side + 2 * middle) / 6;
        const auto powered =
            translate(dir, std::string(light) + "\"float scale\" [2] \"float power\" [10]\n");
        close(powered.scene->projection_lights[0].image_scale,
              20 * projection.image_scale / integral, "analytic rectangular power integral");
        hdr(dir / "portrait.hdr", 1, 3);
        const auto portrait =
            translate(dir, "LightSource \"projection\" \"string filename\" [\"portrait.hdr\"] "
                           "\"float fov\" [60] \"float power\" [7]\n");
        const double portrait_integral = 4 * (2 * std::pow(1 + 4. / 3, -1.5) + 1) / 3;
        close(portrait.scene->projection_lights[0].image_scale,
              7 * projection.image_scale / portrait_integral, "portrait FOV power integral");
        require(portrait.scene->projection_lights[0].screen_half_width == 1 &&
                    portrait.scene->projection_lights[0].screen_half_height == 3,
                "portrait screen bounds");
        for (const auto angle : {"1e-30", "179.999"})
            require(translate(dir, std::string(light) + "\"float fov\" [" + angle + "]\n")
                            .scene->projection_lights.size() == 1,
                    "representable boundary FOV");
        for (const auto power : {"0", "-3", "-1e300"}) {
            const auto result =
                translate(dir, std::string(light) + "\"float power\" [" + power + "]\n");
            require(result.scene->projection_lights[0].image_scale == projection.image_scale,
                    "finite nonpositive power inert");
        }
        auto affine =
            translate(dir, "AttributeBegin\nTransform [-2 0 0 0 1 4 0 0 0 0 .5 0 3 4 5 1]\n" +
                               std::string(light) + "\"float power\" [10]\nAttributeEnd\n");
        const auto &transformed = affine.scene->projection_lights[0];
        require(transformed.position.x == 3 && transformed.position.y == 4 &&
                    transformed.position.z == 5,
                "declaration origin follows translation");
        require(transformed.light_from_world ==
                    std::array<float, 9>{-.5, .125, 0, 0, -.25, 0, 0, 0, 2},
                "actual inverse retains affine scale shear reflection and Y flip");
        close(transformed.image_scale, powered.scene->projection_lights[0].image_scale / 2,
              "power integral independent of affine distortion");
        const auto scoped = translate(
            dir, "Attribute \"light\" \"float scale\" [2] \"string filename\" [\"white.hdr\"]\n"
                 "AttributeBegin\nTranslate 7 0 0\nLightSource \"projection\"\nAttributeEnd\n" +
                     std::string(light) + "\"float scale\" [3]\n" + light + "\n");
        require(scoped.scene->projection_lights[0].position.x == 7 &&
                    scoped.scene->projection_lights[1].position.x == 0,
                "scoped pose");
        close(scoped.scene->projection_lights[0].image_scale, 2 * projection.image_scale,
              "inherited scale");
        close(scoped.scene->projection_lights[1].image_scale, 3 * projection.image_scale,
              "explicit precedence");
        require(scoped.scene->image_textures.size() == 1, "shared RGB image cache");
        const auto fixtures = fs::path(__FILE__).parent_path() / "fixtures";
        for (const auto &[asset, name] :
             {std::pair{"image/baseline-rgb.jpg", "codec.jpg"},
              {"pbrt-render/image-assets/tp1n3p08.png", "codec.png"},
              {"pbrt-render/hdr-io/assets/bright-float.exr", "codec.exr"}}) {
            fs::copy_file(fixtures / asset, dir / name, fs::copy_options::overwrite_existing);
            require(translate(dir, "LightSource \"projection\" \"string filename\" [\"" +
                                       std::string(name) + "\"]\n")
                            .scene->projection_lights.size() == 1,
                    "native RGB/RGBA codec translation");
        }
        // The same decoded file must retain distinct scalar and RGB prepared variants.
        fs::copy_file(fixtures / "pbrt-render/core-textures/assets/emission.png",
                      dir / "emission.png");
        const auto variants =
            translate(dir, "LightSource \"projection\" \"string filename\" [\"emission.png\"]\n"
                           "LightSource \"goniometric\" \"string filename\" [\"emission.png\"]\n");
        require(variants.scene->image_textures.size() == 2 &&
                    variants.scene->projection_lights[0].image_texture !=
                        variants.scene->goniometric_lights[0].image_texture,
                "scalar and RGB prepared image cache must differ");
        auto path = write(dir, "Attribute \"light\" \"string filename\" "
                               "[\"white.hdr\"]\nLightSource \"projection\"\n");
        auto checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        auto owned = checked;
        checked = {};
        fs::remove(dir / "white.hdr");
        auto result = pbrt::translate(owned, pbrt::Location{path.string()});
        require(result.scene && result.scene->projection_lights.size() == 1,
                "checked scene owns decoded inherited image");
        hdr(dir / "white.hdr", 3, 2);
        require(translate(dir, "LightSource \"projection\" \"string filename\" [\"black.hdr\"]\n")
                        .scene->projection_lights.size() == 1,
                "black map without positive power");
        for (const auto scale : {"0", "1"})
            translation_failure(dir,
                                "LightSource \"projection\" \"string filename\" [\"black.hdr\"] "
                                "\"float power\" [1] \"float scale\" [" +
                                    std::string(scale) + "]\n");
        for (const auto suffix :
             {"\"float scale\" [1e-38]", "\"float scale\" [1e-300]",
              "\"float scale\" [1e-38] \"float power\" [1e-300]", "\"float power\" [1e300]",
              "\"float fov\" [179.9999999]", "\"float fov\" [1e-50]"})
            translation_failure(dir, std::string(light) + suffix + "\n");
        for (const auto transform :
             {"Translate 1e-300 0 0", "Translate 1e300 0 0", "Scale 1e300 1 1",
              "Transform [1 0 0 0 1e-300 1 0 0 0 0 1 0 0 0 0 1]",
              "Transform [1 1 0 0 1 1.00000001 0 0 0 0 1 0 0 0 0 1]"})
            translation_failure(dir, "AttributeBegin\n" + std::string(transform) + "\n" + light +
                                         "\nAttributeEnd\n");
        for (const auto invalid :
             {"LightSource \"projection\"\n",
              "LightSource \"projection\" \"string filename\" [\"\"]\n",
              "LightSource \"projection\" \"string filename\" [\"absent.png\"]\n"})
            require(pbrt::check(write(dir, invalid)).exit_code() == 1, "required resource error");
        fs::copy_file(fixtures / "pbrt-render/core-textures/assets/white.png", dir / "gray.png");
        require(pbrt::check(
                    write(dir, "LightSource \"projection\" \"string filename\" [\"gray.png\"]\n"))
                        .exit_code() == 1,
                "gray projection map rejected");
        for (const auto invalid :
             {"\"float fov\" [0]", "\"float fov\" [180]", "\"float scale\" [-1]"})
            require(pbrt::check(write(dir, std::string(light) + invalid + "\n")).exit_code() == 1,
                    "authored numeric domain");
        for (const auto unsupported : {"\"rgb I\" [1 1 1]", "\"rgb L\" [1 1 1]",
                                       "\"point3 from\" [0 0 0]", "\"point3 to\" [0 0 1]"}) {
            const auto invalid = pbrt::check(write(dir, std::string(light) + unsupported + "\n"));
            require(invalid.exit_code() == 1 && !invalid.document.diagnostics.empty() &&
                        invalid.document.diagnostics.front().code == "unused-parameter" &&
                        invalid.document.diagnostics.front().location.line == 7,
                    "source-located schema boundary");
        }
        std::string mixed;
        for (unsigned i = 0; i < max_positional_lights; ++i)
            mixed += i % 4 == 0   ? std::string(light) + "\n"
                     : i % 4 == 1 ? "LightSource \"point\"\n"
                     : i % 4 == 2
                         ? "LightSource \"spot\"\n"
                         : "LightSource \"goniometric\" \"string filename\" [\"emission.png\"]\n";
        const auto many = translate(dir, mixed);
        require(many.scene->projection_lights.size() == 16,
                "combined positional budget accepts 64");
        for (const auto extra :
             {light, "LightSource \"point\"", "LightSource \"spot\"",
              "LightSource \"goniometric\" \"string filename\" [\"emission.png\"]"})
            require(pbrt::check(write(dir, mixed + extra + "\n")).exit_code() == 1,
                    "combined 65th light rejected");
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
