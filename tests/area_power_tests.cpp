// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "render_identity.hpp"
#include <bit>
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
void near(double a, double b, const std::string &message) {
    require(std::abs(a - b) < 2e-6 * std::max(std::abs(b), 1e-20), message);
}
RenderScene base() {
    RenderScene s;
    s.model = RenderModel::pbrt;
    s.materials.push_back({});
    s.geometry.meshes.push_back({.positions = {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 3, 0}},
                                 .triangles = {{0, 1, 2}, {0, 1, 3}}});
    s.geometry.instances.push_back({});
    s.instance_materials = {0};
    s.instance_emissions = {0};
    s.area_emissions.push_back({});
    s.area_emissions[0].radiance.fill(2);
    s.area_emissions[0].power = 6 * std::numbers::pi;
    return s;
}
std::vector<float> factors(const RenderScene &s, std::size_t i = 0) {
    std::vector<std::vector<float>> result;
    std::size_t failed;
    require(prepare_area_power(s, result, failed).empty(), "prepare area power");
    return result[i];
}
void geometry() {
    auto s = base();
    const auto original = s.area_emissions[0].radiance;
    require(factors(s) == std::vector<float>{6, 2},
            "Each unequal triangle gets full declaration power");
    require(s.area_emissions[0].radiance == original, "Preparation preserves shared spectrum");
    s.area_emissions[0].two_sided = true;
    require(factors(s) == std::vector<float>{3, 1}, "Two-sided divisor");
    s.area_emissions[0].two_sided = false;
    s.geometry.instances.push_back({.transform = {{-2, 1, 0, 3, 0, 3, 0, 7, 0, 0, 1, 2}}});
    s.instance_emissions.push_back(0);
    near(factors(s, 1)[0], 1, "Reflection, shear and affine world area");
    near(factors(s, 1)[1], 1. / 3, "Independent placement factor");
    s.geometry.meshes[0] = {.positions = {{0, 0, 0}, {2, 0, 0}, {0, 3, 0}, {2, 3, 0}},
                            .triangles = {},
                            .patches = {{0, 1, 2, 3}}};
    near(factors(s)[0], 1, "Rectangle area six");
    s.geometry.meshes[0].positions[3] = {1, 3, 0};
    near(factors(s)[0], 6 / 4.5, "Planar trapezoid fixed-grid area");
    s.geometry.meshes[0].positions[3] = {2, 3, 2};
    // Independently summed nine quadrilateral diagonal-cross areas in a script.
    near(factors(s)[0], 6 / 7.231471913455165, "Curved bilinear fixed 3x3 area");
    s.area_emissions[0].power = 0;
    require(factors(s).empty(), "Legacy uses no factors");
    for (double invalid :
         {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        s.area_emissions[0].power = invalid;
        std::vector<std::vector<float>> f;
        std::size_t failed;
        require(!prepare_area_power(s, f, failed).empty() && !validate_render_scene(s).empty(),
                "Direct invalid power");
    }
    s = base();
    s.area_emissions[0].power = 1e-300;
    require(!validate_render_scene(s).empty(), "Underflowing factor rejected");
    s = base();
    s.area_emissions[0].radiance.fill(std::numeric_limits<float>::max());
    require(!validate_render_scene(s).empty(), "Overflowing radiance factor rejected");
    s = base();
    s.area_emissions[0].radiance.fill(0);
    require(factors(s).size() == 2, "Black RGB retains exact zero");
    const auto id = render_identity(s, {});
    s.area_emissions[0].power += 1;
    require(render_identity(s, {}) != id, "Power participates in identity");
}
constexpr auto options = "Camera \"perspective\"\nFilm \"rgb\"\nPixelFilter \"box\"\nSampler "
                         "\"independent\"\nIntegrator \"path\"\nWorldBegin\n";
constexpr auto triangle = "Shape \"trianglemesh\" \"point3 P\" [0 0 2 2 0 2 0 1 2 0 3 2] \"integer "
                          "indices\" [0 1 2 0 1 3]\n";
pbrt::RenderTranslation translate(const fs::path &dir, const std::string &body) {
    const auto file = dir / "scene.pbrt";
    std::ofstream(file) << options << body;
    auto checked = pbrt::check(file);
    require(checked.exit_code() == 0, pbrt::report(checked));
    return pbrt::translate(checked, pbrt::Location{file.string()});
}
void translation(const fs::path &dir) {
    for (auto power : {"0", "-1", "-1e300"}) {
        const auto result = translate(dir, "AreaLightSource \"diffuse\" \"float power\" " +
                                               std::string(power) + "\n" + triangle);
        require(result.scene && result.scene->area_emissions[0].power == 0,
                "Canonical inert power");
    }
    auto a = translate(dir, "AreaLightSource \"diffuse\" \"rgb L\" [0.2 0.8 0.4] \"float scale\" 3 "
                            "\"float power\" 7\n" +
                                std::string(triangle));
    auto b =
        translate(dir, "AreaLightSource \"diffuse\" \"rgb L\" [0.2 0.8 0.4] \"float scale\" 3\n" +
                           std::string(triangle));
    require(a.scene && b.scene, "Positive RGB power translates");
    require(a.scene->area_emissions[0].radiance == b.scene->area_emissions[0].radiance,
            "No RGB luminance divisor or scale replacement");
    near(factors(*a.scene)[0], 7 / std::numbers::pi, "RGB uses unit imageMean");
    auto image = [&](const char *name, bool black) {
        std::ofstream out(dir / name, std::ios::binary);
        out << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
        const unsigned char rgb[]{128, 64, 32, 129, 64, 128, 32, 129};
        const unsigned char zero[8]{};
        out.write(reinterpret_cast<const char *>(black ? zero : rgb), 8);
    };
    image("color.hdr", false);
    image("black.hdr", true);
    a = translate(dir, "AreaLightSource \"diffuse\" \"string filename\" \"color.hdr\" \"float "
                       "scale\" 0 \"float power\" 7\n" +
                           std::string(triangle));
    require(a.scene.has_value(), "Positive image with zero scale valid");
    constexpr double mean = .75 * (.212673499227 + .715151622536) + .25 * .072174878238;
    near(factors(*a.scene)[0], 7 / (std::numbers::pi * mean),
         "Native RGB luminance arithmetic mean");
    for (const auto &tail :
         {std::string(triangle), std::string("AreaLightSource \"\"\n") + triangle}) {
        const auto bad = translate(dir, "AreaLightSource \"diffuse\" \"string filename\" "
                                        "\"black.hdr\" \"float scale\" 0 \"float power\" 7\n" +
                                            tail);
        require(!bad.scene && !bad.diagnostics.empty() && bad.diagnostics[0].location.line == 7,
                "Black image invalid with zero scale and when unused; source location");
    }
    a = translate(dir,
                  "AreaLightSource \"diffuse\" \"float power\" 1e-300\n" + std::string(triangle));
    require(!a.scene && a.diagnostics[0].location.line == 7,
            "Factor diagnostic locates declaration");
    for (const auto &parameters :
         {"\"float scale\" 1e-44", "\"rgb L\" [1e-300 1e-300 1e-300]", "\"float scale\" 1e-300",
          "\"blackbody L\" 3000 \"float scale\" 1e-44"}) {
        const auto lost = translate(dir, "AreaLightSource \"diffuse\" \"float power\" 1e30 " +
                                             std::string(parameters) + "\n" + triangle);
        require(!lost.scene && !lost.diagnostics.empty(),
                "Positive power rejects lost base signal");
    }
    // A directly placed displaced PLY refines into generated primitives; each
    // receives full power using its final displaced vertices, not original area.
    std::ofstream(dir / "triangle.ply")
        << "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty "
           "float z\nproperty float u\nproperty float v\nelement face 1\nproperty list uchar int "
           "vertex_indices\nend_header\n0 0 1 0 0\n1 0 1 1 0\n0 1 1 0 1\n3 0 1 2\n";
    a = translate(
        dir, "Texture \"height\" \"float\" \"constant\" \"float value\" 0.125\nAreaLightSource "
             "\"diffuse\" \"float power\" 7\nShape \"plymesh\" \"string filename\" "
             "\"triangle.ply\" \"texture displacement\" \"height\" \"float edgelength\" .5\n");
    require(a.scene.has_value(), "Displaced power translation");
    const auto &mesh = a.scene->geometry.meshes[0];
    auto f = factors(*a.scene);
    require(mesh.triangles.size() > 1 && f.size() == mesh.triangles.size() &&
                mesh.positions[0].z == 1.125f,
            "Power prepared after refinement and displacement");
    double recovered_area = 0;
    for (float factor : f)
        recovered_area += 7 / (std::numbers::pi * factor);
    near(recovered_area, .5, "Refined primitive areas sum to displaced surface area");
}
} // namespace
int main() try {
    geometry();
    const auto dir = fs::temp_directory_path() / ("rypath-area-power-" + std::to_string(getpid()));
    fs::create_directories(dir);
    try {
        translation(dir);
    } catch (...) {
        fs::remove_all(dir);
        throw;
    }
    fs::remove_all(dir);
    std::cout << "Area power portable contracts passed\n";
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
