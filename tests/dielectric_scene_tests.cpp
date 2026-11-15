// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "pbrt_spectra.hpp"
#include "texture_graph.hpp"

#include <array>
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
    require(stream.good(), "Cannot write dielectric fixture");
}
Result inspect(const fs::path &path, std::string_view body) {
    write(path, std::string(options) + std::string(body));
    return check(path);
}
RenderScene scene(const fs::path &path, std::string_view body) {
    const auto input = inspect(path, body);
    require(input.exit_code() == 0, report(input));
    auto output = translate(input, {path.string()});
    for (const auto &diagnostic : output.diagnostics) {
        std::cerr << format_diagnostic(diagnostic);
    }
    require(output.exit_code() == 0 && output.scene.has_value(), "Dielectric translation failed");
    if (!output.scene)
        throw std::runtime_error("Dielectric translation did not return a scene");
    return std::move(output.scene.value());
}
const SpectralMaterial &material(const RenderScene &scene) {
    return scene.materials.at(scene.instance_materials.front());
}
void defaults_and_axes(const fs::path &path) {
    const auto glass =
        scene(path, std::string("Material \"dielectric\"\n") + std::string(triangle));
    const auto &m = material(glass);
    require(m.scattering == ScatteringKind::dielectric && m.remap_roughness && m.u_roughness == 0 &&
                m.v_roughness == 0 && m.k.knots.empty(),
            "Smooth dielectric defaults retain no conductor absorption spectrum");
    require(m.eta.extend_endpoints && optical_spectrum(m.eta, 360) == 1.5f &&
                optical_spectrum(m.eta, 830) == 1.5f,
            "Default eta is constant 1.5");
    require(!glass.instance_texture_frames.empty() &&
                !glass.instance_texture_frames.front().empty(),
            "Constant dielectric prepares anisotropic frames");
    auto incomplete_frame = glass;
    incomplete_frame.instance_reverse_orientation.front() = 1;
    incomplete_frame.instance_surface_transforms.clear();
    require(!validate_render_scene(incomplete_frame).empty(),
            "Direct dielectric callers must retain definition/placement orientation");
    for (const float eta : {0.f, 1.f, 2.5f}) {
        const auto constant =
            scene(path, "Material \"dielectric\" \"float eta\" " + std::to_string(eta) +
                            " \"float roughness\" 3\n" + std::string(triangle));
        const auto &value = material(constant);
        require(optical_spectrum(value.eta, 500) == eta && value.u_roughness == 3 &&
                    value.v_roughness == 3,
                "Scalar eta including zero and unity preserves roughness");
    }
    const auto authored = scene(path, std::string(R"(
Attribute "material" "float roughness" 0.7 "spectrum eta" [400 0 700 4]
MakeNamedMaterial "glass" "string type" "dielectric" "float roughness" 0.5
    "float uroughness" 0.25 "bool remaproughness" false
NamedMaterial "glass"
)") + std::string(triangle));
    const auto &a = material(authored);
    require(a.u_roughness == 0.25f && a.v_roughness == 0.5f && !a.remap_roughness,
            "Per-axis roughness independently falls back to effective common roughness");
    require(!a.eta.extend_endpoints && optical_spectrum(a.eta, 360) == 0 &&
                optical_spectrum(a.eta, 830) == 0 && optical_spectrum(a.eta, 550) == 2,
            "Inherited numeric eta retains zero tails and unclamped values");
    const auto named = scene(path, std::string(R"(Material "dielectric"
        "spectrum eta" "glass-BK7")") + "\n" +
                                       std::string(triangle));
    const auto knots = pbrt_named_spectrum_knots("glass-BK7");
    require(material(named).eta.knots.size() == knots.size() &&
                material(named).eta.extend_endpoints &&
                material(named).eta.knots.front().value == knots.front().value,
            "Named eta retains original knots and endpoint extension");
    const auto zero =
        scene(path, std::string(R"(Material "dielectric" "spectrum eta" [400 0 700 0])") + "\n" +
                        std::string(triangle));
    require(optical_spectrum(material(zero).eta, 500) == 0, "All-zero eta remains raw zero");
}
void precedence(const fs::path &path) {
    for (const auto declaration :
         {R"(Material "dielectric" "float eta" 2 "spectrum eta" "missing.spd")",
          R"(Material "dielectric" "spectrum eta" "missing.spd" "float eta" 2)",
          R"(Attribute "material" "float eta" 2
           Material "dielectric" "spectrum eta" "missing.spd")",
          R"(Attribute "material" "float eta" 3 "spectrum eta" "missing.spd"
           Material "dielectric" "float eta" 2)",
          R"(Attribute "material" "spectrum eta" "missing.spd"
           MakeNamedMaterial "g" "string type" "dielectric" "float eta" 2
           NamedMaterial "g")"}) {
        const auto input = inspect(path, std::string(declaration) + "\n" + std::string(triangle));
        require(input.exit_code() == 0, report(input));
        require(input.document.dependencies.empty(), "Overridden spectral eta opens no dependency");
        const auto output = translate(input, {path.string()});
        require(output.scene && output.exit_code() == 0 &&
                    optical_spectrum(material(output.scene.value()).eta, 500) == 2,
                "PBRT float eta takes precedence independently of type order and inheritance");
    }
}
void files_graphs_and_instances(const fs::path &path) {
    write(path.parent_path() / "eta.spd", "400 0\n700 3\n");
    const auto input = inspect(path, std::string(R"(
Texture "rough" "float" "scale" "texture tex" "base" "float scale" 2
Texture "base" "float" "constant" "float value" 0.2
Texture "bump" "float" "constant" "float value" 0
Material "dielectric" "spectrum eta" "eta.spd" "texture displacement" "bump"
    "texture roughness" "rough" "float vroughness" 0.8
ObjectBegin "glass"
)") + std::string(triangle) + R"(ObjectEnd
Translate 1 0 0
ObjectInstance "glass"
)");
    require(input.exit_code() == 0, report(input));
    require(input.scene && input.scene->inline_materials.front()->checked_spectrum("eta"),
            "Checked eta file is owned by the material descriptor");
    fs::remove(path.parent_path() / "eta.spd");
    auto output = translate(input, {path.string()});
    require(output.scene && output.exit_code() == 0, "Retained checked file survives file removal");
    if (!output.scene)
        throw std::runtime_error("Expected a translated scene");
    const auto &m = material(output.scene.value());
    require(!m.eta.extend_endpoints && optical_spectrum(m.eta, 360) == 0 &&
                optical_spectrum(m.eta, 700) == 3,
            "File eta has raw values and zero tails");
    require(m.u_roughness_root != UINT32_MAX && m.v_roughness_root == UINT32_MAX &&
                m.v_roughness == 0.8f && m.bump_root != UINT32_MAX,
            "Roughness graph axis override and bump binding retained");
    const auto value = evaluate_texture_graph(
        output.scene.value().texture_graph, m.u_roughness_root, texture_graph_wire::Context{}, 550);
    require(value && std::abs(*value - 0.4f) < 1e-6f, "Forward roughness graph evaluates");
    require(!output.scene.value().instance_texture_frames.front().empty(),
            "Placed object dielectric receives surface frame");
}
void normal_precedence(const fs::path &path) {
    // Owned 1x1 RGB8 black fixture; its decoded normal is finite and nonzero.
    constexpr auto png = std::to_array<unsigned char>({
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
        0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9c, 0x63, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0xf6, 0x17, 0x38,
        0x55, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    });
    {
        std::ofstream file(path.parent_path() / "normal.png", std::ios::binary);
        file.write(reinterpret_cast<const char *>(png.data()), png.size());
        require(file.good(), "Write owned normal fixture");
    }
    const auto input = inspect(path, std::string(R"(
Texture "bump" "float" "constant" "float value" 0
MakeNamedMaterial "glass" "string type" "dielectric" "texture displacement" "bump"
    "string normalmap" "normal.png"
NamedMaterial "glass"
)") + std::string(triangle));
    require(input.exit_code() == 0, report(input));
    fs::remove(path.parent_path() / "normal.png");
    const auto output = translate(input, {path.string()});
    require(output.scene && output.exit_code() == 0, "Checked normal image survives file removal");
    if (!output.scene)
        throw std::runtime_error("Expected a translated scene");
    const auto &m = material(output.scene.value());
    require(m.normal_map != UINT32_MAX && m.bump_root == UINT32_MAX,
            "Normal map takes precedence over authored displacement for dielectric");
}
void failures(const fs::path &path) {
    for (const auto body :
         {R"(Material "dielectric" "rgb eta" [-1 2 3])",
          R"(Material "dielectric" "blackbody eta" -3000)",
          R"(Texture "e" "spectrum" "constant" Material "dielectric" "texture eta" "e")",
          R"(Material "dielectric" "float eta" -0.1)",
          R"(Material "dielectric" "float eta" -1e-100)",
          R"(Material "dielectric" "float eta" 1e-100)",
          R"(Material "dielectric" "float eta" 1e100)",
          R"(Material "dielectric" "float eta" nan)",
          R"(Material "dielectric" "spectrum eta" [360 -1e-100 830 1])",
          R"(Material "dielectric" "spectrum eta" [360 1e-100 830 1])",
          R"(Material "dielectric" "spectrum eta" [360 1e100 830 1])",
          R"(Material "dielectric" "spectrum eta" [360 1 360.000000001 2])",
          R"(Material "dielectric" "spectrum eta" [360 1])",
          R"(Material "dielectric" "spectrum eta" "missing.spd")",
          R"(Material "dielectric" "float roughness" -0.1)",
          R"(Material "dielectric" "float roughness" -1e-100)",
          R"(Material "dielectric" "float roughness" 1e100)",
          R"(MakeNamedMaterial "unused" "string type" "dielectric" "float eta" -1)",
          R"(Material "dielectric" "spectrum eta" [360 -1 830 2]
           Material "diffuse")",
          R"(Texture "r" "spectrum" "constant" Material "dielectric" "texture roughness" "r")"}) {
        const auto input = inspect(path, std::string(body) + "\n" + std::string(triangle));
        if (input.exit_code() != 0) {
            require(!input.scene, "Failed preflight withholds checked scene");
            continue;
        }
        const auto output = translate(input, {path.string()});
        require(output.exit_code() != 0 && !output.scene && !output.diagnostics.empty(),
                std::string("Invalid dielectric unexpectedly translated: ") + body);
        require(output.diagnostics.front().location.line > 0, "Source-located diagnostic");
    }
    for (const auto supported : {R"(Material "dielectric" "rgb eta" [1 2 3])",
                                 R"(Material "dielectric" "blackbody eta" 3000)"}) {
        const auto translated = scene(path, std::string(supported) + "\n" + std::string(triangle));
        const auto &m = material(translated);
        const auto value =
            evaluate_texture_graph(translated.texture_graph, m.surface.eta_root, {}, 550);
        require(m.scattering == ScatteringKind::dielectric && value && std::isfinite(*value) &&
                    *value > 0,
                "Standalone RGB and blackbody eta retain continuous evaluable spectra");
    }
    write(path.parent_path() / "negative.spd", "360 -0.1\n830 2\n");
    const auto input =
        inspect(path, std::string(R"(Material "dielectric" "spectrum eta" "negative.spd")") + "\n" +
                          std::string(triangle));
    require(input.exit_code() == 0, report(input));
    require(!translate(input, {path.string()}).scene, "Negative file optical values rejected");
}
void depths(const fs::path &path) {
    for (const bool glass : {false, true}) {
        for (const int depth : {113, 114, 127, 128}) {
            std::string header(options);
            header.replace(header.find("independent"), 11, "sobol");
            header.replace(header.find("Integrator \"path\""), 17,
                           "Integrator \"path\" \"integer maxdepth\" " + std::to_string(depth));
            write(path, header +
                            (glass ? "MakeNamedMaterial \"unused\" \"string type\" \"dielectric\"\n"
                                   : "") +
                            std::string(triangle));
            const auto input = check(path);
            require(input.exit_code() == 0, report(input));
            const auto output = translate(input, {path.string()});
            const bool accepted = depth <= (glass ? 113 : 127);
            require(output.scene.has_value() == accepted,
                    "Sobol material-dependent depth boundary");
            if (!accepted) {
                require(!output.diagnostics.empty() &&
                            output.diagnostics.front().location.line == 5,
                        "Sobol depth failure points to integrator");
            }
        }
    }
}
} // namespace
int main() {
    fs::path directory;
    try {
        directory = fs::temp_directory_path() /
                    ("rypath-dielectric-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory);
        const auto path = directory / "scene.pbrt";
        defaults_and_axes(path);
        precedence(path);
        files_graphs_and_instances(path);
        normal_precedence(path);
        failures(path);
        depths(path);
        fs::remove_all(directory);
        std::cout << "Dielectric scene checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
