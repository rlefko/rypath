// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "pbrt_spectra.hpp"
#include "texture_graph.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
using namespace rypath::pbrt;
namespace fs = std::filesystem;
constexpr std::string_view options = R"(Camera "perspective"
Film "rgb" "integer xresolution" 8 "integer yresolution" 8
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 1
Integrator "path"
WorldBegin
)";
constexpr std::string_view triangle = R"(Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1]
)";
void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}
void write(const fs::path &path, std::string_view data) {
    std::ofstream stream(path);
    stream << data;
    require(stream.good(), "Cannot write conductor fixture");
}
Result inspect(const fs::path &path, std::string_view body) {
    write(path, std::string(options) + std::string(body));
    return check(path);
}
RenderScene scene(const fs::path &path, std::string_view body) {
    const auto input = inspect(path, body);
    require(input.exit_code() == 0, report(input));
    auto output = translate(input, {path.string()});
    if (!output.scene) {
        for (const auto &diagnostic : output.diagnostics) {
            std::cerr << format_diagnostic(diagnostic);
        }
    }
    if (output.exit_code() != 0 || !output.scene)
        throw std::runtime_error("Conductor translation failed");
    return std::move(*output.scene);
}
const SpectralMaterial &material(const RenderScene &scene) {
    return scene.materials.at(scene.instance_materials.front());
}
void defaults_and_axes(const fs::path &path) {
    const auto copper =
        scene(path, std::string("Material \"conductor\"\n") + std::string(triangle));
    const auto &m = material(copper);
    require(m.scattering == ScatteringKind::conductor && m.remap_roughness && m.u_roughness == 0 &&
                m.v_roughness == 0,
            "Copper and smooth defaults");
    const auto eta = pbrt_named_spectrum_knots("metal-Cu-eta");
    const auto k = pbrt_named_spectrum_knots("metal-Cu-k");
    require(m.eta.knots.size() == eta.size() && m.k.knots.size() == k.size() &&
                m.eta.knots.front().value == eta.front().value &&
                m.k.knots.back().value == k.back().value && m.eta.extend_endpoints &&
                m.k.extend_endpoints,
            "Copper retains original optical knots");
    require(!copper.instance_texture_frames.empty() &&
                !copper.instance_texture_frames.front().empty(),
            "Constant conductor prepares anisotropic surface frames");
    const auto authored = scene(path, std::string(R"(
Attribute "material" "float roughness" 0.7 "spectrum eta" [400 2 700 4]
MakeNamedMaterial "metal" "string type" "conductor" "float roughness" 0.5
    "float uroughness" 0.25 "bool remaproughness" false
NamedMaterial "metal"
)") + std::string(triangle));
    const auto &a = material(authored);
    require(a.u_roughness == 0.25f && a.v_roughness == 0.5f && !a.remap_roughness,
            "Each roughness axis independently falls back to effective common roughness");
    require(optical_spectrum(a.eta, 360) == 0 && optical_spectrum(a.eta, 830) == 0 &&
                optical_spectrum(a.eta, 550) == 3 && a.k.knots.size() == k.size(),
            "Inherited numeric eta has zero tails and k defaults independently");
    const auto independent = scene(path, std::string(R"(Material "conductor"
        "spectrum k" [400 5 700 8] "float roughness" 3 "float vroughness" 2
)") + std::string(triangle));
    const auto &i = material(independent);
    require(i.eta.knots.size() == eta.size() && i.k.knots.front().value == 5 &&
                i.u_roughness == 3 && i.v_roughness == 2,
            "Eta defaults independently; optical constants and roughness have no unit upper bound");
}
void files_and_graphs(const fs::path &path) {
    write(path.parent_path() / "eta.spd", "400 2\n700 3\n");
    write(path.parent_path() / "k.spd", "360 5\n830 7\n");
    const auto input = inspect(path, std::string(R"(
Texture "rough" "float" "scale" "texture tex" "base" "float scale" 2
Texture "base" "float" "constant" "float value" 0.2
Material "conductor" "spectrum eta" "eta.spd" "spectrum k" "k.spd"
    "texture roughness" "rough" "float vroughness" 0.8
)") + std::string(triangle));
    require(input.exit_code() == 0, report(input));
    if (!input.scene)
        throw std::runtime_error("Conductor preflight returned no scene");
    const auto &descriptor = *input.scene->inline_materials.front();
    require(descriptor.checked_spectrum("eta") && descriptor.checked_spectrum("k") &&
                descriptor.checked_spectrum("eta") != descriptor.checked_spectrum("k"),
            "Both conductor optical file dependencies are retained");
    fs::remove(path.parent_path() / "eta.spd");
    fs::remove(path.parent_path() / "k.spd");
    auto output = translate(input, {path.string()});
    if (output.exit_code() != 0 || !output.scene)
        throw std::runtime_error("Use checked optical files after removal");
    const auto &m = material(*output.scene);
    require(!m.eta.extend_endpoints && !m.k.extend_endpoints && optical_spectrum(m.eta, 360) == 0 &&
                optical_spectrum(m.eta, 400) == 2 && optical_spectrum(m.k, 830) == 7,
            "File spectra have zero external support");
    require(m.u_roughness_root != UINT32_MAX && m.v_roughness_root == UINT32_MAX &&
                m.v_roughness == 0.8f,
            "Roughness graph binding preserves axis override");
    const auto value = evaluate_texture_graph(output.scene->texture_graph, m.u_roughness_root,
                                              texture_graph_wire::Context{}, 550);
    require(value && std::abs(*value - 0.4f) < 1e-6f, "Forward scalar graph evaluates roughness");
}
void extended_optics(const fs::path &path) {
    for (const auto body : {R"(Material "conductor" "rgb eta" [1 2 3])",
                            R"(Material "conductor" "blackbody eta" 3000)",
                            R"(Texture "optics" "spectrum" "constant" "rgb value" [.3 .5 .7]
                              Material "conductor" "texture eta" "optics")"}) {
        const auto translated = scene(path, std::string(body) + "\n" + std::string(triangle));
        const auto &m = material(translated);
        const auto value =
            evaluate_texture_graph(translated.texture_graph, m.surface.eta_root, {}, 550);
        require(m.scattering == ScatteringKind::conductor && value && std::isfinite(*value) &&
                    *value > 0,
                "Conductor extended optics retain evaluable continuous spectra");
    }
    const auto shortcut =
        scene(path, std::string(R"(Material "conductor" "rgb reflectance" [.3 .4 .5])") + "\n" +
                        std::string(triangle));
    require(material(shortcut).surface.conductor_reflectance_root != UINT32_MAX,
            "Conductor reflectance shortcut keeps explicit spectral root");
}
void failures(const fs::path &path) {
    for (const auto body : {
             R"(Material "conductor" "rgb eta" [-1 2 3])",
             R"(Material "conductor" "blackbody k" -3000)",
             R"(Texture "optics" "float" "constant"
                Material "conductor" "texture eta" "optics")",
             R"(Material "conductor" "rgb reflectance" [0.3 0.4 0.5] "spectrum k" [360 1 830 1])",
             R"(Material "conductor" "spectrum eta" [360 -1 830 1])",
             R"(Material "conductor" "spectrum eta" [360 0 830 0]
                "spectrum k" [360 0 830 0])",
             R"(Material "conductor" "float roughness" -0.1)",
             R"(MakeNamedMaterial "unused" "string type" "conductor" "float roughness" -1)",
             R"(Texture "r" "spectrum" "constant"
                Material "conductor" "texture roughness" "r")",
             R"(Material "conductor" "spectrum eta" "missing.spd")",
             R"(Material "conductor" "float roughness" 1e100)",
             R"(Material "conductor" "spectrum k" [360 1e100 830 1])",
         }) {
        const auto input = inspect(path, std::string(body) + "\n" + std::string(triangle));
        if (input.exit_code() != 0) {
            require(!input.scene, "Failed preflight withholds checked scene");
            continue;
        }
        const auto output = translate(input, {path.string()});
        require(output.exit_code() != 0 && !output.scene && !output.diagnostics.empty(),
                std::string("Invalid conductor unexpectedly translated: ") + body);
        require(output.diagnostics.front().location.line > 0,
                "Source-located conductor diagnostic");
    }
}
} // namespace
int main() {
    fs::path directory;
    try {
        directory = fs::temp_directory_path() /
                    ("rypath-conductor-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory);
        const auto path = directory / "scene.pbrt";
        defaults_and_axes(path);
        files_and_graphs(path);
        extended_optics(path);
        failures(path);
        fs::remove_all(directory);
        std::cout << "Conductor scene checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
