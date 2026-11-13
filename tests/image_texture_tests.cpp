// SPDX-License-Identifier: GPL-3.0-only

#include "image_texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace rypath;
using Bytes = std::vector<std::uint8_t>;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename T> T success(ImageResult<T> result) {
    if (!result.value) {
        throw std::runtime_error(result.diagnostic);
    }
    return std::move(*result.value);
}

void append_u32(Bytes &bytes, std::uint32_t value) {
    for (int shift : {24, 16, 8, 0}) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_chunk(Bytes &png, std::string_view type, std::span<const std::uint8_t> payload = {}) {
    append_u32(png, static_cast<std::uint32_t>(payload.size()));
    const auto begin = png.size();
    png.insert(png.end(), type.begin(), type.end());
    png.insert(png.end(), payload.begin(), payload.end());
    // Bit-at-a-time CRC-32 keeps this fixture generator independent of the decoder.
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : std::span(png).subspan(begin)) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) == 0 ? 0u : 0xedb88320u);
        }
    }
    append_u32(png, ~crc);
}

Bytes insert_chunk(Bytes png, std::size_t offset, std::string_view type, const Bytes &data) {
    Bytes chunk;
    append_chunk(chunk, type, data);
    png.insert(png.begin() + static_cast<std::ptrdiff_t>(offset), chunk.begin(), chunk.end());
    return png;
}

Bytes stored_zlib(std::span<const std::uint8_t> scanlines) {
    Bytes bytes{0x78, 0x01};
    auto remaining = scanlines;
    do {
        const auto length =
            static_cast<std::uint16_t>(std::min<std::size_t>(65535, remaining.size()));
        const auto block = remaining.first(length);
        remaining = remaining.subspan(length);
        bytes.insert(bytes.end(),
                     {static_cast<std::uint8_t>(remaining.empty()),
                      static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8),
                      static_cast<std::uint8_t>(~length),
                      static_cast<std::uint8_t>((~static_cast<unsigned>(length)) >> 8)});
        bytes.insert(bytes.end(), block.begin(), block.end());
    } while (!remaining.empty());
    std::uint32_t a = 1, b = 0;
    for (const auto byte : scanlines) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    append_u32(bytes, (b << 16) | a);
    return bytes;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Bytes make_png(std::uint32_t width, std::uint32_t height, std::uint8_t color_type,
               std::span<const std::uint8_t> scanlines, std::uint8_t bit_depth = 8,
               std::uint8_t interlace = 0, std::string_view chunk = {}) {
    Bytes png{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes header;
    append_u32(header, width);
    append_u32(header, height);
    header.insert(header.end(), {bit_depth, color_type, 0, 0, interlace});
    append_chunk(png, "IHDR", header);
    if (chunk == "acTL") {
        append_chunk(png, chunk, Bytes{0, 0, 0, 1, 0, 0, 0, 0});
    } else if (chunk == "PLTE") {
        append_chunk(png, chunk, Bytes{100, 0, 50});
    } else if (chunk == "gAMA") {
        append_chunk(png, chunk, Bytes{0, 1, 134, 160});
    } else if (chunk == "zTXt") {
        // Unusable compressed metadata exercises the metadata bypass.
        append_chunk(png, chunk, Bytes{'k', 0, 0, 0xff});
    }
    const auto zlib = stored_zlib(scanlines);
    append_chunk(png, "IDAT", std::span(zlib).first(3));
    append_chunk(png, "IDAT", std::span(zlib).subspan(3));
    append_chunk(png, "IEND");
    return png;
}

void decode_profiles(const std::filesystem::path &directory) {
    // PNG Sub filter on the second RGB pixel. Rows remain in file order.
    const auto rgb = make_png(2, 2, 2, Bytes{1, 10, 20, 30, 90, 180, 250, 0, 2, 4, 6, 7, 8, 9});
    const auto image = success(decode_png(rgb));
    require(image.width == 2 && image.height == 2 &&
                image.rgb == Bytes{10, 20, 30, 100, 200, 24, 2, 4, 6, 7, 8, 9},
            "RGB PNG scanline order or Sub filter changed");
    std::filesystem::create_directories(directory);
    std::ofstream output(directory / "rgb.png", std::ios::binary);
    output.write(reinterpret_cast<const char *>(rgb.data()),
                 static_cast<std::streamsize>(rgb.size()));
    output.close();
    require(output.good(), "Could not write RGB PNG fixture");

    require(success(decode_png(make_png(1, 1, 6, Bytes{0, 7, 91, 180, 0}))).rgb ==
                Bytes{7, 91, 180},
            "Transparent RGBA lost RGB values");
    require(success(decode_png(make_png(2, 1, 4, Bytes{0, 29, 0, 44, 128}))).rgb ==
                Bytes{29, 29, 29, 44, 44, 44},
            "Gray-alpha conversion changed luminance");
    require(success(decode_png(make_png(1, 1, 0, Bytes{0, 132}, 8, 0, "gAMA"))).rgb ==
                Bytes{132, 132, 132},
            "Gamma metadata changed decoded bytes");
    require(success(decode_png(make_png(1, 1, 0, Bytes{0, 132}, 8, 0, "zTXt"))).rgb ==
                Bytes{132, 132, 132},
            "Compressed metadata reached the decoder");
}

void decode_errors() {
    auto malformed = [](const Bytes &bytes) {
        const auto result = decode_png(bytes);
        require(!result.value && !result.unsupported && !result.limit_exceeded &&
                    !result.diagnostic.empty(),
                "Malformed PNG was not diagnosed");
    };
    malformed({});
    malformed(make_png(0, 1, 2, Bytes{0}));
    malformed(make_png(1, 1, 2, Bytes{5, 11, 22, 33})); // Bad filter.
    malformed(make_png(1, 1, 2, Bytes{0, 11, 22}));     // Short inflate.
    malformed(make_png(1, 1, 2, Bytes(65535)));         // Maximal stored-block overrun.
    auto png = make_png(1, 1, 2, Bytes{0, 11, 22, 33});
    png.back() ^= 1u;
    malformed(png);
    png.pop_back();
    malformed(png);
    auto bad_ancillary_crc = make_png(1, 1, 2, Bytes{0, 11, 22, 33}, 8, 0, "gAMA");
    bad_ancillary_crc[42] ^= 1u;
    malformed(bad_ancillary_crc);
    auto trailing_bytes = make_png(1, 1, 2, Bytes{0, 11, 22, 33});
    trailing_bytes.push_back(0);
    malformed(trailing_bytes);

    const auto valid = make_png(1, 1, 2, Bytes{0, 11, 22, 33});
    constexpr std::size_t after_ihdr = 33;
    auto insert_palette = [](Bytes png, std::size_t offset, const Bytes &data) {
        return insert_chunk(std::move(png), offset, "PLTE", data);
    };
    const Bytes palette{0, 127, 255};
    require(success(decode_png(insert_palette(valid, after_ihdr, palette))).rgb ==
                Bytes{11, 22, 33},
            "Suggested RGB palette changed truecolor samples");
    malformed(insert_palette(valid, after_ihdr, {}));
    malformed(insert_palette(valid, after_ihdr, {0, 127, 255, 0}));
    malformed(insert_palette(valid, after_ihdr, Bytes(771)));
    malformed(insert_palette(valid, valid.size() - 12, palette));
    malformed(insert_palette(insert_palette(valid, after_ihdr, palette), after_ihdr, palette));
    malformed(make_png(1, 1, 0, Bytes{0, 0}, 8, 0, "PLTE"));
    malformed(make_png(1, 1, 3, Bytes{0, 0}));
    malformed(insert_palette(make_png(1, 1, 3, Bytes{0, 0}, 1), after_ihdr, Bytes(9)));

    const Bytes rgb_key{0, 11, 0, 22, 0, 33};
    const auto transparent = insert_chunk(valid, after_ihdr, "tRNS", rgb_key);
    require(success(decode_png(transparent)).rgb == Bytes{11, 22, 33},
            "Transparency key changed decoded RGB samples");
    malformed(insert_chunk(valid, after_ihdr, "tRNS", {0, 1}));
    malformed(insert_chunk(valid, valid.size() - 12, "tRNS", rgb_key));
    malformed(insert_chunk(transparent, after_ihdr, "tRNS", rgb_key));
    malformed(insert_palette(transparent, after_ihdr + 12 + rgb_key.size(), palette));
    malformed(insert_chunk(make_png(1, 1, 6, Bytes{0, 1, 2, 3, 0}), after_ihdr, "tRNS", rgb_key));
    malformed(insert_chunk(make_png(1, 1, 4, Bytes{0, 1, 0}), after_ihdr, "tRNS", {0, 1}));
    malformed(insert_chunk(make_png(1, 1, 0, Bytes{0, 1}), after_ihdr, "tRNS", rgb_key));
    const auto indexed = make_png(1, 1, 3, Bytes{0, 0}, 8, 0, "PLTE");
    malformed(insert_chunk(indexed, after_ihdr, "tRNS", {0}));
    malformed(insert_chunk(indexed, after_ihdr + 15, "tRNS", {0, 0}));

    for (const auto &unsupported :
         {make_png(1, 1, 0, Bytes{0, 1, 2}, 16), make_png(1, 1, 3, Bytes{0, 0}, 8, 0, "PLTE"),
          make_png(1, 1, 2, Bytes{0, 11, 22, 33}, 8, 1),
          make_png(1, 1, 2, Bytes{0, 11, 22, 33}, 8, 0, "acTL")}) {
        const auto result = decode_png(unsupported);
        require(!result.value && result.unsupported && !result.limit_exceeded,
                "Unsupported PNG profile was accepted or marked malformed");
        malformed(Bytes(unsupported.begin(), unsupported.end() - 1));
    }
    const auto rgb16 = make_png(1, 1, 2, Bytes{0, 1, 2, 3, 4, 5, 6}, 16);
    malformed(Bytes(rgb16.begin(), rgb16.begin() + after_ihdr));

    require(decode_png(valid, {.max_dimension = 0}).limit_exceeded, "PNG ignored axis limit");
    require(decode_png(valid, {.retained_bytes = 2}).limit_exceeded, "PNG ignored retained limit");
    require(decode_png(valid, {.scratch_bytes = 0}).limit_exceeded, "PNG ignored scratch limit");
    require(decode_png(make_png(4097, 1, 2, Bytes{0})).limit_exceeded,
            "PNG ignored hard axis limit");
    require(decode_png(Bytes(16 * 1024 * 1024 + 1)).limit_exceeded, "PNG ignored hard input limit");
    constexpr auto decoded_bytes = std::size_t{512} * 512 * 3;
    const auto grayscale = make_png(512, 512, 0, Bytes(std::size_t{513} * 512));
    require(decode_png(grayscale, {.scratch_bytes = 2 * decoded_bytes - 1}).limit_exceeded,
            "Grayscale decode ignored simultaneous RGB buffers");
    require(success(decode_png(grayscale, {.scratch_bytes = 2 * decoded_bytes})).rgb.size() ==
                decoded_bytes,
            "Grayscale decode rejected exact RGB buffer limit");
}

void deflate_work_limit() {
    auto fixture = [](std::size_t empty_blocks) {
        // An empty non-final stored block is five bytes, including its byte-aligned
        // header and zero length. Preserve the real block and Adler-32 after them.
        const auto one_pixel = stored_zlib(Bytes{0, 11, 22, 33});
        Bytes zlib{one_pixel[0], one_pixel[1]};
        for (std::size_t i = 0; i < empty_blocks; ++i) {
            zlib.insert(zlib.end(), {0, 0, 0, 255, 255});
        }
        zlib.insert(zlib.end(), one_pixel.begin() + 2, one_pixel.end());
        auto png = make_png(1, 1, 2, Bytes{0, 11, 22, 33});
        png.resize(33);
        append_chunk(png, "IDAT", zlib);
        append_chunk(png, "IEND");
        return png;
    };
    constexpr std::size_t block_limit = 65536;
    require(success(decode_png(fixture(block_limit - 1))).rgb == Bytes{11, 22, 33},
            "The DEFLATE block limit rejected its exact boundary");
    const auto over_limit = decode_png(fixture(block_limit));
    require(!over_limit.value && !over_limit.unsupported && over_limit.limit_exceeded &&
                over_limit.diagnostic.find("DEFLATE block limit") != std::string::npos,
            "The decoder accepted a stream with excessive DEFLATE blocks");
}

DecodedImage gray_strip(std::span<const std::uint8_t> values, bool vertical = false) {
    const auto count = static_cast<std::uint32_t>(values.size());
    DecodedImage image{vertical ? 1u : count, vertical ? count : 1u, {}};
    for (const auto v : values) {
        image.rgb.insert(image.rgb.end(), {v, v, v});
    }
    return image;
}

void require_levels(const ImagePyramid &pyramid, std::span<const ImageMipLevel> levels,
                    std::span<const std::uint8_t> gray_bytes) {
    require(pyramid.levels.size() == levels.size(), "Wrong mip level count");
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const auto &actual = pyramid.levels[i];
        const auto &expected = levels[i];
        require(actual.width == expected.width && actual.height == expected.height &&
                    actual.offset == expected.offset,
                "Wrong mip dimensions or offset");
    }
    require(pyramid.texels.size() == gray_bytes.size(), "Wrong packed texel count");
    for (std::size_t i = 0; i < gray_bytes.size(); ++i) {
        require(pyramid.texels[i] == gray_bytes[i] * 0x010101u, "Unexpected packed gray texel");
    }
}

void mip_pyramid() {
    const auto image = gray_strip(Bytes{0, 9, 255, 0});
    const auto srgb = success(build_image_pyramid(image, ImageEncoding::srgb, ImageWrap::repeat));
    const auto linear =
        success(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::repeat));
    const std::array levels{ImageMipLevel{4, 1, 0}, ImageMipLevel{2, 1, 4}, ImageMipLevel{1, 1, 6}};
    // The top sRGB byte would be 138 if it were built from re-decoded intermediate bytes.
    require_levels(srgb, levels, Bytes{0, 9, 255, 0, 5, 188, 137});
    require_levels(linear, levels, Bytes{0, 9, 255, 0, 5, 128, 66});
    require_levels(success(build_image_pyramid(image, ImageEncoding::srgb, ImageWrap::repeat)),
                   levels, Bytes{0, 9, 255, 0, 5, 188, 137});
    const auto vertical = gray_strip(Bytes{0, 9, 255, 0}, true);
    require_levels(
        success(build_image_pyramid(vertical, ImageEncoding::srgb, ImageWrap::repeat)),
        std::array{ImageMipLevel{1, 4, 0}, ImageMipLevel{1, 2, 4}, ImageMipLevel{1, 1, 6}},
        Bytes{0, 9, 255, 0, 5, 188, 137});

    const auto npot = gray_strip(Bytes{255, 0, 64});
    require_levels(success(build_image_pyramid(npot, ImageEncoding::linear, ImageWrap::repeat)),
                   levels, Bytes{252, 92, 0, 87, 172, 44, 108});
    require_levels(success(build_image_pyramid(npot, ImageEncoding::linear, ImageWrap::clamp)),
                   levels, Bytes{255, 85, 2, 68, 178, 35, 106});
    require(build_image_pyramid(npot, ImageEncoding::linear, ImageWrap::black).unsupported,
            "Non-power-of-two black wrapping was accepted");

    require(
        build_image_pyramid(npot, ImageEncoding::linear, ImageWrap::repeat, {.max_dimension = 3})
            .limit_exceeded,
        "Mipmap ignored resized axis limit");
    const auto retained = levels.size() * sizeof(ImageMipLevel) + 7 * sizeof(std::uint32_t);
    require(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::repeat,
                                {.retained_bytes = retained - 1})
                .limit_exceeded,
            "Mipmap ignored retained limit");
    require(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::repeat,
                                {.retained_bytes = retained})
                .value.has_value(),
            "Mipmap rejected its exact retained-byte budget");
    require(
        build_image_pyramid(image, ImageEncoding::linear, ImageWrap::repeat, {.scratch_bytes = 0})
            .limit_exceeded,
        "Mipmap ignored scratch limit");
    require(!build_image_pyramid({1, 1, {42}}, ImageEncoding::linear, ImageWrap::repeat).value,
            "Malformed RGB pixel buffer was accepted");
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto invalid_encoding = static_cast<ImageEncoding>(-1);
    const auto invalid_wrap = static_cast<ImageWrap>(-1);
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
    require(!build_image_pyramid(image, invalid_encoding, ImageWrap::repeat).value &&
                !build_image_pyramid(image, ImageEncoding::linear, invalid_wrap).value,
            "Invalid image encoding or wrap mode was accepted");
}

void lookup() {
    const DecodedImage image{2, 2, {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}};
    const auto repeat =
        success(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::repeat));
    const auto clamp = success(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::clamp));
    const auto black = success(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::black));
    const auto gray = 128.0f / 255.0f;
    using Rgb = std::array<float, 3>;
    require(repeat.texels ==
                std::vector<std::uint32_t>{0x0000ff, 0x00ff00, 0xff0000, 0xffffff, 0x808080},
            "Packed RGB order changed");
    require(image_linear_rgb(repeat, 0.25f, 0.25f, 0, 0, 0, 0) == Rgb{1, 0, 0},
            "Wrong texel center");
    require(image_linear_rgb(repeat, 0.5f, 0.5f, 0, 0, 0, 0) == Rgb{0.5f, 0.5f, 0.5f},
            "Wrong bilerp");
    require(image_linear_rgb(repeat, -0.25f, 0.25f, 0, 0, 0, 0) == Rgb{0, 1, 0}, "Wrong repeat");
    require(image_linear_rgb(clamp, -0.25f, 0.25f, 0, 0, 0, 0) == Rgb{1, 0, 0}, "Wrong clamp");
    require(image_linear_rgb(black, 0.0f, 0.25f, 0, 0, 0, 0) == Rgb{0.5f, 0, 0},
            "Wrong black border");
    require(image_linear_rgb(black, -20.0f, 3.0f, 0, 0, 0, 0) == Rgb{}, "Wrong exterior black");
    require(image_linear_rgb(black, -20.0f, 3.0f, 0, 0, 0, 0.5f) == Rgb{gray, gray, gray},
            "Wide footprints must return the coarsest texel regardless of wrap");
    const auto strip = gray_strip(Bytes{0, 9, 255, 0});
    const auto lods = success(build_image_pyramid(strip, ImageEncoding::linear, ImageWrap::clamp));
    require(image_linear_rgb(lods, 0.25f, 0.5f, 0.2f, 0, 0, 0) ==
                Rgb{4.5f / 255.0f, 4.5f / 255.0f, 4.5f / 255.0f},
            "Bilinear lookup blended between mip levels");
    require(image_linear_rgb(lods, 0.25f, 0.5f, 0, 0, 0, -0.25f) ==
                Rgb{5.0f / 255.0f, 5.0f / 255.0f, 5.0f / 255.0f},
            "LOD did not account for the largest absolute derivative");
    const auto huge = std::numeric_limits<float>::max();
    require(image_linear_rgb(clamp, huge, -huge, 0, 0, 0, 0) == Rgb{0, 1, 0},
            "Huge finite coordinates must wrap safely");
    require(!image_linear_rgb(repeat, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0, 0),
            "Nonfinite coordinates were accepted");
    require(!image_linear_rgb(repeat, 0, 0, 0, std::numeric_limits<float>::infinity(), 0, 0),
            "Nonfinite derivatives were accepted");
    require(image_linear_rgb(black, -20.0f, 3.0f, huge, 0, 0, 0) == Rgb{gray, gray, gray},
            "Huge finite gradient did not select the coarsest mip");
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        require(argc == 2, "Expected a temporary test directory argument");
        decode_profiles(argv[1]);
        decode_errors();
        deflate_work_limit();
        mip_pyramid();
        lookup();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
