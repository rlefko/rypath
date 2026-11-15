// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table.hpp"
#include "bsdf_table_fixture.hpp"
#include "pbrt_render.hpp"
#include "rgb_spectrum.hpp"
#include "texture_graph.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace rypath;
using namespace rypath::pbrt;
namespace fs = std::filesystem;
namespace {
void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
constexpr std::string_view options = R"(Camera "perspective"
Film "rgb" "integer xresolution" 4 "integer yresolution" 4
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 1
Integrator "path"
WorldBegin
)";
constexpr std::string_view triangle = R"(Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1]
)";
Result inspect(const fs::path &path, std::string_view body, const Limits &limits = {}) {
    {
        std::ofstream out(path);
        out << options << body;
    }
    return check(path, limits);
}
RenderScene translate_checked(const Result &checked, const fs::path &path) {
    require(checked.exit_code() == 0, report(checked));
    auto output = translate(checked, {path.string()});
    for (const auto &d : output.diagnostics)
        std::cerr << format_diagnostic(d);
    require(output.exit_code() == 0 && output.scene.has_value(), "Surface translation failed");
    if (!output.scene)
        throw std::runtime_error("Expected translated surface scene");
    return std::move(*output.scene);
}
RenderScene make(const fs::path &path, std::string_view body) {
    return translate_checked(inspect(path, std::string(body) + std::string(triangle)), path);
}
const SpectralMaterial &surface(const RenderScene &scene) {
    return scene.materials.at(scene.instance_materials.front());
}
void defaults(const fs::path &path) {
    for (const auto &[name, kind] :
         {std::pair{"thindielectric", ScatteringKind::thin_dielectric},
          {"coateddiffuse", ScatteringKind::coated_diffuse},
          {"coatedconductor", ScatteringKind::coated_conductor},
          {"diffusetransmission", ScatteringKind::diffuse_transmission}}) {
        const auto scene = make(path, std::string("Material \"") + name + "\"\n");
        const auto &m = surface(scene);
        require(m.scattering == kind, "Material family preserved");
        require(!scene.instance_texture_frames.empty() &&
                    !scene.instance_texture_frames.front().empty(),
                "Surface prepares frames");
        if (kind == ScatteringKind::coated_diffuse || kind == ScatteringKind::coated_conductor)
            require(m.surface.max_depth == 10 && m.surface.samples == 1 &&
                        m.surface.thickness == 0.01f,
                    "Layer defaults follow pinned source");
        if (kind == ScatteringKind::thin_dielectric || kind == ScatteringKind::coated_diffuse)
            require(optical_spectrum(m.eta, 550) == 1.5f, "Interface eta defaults to 1.5");
        if (kind == ScatteringKind::coated_conductor)
            require(optical_spectrum(m.surface.interface_eta, 550) == 1.5f && !m.k.knots.empty(),
                    "Coated conductor keeps independent interface/substrate optics");
        if (kind == ScatteringKind::diffuse_transmission)
            require(m.constant == 0.25f && m.surface.transmission_scale == 1 &&
                        m.surface.transmittance_root != UINT32_MAX,
                    "Transmission defaults preserved");
    }
}
void references(const fs::path &path) {
    const auto scene = make(
        path,
        R"(MakeNamedMaterial "outer" "string type" "mix" "string materials" ["inner" "base"] "float amount" .3
MakeNamedMaterial "inner" "string type" "mix" "string materials" ["base" "glass"]
MakeNamedMaterial "base" "string type" "diffuse"
MakeNamedMaterial "glass" "string type" "thindielectric"
NamedMaterial "outer"
)");
    const auto &m = surface(scene);
    require(m.scattering == ScatteringKind::mix && m.surface.mix_amount == .3f,
            "Forward nested mix translated");
    require(scene.materials.at(m.surface.first_material).scattering == ScatteringKind::mix &&
                scene.materials.at(m.surface.second_material).scattering == ScatteringKind::diffuse,
            "Mix child IDs resolve graph");
    for (
        const auto body :
        {R"(MakeNamedMaterial "a" "string type" "mix" "string materials" ["a" "a"])",
         R"(MakeNamedMaterial "a" "string type" "mix" "string materials" ["b" "b"]
MakeNamedMaterial "b" "string type" "mix" "string materials" ["a" "a"])",
         R"(MakeNamedMaterial "a" "string type" "mix" "string materials" ["missing" "missing"])"}) {
        const auto checked = inspect(path, body);
        require(checked.exit_code() == 1 && !checked.scene,
                "Unused cyclic and undefined mixes fail preflight");
        require(!checked.document.diagnostics.empty() &&
                    *checked.document.diagnostics.front().location.file == path.string(),
                "Reference diagnostic source retained");
    }
}
void parameters(const fs::path &path) {
    const auto scene = make(path, R"(Texture "r" "float" "constant" "float value" .2
Texture "metal" "spectrum" "constant" "spectrum value" [360 2 830 3]
Material "coatedconductor" "texture conductor.eta" "metal" "texture conductor.uroughness" "r" "float interface.eta" 1.4 "float interface.roughness" .1 "float thickness" .2 "float g" .4 "rgb albedo" [.1 .2 .3] "integer maxdepth" 12 "integer nsamples" 2
)");
    const auto &m = surface(scene);
    require(m.surface.eta_root != UINT32_MAX && m.u_roughness_root != UINT32_MAX &&
                m.surface.interface_u_roughness == .1f,
            "Typed optics/roughness roots preserved");
    require(m.surface.thickness == .2f && m.surface.g == .4f && m.surface.max_depth == 12 &&
                m.surface.samples == 2,
            "Layer fields retained");
    const auto reflectance = make(path, R"(Material "conductor" "rgb reflectance" [.4 .5 .6]
)");
    require(surface(reflectance).surface.conductor_reflectance_root != UINT32_MAX,
            "Conductor shortcut is explicit root");
    for (
        const auto body :
        {R"(Material "conductor" "rgb reflectance" [.4 .5 .6] "spectrum eta" [360 1 830 1])",
         R"(Material "coatedconductor" "rgb reflectance" [.4 .5 .6] "spectrum conductor.k" [360 1 830 1])",
         R"(Material "coateddiffuse" "integer maxdepth" 1025)",
         R"(Material "coateddiffuse" "integer nsamples" 0)",
         R"(Material "thindielectric" "float roughness" .1)",
         R"(Material "dielectric" "texture eta" "foo")",
         R"(Material "diffusetransmission" "texture scale" "foo")",
         R"(Material "fourier" "string filename" "table.bsdf")"})
        require(inspect(path, body).exit_code() == 1, "Invalid material parameter rejected");
}
void optical_roles(const fs::path &path) {
    for (const auto family : {"dielectric", "thindielectric", "coateddiffuse", "coatedconductor"}) {
        const bool coated = std::string_view(family) == "coatedconductor";
        const auto field = coated ? "interface.eta" : "eta";
        const auto rgb_scene = make(path, std::string("Material \"") + family + "\" \"rgb " +
                                              field + "\" [1.2 1.5 1.8]\n");
        const auto &rgb_material = surface(rgb_scene);
        const auto root =
            coated ? rgb_material.surface.interface_eta_root : rgb_material.surface.eta_root;
        const auto value = evaluate_texture_graph(rgb_scene.texture_graph, root, {}, 550);
        const auto expected = evaluate_unbounded_srgb({1.2f, 1.5f, 1.8f}, 550);
        require(value && expected && std::abs(*value - *expected) < 1e-6f,
                "Standalone interface RGB retains continuous unbounded reconstruction");
        const auto bb_scene = make(path, std::string("Material \"") + family + "\" \"blackbody " +
                                             field + "\" 5000\n");
        const auto &bb_material = surface(bb_scene);
        const auto bb_root =
            coated ? bb_material.surface.interface_eta_root : bb_material.surface.eta_root;
        require(bb_scene.texture_graph.nodes.at(bb_root).kind == texture_graph_wire::blackbody,
                "Standalone blackbody eta stays continuous");
        const auto precedence =
            make(path, std::string("Material \"") + family + "\" \"rgb " + field +
                           "\" [1.2 1.5 1.8] \"float " + field + "\" 1.3\n");
        const auto &p = surface(precedence);
        require((coated ? p.surface.interface_eta_root : p.surface.eta_root) == UINT32_MAX &&
                    optical_spectrum(coated ? p.surface.interface_eta : p.eta, 550) == 1.3f,
                "Float interface eta takes precedence over RGB independent of ordering");
    }

    const auto scene = make(path, R"(Texture "shared" "spectrum" "constant" "rgb value" [.2 .4 .7]
MakeNamedMaterial "paint" "string type" "diffuse" "texture reflectance" "shared"
Material "conductor" "texture eta" "shared" "rgb k" [2 3 4]
)");
    const auto &m = surface(scene);
    const auto eta = evaluate_texture_graph(scene.texture_graph, m.surface.eta_root, {}, 550);
    const auto k = evaluate_texture_graph(scene.texture_graph, m.surface.k_root, {}, 550);
    const auto expected_eta = evaluate_unbounded_srgb({.2f, .4f, .7f}, 550);
    const auto expected_k = evaluate_unbounded_srgb({2, 3, 4}, 550);
    require(eta && expected_eta && std::abs(*eta - *expected_eta) < 1e-6f && k && expected_k &&
                std::abs(*k - *expected_k) < 1e-6f,
            "Optical RGB follows unbounded normalization");
    // One HDR image is shared by a diffuse and conductor consumer. Each graph
    // captures its own spectrum role while preserving the same image payload.
    const auto hdr = path.parent_path() / "optics.hdr";
    {
        std::ofstream out(hdr, std::ios::binary);
        out << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
        const unsigned char pixel[4]{128, 192, 64, 131};
        out.write(reinterpret_cast<const char *>(pixel), 4);
    }
    const auto image_scene = make(
        path,
        R"(Texture "shared" "spectrum" "imagemap" "string filename" "optics.hdr" "string filter" "point"
MakeNamedMaterial "paint" "string type" "diffuse" "texture reflectance" "shared"
Material "conductor" "texture eta" "shared"
)");
    const auto &im = surface(image_scene);
    const auto &node = image_scene.texture_graph.nodes.at(im.surface.eta_root);
    require(node.kind == texture_graph_wire::image && node.payload[1] == 1,
            "Optical HDR image root carries unbounded role");
    bool saw_bounded = false;
    for (const auto &n : image_scene.texture_graph.nodes)
        saw_bounded |= n.kind == texture_graph_wire::image && n.payload[0] == node.payload[0] &&
                       n.payload[1] == 0;
    require(saw_bounded, "Shared image keeps independent bounded albedo root");
    const TextureImageSampler sampler{
        &image_scene.image_textures,
        [](const void *user, texture_graph_wire::Word index, bool spectrum, bool unbounded,
           texture_graph_wire::Mapped2D c, float wavelength) -> std::optional<float> {
            if (!spectrum || !unbounded)
                return std::nullopt;
            const auto &textures = *static_cast<const std::vector<ImageTexture> *>(user);
            const auto &texture = textures.at(index);
            const auto rgb = image_linear_rgb(*texture.image, c.s, 1 - c.t, 0, 0, 0, 0,
                                              texture.filter, texture.max_anisotropy);
            if (!rgb)
                return std::nullopt;
            return evaluate_unbounded_srgb(*rgb, wavelength);
        }};
    texture_graph_wire::Context context{};
    context.u = context.v = .5f;
    const auto sampled = evaluate_texture_graph(image_scene.texture_graph, im.surface.eta_root,
                                                context, 550, sampler);
    const auto expected = evaluate_unbounded_srgb({4, 6, 2}, 550);
    require(sampled && expected && std::abs(*sampled - *expected) < 1e-5f && *sampled > 1,
            "HDR optical image preserves values greater than one");
}
void bounded_surface_consumers(const fs::path &path) {
    const auto expected = reconstruct_srgb({.2f, .4f, .7f});
    if (!expected)
        throw std::runtime_error("Test RGB reconstruction failed");
    for (const auto family : {"diffusetransmission", "coateddiffuse"}) {
        const auto field =
            std::string_view(family) == "diffusetransmission" ? "transmittance" : "albedo";
        const auto scene =
            make(path, std::string(R"(Texture "shared" "spectrum" "constant" "rgb value" [.2 .4 .7]
MakeNamedMaterial "metal" "string type" "conductor" "texture eta" "shared"
Material ")") + family + "\" \"texture " +
                           field + "\" \"shared\"\n");
        const auto &m = surface(scene);
        const auto root = std::string_view(family) == "diffusetransmission"
                              ? m.surface.transmittance_root
                              : m.surface.albedo_root;
        for (float wavelength : {420.f, 550.f, 700.f}) {
            const auto value = evaluate_texture_graph(scene.texture_graph, root, {}, wavelength);
            require(value && std::abs(*value - evaluate_srgb(*expected, wavelength)) < 1e-6f,
                    "Surface albedo/transmittance use Albedo RGB even when shared with optical "
                    "textures");
        }
    }
    {
        std::ofstream out(path.parent_path() / "unbounded.spd");
        out << "360 2 830 2\n";
    }
    for (
        const auto authored :
        {R"(Material "diffusetransmission" "spectrum reflectance" "unbounded.spd" "spectrum transmittance" "unbounded.spd" "float scale" .25)",
         R"(Texture "raw" "spectrum" "constant" "spectrum value" "unbounded.spd"
Material "diffusetransmission" "texture reflectance" "raw" "texture transmittance" "raw" "float scale" .25)"}) {
        const auto scene = make(path, std::string(authored) + "\n");
        const auto &m = surface(scene);
        const auto r = evaluate_texture_graph(scene.texture_graph, m.texture_root, {}, 550);
        const auto t =
            evaluate_texture_graph(scene.texture_graph, m.surface.transmittance_root, {}, 550);
        require(r && t && *r == 2 && *t == 2 && m.surface.transmission_scale == .25f,
                "Diffuse transmission preserves raw curves for scale-before-clamp consumption");
    }
}
void tables(const fs::path &path) {
    const auto table_path = path.parent_path() / "tiny.bsdf";
    table_fixture::write(table_path, table_fixture::fourier());
    const auto checked =
        inspect(path, std::string("Material \"fourier\" \"string bsdffile\" \"tiny.bsdf\"\n") +
                          std::string(triangle));
    require(checked.exit_code() == 0, report(checked));
    if (!checked.scene)
        throw std::runtime_error("Expected checked table scene");
    require(checked.scene->inline_materials.front()->bsdf_table != nullptr,
            "Preflight retains checked table");
    fs::remove(table_path);
    const auto scene = translate_checked(checked, path);
    require(surface(scene).table == checked.scene->inline_materials.front()->bsdf_table &&
                surface(scene).scattering == ScatteringKind::fourier,
            "Translation reuses snapshot without reopening file");
    require(
        inspect(path, R"(Material "measured" "string filename" "missing.tensor")").exit_code() == 1,
        "Missing table rejected");
    table_fixture::write(table_path, table_fixture::fourier());
    Limits limits;
    limits.bsdf_working_bytes = 1;
    require(
        inspect(path, R"(Material "fourier" "string bsdffile" "tiny.bsdf")", limits).exit_code() ==
            1,
        "Dedicated table working budget enforced");
}
} // namespace
int main() {
    fs::path directory;
    try {
        directory = fs::temp_directory_path() /
                    ("rypath-surface-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory);
        const auto path = directory / "scene.pbrt";
        defaults(path);
        references(path);
        parameters(path);
        optical_roles(path);
        bounded_surface_consumers(path);
        tables(path);
        fs::remove_all(directory);
        std::cout << "Surface material scene checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
