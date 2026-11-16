// SPDX-License-Identifier: GPL-3.0-only
#include "image_texture.hpp"
#include "pbrt.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
ImagePyramid prepared(const DecodedImage &source) {
    auto result = build_goniometric_image_base(source);
    if (!result.value)
        throw std::runtime_error(result.diagnostic);
    require(result.value->source_channels == 1 && result.value->native_base &&
                result.value->levels.size() == 1 && result.value->wrap == ImageWrap::clamp,
            "native scalar map contract");
    return std::move(*result.value);
}
DecodedImage floats(ImageSampleFormat format, std::array<float, 3> rgb) {
    DecodedImage result;
    result.width = result.height = 1;
    result.sample_format = format;
    result.file_format = ImageFileFormat::exr;
    result.floating_rgb.assign(rgb.begin(), rgb.end());
    return result;
}
void storage() {
    DecodedImage gray;
    gray.width = gray.height = 16;
    gray.source_channels = 1;
    for (unsigned i = 0; i < 256; ++i)
        gray.rgb.insert(gray.rgb.end(), 3, static_cast<std::uint8_t>(i));
    const auto gray8 = prepared(gray);
    for (unsigned i = 0; i < 256; ++i)
        require(gray8.texels[i] == i * 0x010101u, "gray byte codes must survive unchanged");
    DecodedImage rgb{.width = 1, .height = 1, .rgb = {255, 0, 0}};
    const auto rgb8 = prepared(rgb);
    require(rgb8.storage == ImageStorage::encoded8 && rgb8.texels[0] == 0x9c9c9cu,
            "RGB8 decoded mean must be re-encoded to byte 156");
    require(image_base_texel_linear_rgb(rgb8, 0, 0)->at(0) ==
                image_transfer_table(ImageEncoding::srgb)[156],
            "sample rounded storage, not unrounded mean");
    rgb.source_channels = 4;
    rgb.alpha = {0};
    require(prepared(rgb).texels == rgb8.texels, "byte alpha must be ignored");
    rgb.source_channels = 3;
    rgb.alpha.clear();
    rgb.sample_format = ImageSampleFormat::unorm16;
    rgb.rgb = {255, 255, 0, 0, 0, 0};
    const auto rgb16 = prepared(rgb);
    require(rgb16.storage == ImageStorage::linear_half &&
                rgb16.texels == std::vector<std::uint32_t>{0x35553555u, 0x3555u},
            "RGB16 mean must round to binary16 one-third");
    rgb.source_channels = 1;
    rgb.rgb = {128, 0, 128, 0, 128, 0};
    const auto gray16 = prepared(rgb);
    const auto native = build_image_base(rgb, ImageEncoding::srgb, ImageWrap::clamp);
    require(native.value && gray16.texels == native.value->texels,
            "gray16 must retain original decoded half");
    // Values near a rounding boundary distinguish per-channel half rounding
    // before averaging from averaging continuous PNG16 transfers directly.
    rgb.source_channels = 3;
    rgb.rgb = {0x80, 0x01, 0x40, 0x02, 0x20, 0x03};
    const auto rounded = prepared(rgb);
    require(rounded.texels == std::vector<std::uint32_t>{0x2df62df6u, 0x2df6u},
            "PNG16 must decode to half before computing scalar");
    auto half = floats(ImageSampleFormat::half, {1, .5f, .25f});
    require(prepared(half).texels == std::vector<std::uint32_t>{0x38ab38abu, 0x38abu},
            "HALF RGB mean rounds back to HALF");
    half.source_channels = 4;
    half.floating_alpha = {-1};
    require(prepared(half).texels ==
                prepared(floats(ImageSampleFormat::half, {1, .5f, .25f})).texels,
            "floating alpha must not enter scalar");
    auto full = floats(ImageSampleFormat::float32, {1.00000011920928955078125f, .5f, .25f});
    const float mean = (full.floating_rgb[0] + full.floating_rgb[1] + full.floating_rgb[2]) / 3;
    const auto float_map = prepared(full);
    require(float_map.storage == ImageStorage::linear_float &&
                float_map.texels[0] == std::bit_cast<std::uint32_t>(mean) &&
                float_map.texels[1] == float_map.texels[0] &&
                float_map.texels[2] == float_map.texels[0],
            "FLOAT mean precision");
    full.source_channels = 1;
    full.floating_rgb = {0x1.000002p0f, 0x1.000002p0f, 0x1.000002p0f};
    require(prepared(full).texels[0] == std::bit_cast<std::uint32_t>(full.floating_rgb[0]),
            "gray FLOAT must retain source value");
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
    std::vector<std::uint8_t> hdr(header.begin(), header.end());
    hdr.insert(hdr.end(), {128, 64, 32, 129});
    const auto decoded = decode_hdr(hdr);
    require(decoded.value && image_base_texel_linear_rgb(prepared(*decoded.value), 0, 0)->at(0) ==
                                 (1.0f + .5f + .25f) / 3,
            "HDR scalar input");
}
void invalid() {
    auto source = floats(ImageSampleFormat::float32, {1, 2, 3});
    for (float value :
         {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        source.floating_rgb[0] = value;
        require(!build_goniometric_image_base(source).value, "invalid RGB source accepted");
    }
    source.floating_rgb.assign(3, std::numeric_limits<float>::max());
    require(!build_goniometric_image_base(source).value, "actual float sum overflow accepted");
    source.floating_rgb.assign(3, std::numeric_limits<float>::denorm_min());
    require(!build_goniometric_image_base(source).value, "subnormal prepared scalar accepted");
    source.source_channels = 1;
    require(!build_goniometric_image_base(source).value, "gray subnormal accepted");
    source.floating_rgb.assign(3, -1);
    require(!build_goniometric_image_base(source).value, "negative gray accepted");
    source.floating_rgb.assign(3, 1);
    require(build_goniometric_image_base(source, {.max_dimension = 0}).limit_exceeded &&
                build_goniometric_image_base(source, {.retained_bytes = 27}).limit_exceeded,
            "native image limits");
    require(build_goniometric_image_base(source, {.retained_bytes = 28, .scratch_bytes = 0})
                .value.has_value(),
            "native scalar preparation should not allocate pixel scratch");
    source.height = 2;
    require(!build_goniometric_image_base(source).value, "non-square accepted");
    source.height = 1;
    source.floating_rgb.pop_back();
    require(!build_goniometric_image_base(source).value, "malformed channels accepted");
}
void write(const fs::path &path, const std::string &contents) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(static_cast<bool>(stream), "fixture write failed");
}
void preflight(const fs::path &directory) {
    fs::create_directories(directory);
    const auto path = directory / "scene.pbrt";
    std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
    hdr.append("\200\100\040\201", 4);
    write(directory / "map.hdr", hdr);
    const std::string gonio = "LightSource \"goniometric\" \"string filename\" \"map.hdr\"\n";
    const auto run = [&](const std::string &body, const pbrt::Limits &limits = {}) {
        write(path, "WorldBegin\n" + body);
        return pbrt::check(path, limits);
    };
    const auto result =
        run("LightSource \"infinite\" \"string filename\" \"map.hdr\"\n" + gonio + gonio);
    if (result.exit_code() != 0 || !result.scene || result.scene->lights.size() != 3)
        throw std::runtime_error("goniometric preflight failed");
    const auto &lights = result.scene.value().lights;
    require(lights[0].native_image_source == lights[1].native_image_source &&
                lights[0].native_image != lights[1].native_image &&
                lights[1].native_image == lights[2].native_image,
            "decoded source sharing and scalar cache separation");
    require(lights[0].native_image->source_channels == 3 &&
                lights[1].native_image->source_channels == 1,
            "native RGB map mutated by scalar preparation");
    pbrt::Limits limits;
    limits.image.retained_bytes = 12 + 2 * (16 + sizeof(ImageMipLevel));
    require(
        run("LightSource \"infinite\" \"string filename\" \"map.hdr\"\n" + gonio + gonio, limits)
                .exit_code() == 0,
        "shared scalar cache charged twice");
    --limits.image.retained_bytes;
    require(run("LightSource \"infinite\" \"string filename\" \"map.hdr\"\n" + gonio, limits)
                    .exit_code() != 0,
            "scalar preparation storage not charged");
    for (const auto &body :
         {std::string("LightSource \"goniometric\"\n"),
          std::string("LightSource \"goniometric\" \"string filename\" \"\"\n"),
          gonio + "LightSource \"goniometric\" \"string filename\" \"map.hdr\" \"point3 from\" [0 "
                  "0 0]\n",
          gonio + "LightSource \"goniometric\" \"string filename\" \"missing.png\"\n"})
        require(run(body).exit_code() != 0, "invalid goniometric declaration accepted");
    std::string many;
    for (unsigned i = 0; i < 64; ++i)
        many += i % 2 ? gonio : "LightSource \"point\"\n";
    require(run(many).exit_code() == 0 && run(many + gonio).exit_code() != 0,
            "combined point/spot/goniometric bound");
    const auto medium = run("MakeNamedMedium \"air\" \"string type\" \"homogeneous\"\n"
                            "MediumInterface \"\" \"air\"\n" +
                            gonio);
    require(medium.exit_code() == 0 && medium.scene &&
                medium.scene->lights.front().outside_medium == "air",
            "goniometric endpoint medium must be retained");
    write(directory / "intensity.spd", "400 1\n700 1\n");
    require(run(gonio + "LightSource \"goniometric\" \"string filename\" \"map.hdr\" \"spectrum "
                        "I\" \"intensity.spd\"\n")
                    .exit_code() == 0,
            "goniometric file spectrum dependency");
}
} // namespace
int main(int argc, char **argv) {
    try {
        require(argc == 2, "Expected scratch fixture directory");
        storage();
        invalid();
        preflight(argv[1]);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
