// SPDX-License-Identifier: GPL-3.0-only

#include "pbrt_render.hpp"

#include <algorithm>
#include <array>
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

constexpr string_view options = R"(Rotate 90 0 0 1
Camera "perspective"
Film "rgb" "integer xresolution" 16 "integer yresolution" 16
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 4
Integrator "path"
WorldBegin
)";
constexpr string_view triangle = R"(Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1]
)";
constexpr string_view image =
    R"(Texture "paint" "spectrum" "imagemap" "string filename" "color.png")";

// Project-authored 2x2 RGB8 PNG. All four pixels are RGB(128, 64, 32).
constexpr auto rgb_png = std::to_array<unsigned char>({
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xfd,
    0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x68,
    0x70, 0x50, 0x00, 0x22, 0x06, 0x08, 0x05, 0x00, 0x1a, 0x0e, 0x03, 0x81, 0x9c, 0x0e, 0x39,
    0xc7, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
});
constexpr auto npot_png = std::to_array<unsigned char>({
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0x12,
    0x16, 0xf1, 0x4d, 0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x68,
    0x70, 0x50, 0x80, 0x20, 0x06, 0x38, 0x0b, 0x00, 0x36, 0xd4, 0x05, 0x41, 0x61, 0x50, 0x19,
    0x34, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
});
constexpr auto rgb16_png = std::to_array<unsigned char>({
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x02, 0x00, 0x00, 0x00, 0xc0,
    0xe7, 0x8f, 0x9d, 0x00, 0x00, 0x00, 0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x68,
    0x60, 0x70, 0x60, 0x50, 0x60, 0x00, 0x00, 0x04, 0x47, 0x00, 0xe1, 0xac, 0xbc, 0x3e, 0x9d,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
});

void require(bool condition, string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void write(const fs::path &path, string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "Could not write image test fixture");
}

void write_png(const fs::path &path, std::span<const unsigned char> bytes) {
    write(path, {reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}

void diagnostic(const Result &result, string_view code) {
    require(std::ranges::any_of(result.document.diagnostics,
                                [&](const Diagnostic &issue) {
                                    return issue.code == code && issue.location.line >= 2 &&
                                           !issue.location.file->empty();
                                }),
            report(result));
}

RenderScene translate_scene(const Result &result, const fs::path &path) {
    auto translated = translate(result, {path.string()});
    for (const auto &issue : translated.diagnostics) {
        std::cerr << format_diagnostic(issue) << '\n';
    }
    if (translated.exit_code() != 0 || !translated.scene) {
        throw std::runtime_error("Imagemap scene must translate");
    }
    return std::move(translated.scene.value());
}

void bindings_and_frames(const fs::path &directory) {
    const auto path = directory / "world.pbrt";
    write(path, std::string(options) + R"(
AttributeBegin
Attribute "material" "texture reflectance" "paint"
MakeNamedMaterial "textured" "string type" "diffuse"
AttributeEnd
ObjectBegin "member"
NamedMaterial "textured"
Scale 2 3 1
)" + std::string(triangle) +
                    R"(
ObjectEnd
AttributeBegin
Scale -5 7 1
ObjectInstance "member"
AttributeEnd
Material "diffuse" "texture reflectance" "paint" "rgb reflectance" [0.4 0.4 0.4]
)" + std::string(triangle) +
                    R"(
Material "diffuse" "rgb reflectance" [0.4 0.4 0.4] "texture reflectance" "paint"
Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1] "point2 uv" [0 0 0 0 0 0]
Include "nested/maps.pbrt"
)");
    fs::create_directories(directory / "nested");
    write(directory / "nested/maps.pbrt", R"(
Texture "paint" "float" "constant"
AttributeBegin
Attribute "texture" "string filename" "color.png" "float uscale" 8 "float vscale" -2
Texture "paint" "spectrum" "imagemap" "float uscale" 3 "float uscale" 4
    "float udelta" 0.25 "float vdelta" -0.5 "bool invert" true "float scale" 0.75
    "float maxanisotropy" 1.5
Texture "copy" "spectrum" "imagemap" "float uscale" 1
Texture "linear" "spectrum" "imagemap" "string encoding" "linear"
Texture "clamp" "spectrum" "imagemap" "string wrap" "clamp"
AttributeEnd
)");
    const auto checked = check(path);
    if (checked.exit_code() != 0 || !checked.scene || !checked.document.dependencies_complete) {
        throw std::runtime_error(report(checked));
    }
    require(checked.document.dependencies.size() == 2, "One Include and one unique image source");
    const auto &dependency = checked.document.dependencies.at(1);
    require(dependency.kind == DependencyKind::image && dependency.visited &&
                dependency.resolved_path == directory / "color.png" &&
                fs::path(*dependency.location.file).filename() == "maps.pbrt",
            "Inherited filenames resolve from the entry directory and preserve Include provenance");
    const auto &definitions = checked.scene.value().spectrum_image_textures;
    require(definitions.size() == 4 && checked.scene.value().float_constant_textures.size() == 1,
            "Float and spectrum names occupy distinct namespaces");
    const auto &paint = definitions.at(0).texture;
    require(paint.uscale == 4 && paint.vscale == -2 && paint.udelta == 0.25f &&
                paint.vdelta == -0.5f && paint.scale == 0.75f && paint.invert &&
                paint.image->encoding == ImageEncoding::srgb &&
                paint.image->wrap == ImageWrap::repeat,
            "Last authored value wins and absent fields inherit or default");
    require(definitions.at(0).source == definitions.at(3).source &&
                definitions.at(1).texture.image == paint.image &&
                definitions.at(2).texture.image != paint.image &&
                definitions.at(3).texture.image != paint.image &&
                definitions.at(2).texture.image->encoding == ImageEncoding::linear &&
                definitions.at(3).texture.image->wrap == ImageWrap::clamp,
            "One decoded asset supplies each unique encoding and wrap combination");

    const auto translated = translate_scene(checked, path);
    require(translated.image_textures.at(1).image == paint.image &&
                translated.materials.at(translated.instance_materials.at(0)).kind !=
                    SpectralMaterialKind::image &&
                translated.instance_materials.at(1) == translated.instance_materials.at(2),
            "Inline and named forward references bind the same image material");
    const auto &frames = translated.instance_texture_frames;
    require(frames.size() == 3 && frames.at(0).empty() && frames.at(1).size() == 1 &&
                frames.at(2).size() == 1,
            "Direct shapes precede object placements and only image triangles have frames");
    const auto &fallback = frames.at(1).front();
    const auto &instanced = frames.at(2).front();
    require(fallback.dpdu.z == 0 && fallback.dpdv.z == 0 &&
                fallback.dpdu.x * fallback.dpdv.y - fallback.dpdu.y * fallback.dpdv.x != 0,
            "Degenerate UVs use a tangent frame in the triangle plane");
    require(instanced.dpdu.x == -10 && instanced.dpdu.y == 0 && instanced.dpdv.x == 0 &&
                instanced.dpdv.y == 21,
            "Missing UVs use PBRT defaults with shape and placement transforms");
    require(translated.texture_camera.world_from_camera.at(1) == 1 &&
                translated.texture_camera.camera_from_world.at(1) == -1,
            "Translated image camera preserves authored rotation and its inverse");
}

void failures(const fs::path &directory) {
    const auto path = directory / "bad.pbrt";
    const auto check_image = [&](string_view definition, const Limits &limits = {}) {
        write(path, std::string("WorldBegin\n") + std::string(definition));
        return check(path, limits);
    };
    struct Case {
        string_view definition;
        string_view code;
        int status;
    };
    const std::array cases{
        Case{R"(Texture "t" "spectrum" "imagemap")", "missing-parameter", 1},
        Case{R"(Texture "t" "spectrum" "imagemap" "string filename" "")", "missing-parameter", 1},
        Case{R"(Texture "t" "spectrum" "imagemap" "string filename" "missing.png")", "missing-file",
             1},
        Case{R"(Texture "t" "spectrum" "imagemap" "string filename" "broken.png")", "invalid-image",
             1},
        Case{R"(Texture "t" "spectrum" "imagemap" "string filename" "rgb16.png")",
             "unsupported-feature", 2},
        Case{
            R"(Texture "t" "spectrum" "imagemap" "string filename" "npot.png" "string wrap" "black")",
            "unsupported-feature", 2},
        Case{R"(Texture "t" "float" "imagemap" "string filename" "color.png")",
             "unsupported-feature", 2},
    };
    for (const auto &test : cases) {
        const auto checked = check_image(test.definition);
        require(checked.exit_code() == test.status && !checked.scene, report(checked));
        diagnostic(checked, test.code);
        if (test.code == "invalid-image") {
            require(!checked.document.dependencies.front().visited,
                    "Corrupt images stay unvisited");
        }
    }
    for (const auto field : {R"("string encoding" "gamma 2.2")", R"("string filter" "ewa")",
                             R"("string mapping" "planar" "vector3 v1" [1 0 0])",
                             R"("string wrap" "octahedralsphere")"}) {
        const auto checked = check_image(std::string(image) + ' ' + field);
        require(checked.exit_code() == 2 && !checked.document.dependencies.front().visited,
                report(checked));
        diagnostic(checked, "unsupported-feature");
    }
    for (const auto field : {R"("string encoding" "gamma nonsense")", R"("string filter" "weird")",
                             R"("string mapping" "none")", R"("string wrap" "mirror")",
                             R"("float vscale" [1 2])", R"("float udelta" 1e100)"}) {
        const auto checked = check_image(std::string(image) + ' ' + field);
        require(checked.exit_code() == 1 && !checked.document.dependencies.front().visited,
                report(checked));
        diagnostic(checked, "invalid-parameter");
    }
    const auto duplicate = check_image(std::string(image) + R"(
Texture "paint" "spectrum" "constant")");
    diagnostic(duplicate, "duplicate-definition");
    const auto wrong_namespace = check_image(R"(
Material "diffuse" "texture reflectance" "paint"
Texture "paint" "float" "constant")");
    diagnostic(wrong_namespace, "undefined-reference");
    const auto unused = check_image(std::string(image) + R"( "float ignored" 3)");
    diagnostic(unused, "unused-parameter");

    Limits limits;
    limits.image.max_dimension = 1;
    diagnostic(check_image(image, limits), "limit-exceeded");
    limits = {};
    limits.image.scratch_bytes = 1;
    diagnostic(check_image(image, limits), "limit-exceeded");
    limits = {};
    limits.expansions = 1;
    diagnostic(check_image(image, limits), "limit-exceeded");
    limits = {};
    limits.expanded_bytes = 11 + image.size() + rgb_png.size() - 1;
    diagnostic(check_image(image, limits), "limit-exceeded");
    limits = {};
    constexpr std::size_t source_bytes = 12;
    constexpr std::size_t pyramid_bytes = 5 * sizeof(std::uint32_t) + 2 * sizeof(ImageMipLevel);
    limits.image.retained_bytes = source_bytes + pyramid_bytes;
    const auto shared = check_image(std::string(image) + R"(
Texture "another" "spectrum" "imagemap" "string filename" "color.png")",
                                    limits);
    require(shared.exit_code() == 0, report(shared));
    const auto variant = check_image(std::string(image) + R"(
Texture "another" "spectrum" "imagemap" "string filename" "color.png" "string encoding" "linear")",
                                     limits);
    diagnostic(variant, "limit-exceeded");
    const auto second_source = check_image(std::string(image) + R"(
Texture "another" "spectrum" "imagemap" "string filename" "different.png")",
                                           limits);
    diagnostic(second_source, "limit-exceeded");
}

void translated_frames(const fs::path &directory) {
    const auto path = directory / "translated.pbrt";
    constexpr string_view settings = R"(Camera "perspective"
Film "rgb" "integer xresolution" 16 "integer yresolution" 16
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 4
Integrator "path"
WorldBegin
)";
    const std::string surface = std::string(image) + R"(
Material "diffuse" "texture reflectance" "paint"
)";
    write(path, std::string(settings) + surface + std::string(triangle));
    const auto centered = translate_scene(check(path), path);
    write(path, "Translate -100000000 0 0\n" + std::string(settings) + surface +
                    "Translate 100000000 0 0\n" + std::string(triangle));
    const auto translated = translate_scene(check(path), path);
    const auto &a = centered.instance_texture_frames.front().front();
    const auto &b = translated.instance_texture_frames.front().front();
    const auto coordinates = [](GeometryPoint p) { return std::array{p.x, p.y, p.z}; };
    require(coordinates(a.dpdu) == coordinates(b.dpdu) &&
                coordinates(a.dpdv) == coordinates(b.dpdv),
            "Shared camera and shape translation changed texture frames");

    write(path, "Scale 2 1 1\n" + std::string(settings) + surface + std::string(triangle));
    const auto unsupported_camera = translate(check(path), {path.string()});
    require(unsupported_camera.exit_code() == 2 && !unsupported_camera.scene,
            "Nonrigid image camera must be diagnosed before texture frame preparation");
}

void frame_limit(const fs::path &directory) {
    const auto path = directory / "frame-limit.pbrt";
    std::string source = std::string(options) + std::string(image) + R"(
Material "diffuse" "texture reflectance" "paint"
ObjectBegin "member"
)" + std::string(triangle) +
                         "ObjectEnd\n";
    for (int i = 0; i < 8; ++i) {
        source += "ObjectInstance \"member\"\n";
    }
    write(path, source);
    const auto checked = check(path);
    require(checked.exit_code() == 0, report(checked));
    Limits limits;
    limits.total_values = 32;
    const auto translated = translate(checked, {path.string()}, limits);
    require(translated.exit_code() == 1 && !translated.scene &&
                translated.diagnostics.size() == 1 &&
                translated.diagnostics.front().code == "limit-exceeded" &&
                translated.diagnostics.front().message.find("texture frame") != std::string::npos,
            "Repeated image-bound instances must charge texture frame work");
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "Expected an image test work directory");
        const auto directory = fs::absolute(argv[1]);
        fs::create_directories(directory);
        write_png(directory / "color.png", rgb_png);
        write_png(directory / "different.png", rgb_png);
        write_png(directory / "broken.png", std::span{rgb_png}.first(40));
        write_png(directory / "npot.png", npot_png);
        write_png(directory / "rgb16.png", rgb16_png);
        bindings_and_frames(directory);
        failures(directory);
        translated_frames(directory);
        frame_limit(directory);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
