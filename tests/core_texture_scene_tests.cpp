// SPDX-License-Identifier: GPL-3.0-only

#include "packed_grayscale_fixture.hpp"
#include "pbrt_render.hpp"
#include "texture_graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace rypath;
using namespace rypath::pbrt;
using std::string_view;
namespace graph = rypath::texture_graph_wire;

constexpr string_view options = R"(Camera "perspective"
Film "rgb" "integer xresolution" 8 "integer yresolution" 8
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 4
Integrator "path"
WorldBegin
)";

constexpr string_view triangle = R"(Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1]
    "point2 uv" [0 0 1 0 1 1]
)";

// 4x4 RGB8 black/white checker. Positive-illuminance normalization sees interior texels.
constexpr auto binary_png = std::to_array<unsigned char>(
    {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
     0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x02, 0x00, 0x00, 0x00, 0x26, 0x93, 0x09,
     0x29, 0x00, 0x00, 0x00, 0x16, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0xf8,
     0xff, 0xff, 0x3f, 0x94, 0x44, 0xb0, 0x20, 0x00, 0xbb, 0x0c, 0x00, 0x6d, 0xe2, 0x17, 0xe9, 0x45,
     0x63, 0xc9, 0xbb, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82});

constexpr auto black_png = std::to_array<unsigned char>({
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
    0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x78,
    0x9c, 0x63, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0xf6, 0x17, 0x38,
    0x55, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
});

void require(bool pass, string_view message) {
    if (!pass) {
        throw std::runtime_error(std::string(message));
    }
}

void write(const fs::path &path, const auto &data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
    require(file.good(), "Cannot write test input");
}

Result checked(const fs::path &path, string_view body) {
    write(path, std::string(options) + std::string(body));
    return check(path);
}

RenderScene translated(const Result &result, const fs::path &path) {
    require(result.exit_code() == 0 && result.scene.has_value(), report(result));
    auto rendered = translate(result, {path.string()});
    if (!rendered.scene || rendered.exit_code() != 0) {
        for (const auto &diagnostic : rendered.diagnostics) {
            std::cerr << format_diagnostic(diagnostic);
        }
        throw std::runtime_error("Scene translation failed");
    }
    return std::move(rendered.scene.value());
}

void graph_and_spectra(const fs::path &directory) {
    const auto path = directory / "graph.pbrt";
    write(directory / "a.spd", string_view("360 0.2 830 0.2\n"));
    write(directory / "b.spd", string_view("360 2 830 2\n"));
    const auto inspected = checked(path, std::string(R"(
Texture "result" "spectrum" "mix" "texture tex1" "curves" "texture tex2" "raw"
    "texture amount" "half"
Texture "curves" "spectrum" "bilerp" "spectrum v00" "a.spd" "spectrum v01" "b.spd"
    "spectrum v10" "a.spd" "spectrum v11" "b.spd"
Texture "raw" "spectrum" "constant" "spectrum value" "b.spd"
Texture "half" "float" "constant" "float value" 0.25
Texture "scaled" "float" "scale" "texture tex" "half" "float scale" 2
Texture "signed" "float" "directionmix" "texture tex2" "scaled" "vector3 dir" [0 3 0]
Texture "checker" "spectrum" "checkerboard" "integer dimension" 3 "texture tex2" "result"
Texture "dots" "float" "dots" "texture inside" "signed" "string mapping" "spherical"
Texture "noise" "float" "fbm" "integer octaves" 4
Texture "creases" "float" "wrinkled"
Texture "gusts" "float" "windy"
Texture "marble" "spectrum" "marble" "float variation" 0.3
Material "diffuse" "texture reflectance" "result"
)") + std::string(triangle));
    if (!inspected.scene) {
        throw std::runtime_error(report(inspected));
    }
    const auto &curves = inspected.scene.value().procedural_textures.at(1);
    require(curves.checked_spectrum("v00") != curves.checked_spectrum("v01") &&
                curves.checked_spectrum("v00") == curves.checked_spectrum("v10") &&
                curves.checked_spectrum("v01") == curves.checked_spectrum("v11"),
            "Every effective bilerp operand retains its own checked asset");
    fs::remove(directory / "a.spd");
    fs::remove(directory / "b.spd");
    const auto scene = translated(inspected, path);
    const auto &material = scene.materials.at(scene.instance_materials.front());
    graph::Context context{};
    context.u = 0.75f;
    context.v = 0.25f;
    const auto value =
        evaluate_texture_graph(scene.texture_graph, material.texture_root, context, 550);
    require(value && std::abs(*value - 0.9875f) < 1e-6f,
            "Bilerp and mixed spectrum operands reproduce the selected graph value");
}

void image_consumers(const fs::path &directory) {
    const auto path = directory / "images.pbrt";
    write(directory / "mask.png", binary_png);
    write(directory / "zero.png", black_png);
    write(directory / "gray.png", test::grayscale2_png);
    const auto inspected = checked(path, R"(
Texture "mask" "float" "imagemap" "string filename" "mask.png" "string filter" "point"
    "string wrap" "black" "bool invert" true
Texture "mask-alias" "float" "scale" "texture tex" "mask"
Texture "zero" "float" "imagemap" "string filename" "zero.png" "string filter" "point"
Texture "bump" "float" "imagemap" "string filename" "gray.png" "string encoding" "gamma 2.2"
    "string mapping" "cylindrical"
Texture "paint" "spectrum" "imagemap" "string filename" "mask.png" "string mapping" "planar"
    "vector3 v1" [1 0 0] "vector3 v2" [0 1 0]
Material "diffuse" "texture reflectance" "paint" "texture displacement" "bump"
Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1] "float alpha" 0.5
Material "diffuse" "texture displacement" "bump" "string normalmap" "mask.png"
AreaLightSource "diffuse" "string filename" "mask.png" "bool twosided" true
Shape "trianglemesh" "point3 P" [0 0 2 1 0 2 1 1 2] "texture alpha" "mask-alias"
    "vector3 S" [0 1 0 0 1 0 0 1 0]
Shape "trianglemesh" "point3 P" [0 0 3 1 0 3 1 1 3] "float alpha" 0
Shape "trianglemesh" "point3 P" [0 0 4 1 0 4 1 1 4] "texture alpha" "zero"
Rotate 90 1 0 0
LightSource "infinite" "string filename" "mask.png" "float illuminance" 1
)");
    if (!inspected.scene) {
        throw std::runtime_error(report(inspected));
    }
    const auto &checked_scene = inspected.scene.value();
    require(checked_scene.float_image_textures.at(0).binary_base &&
                checked_scene.float_image_textures.at(1).binary_base &&
                checked_scene.lights.front().native_image_source ==
                    checked_scene.float_image_textures.at(0).source &&
                checked_scene.area_lights.front()->native_image ==
                    checked_scene.lights.front().native_image,
            "Binary certification and shared native and mip views survive preflight");
    fs::remove(directory / "mask.png");
    fs::remove(directory / "zero.png");
    const auto scene = translated(inspected, path);
    require(scene.environment_image != graph::no_texture && scene.environment_image_scale > 0 &&
                scene.environment_image == scene.area_emissions.front().image_texture &&
                scene.environment_texture_from_world.values[6] == 1,
            "Area and equal-area environment consumers share the native image and retain pose");
    const auto &fractional = scene.texture_graph.nodes.at(scene.instance_alpha_roots.at(0));
    require(fractional.kind == graph::constant_node && fractional.values[0] == 0.5f &&
                scene.instance_invisible_emitters == std::vector<std::uint32_t>{0, 0, 1, 0},
            "Fractional coverage is separate from binary emitter and constant-zero special cases");
    const auto &emitter = scene.materials.at(scene.instance_materials.at(1));
    require(emitter.normal_map != graph::no_texture && emitter.bump_root == graph::no_texture &&
                scene.geometry.meshes.at(1).tangents.at(0).y == 1,
            "The authored tangent and normal map take priority over the bump binding");
    const auto &surface = scene.materials.at(scene.instance_materials.at(0));
    require(surface.bump_root != graph::no_texture && surface.texture_root != graph::no_texture &&
                scene.texture_graph.nodes.at(surface.bump_root).mapping.kind ==
                    graph::cylindrical &&
                scene.texture_graph.nodes.at(surface.texture_root).mapping.kind == graph::planar,
            "Mapped float and spectral consumers carry their declarations to the renderer");
}

void displacement(const fs::path &directory) {
    write(directory / "triangle.ply", string_view(R"(ply
format ascii 1.0
element vertex 3
property float x
property float y
property float z
property float u
property float v
element face 1
property list uchar int vertex_indices
end_header
0 0 1 0 0
1 0 1 1 0
0 1 1 0 1
3 0 1 2
)"));
    const auto path = directory / "displaced.pbrt";
    const std::string authored =
        "Option \"displacementEdgeScale\" 0.5\n" + std::string(options) + R"(
Texture "height" "float" "constant" "float value" 0.125
ObjectBegin "instance"
Shape "plymesh" "string filename" "triangle.ply" "texture displacement" "height"
    "float edgelength" 1
ObjectEnd
ObjectInstance "instance"
Translate 2 0 0
ObjectInstance "instance"
)";
    write(path, authored);
    const auto inspected = check(path);
    const auto scene = translated(inspected, path);
    const auto &mesh = scene.geometry.meshes.front();
    require(scene.geometry.meshes.size() == 1 && scene.geometry.instances.size() == 2 &&
                mesh.triangles.size() > 1 && mesh.positions.front().z == 1.125f,
            "One refined and displaced object mesh is shared by both placements");
}

void diagnostics(const fs::path &directory) {
    const auto path = directory / "invalid.pbrt";
    struct Rejection {
        string_view body;
        string_view code;
        std::size_t line;
    };
    constexpr std::array cases{
        Rejection{.body = R"(Texture "a" "float" "scale" "texture tex" "b"
Texture "b" "float" "mix" "texture tex2" "a"
)",
                  .code = "cyclic-texture",
                  .line = 8},
        Rejection{.body = R"(Texture "a" "spectrum" "constant"
Texture "b" "spectrum" "mix" "texture amount" "a"
)",
                  .code = "undefined-reference",
                  .line = 8},
        Rejection{.body =
                      R"(Texture "a" "float" "bilerp" "string mapping" "spherical" "float uscale" 2
)",
                  .code = "unused-parameter",
                  .line = 7},
        Rejection{
            .body =
                R"(Texture "a" "float" "imagemap" "string filename" "gray.png" "string filter" "point"
AreaLightSource "diffuse"
Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1] "texture alpha" "a"
)",
            .code = "unsupported-feature",
            .line = 9},
        Rejection{.body = R"(Material "diffuse" "string normalmap" "gray.png"
)",
                  .code = "invalid-image",
                  .line = 7},
    };
    for (const auto &test : cases) {
        const auto inspected = checked(path, test.body);
        require(!inspected.scene.has_value() && inspected.exit_code() != 0, report(inspected));
        const auto matches = [&](const Diagnostic &diagnostic) {
            return diagnostic.code == test.code && diagnostic.location.line == test.line &&
                   *diagnostic.location.file == path.string();
        };
        require(std::ranges::any_of(inspected.document.diagnostics, matches), report(inspected));
    }
}

void spectral_color_context(const fs::path &directory) {
    const auto path = directory / "color-context.pbrt";
    constexpr string_view captured = R"(ColorSpace "rec2020"
Attribute "texture" "rgb tex1" [0.25 0.25 0.25] "rgb v00" [0.25 0.25 0.25]
ColorSpace "srgb"
)";
    const auto mix = checked(path, std::string(captured) + R"(Texture "mix" "spectrum" "mix"
Material "diffuse" "texture reflectance" "mix"
)" + std::string(triangle));
    const auto scene = translated(mix, path);
    require(scene.materials.at(scene.instance_materials.front()).texture_root != graph::no_texture,
            "Inline RGB graph texture operands use the declaration's sRGB");

    const auto bilerp =
        checked(path, std::string(captured) + R"(Texture "bilerp" "spectrum" "bilerp"
)" + std::string(triangle));
    const auto render = translate(bilerp, {path.string()});
    require(bilerp.exit_code() == 0 && render.exit_code() == 2 && render.diagnostics.size() == 1 &&
                render.diagnostics.front().location.line == 8,
            "Bilerp's standalone RGB spectrum retains its captured unsupported color space");
}

void exclusive_branch_budget(const fs::path &directory) {
    const auto path = directory / "dots-budget.pbrt";
    std::string source = "Texture \"d0\" \"float\" \"constant\"\n";
    for (unsigned i = 1; i <= 13; ++i) {
        const auto name = "d" + std::to_string(i);
        const auto shared = "d" + std::to_string(i - 1);
        source += "Texture \"" + name + "\" \"float\" \"dots\" \"texture outside\" \"" + shared +
                  "\" \"texture inside\" \"" + shared + "\"\n";
    }
    source += "Material \"diffuse\" \"texture displacement\" \"d13\"\n";
    source += triangle;
    const auto result = translated(checked(path, source), path);
    const auto &material = result.materials.at(result.instance_materials.front());
    const auto value = evaluate_texture_graph(result.texture_graph, material.bump_root, {}, 550);
    require(value == 1, "A dots chain visits one branch per node despite shared children");
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "Expected a scratch directory");
        const fs::path directory = argv[1];
        fs::create_directories(directory);
        graph_and_spectra(directory);
        image_consumers(directory);
        displacement(directory);
        diagnostics(directory);
        spectral_color_context(directory);
        exclusive_branch_budget(directory);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
