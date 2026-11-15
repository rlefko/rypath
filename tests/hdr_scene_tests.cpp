// SPDX-License-Identifier: GPL-3.0-only

#include "exr_image.hpp"
#include "pbrt_render.hpp"
#include "texture_graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace rypath;
using namespace rypath::pbrt;
using Bytes = std::vector<std::uint8_t>;
constexpr std::string_view options = R"(Camera "perspective"
Film "rgb" "integer xresolution" 8 "integer yresolution" 8
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 1
Integrator "path"
WorldBegin
)";
constexpr std::string_view triangle = R"(Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1]
    "point2 uv" [0 0 1 0 1 1]
)";
template <typename T> T sampled(std::optional<T> value) {
    if (!value)
        throw std::runtime_error("Expected a finite image lookup result");
    return *value;
}

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
void write(const fs::path &path, const auto &data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
    require(file.good(), "Cannot write HDR scene fixture");
}
void exr(const fs::path &path, unsigned width, unsigned height, std::span<const float> values,
         ExrPrecision precision = ExrPrecision::float32) {
    Bytes bytes;
    const auto diagnostic =
        encode_exr({width, height, values, precision},
                   [&](std::uint64_t offset, std::span<const std::uint8_t> data) {
                       if (offset > std::uint64_t{1024} * 1024 ||
                           data.size() > std::uint64_t{1024} * 1024 - offset)
                           return false;
                       bytes.resize(std::max(bytes.size(), std::size_t(offset) + data.size()));
                       std::copy(data.begin(), data.end(),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                       return true;
                   });
    require(diagnostic.empty(), diagnostic);
    write(path, bytes);
}
void hdr(const fs::path &path, unsigned width = 1, unsigned height = 1) {
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
                               std::to_string(height) + " +X " + std::to_string(width) + "\n";
    Bytes bytes(header.begin(), header.end());
    for (unsigned i = 0; i < width * height; ++i)
        bytes.insert(bytes.end(), {128, 64, 32, 129});
    write(path, bytes);
}
Result checked(const fs::path &path, std::string_view body, const Limits &limits = {}) {
    write(path, std::string(options) + std::string(body));
    return check(path, limits);
}
RenderScene translated(const Result &result, const fs::path &path) {
    require(result.exit_code() == 0 && result.scene.has_value(), report(result));
    auto output = translate(result, {path.string()});
    if (output.exit_code() != 0 || !output.scene) {
        std::string text;
        for (const auto &issue : output.diagnostics)
            text += format_diagnostic(issue) + '\n';
        throw std::runtime_error(text);
    }
    return std::move(*output.scene);
}
void diagnostic(const Result &result, const fs::path &path, std::string_view code,
                std::string_view text = {}) {
    require(result.exit_code() != 0 && !result.scene, "Invalid HDR consumer scene accepted");
    require(std::ranges::any_of(result.document.diagnostics,
                                [&](const Diagnostic &issue) {
                                    return issue.code == code &&
                                           *issue.location.file == path.string() &&
                                           issue.location.line >= 7 && issue.location.column > 0 &&
                                           issue.message.find(text) != std::string::npos;
                                }),
            report(result));
}
void typed_images(const fs::path &directory) {
    const auto path = directory / "typed.pbrt";
    exr(directory / "float.exr", 1, 1, std::array{70000.125f, 2.0f, .5f});
    exr(directory / "half.exr", 1, 1, std::array{2.0f, .5f, .25f}, ExrPrecision::half);
    hdr(directory / "linear.hdr");
    const auto result = checked(path, std::string(R"(
Texture "same" "spectrum" "imagemap" "string filename" "float.exr" "string encoding" "sRGB"
Texture "same" "float" "imagemap" "string filename" "float.exr" "string encoding" "gamma 2.2"
Texture "half" "spectrum" "imagemap" "string filename" "half.exr" "string encoding" "gamma 2"
Texture "rgbe" "spectrum" "imagemap" "string filename" "linear.hdr" "string encoding" "sRGB"
Texture "rgbe" "float" "imagemap" "string filename" "linear.hdr" "string encoding" "gamma 3"
Texture "scaled" "spectrum" "scale" "texture tex" "same" "float scale" 0.5
Texture "scaled" "float" "scale" "texture tex" "same" "float scale" 0.5
Material "diffuse" "texture reflectance" "scaled" "texture displacement" "scaled"
)") + std::string(triangle));
    if (!result.scene)
        throw std::runtime_error(report(result));
    const auto &s = *result.scene;
    require(s.spectrum_image_textures.size() == 3 && s.float_image_textures.size() == 2,
            "HDR typed texture namespaces lost");
    const auto &floating = s.spectrum_image_textures[0];
    require(floating.source == s.float_image_textures[0].source &&
                floating.texture.image == s.float_image_textures[0].texture.image &&
                s.spectrum_image_textures[2].source == s.float_image_textures[1].source &&
                s.spectrum_image_textures[2].texture.image ==
                    s.float_image_textures[1].texture.image,
            "HDR decoded source and linearized pyramid must be shared across typed encodings");
    require(floating.source->pixels->floating_rgb[0] == 70000.125f &&
                floating.texture.image->storage == ImageStorage::linear_float &&
                s.spectrum_image_textures[1].texture.image->storage == ImageStorage::linear_half,
            "Source FLOAT precision and HALF format lost");
    for (const auto &texture : s.spectrum_image_textures)
        require(texture.texture.image->encoding == ImageEncoding::linear,
                "Authored HDR encoding applied");
    require(sampled(image_linear_rgb(*floating.texture.image, .5f, .5f, 0, 0, 0, 0))[0] ==
                    70000.125f &&
                sampled(image_linear_rgb(*s.spectrum_image_textures[2].texture.image, .5f, .5f, 0,
                                         0, 0, 0))[0] == 1,
            "Scene-backed HDR values changed");
    fs::remove(directory / "float.exr");
    fs::remove(directory / "half.exr");
    fs::remove(directory / "linear.hdr");
    const auto scene = translated(result, path);
    const auto &material = scene.materials.at(scene.instance_materials.front());
    require(material.texture_root != UINT32_MAX && material.bump_root != UINT32_MAX,
            "Typed HDR graph roots not translated from retained sources");
}
void native_images(const fs::path &directory) {
    const auto path = directory / "native.pbrt";
    exr(directory / "radiance.exr", 1, 1, std::array{70000.125f, 2.f, 1.f});
    exr(directory / "normal.exr", 1, 1, std::array{.5f, .5f, 1.f}, ExrPrecision::half);
    hdr(directory / "radiance.hdr");
    const auto result = checked(path, std::string(R"(
Texture "radiance" "spectrum" "imagemap" "string filename" "radiance.exr"
Material "diffuse" "string normalmap" "normal.exr"
AreaLightSource "diffuse" "string filename" "radiance.hdr"
)") + std::string(triangle) + R"(
LightSource "infinite" "string filename" "radiance.exr"
)");
    if (!result.scene)
        throw std::runtime_error(report(result));
    const auto &source = *result.scene;
    require(source.lights.front().native_image_source ==
                    source.spectrum_image_textures.front().source &&
                source.lights.front().native_image->native_base &&
                source.area_lights.front()->native_image->native_base &&
                source.inline_materials.back()->native_image->storage == ImageStorage::linear_half,
            "Native normal/light image storage and decoded source sharing lost");
    const auto scene = translated(result, path);
    require(scene.environment_image != UINT32_MAX && scene.area_emissions.size() == 1 &&
                sampled(image_base_texel_linear_rgb(
                    *scene.image_textures.at(scene.environment_image).image, 0, 0))[0] ==
                    70000.125f &&
                sampled(image_base_texel_linear_rgb(
                    *scene.image_textures.at(scene.area_emissions.front().image_texture).image, 0,
                    0))[0] == 1.f,
            "FLOAT EXR environment and RGBE area emission must retain linear native samples");
}
void retained_budget(const fs::path &directory) {
    const auto path = directory / "budget.pbrt";
    exr(directory / "budget.exr", 8, 8, std::vector<float>(std::size_t{8} * 8 * 3, .5f),
        ExrPrecision::half);
    constexpr std::string_view body =
        R"(Texture "a" "spectrum" "imagemap" "string filename" "budget.exr"
Texture "a" "float" "imagemap" "string filename" "budget.exr" "string encoding" "gamma 2"
)";
    // 192 widened source samples + 85 packed HALF texels + four level descriptors.
    constexpr std::size_t retained = 192 * sizeof(float) +
                                     std::size_t{85} * 2 * sizeof(std::uint32_t) +
                                     4 * sizeof(ImageMipLevel);
    const auto exact = checked(path, body, {.image = {.retained_bytes = retained}});
    if (!exact.scene)
        throw std::runtime_error(report(exact));
    require(exact.scene->spectrum_image_textures[0].texture.image ==
                exact.scene->float_image_textures[0].texture.image,
            "Budget must count shared HDR views once");
    diagnostic(checked(path, body, {.image = {.retained_bytes = retained - 1}}), path,
               "limit-exceeded");
}
void consumer_diagnostics(const fs::path &directory) {
    const auto path = directory / "rejected.pbrt";
    write(directory / "corrupt.exr", std::string("broken"));
    write(directory / "unsupported.hdr",
          std::string("#?RADIANCE\nFORMAT=32-bit_rle_xyze\n\n-Y 1 +X 1\n"));
    for (const auto &[filename, code] : std::array{
             std::pair{"missing.exr", "missing-file"}, std::pair{"corrupt.exr", "invalid-image"},
             std::pair{"unsupported.hdr", "unsupported-feature"}}) {
        diagnostic(checked(path, "Texture \"x\" \"float\" \"imagemap\" \"string filename\" \"" +
                                     std::string(filename) + "\"\n"),
                   path, code);
    }
    fs::copy_file(fs::path(__FILE__).parent_path() / "fixtures/exr/gray.exr",
                  directory / "gray.exr", fs::copy_options::overwrite_existing);
    diagnostic(checked(path, R"(LightSource "infinite" "string filename" "gray.exr"
)"),
               path, "invalid-image", "R, G, and B");
    exr(directory / "negative.exr", 1, 1, std::array{-1.0f, 1.0f, 1.0f});
    diagnostic(checked(path, R"(LightSource "infinite" "string filename" "negative.exr"
)"),
               path, "invalid-image", "nonnegative");
    diagnostic(
        checked(path, std::string(R"(AreaLightSource "diffuse" "string filename" "negative.exr"
)") + std::string(triangle)),
        path, "invalid-image", "nonnegative");
    hdr(directory / "wide.hdr", 2, 1);
    diagnostic(checked(path, R"(LightSource "infinite" "string filename" "wide.hdr"
)"),
               path, "invalid-image", "square");
    exr(directory / "zero-normal.exr", 1, 1, std::array{.5f, .5f, .5f});
    const auto zero_normal =
        checked(path, std::string(R"(Material "diffuse" "string normalmap" "zero-normal.exr"
)") + std::string(triangle));
    require(translated(zero_normal, path).materials.back().normal_map != UINT32_MAX,
            "Zero-vector normal map must reach the runtime validation boundary");
}
void signed_geometry(const fs::path &directory) {
    const auto path = directory / "signed.pbrt";
    exr(directory / "height.exr", 1, 1, std::array{-.125f, -.125f, -.125f}, ExrPrecision::half);
    auto result = checked(
        path, std::string(R"(Texture "height" "float" "imagemap" "string filename" "height.exr"
Material "diffuse" "texture displacement" "height"
)") + std::string(triangle));
    const auto scene = translated(result, path);
    const auto &material = scene.materials.at(scene.instance_materials.front());
    require(material.bump_root != UINT32_MAX &&
                image_linear_scalar(*scene.image_textures.front().image, .5f, .5f, 0, 0, 0, 0) ==
                    -.125f,
            "Signed HDR scalar bump samples must be retained");
    write(directory / "triangle.ply", std::string(R"(ply
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
    result = checked(path, R"(Texture "height" "float" "imagemap" "string filename" "height.exr"
Shape "plymesh" "string filename" "triangle.ply" "texture displacement" "height" "float edgelength" 1
)");
    const auto displaced = translated(result, path);
    require(displaced.geometry.meshes.front().positions.front().z == .875f,
            "Signed HDR geometric displacement rejected or clamped");
}
void binary_emission(const fs::path &directory) {
    const auto path = directory / "emitter.pbrt";
    const std::string body =
        R"(Texture "mask" "float" "imagemap" "string filename" "mask.exr" "string filter" "point"
AreaLightSource "diffuse"
Shape "trianglemesh" "point3 P" [0 0 1 1 0 1 1 1 1] "texture alpha" "mask"
)";
    exr(directory / "mask.exr", 2, 1, std::array{0.f, 0.f, 0.f, 1.f, 1.f, 1.f});
    const auto checked_binary = checked(path, body);
    if (!checked_binary.scene)
        throw std::runtime_error(report(checked_binary));
    require(checked_binary.scene->float_image_textures.front().binary_base,
            "FLOAT exact 0/1 emitter mask not certified");
    const auto scene = translated(checked_binary, path);
    require(certify_binary_emitter_mask(scene.texture_graph, scene.instance_alpha_roots.front(),
                                        scene.image_textures),
            "Translated FLOAT binary mask lost certification");
    const auto almost = std::nextafter(1.f, 0.f);
    exr(directory / "mask.exr", 2, 1, std::array{0.f, 0.f, 0.f, almost, almost, almost});
    diagnostic(checked(path, body), path, "unsupported-feature", "binary");
    exr(directory / "mask.exr", 2, 1, std::array{0.f, 0.f, 0.f, .5f, .5f, .5f});
    diagnostic(checked(path, body), path, "unsupported-feature", "binary");
}
void displacement_overflow(const fs::path &directory) {
    const auto path = directory / "overflow.pbrt";
    exr(directory / "overflow.exr", 2, 1,
        std::array{1.2e38f, 1.2e38f, 1.2e38f, -1.2e38f, -1.2e38f, -1.2e38f});
    write(directory / "overflow.ply", std::string(R"(ply
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
0 0 0 .25 .5
1 0 0 .25 .5
0 1 0 .25 .5
3 0 1 2
)"));
    const auto result =
        checked(path, R"(Texture "height" "float" "imagemap" "string filename" "overflow.exr"
    "float scale" 0 "bool invert" true
Shape "plymesh" "string filename" "overflow.ply" "texture displacement" "height" "float edgelength" 2
)");
    if (!result.scene)
        throw std::runtime_error(report(result));
    const auto output = translate(result, {path.string()});
    require(output.exit_code() != 0 && !output.scene,
            "Overflowing displacement must fail before zero scaling and inversion can mask it");
    require(std::ranges::any_of(output.diagnostics,
                                [&](const Diagnostic &issue) {
                                    return issue.severity == Severity::error &&
                                           *issue.location.file == path.string() &&
                                           issue.location.line >= 7;
                                }),
            "Displacement overflow must report a source-located error");
}

} // namespace
int main(int argc, char **argv) {
    try {
        require(argc == 2, "Expected a scratch directory");
        const fs::path directory = argv[1];
        fs::create_directories(directory);
        unsigned failures = 0;
        for (const auto &[name, test] :
             std::array{std::pair{"typed_images", &typed_images},
                        std::pair{"native_images", &native_images},
                        std::pair{"retained_budget", &retained_budget},
                        std::pair{"consumer_diagnostics", &consumer_diagnostics},
                        std::pair{"signed_geometry", &signed_geometry},
                        std::pair{"binary_emission", &binary_emission},
                        std::pair{"displacement_overflow", &displacement_overflow}}) {
            try {
                test(directory);
            } catch (const std::exception &error) {
                std::cerr << name << ": " << error.what() << '\n';
                ++failures;
            }
        }
        return failures ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
