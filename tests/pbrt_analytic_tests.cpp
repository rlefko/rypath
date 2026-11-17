// SPDX-License-Identifier: GPL-3.0-only
#include "analytic.hpp"
#include "pbrt_render.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
namespace {
using namespace rypath;
void require(bool test, const std::string &message) {
    if (!test)
        throw std::runtime_error(message);
}
struct Scratch {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("rypath-analytic-pbrt-" + std::to_string(getpid()));
    Scratch() { std::filesystem::create_directories(dir); }
    ~Scratch() { std::filesystem::remove_all(dir); }
    std::filesystem::path write(const std::string &body) {
        auto path = dir / "scene.pbrt";
        std::ofstream(path) << "Camera \"perspective\"\nFilm \"rgb\"\nPixelFilter \"box\"\nSampler "
                               "\"independent\"\nIntegrator \"volpath\"\nWorldBegin\n"
                            << body << '\n';
        return path;
    }
};
pbrt::Result checked(Scratch &s, const std::string &body) {
    auto r = pbrt::check(s.write(body));
    require(r.exit_code() == 0 && r.scene.has_value(), pbrt::report(r));
    return r;
}
pbrt::RenderTranslation translated(const pbrt::Result &checked) {
    auto r = pbrt::translate(checked, {});
    std::string why;
    for (const auto &d : r.diagnostics)
        why += d.message + "\n";
    if (r.exit_code() != 0 || !r.scene)
        throw std::runtime_error(why);
    require(validate_render_scene(*r.scene).empty(), validate_render_scene(*r.scene));
    return r;
}
void reject(Scratch &s, const std::string &body, int status = 1) {
    auto r = pbrt::check(s.write(body));
    require(r.exit_code() == status && !r.scene, "invalid analytic shape accepted");
    require(!r.document.diagnostics.empty() && r.document.diagnostics.front().location.line >= 7,
            "analytic diagnostic source location");
}
void run() {
    Scratch s;
    for (const auto &type : {"sphere", "disk", "cylinder"}) {
        auto c = checked(s, std::string("Shape \"") + type + "\"");
        require(c.inventory.analytics == 1 && c.inventory.triangles == 0 &&
                    c.inventory.patches == 0,
                "analytic inventory");
        require(c.scene->geometry.meshes[0].analytics.size() == 1 &&
                    c.scene->geometry.meshes[0].positions.empty(),
                "canonical analytic shape without tessellation");
        translated(c);
        reject(s, std::string("Shape \"") + type + "\" \"float radius\" [0]");
        reject(s, std::string("Shape \"") + type + "\" \"float phimax\" [-1]");
        reject(s, std::string("Shape \"") + type + "\" \"float radius\" [1e100]");
        reject(s, std::string("Shape \"") + type + "\" \"point3 P\" [0 0 0]");
    }
    auto c = checked(s, "Attribute \"shape\" \"float radius\" [2]\nShape \"sphere\" \"float zmin\" "
                        "[4] \"float zmax\" [-4] \"float phimax\" [400]");
    const auto &a = c.scene->geometry.meshes[0].analytics[0];
    require(a.radius == 2 && a.z_min == -2 && a.z_max == 2 &&
                a.phi_max == 2 * std::numbers::pi_v<float>,
            "sphere defaults sorting and clipping");
    translated(c);
    c = checked(s,
                "Shape \"cylinder\" \"float zmin\" [3] \"float zmax\" [-2] \"float phimax\" [90]");
    require(c.scene->geometry.meshes[0].analytics[0].z_min == -2 &&
                c.scene->geometry.meshes[0].analytics[0].z_max == 3,
            "cylinder sorts endpoints");
    translated(c);
    c = checked(s, "Shape \"disk\" \"float radius\" [2] \"float innerradius\" [.25] \"float "
                   "height\" [3] \"float phimax\" [120]");
    translated(c);
    reject(s, "Shape \"sphere\" \"float zmin\" [2] \"float zmax\" [3]");
    reject(s, "Shape \"disk\" \"float innerradius\" [1]");
    reject(s, "Shape \"disk\" \"float innerradius\" [-.1]");
    for (const auto &type : {"cone", "paraboloid", "hyperboloid"})
        reject(s, std::string("Shape \"") + type + "\"");
    c = checked(s, "ObjectBegin \"analytic\"\nTranslate 1 2 3\nReverseOrientation\nShape "
                   "\"sphere\"\nObjectEnd\nScale -2 3 4\nObjectInstance \"analytic\"\nTranslate 2 "
                   "0 0\nObjectInstance \"analytic\"");
    auto t = translated(c);
    require(t.scene->geometry.instances.size() == 2 && t.scene->geometry.meshes.size() == 1,
            "analytic object reuse");
    require(t.scene->instance_reverse_orientation == std::vector<std::uint32_t>{0, 0},
            "reflection and reverse orientation compose");
    require(t.scene->geometry.instances[0].transform.values[3] == -2,
            "definition and placement affine composition");
    for (const auto &type : {"sphere", "disk", "cylinder"}) {
        c = checked(
            s,
            std::string("Scale 2 2 2\nAreaLightSource \"diffuse\" \"float power\" [8]\nShape \"") +
                type + "\" \"float phimax\" [180]");
        t = translated(c);
        std::vector<std::vector<float>> factors;
        std::size_t failed;
        require(prepare_area_power(*t.scene, factors, failed).empty() && factors.size() == 1 &&
                    factors[0].size() == 1,
                "analytic power factor");
        const auto &shape = t.scene->geometry.meshes[0].analytics[0];
        const auto area = analytic_world_area(shape, t.scene->geometry.instances[0].transform);
        require(area.has_value() && std::abs(factors[0][0] * std::numbers::pi * *area - 8) < 1e-5,
                "world analytic power calibration");
    }
    c = checked(s, "Scale 2 3 4\nAreaLightSource \"diffuse\" \"float power\" [8]\nShape \"disk\" "
                   "\"float innerradius\" [.5]");
    translated(c);
    for (const auto &type : {"sphere", "cylinder"}) {
        c = checked(
            s,
            std::string("Scale 2 3 4\nAreaLightSource \"diffuse\" \"float power\" [8]\nShape \"") +
                type + "\"");
        t = pbrt::translate(c, {});
        require(t.exit_code() != 0 && !t.scene, "nonsimilarity curved power must diagnose");
        c = checked(s, std::string("Scale 2 3 4\nAreaLightSource \"diffuse\"\nShape \"") + type +
                           "\"");
        translated(c);
    }
    c = checked(s, "LightSource \"distant\"\nShape \"sphere\"");
    t = translated(c);
    require(t.scene->distant_scene_radius > 0, "analytic distant-light bounds");
    reject(s, "ObjectBegin \"emitter\"\nAreaLightSource \"diffuse\"\nShape \"sphere\"\nObjectEnd",
           2);
    std::cout << "PBRT analytic shapes and power calibration checks passed\n";
}
} // namespace
int main() {
    try {
        run();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
