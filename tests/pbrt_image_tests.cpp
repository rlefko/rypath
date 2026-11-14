// SPDX-License-Identifier: GPL-3.0-only

#include "packed_grayscale_fixture.hpp"
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

// Project-authored 2x2 indexed PNG. Indices are [0, 1] and [1, 0].
constexpr auto indexed_png = std::to_array<unsigned char>({
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x00, 0x48,
    0x78, 0x9f, 0x67, 0x00, 0x00, 0x00, 0x06, 0x50, 0x4c, 0x54, 0x45, 0x80, 0x40, 0x20, 0xd0,
    0x9e, 0x1e, 0xdb, 0x52, 0xf5, 0xa7, 0x00, 0x00, 0x00, 0x02, 0x74, 0x52, 0x4e, 0x53, 0x00,
    0x7f, 0xb6, 0x29, 0xa1, 0x95, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c,
    0x63, 0x70, 0x60, 0x68, 0x00, 0x00, 0x01, 0x44, 0x00, 0xc1, 0x3a, 0x7a, 0x1c, 0x56, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
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

RenderScene translate_scene(const Result &result, const fs::path &path,
                            std::string_view expected_filter) {
    auto translated = translate(result, {path.string()});
    for (const auto &issue : translated.diagnostics) {
        std::cerr << format_diagnostic(issue) << '\n';
    }
    if (translated.exit_code() != 0 || !translated.scene) {
        throw std::runtime_error("Imagemap scene must translate");
    }
    constexpr string_view prefix = "Image textures: PNG, UV mapping, mipmapped ";
    const auto provenance = std::ranges::find_if(
        translated.provenance, [prefix](const auto &line) { return line.starts_with(prefix); });
    if (expected_filter.empty()) {
        require(provenance == translated.provenance.end(), "Unused image filters in provenance");
    } else {
        const auto expected = std::string(prefix) + std::string(expected_filter) + " filtering";
        require(provenance != translated.provenance.end() && *provenance == expected,
                "Image provenance did not identify the bound filters");
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
    "string filter" "point"
Texture "paint" "spectrum" "imagemap" "float uscale" 3 "float uscale" 4
    "float udelta" 0.25 "float vdelta" -0.5 "bool invert" true "float scale" 0.75
    "float maxanisotropy" 1.5
Texture "copy" "spectrum" "imagemap" "float uscale" 1 "string filter" "bilinear"
Texture "linear" "spectrum" "imagemap" "string encoding" "linear" "string filter" "trilinear"
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
                paint.image->wrap == ImageWrap::repeat && paint.filter == ImageFilter::point,
            "Last authored value wins and absent fields inherit or default");
    require(definitions.at(0).source == definitions.at(3).source &&
                definitions.at(1).texture.image == paint.image &&
                definitions.at(2).texture.image != paint.image &&
                definitions.at(3).texture.image != paint.image &&
                definitions.at(2).texture.image->encoding == ImageEncoding::linear &&
                definitions.at(3).texture.image->wrap == ImageWrap::clamp,
            "One decoded asset supplies each unique encoding and wrap combination");

    const auto translated = translate_scene(checked, path, "point");
    require(translated.image_textures.at(0).filter == ImageFilter::point &&
                translated.image_textures.at(1).filter == ImageFilter::bilinear &&
                translated.image_textures.at(2).filter == ImageFilter::trilinear &&
                translated.image_textures.at(1).image == paint.image &&
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
        Case{R"(Texture "t" "spectrum" "imagemap" "string filename" "broken16.png")",
             "invalid-image", 1},
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
    for (const auto field :
         {R"("string encoding" "gamma 2.2")", R"("string mapping" "planar" "vector3 v1" [1 0 0])",
          R"("string wrap" "octahedralsphere")"}) {
        const auto checked = check_image(std::string(image) + ' ' + field);
        require(checked.exit_code() == 2 && !checked.document.dependencies.front().visited,
                report(checked));
        diagnostic(checked, "unsupported-feature");
    }
    for (const auto field :
         {R"("string encoding" "gamma nonsense")", R"("string filter" "weird")",
          R"("string filter" "Point")", R"("string filter" "Ewa")", R"("string filter" "")",
          R"("string filter" ["point" "bilinear"])", R"("string mapping" "none")",
          R"("string wrap" "mirror")", R"("float vscale" [1 2])", R"("float udelta" 1e100)"}) {
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
Texture "point" "spectrum" "imagemap" "string filename" "color.png" "string filter" "point"
Texture "trilinear" "spectrum" "imagemap" "string filename" "color.png" "string filter" "trilinear"
Texture "ewa" "spectrum" "imagemap" "string filename" "color.png" "string filter" "ewa"
Texture "cap" "spectrum" "imagemap" "string filename" "color.png" "string filter" "EWA" "float maxanisotropy" 16)",
                                    limits);
    require(shared.exit_code() == 0 && shared.scene, report(shared));
    const auto &definitions = shared.scene->spectrum_image_textures;
    require(definitions.size() == 5 &&
                std::ranges::all_of(definitions,
                                    [&](const auto &definition) {
                                        return definition.texture.image ==
                                               definitions.front().texture.image;
                                    }),
            "All filters and anisotropy caps must share one pyramid at the byte limit");
    require(definitions[3].texture.filter == ImageFilter::ewa &&
                definitions[3].texture.max_anisotropy == 8 &&
                definitions[4].texture.filter == ImageFilter::ewa &&
                definitions[4].texture.max_anisotropy == 16,
            "EWA aliases must preserve the default and authored cap");
    const auto variant = check_image(std::string(image) + R"(
Texture "another" "spectrum" "imagemap" "string filename" "color.png" "string encoding" "linear")",
                                     limits);
    diagnostic(variant, "limit-exceeded");
    const auto second_source = check_image(std::string(image) + R"(
Texture "another" "spectrum" "imagemap" "string filename" "different.png")",
                                           limits);
    diagnostic(second_source, "limit-exceeded");
}

void sixteen_bit_sources(const fs::path &directory) {
    const auto path = directory / "image16.pbrt";
    const auto definition = R"(
Texture "paint" "spectrum" "imagemap" "string filename" "rgb16.png"
Texture "point" "spectrum" "imagemap" "string filename" "rgb16.png" "string filter" "point"
Texture "trilinear" "spectrum" "imagemap" "string filename" "rgb16.png" "string filter" "trilinear"
Texture "ewa" "spectrum" "imagemap" "string filename" "rgb16.png" "string filter" "ewa"
Material "diffuse" "texture reflectance" "paint"
)";
    write(path, std::string(options) + definition + std::string(triangle));
    constexpr std::size_t source_bytes = 6;
    constexpr std::size_t pyramid_bytes = 2 * sizeof(std::uint32_t) + sizeof(ImageMipLevel);
    Limits limits;
    limits.image.retained_bytes = source_bytes + pyramid_bytes;
    const auto checked = check(path, limits);
    if (checked.exit_code() != 0 || !checked.scene) {
        throw std::runtime_error(report(checked));
    }
    const auto &definitions = checked.scene->spectrum_image_textures;
    require(checked.document.dependencies.size() == 1 &&
                checked.document.dependencies.front().visited && definitions.size() == 4,
            "A 16-bit source is decoded once for all filters");
    const auto &first = definitions.front();
    require(first.source->pixels->bit_depth == 16 &&
                first.source->pixels->rgb == std::vector<std::uint8_t>{128, 0, 64, 0, 32, 0} &&
                first.texture.image->storage == ImageStorage::linear_half &&
                first.texture.image->encoding == ImageEncoding::srgb &&
                first.texture.image->texels.size() == 2,
            "Checked sources preserve 16-bit channels and half-float mip storage");
    require(std::ranges::all_of(definitions,
                                [&](const auto &entry) {
                                    return entry.source == first.source &&
                                           entry.texture.image == first.texture.image;
                                }),
            "Filter descriptors must share decoded and mip storage at the exact byte limit");
    --limits.image.retained_bytes;
    const auto below_limit = check(path, limits);
    require(below_limit.exit_code() == 1 && !below_limit.scene, report(below_limit));
    diagnostic(below_limit, "limit-exceeded");

    write(path, std::string(options) + definition + R"(
Texture "linear" "spectrum" "imagemap" "string filename" "rgb16.png" "string encoding" "linear"
Texture "clamp" "spectrum" "imagemap" "string filename" "rgb16.png" "string wrap" "clamp"
)" + std::string(triangle));
    limits.image.retained_bytes = source_bytes + 3 * pyramid_bytes;
    const auto variants = check(path, limits);
    if (variants.exit_code() != 0 || !variants.scene) {
        throw std::runtime_error(report(variants));
    }
    const auto &entries = variants.scene->spectrum_image_textures;
    require(entries.at(4).source == entries.at(0).source &&
                entries.at(5).source == entries.at(0).source &&
                entries.at(4).texture.image != entries.at(0).texture.image &&
                entries.at(5).texture.image != entries.at(0).texture.image &&
                entries.at(4).texture.image->texels ==
                    std::vector<std::uint32_t>{0x34003800, 0x3000},
            "Encoding and wrap variants share source bytes but retain distinct checked pyramids");
    --limits.image.retained_bytes;
    diagnostic(check(path, limits), "limit-exceeded");

    // Translation consumes the immutable checked image; it must not reopen a changed source.
    write(directory / "rgb16.png", "changed after preflight");
    const auto translated = translate_scene(checked, path, "bilinear");
    require(translated.image_textures.front().image == first.texture.image &&
                validate_render_scene(translated).empty(),
            "Translated 16-bit bindings retain their checked image lifetime");
    write_png(directory / "rgb16.png", rgb16_png);
}

void indexed_sources(const fs::path &directory) {
    const auto path = directory / "indexed.pbrt";
    const auto asset = directory / "indexed.png";
    write_png(asset, indexed_png);
    write(directory / "indexed-maps.pbrt", R"(# These two filters share one checked pyramid.
Texture "paint" "spectrum" "imagemap" "string filename" "indexed.png"
Texture "point" "spectrum" "imagemap" "string filename" "indexed.png" "string filter" "point"
Texture "linear" "spectrum" "imagemap" "string filename" "indexed.png" "string encoding" "linear"
)");
    write(path, std::string(options) + R"(Include "indexed-maps.pbrt"
Material "diffuse" "texture reflectance" "paint"
)" + std::string(triangle));

    const std::vector<std::uint8_t> rgb{128, 64, 32, 208, 158, 30, 208, 158, 30, 128, 64, 32};
    constexpr std::size_t pyramid_bytes = 5 * sizeof(std::uint32_t) + 2 * sizeof(ImageMipLevel);
    Limits limits;
    limits.image.retained_bytes = rgb.size() + 2 * pyramid_bytes;
    auto checked = check(path, limits);
    if (checked.exit_code() != 0 || !checked.scene) {
        throw std::runtime_error(report(checked));
    }
    const auto &definitions = checked.scene->spectrum_image_textures;
    require(checked.document.dependencies.size() == 2 &&
                checked.document.dependencies.back().visited && definitions.size() == 3,
            "One Include and one decoded indexed asset supply three descriptors");
    const auto &first = definitions.front();
    require(first.source->pixels->bit_depth == 8 && first.source->pixels->rgb == rgb &&
                first.texture.image->storage == ImageStorage::encoded8 &&
                definitions[1].source == first.source && definitions[2].source == first.source &&
                definitions[1].texture.image == first.texture.image &&
                definitions[2].texture.image != first.texture.image,
            "Indexed expansion shares RGB8 source bytes and encoding-specific pyramids");

    --limits.image.retained_bytes;
    const auto over_budget = check(path, limits);
    require(over_budget.exit_code() == 1 && !over_budget.scene &&
                std::ranges::any_of(over_budget.document.diagnostics,
                                    [](const Diagnostic &issue) {
                                        return issue.code == "limit-exceeded" &&
                                               fs::path(*issue.location.file).filename() ==
                                                   "indexed-maps.pbrt" &&
                                               issue.location.line == 4;
                                    }),
            report(over_budget));

    // Translation retains the checked pyramid after the input and check are gone.
    require(fs::remove(asset), "Could not remove checked indexed PNG");
    auto translated = translate_scene(checked, path, "bilinear");
    require(translated.image_textures.front().image == first.texture.image,
            "Indexed translation must reuse the exact checked pyramid");
    checked = {};
    require(validate_render_scene(translated).empty() &&
                translated.image_textures.front().image->texels.front() == 0x204080,
            "Translated indexed bindings retain expanded RGB8 mip storage");
}

void grayscale_sources(const fs::path &directory) {
    const auto path = directory / "grayscale.pbrt";
    const auto asset = directory / "grayscale.png";
    write_png(asset, test::grayscale2_png);
    write(path, std::string(options) + R"(
Texture "paint" "spectrum" "imagemap" "string filename" "grayscale.png"
Texture "point" "spectrum" "imagemap" "string filename" "grayscale.png" "string filter" "point"
Material "diffuse" "texture reflectance" "paint"
)" + std::string(triangle));
    constexpr std::size_t pyramid_bytes = 11 * sizeof(std::uint32_t) + 3 * sizeof(ImageMipLevel);
    Limits limits;
    limits.image.retained_bytes = test::grayscale2_rgb.size() + pyramid_bytes;
    auto checked = check(path, limits);
    if (checked.exit_code() != 0 || !checked.scene) {
        throw std::runtime_error(report(checked));
    }
    const auto &definitions = checked.scene->spectrum_image_textures;
    require(definitions.size() == 2, "Expected two packed grayscale texture descriptors");
    const auto &first = definitions.front();
    require(checked.document.dependencies.size() == 1 &&
                checked.document.dependencies.front().visited &&
                first.source->pixels->bit_depth == 8 &&
                std::ranges::equal(first.source->pixels->rgb, test::grayscale2_rgb) &&
                first.texture.image->storage == ImageStorage::encoded8 &&
                definitions[1].source == first.source &&
                definitions[1].texture.image == first.texture.image,
            "Packed grayscale must share expanded RGB8 storage at the retained limit");
    --limits.image.retained_bytes;
    const auto over_budget = check(path, limits);
    require(over_budget.exit_code() == 1 && !over_budget.scene, report(over_budget));
    diagnostic(over_budget, "limit-exceeded");
    require(fs::remove(asset), "Could not remove checked grayscale PNG");
    auto translated = translate_scene(checked, path, "bilinear");
    require(translated.image_textures.size() == 2,
            "Expected two translated grayscale texture descriptors");
    require(translated.image_textures.front().image == first.texture.image,
            "Grayscale translation must reuse the checked pyramid");
    checked = {};
    require(validate_render_scene(translated).empty() &&
                translated.image_textures.front().image->texels.at(3) == 0xffffff,
            "Translated grayscale must retain transparent white after source destruction");
}

void filter_provenance(const fs::path &directory) {
    const auto path = directory / "filters.pbrt";
    const auto definitions = std::string(options) + R"(
Texture "trilinear" "spectrum" "imagemap" "string filename" "color.png" "string filter" "trilinear"
Texture "bilinear" "spectrum" "imagemap" "string filename" "color.png"
Texture "point" "spectrum" "imagemap" "string filename" "color.png" "string filter" "point"
Texture "ewa" "spectrum" "imagemap" "string filename" "color.png" "string filter" "EWA"
MakeNamedMaterial "unused" "string type" "diffuse" "texture reflectance" "point"
ObjectBegin "unused"
NamedMaterial "unused"
)" + std::string(triangle) + "ObjectEnd\n";
    constexpr std::array names{string_view{"trilinear"}, string_view{"bilinear"},
                               string_view{"point"}, string_view{"ewa"}};
    constexpr std::array expected{string_view{},
                                  string_view{"trilinear"},
                                  string_view{"bilinear"},
                                  string_view{"bilinear and trilinear"},
                                  string_view{"point"},
                                  string_view{"point and trilinear"},
                                  string_view{"point and bilinear"},
                                  string_view{"point, bilinear and trilinear"},
                                  string_view{"ewa"},
                                  string_view{"trilinear and ewa"},
                                  string_view{"bilinear and ewa"},
                                  string_view{"bilinear, trilinear and ewa"},
                                  string_view{"point and ewa"},
                                  string_view{"point, trilinear and ewa"},
                                  string_view{"point, bilinear and ewa"},
                                  string_view{"point, bilinear, trilinear and ewa"}};
    for (std::size_t subset = 0; subset < expected.size(); ++subset) {
        auto source = definitions + std::string(triangle);
        for (std::size_t i = 0; i < names.size(); ++i) {
            if ((subset & (1U << i)) != 0) {
                source += "Material \"diffuse\" \"texture reflectance\" \"";
                source += names[i];
                source += "\"\n";
                source += triangle;
            }
        }
        write(path, source);
        auto scene = translate_scene(check(path), path, expected[subset]);
        if (subset == expected.size() - 1) {
            require(scene.image_textures[3].image == scene.image_textures[0].image,
                    "Translated descriptors must share their pyramid");
            scene.image_textures[3].max_anisotropy = 0;
            require(validate_render_scene(scene) ==
                        "Image texture EWA maxanisotropy must be in [1, 16]",
                    "A shared pyramid must not bypass EWA cap validation");
            scene.image_textures[3].max_anisotropy = 8;
            // Reject a malformed later descriptor even after its pyramid has been validated.
            // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
            scene.image_textures[2].filter = static_cast<ImageFilter>(99);
            require(validate_render_scene(scene) == "Invalid image texture filter",
                    "A shared pyramid must not bypass descriptor filter validation");
        }
    }
}

void ewa_parameters(const fs::path &directory) {
    const auto path = directory / "ewa-parameters.pbrt";
    const auto attributes = directory / "ewa-attrib.pbrt";
    const auto source = std::string(options) + std::string(image) +
                        "\nAttributeBegin\nInclude \"ewa-attrib.pbrt\"\n";
    constexpr string_view definition =
        R"(Texture "inherited" "spectrum" "imagemap" "string filename" "color.png")";
    const auto write_scene = [&](string_view cap, string_view local) {
        write(attributes,
              std::string(R"(Attribute "texture" "string filter" "EWA" "float maxanisotropy" )") +
                  std::string(cap) + '\n');
        write(path,
              source + std::string(definition) + std::string(local) +
                  "\nAttributeEnd\nMaterial \"diffuse\" \"texture reflectance\" \"inherited\"\n" +
                  std::string(triangle));
    };
    write_scene("1", "");
    auto checked = check(path);
    auto translated = translate_scene(checked, path, "ewa");
    require(translated.image_textures[1].filter == ImageFilter::ewa &&
                translated.image_textures[1].max_anisotropy == 1,
            "EWA cap and filter inherit from a texture attribute");
    write_scene("17", R"( "float maxanisotropy" 4)");
    translated = translate_scene(check(path), path, "ewa");
    require(translated.image_textures[1].max_anisotropy == 4,
            "Local cap overrides an inherited unsupported value");
    for (const auto filter : {"point", "bilinear", "trilinear"}) {
        const auto local = std::string(R"( "string filter" ")") + filter + "\"";
        for (const auto cap : {"-1e100", "1e100"}) {
            write_scene(cap, local);
            translated = translate_scene(check(path), path, filter);
            require(translated.image_textures[1].max_anisotropy == 8,
                    "Inert finite caps must not alter non-EWA descriptors");
        }
    }
    for (const auto cap : {"0.999999999", "16.000000001", "1e300"}) {
        write_scene(cap, "");
        checked = check(path);
        require(checked.exit_code() == 2 && !checked.scene &&
                    checked.document.diagnostics.size() == 1,
                report(checked));
        const auto &issue = checked.document.diagnostics.front();
        const auto column =
            std::string_view(R"(Attribute "texture" "string filter" "EWA" "float maxanisotropy" )")
                .size() +
            1;
        require(issue.code == "unsupported-feature" &&
                    issue.message.find("EWA maxanisotropy") != std::string::npos &&
                    *issue.location.file == attributes.string() && issue.location.line == 1 &&
                    issue.location.column == column,
                "Out-of-range inherited cap must diagnose the effective value token");
    }
    for (const auto cap : {"[1 2]", "1e999"}) {
        write_scene(cap, "");
        checked = check(path);
        require(checked.exit_code() == 1 && !checked.scene, report(checked));
    }
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
    const auto centered = translate_scene(check(path), path, "bilinear");
    require(centered.image_textures.front().filter == ImageFilter::bilinear,
            "Omitted filter must default to bilinear");
    write(path, std::string(settings) + std::string(image) +
                    R"( "string filter" "trilinear"
Material "diffuse" "texture reflectance" "paint"
)" + std::string(triangle));
    const auto trilinear = translate_scene(check(path), path, "trilinear");
    require(trilinear.image_textures.front().filter == ImageFilter::trilinear,
            "Authored trilinear filter did not survive translation");
    write(path, "Translate -100000000 0 0\n" + std::string(settings) + surface +
                    "Translate 100000000 0 0\n" + std::string(triangle));
    const auto translated = translate_scene(check(path), path, "bilinear");
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
        write_png(directory / "broken16.png", std::span{rgb16_png}.first(40));
        write_png(directory / "npot.png", npot_png);
        write_png(directory / "rgb16.png", rgb16_png);
        bindings_and_frames(directory);
        write_png(directory / "color.png", rgb16_png);
        bindings_and_frames(directory);
        write_png(directory / "color.png", rgb_png);
        failures(directory);
        sixteen_bit_sources(directory);
        indexed_sources(directory);
        grayscale_sources(directory);
        filter_provenance(directory);
        ewa_parameters(directory);
        translated_frames(directory);
        frame_limit(directory);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
