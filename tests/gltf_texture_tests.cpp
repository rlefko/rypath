// SPDX-License-Identifier: GPL-3.0-only

#include "image_texture.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
using Bytes = std::vector<std::uint8_t>;
using Rgba = std::array<float, 4>;

void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
template <typename T> T success(ImageResult<T> result) {
    if (!result.value)
        throw std::runtime_error(result.diagnostic);
    return std::move(*result.value);
}
void near(const std::optional<Rgba> &actual, Rgba expected, std::string_view message) {
    if (!actual)
        throw std::runtime_error(std::string(message));
    for (std::size_t c = 0; c < 4; ++c)
        require(std::abs((*actual)[c] - expected[c]) < 2e-6f, message);
}
ImageTexture texture(ImagePyramid image) {
    ImageTexture result;
    result.image = std::make_shared<ImagePyramid>(std::move(image));
    result.gltf_sampler = GltfSampler{};
    return result;
}
GltfSampler &sampler(ImageTexture &image) {
    if (!image.gltf_sampler)
        throw std::runtime_error("Expected a glTF sampler in test texture");
    return *image.gltf_sampler;
}

void append(Bytes &bytes, std::uint32_t v) {
    for (int shift : {24, 16, 8, 0})
        bytes.push_back(static_cast<std::uint8_t>(v >> shift));
}
void chunk(Bytes &bytes, std::string_view type, const Bytes &data) {
    append(bytes, static_cast<std::uint32_t>(data.size()));
    const auto begin = bytes.size();
    bytes.insert(bytes.end(), type.begin(), type.end());
    bytes.insert(bytes.end(), data.begin(), data.end());
    std::uint32_t crc = 0xffffffffu;
    for (auto i = begin; i < bytes.size(); ++i) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; ++j)
            crc = crc >> 1 ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    append(bytes, ~crc);
}
// PNG fixture fields deliberately follow color/depth, pixels, transparency, palette.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Bytes png(std::uint8_t color, std::uint8_t depth, Bytes row, Bytes transparency = {},
          Bytes palette = {}) {
    Bytes bytes{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes header;
    append(header, 1);
    append(header, 1);
    header.insert(header.end(), {depth, color, 0, 0, 0});
    chunk(bytes, "IHDR", header);
    if (!palette.empty())
        chunk(bytes, "PLTE", palette);
    if (!transparency.empty())
        chunk(bytes, "tRNS", transparency);
    row.insert(row.begin(), 0);
    const auto length = static_cast<std::uint16_t>(row.size());
    Bytes zlib{0x78,
               1,
               1,
               static_cast<std::uint8_t>(length),
               static_cast<std::uint8_t>(length >> 8),
               static_cast<std::uint8_t>(~length),
               static_cast<std::uint8_t>(~length >> 8)};
    zlib.insert(zlib.end(), row.begin(), row.end());
    std::uint32_t a = 1, b = 0;
    for (auto v : row) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    append(zlib, b << 16 | a);
    chunk(bytes, "IDAT", zlib);
    chunk(bytes, "IEND", {});
    return bytes;
}
void alpha_profiles() {
    struct Case {
        Bytes bytes;
        Bytes rgb;
        Bytes alpha;
    };
    const std::array cases{
        Case{png(6, 8, {91, 123, 217, 37}), {91, 123, 217}, {37}},
        Case{png(4, 8, {93, 0}), {93, 93, 93}, {0}},
        Case{png(3, 1, {0}, {17}, {41, 83, 127}), {41, 83, 127}, {17}},
        Case{png(0, 2, {0x80}, {0, 2}), {170, 170, 170}, {0}},
        Case{png(2, 8, {9, 23, 71}, {0, 9, 0, 23, 0, 71}), {9, 23, 71}, {0}},
        Case{png(4, 16, {0x23, 0x45, 0x12, 0x34}),
             {0x23, 0x45, 0x23, 0x45, 0x23, 0x45},
             {0x12, 0x34}},
        Case{png(6, 16, {0x12, 0x34, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}),
             {0x12, 0x34, 0x45, 0x67, 0x89, 0xab},
             {0xcd, 0xef}},
    };
    for (const auto &test : cases) {
        const auto decoded = success(decode_png(test.bytes, {}, true));
        require(decoded.rgb == test.rgb && decoded.alpha == test.alpha &&
                    decoded.source_channels == 4,
                "glTF PNG did not preserve exact unassociated source channels");
        const auto tex = texture(success(build_gltf_image_pyramid(decoded, ImageEncoding::srgb)));
        const auto sampled = gltf_image_rgba(tex, .5f, .5f, 0, 0, 0, 0);
        const auto code = test.alpha.size() == 1
                              ? static_cast<float>(test.alpha[0]) / 255.0f
                              : static_cast<float>((test.alpha[0] << 8) | test.alpha[1]) / 65535.0f;
        require(sampled && std::abs((*sampled)[3] - code) < 1e-7f,
                "RGB transfer incorrectly applied to alpha");
        if (test.bytes[25] != 6)
            require(success(decode_png(test.bytes)).alpha.empty(), "PBRT alpha default changed");
        const auto retained = decoded_image_retained_bytes(decoded);
        require(decode_png(test.bytes, {.retained_bytes = retained - 1}, true).limit_exceeded,
                "glTF alpha allocation bypassed retained budget");
    }
}
void native_mips_and_precision() {
    DecodedImage decoded{3, 5, {}};
    decoded.rgb.resize(45);
    // Only the odd rightmost column is red; only the odd bottom row is green.
    for (std::size_t y = 0; y < 5; ++y)
        for (std::size_t x = 0; x < 3; ++x) {
            decoded.rgb[(y * 3 + x) * 3] = x == 2 ? 255 : 0;
            decoded.rgb[(y * 3 + x) * 3 + 1] = y == 4 ? 255 : 0;
        }
    auto image = success(build_gltf_image_pyramid(decoded, ImageEncoding::linear));
    require(image.levels.size() == 3 && image.levels[0].width == 3 && image.levels[0].height == 5 &&
                image.levels[1].width == 1 && image.levels[1].height == 2,
            "glTF base was resized or NPOT mip dimensions changed");
    auto tex = texture(image);
    near(gltf_image_rgba(tex, .5f, .5f, 100, 0, 0, 0), {1.0f / 3, .2f, 0, 1},
         "NPOT area mip dropped edge pixels");
    const auto retained = image.texels.size() * 4 + image.levels.size() * sizeof(ImageMipLevel);
    require(
        build_gltf_image_pyramid(decoded, ImageEncoding::linear, {.retained_bytes = retained - 1})
            .limit_exceeded,
        "glTF pyramid retained limit not enforced");
    require(build_gltf_image_pyramid(decoded, ImageEncoding::linear, {.scratch_bytes = 1})
                .limit_exceeded,
            "glTF pyramid scratch limit not enforced");
    decoded = {1, 1, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}, ImageSampleFormat::unorm16};
    near(gltf_image_rgba(texture(success(build_gltf_image_pyramid(decoded, ImageEncoding::linear))),
                         .5f, .5f, 0, 0, 0, 0),
         {0x1234 / 65535.0f, 0x5678 / 65535.0f, 0x9abc / 65535.0f, 1},
         "16-bit glTF samples were rounded to half precision");
    const auto path =
        std::filesystem::path(__FILE__).parent_path() / "fixtures/image/baseline-rgb.jpg";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Missing owned JPEG fixture");
    const Bytes jpeg{std::istreambuf_iterator<char>(input), {}};
    decoded = success(decode_jpeg(jpeg));
    tex = texture(success(build_gltf_image_pyramid(decoded, ImageEncoding::linear)));
    sampler(tex).mag_filter = 9728;
    near(gltf_image_rgba(tex, .5f / static_cast<float>(decoded.width),
                         .5f / static_cast<float>(decoded.height), 0, 0, 0, 0),
         {static_cast<float>(decoded.rgb[0]) / 255.0f, static_cast<float>(decoded.rgb[1]) / 255.0f,
          static_cast<float>(decoded.rgb[2]) / 255.0f, 1},
         "glTF data JPEG incorrectly forced sRGB");
}
void wrap_and_orientation() {
    auto tex = texture(success(build_gltf_image_pyramid(
        {2, 2, {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}}, ImageEncoding::linear)));
    sampler(tex).mag_filter = 9728;
    const auto sample = [&](float s, float t) { return gltf_image_rgba(tex, s, t, 0, 0, 0, 0); };
    near(sample(.25f, .25f), {1, 0, 0, 1}, "glTF top-left UV was flipped");
    near(sample(.25f, .75f), {0, 0, 1, 1}, "glTF bottom row was flipped");
    near(sample(-.25f, .25f), {0, 1, 0, 1}, "Negative repeat failed");
    near(sample(-0x1p-30f, .25f), {0, 1, 0, 1}, "Tiny negative repeat lost the last texel");
    near(sample(1, 1), {1, 0, 0, 1}, "Repeat endpoint failed");
    sampler(tex).wrap_s = 33071;
    near(sample(-.25f, .75f), {0, 0, 1, 1}, "Independent S clamp failed");
    near(sample(1, 1), {0, 1, 0, 1}, "Mixed clamp/repeat endpoint failed");
    sampler(tex).wrap_t = 33648;
    near(sample(.25f, -.25f), {1, 0, 0, 1}, "Independent negative T mirror failed");
    near(sample(.25f, 1.25f), {0, 0, 1, 1}, "T mirror failed");
    sampler(tex).wrap_s = 33648;
    near(sample(-.25f, .25f), {1, 0, 0, 1}, "Negative S mirror failed");
    near(sample(1, 1), {1, 1, 1, 1}, "Mirrored endpoint failed");
    sampler(tex).mag_filter = 9729;
    near(sample(1, .25f), {0, 1, 0, 1}, "Mirrored edge duplicated wrong tap");
    sampler(tex).wrap_s = 10497;
    near(sample(0, .25f), {.5f, .5f, 0, 1}, "Linear repeat seam failed");
    sampler(tex).wrap_s = 33071;
    near(sample(0, .25f), {1, 0, 0, 1}, "Linear clamp edge failed");
    near(sample(std::numeric_limits<float>::max(), .25f), {0, 1, 0, 1}, "Huge UV clamp failed");
    require(!sample(std::numeric_limits<float>::infinity(), 0), "Nonfinite UV accepted");
}
void npot_lod_boundary() {
    auto tex = texture(success(build_gltf_image_pyramid(
        {3, 1, {0, 0, 0, 100, 100, 100, 255, 255, 255}}, ImageEncoding::linear)));
    sampler(tex).min_filter = 9728;
    sampler(tex).mag_filter = 9729;
    const float unit = 1.0f / 3.0f;
    const auto sample = [&](float derivative) {
        return gltf_image_rgba(tex, unit, .5f, derivative, 0, 0, 0);
    };
    near(sample(std::nextafter(unit, 0.0f)), {50 / 255.0f, 50 / 255.0f, 50 / 255.0f, 1},
         "NPOT below-boundary derivative did not magnify");
    near(sample(unit), {50 / 255.0f, 50 / 255.0f, 50 / 255.0f, 1},
         "NPOT rounded texel footprint missed exact min/mag boundary");
    near(sample(std::nextafter(unit, 1.0f)), {100 / 255.0f, 100 / 255.0f, 100 / 255.0f, 1},
         "NPOT above-boundary derivative did not minify");
}

void all_filters() {
    ImagePyramid image{.encoding = ImageEncoding::linear,
                       .wrap = ImageWrap::repeat,
                       .levels = {{4, 1, 0}, {2, 1, 16}, {1, 1, 24}},
                       .texels = {},
                       .storage = ImageStorage::linear_float,
                       .source_channels = 4,
                       .gltf = true};
    const std::array<Rgba, 7> pixels{{{0, .1f, .2f, .3f},
                                      {1, .2f, .4f, .6f},
                                      {.2f, .3f, .6f, .9f},
                                      {.8f, .4f, .8f, 1},
                                      {.4f, .5f, .6f, .7f},
                                      {.8f, .9f, 1, .3f},
                                      {1, 0, .5f, .5f}}};
    for (auto p : pixels)
        for (float c : p)
            image.texels.push_back(std::bit_cast<std::uint32_t>(c));
    auto tex = texture(image);
    sampler(tex).mag_filter = 9728;
    auto sample = [&](float rho) { return gltf_image_rgba(tex, .25f, .5f, rho / 4, 0, 0, 0); };
    near(sample(1), pixels[1], "Exact min/mag boundary did not use mag filter");
    sampler(tex).mag_filter = 9729;
    near(sample(0), {.5f, .15f, .3f, .45f}, "Linear magnification failed");
    sampler(tex).mag_filter = 9728;
    for (std::uint32_t filter : {9728u, 9729u, 9984u, 9985u, 9986u, 9987u}) {
        sampler(tex).min_filter = filter;
        const auto expected = filter == 9728   ? pixels[1]
                              : filter == 9729 ? Rgba{.5f, .15f, .3f, .45f}
                                               : pixels[4];
        near(sample(2), expected, "Minification mode failed at exact integer LOD");
        const auto wide = filter == 9728   ? pixels[1]
                          : filter == 9729 ? Rgba{.5f, .15f, .3f, .45f}
                                           : pixels[6];
        near(sample(std::numeric_limits<float>::max()), wide,
             "Minification mode selected wrong coarsest mip");
    }
    for (std::uint32_t filter : {9986u, 9987u}) {
        sampler(tex).min_filter = filter;
        const auto a = filter == 9986 ? pixels[1] : Rgba{.5f, .15f, .3f, .45f};
        Rgba expected{};
        constexpr float fraction = .25f;
        for (std::size_t c = 0; c < 4; ++c)
            expected[c] = .75f * a[c] + fraction * pixels[4][c];
        near(sample(std::exp2(fraction)), expected,
             "Mip-linear weights or per-level filter failed");
    }
    sampler(tex).min_filter = 9984;
    near(gltf_image_rgba(tex, .5f, .5f, .5f, 0, 0, 0), pixels[5],
         "Nearest mip filter interpolated adjacent texels");
    sampler(tex).min_filter = 9985;
    near(gltf_image_rgba(tex, .5f, .5f, .5f, 0, 0, 0), {.6f, .7f, .8f, .5f},
         "Linear mip filter failed to interpolate adjacent texels");
    sampler(tex).min_filter = 9984;
    near(gltf_image_rgba(tex, .25f, .5f, .35355339059f, 1.41421356237f, 0, 0), pixels[4],
         "LOD used max gradient component instead of texel-space Euclidean length");
    near(sample(std::exp2(.49f)), pixels[1], "Mip-nearest rounded down on wrong boundary");
    near(sample(std::exp2(.51f)), pixels[4], "Mip-nearest rounded up on wrong boundary");
    near(gltf_image_rgba(tex, .25f, .5f, 0, 2, 0, 0), pixels[4], "LOD ignored height derivative");
    auto bad = image;
    bad.texels.pop_back();
    tex.image = std::make_shared<ImagePyramid>(bad);
    require(!sample(16), "Selected truncated mip accepted");
    require(sample(0).has_value(), "Unused truncated mip invalidated base lookup");
}
} // namespace
int main() {
    try {
        alpha_profiles();
        native_mips_and_precision();
        wrap_and_orientation();
        all_filters();
        npot_lod_boundary();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
