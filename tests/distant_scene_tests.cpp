// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "rgb_spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
void close(double a, double b, const std::string &message) {
    require(std::abs(a - b) <= 3e-6 * std::max(1., std::abs(b)), message);
}
constexpr auto options = "Camera \"perspective\"\nFilm \"rgb\"\nPixelFilter \"box\"\nSampler "
                         "\"independent\"\nIntegrator \"path\"\nWorldBegin\n";
constexpr auto triangle = "Shape \"trianglemesh\" \"point3 P\" [0 0 0 2 0 0 0 2 0]\n";
pbrt::Result checked(const fs::path &dir, const std::string &source) {
    const auto path = dir / "scene.pbrt";
    std::ofstream(path) << options << source;
    return pbrt::check(path);
}
auto translate(const fs::path &dir, const std::string &source) {
    const auto input = checked(dir, source);
    require(input.exit_code() == 0, pbrt::report(input));
    return pbrt::translate(input, pbrt::Location{(dir / "scene.pbrt").string()});
}
auto scene(const fs::path &dir, const std::string &source) {
    auto result = translate(dir, source);
    if (!result.scene)
        throw std::runtime_error("distant translation failed");
    return std::move(*result.scene);
}
} // namespace
int main() try {
    const auto dir =
        fs::temp_directory_path() / ("rypath-distant-scene-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const auto base = scene(dir, std::string("LightSource \"distant\"\n") + triangle);
    require(base.distant_lights.size() == 1, "default distant count");
    require(base.distant_lights[0].direction.z == -1, "default points toward source");
    close(base.distant_scene_radius, std::sqrt(2.), "planar scene has positive radius");
    const auto d65 = pbrt_d65();
    const auto norm = pbrt_y_inner_product(d65);
    for (std::size_t i = 0; i < d65.size(); ++i)
        close(base.distant_lights[0].radiance[i], d65[i] / norm, "normalized default spectrum");
    for (const auto *e : {"0", "-2", "3"}) {
        const auto value = scene(
            dir,
            std::string("LightSource \"distant\" \"float scale\" [2] \"float illuminance\" [") + e +
                "]\n" + triangle);
        close(value.distant_lights[0].radiance[195],
              base.distant_lights[0].radiance[195] * (std::string(e) == "3" ? 6.f : 2.f),
              "positive illuminance only, no pi or world-up correction");
    }
    const auto affine = scene(
        dir,
        std::string("AttributeBegin\nTranslate 100 200 300\nScale -2 3 4\nLightSource \"distant\" "
                    "\"point3 from\" [1 2 3] \"point3 to\" [2 4 6]\nAttributeEnd\n") +
            triangle);
    const auto w = affine.distant_lights[0].direction;
    close(w.x, 2 / std::sqrt(184.), "affine direction x");
    close(w.y, -6 / std::sqrt(184.), "affine direction y");
    close(w.z, -12 / std::sqrt(184.), "affine direction z");
    for (const auto *spectrum : {"\"rgb L\" [0 0 0]", "\"blackbody L\" [3000]",
                                 "\"spectrum L\" [360 1 830 1]", "\"spectrum L\" \"stdillum-F11\""})
        require(scene(dir, std::string("LightSource \"distant\" ") + spectrum + "\n" + triangle)
                        .distant_lights.size() == 1,
                "spectral emission form");
    std::ofstream(dir / "light.spd") << "360 1\n830 1\n";
    require(
        scene(dir,
              std::string(
                  "Attribute \"light\" \"spectrum L\" \"light.spd\"\nLightSource \"distant\"\n") +
                  triangle)
                .distant_lights.size() == 1,
        "inherited spectral dependency");
    for (const auto *bad :
         {"\"point3 from\" [0 0 1]", "\"point3 to\" [0 0]", "\"float scale\" [-1]",
          "\"string filename\" \"image.png\"", "\"float power\" [2]"}) {
        const auto value =
            checked(dir, std::string("LightSource \"distant\" ") + bad + "\n" + triangle);
        require(value.exit_code() != 0, "malformed or unsupported distant parameter");
        require(!value.document.diagnostics.empty() &&
                    value.document.diagnostics[0].location.line > 0,
                "source-located diagnostic");
    }
    for (const auto *source :
         {"LightSource \"distant\" \"point3 to\" [1e300 1e-300 0]\n",
          "Scale 1e300 1e-300 1\nLightSource \"distant\" \"point3 to\" [1 1 0]\n"})
        require(
            !translate(dir, std::string("AttributeBegin\n") + source + "AttributeEnd\n" + triangle)
                 .scene,
            "unrepresentable directional component rejected");
    const auto cancellation = scene(
        dir, std::string("AttributeBegin\nTransform [1e300 0 0 0 1 1 0 0 -1e300 0 1 0 0 0 0 1]\n"
                         "LightSource \"distant\" \"point3 from\" [1 1 1] \"point3 to\" [0 0 "
                         "0]\nAttributeEnd\n") +
                 triangle);
    const auto cancellation_w = cancellation.distant_lights[0].direction;
    close(cancellation_w.x, 1 / std::sqrt(3.), "certified cancellation retains small x term");
    close(cancellation_w.y, 1 / std::sqrt(3.), "certified cancellation y direction");
    close(cancellation_w.z, 1 / std::sqrt(3.), "certified cancellation z direction");
    const auto endpoint_tail = scene(
        dir, std::string("AttributeBegin\nTransform [1e18 0 0 0 1e18 1 0 0 -1e18 0 1 0 0 0 0 1]\n"
                         "LightSource \"distant\" \"point3 from\" [1 1e-17 1] \"point3 to\" [1e-17 "
                         "0 0]\nAttributeEnd\n") +
                 triangle);
    require(endpoint_tail.distant_lights[0].direction.x == 0,
            "endpoint subtraction tail preserves exact common cancellation zero");
    const auto exact_zero =
        scene(dir, std::string("AttributeBegin\nRotate 90 0 0 1\nLightSource \"distant\" \"point3 "
                               "from\" [1 0 0] \"point3 to\" [0 0 0]\nAttributeEnd\n") +
                       triangle);
    close(exact_zero.distant_lights[0].direction.y, 1, "benign rotation retained");
    for (const auto *uniform : {"1e300", "1e-300"}) {
        const auto robust =
            scene(dir, std::string("AttributeBegin\nScale ") + uniform + " " + uniform + " " +
                           uniform + "\nLightSource \"distant\"\nAttributeEnd\n" + triangle);
        require(robust.distant_lights[0].direction.z == -1,
                "extreme uniform scale retains direction");
    }
    std::string lights;
    for (int i = 0; i < 64; ++i)
        lights += "LightSource \"distant\"\nLightSource \"point\"\n";
    require(scene(dir, lights + triangle).distant_lights.size() == 64,
            "independent distant and positional limits");
    require(checked(dir, lights + "LightSource \"distant\"\n" + triangle).exit_code() != 0,
            "65 distant rejected");
    const auto unused = scene(
        dir, "LightSource \"distant\"\nObjectBegin \"unused\"\nShape \"trianglemesh\" \"point3 P\" "
             "[100 100 100 200 100 100 100 200 100]\nObjectEnd\nShape \"trianglemesh\" \"point3 "
             "P\" [0 0 0 2 0 0 0 2 0 1000 1000 1000] \"integer indices\" [0 1 2]\n");
    close(unused.distant_scene_radius, std::sqrt(2.), "unused object and vertices excluded");
    const auto grouped = scene(
        dir, "LightSource \"distant\"\nObjectBegin \"pair\"\nShape \"trianglemesh\" \"point3 P\" "
             "[0 0 0 2 0 0 0 2 0]\nObjectEnd\nRotate 45 0 0 1\nObjectInstance \"pair\"\n");
    close(grouped.distant_scene_radius, 2,
          "placement transforms object aggregate corners, not tight vertices");
    std::ofstream(dir / "patch.ply")
        << "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty "
           "float z\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n2 "
           "0 0\n0 2 0\n2 2 0\n4 0 1 3 2\n";
    const auto patch = scene(
        dir, "LightSource \"distant\"\nShape \"plymesh\" \"string filename\" \"patch.ply\"\n");
    close(patch.distant_scene_radius, std::sqrt(2.), "all four patch corners included");
    const auto scoped = scene(
        dir, std::string("AttributeBegin\nAttribute \"light\" \"float scale\" [3]\nLightSource "
                         "\"distant\"\nAttributeEnd\nLightSource \"distant\"\n") +
                 triangle);
    close(scoped.distant_lights[0].radiance[195], 3 * scoped.distant_lights[1].radiance[195],
          "inherited light parameters obey attribute scope");
    const auto null_bounds = scene(dir, "LightSource \"distant\"\nMaterial \"interface\"\nShape "
                                        "\"trianglemesh\" \"point3 P\" [0 0 0 2 0 0 0 2 0]\n");
    close(null_bounds.distant_scene_radius, std::sqrt(2.), "null geometry included");
    std::ofstream(dir / "displaced.ply")
        << "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty "
           "float z\nproperty float u\nproperty float v\nelement face 1\nproperty list uchar int "
           "vertex_indices\nend_header\n0 0 0 0 0\n2 0 0 1 0\n0 2 0 0 1\n3 0 1 2\n";
    const auto displaced =
        scene(dir, std::string("LightSource \"distant\"\nTexture \"raise\" \"float\" \"constant\" "
                               "\"float value\" [4]\nShape \"plymesh\" \"string filename\" "
                               "\"displaced.ply\" \"texture displacement\" \"raise\"\n") +
                       triangle);
    close(displaced.distant_scene_radius, std::sqrt(6.),
          "prepared displaced vertices determine radius");
    const auto overflow = translate(dir, "LightSource \"distant\"\nShape \"trianglemesh\" \"point3 "
                                         "P\" [2e38 0 0 3e38 0 0 2e38 1 0]\n");
    require(!overflow.scene, "binary32 bound-center overflow rejected");
    auto zero_checked = checked(dir, std::string("LightSource \"distant\"\n") + triangle);
    if (!zero_checked.scene)
        throw std::runtime_error("zero-radius fixture check failed");
    for (auto &p : zero_checked.scene->geometry.meshes[0].positions)
        p = {};
    require(!pbrt::translate(zero_checked, pbrt::Location{(dir / "scene.pbrt").string()}).scene,
            "zero scene radius rejected");
    const auto alpha = scene(dir, "LightSource \"distant\"\nShape \"trianglemesh\" \"point3 P\" [0 "
                                  "0 0 2 0 0 0 2 0] \"float alpha\" [0]\n");
    close(alpha.distant_scene_radius, std::sqrt(2.), "alpha-zero geometry included");
    require(!translate(dir, "LightSource \"distant\" \"rgb L\" [0 0 0]\n").scene,
            "black distant still requires nonempty bounds");
    fs::remove_all(dir);
    std::cout << "distant scene checks passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
