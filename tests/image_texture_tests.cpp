// SPDX-License-Identifier: GPL-3.0-only

#include "image_texture.hpp"

#include "ewa_reference.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <numbers>
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Bytes make_indexed_png(std::uint32_t width, std::uint32_t height, std::uint8_t depth,
                       std::span<const std::uint8_t> scanlines, const Bytes &palette) {
    return insert_chunk(make_png(width, height, 3, scanlines, depth), 33, "PLTE", palette);
}

void decode_profiles(const std::filesystem::path &directory) {
    // PNG Sub filter on the second RGB pixel. Rows remain in file order.
    const auto rgb = make_png(2, 2, 2, Bytes{1, 10, 20, 30, 90, 180, 250, 0, 2, 4, 6, 7, 8, 9});
    const auto image = success(decode_png(rgb));
    require(image.width == 2 && image.height == 2 && image.bit_depth == 8 &&
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

void decode_16_bit_profiles() {
    // PNG Sub filter operates on bytes, including each sample's low byte.
    const auto rgb = success(decode_png(
        make_png(2, 1, 2, Bytes{1, 0x12, 0xfe, 0xab, 0xcd, 0x00, 0x01, 0, 1, 1, 2, 3, 4}, 16)));
    require(rgb.width == 2 && rgb.height == 1 && rgb.bit_depth == 16 &&
                rgb.rgb ==
                    Bytes{0x12, 0xfe, 0xab, 0xcd, 0x00, 0x01, 0x12, 0xff, 0xac, 0xcf, 0x03, 0x05},
            "RGB16 PNG changed high or low bytes");
    const auto rgba = success(
        decode_png(make_png(1, 1, 6, Bytes{0, 0x12, 0x34, 0xab, 0xcd, 0xfe, 0xdc, 0, 0}, 16)));
    require(rgba.bit_depth == 16 && rgba.rgb == Bytes{0x12, 0x34, 0xab, 0xcd, 0xfe, 0xdc},
            "Transparent RGBA16 lost RGB values");
    const auto gray = make_png(1, 1, 0, Bytes{0, 0x01, 0x23}, 16, 0, "gAMA");
    const auto gray_transparent = insert_chunk(gray, 33, "tRNS", {0x01, 0x23});
    const auto gray_alpha = make_png(1, 1, 4, Bytes{0, 0x01, 0x23, 0, 0}, 16);
    for (const auto &png : {gray, gray_transparent, gray_alpha}) {
        const auto image = success(decode_png(png, {.retained_bytes = 6}));
        require(image.bit_depth == 16 && image.rgb == Bytes{0x01, 0x23, 0x01, 0x23, 0x01, 0x23},
                "Gray16 was not replicated with full precision independently of transparency");
        require(decode_png(png, {.retained_bytes = 5}).limit_exceeded,
                "PNG16 ignored its six-byte retained pixel cost");
    }

    constexpr auto pixels = std::size_t{512} * 512;
    const auto gray16 = make_png(512, 512, 0, Bytes(pixels * 2 + 512), 16);
    require(decode_png(gray16, {.scratch_bytes = 12 * pixels - 1}).limit_exceeded &&
                success(decode_png(gray16, {.scratch_bytes = 12 * pixels})).rgb.size() ==
                    6 * pixels,
            "Gray16 scratch did not include both simultaneous RGB16 allocations");
    constexpr auto rgba16_scanline_bytes = pixels * 8 + 512;
    const auto rgba16 = make_png(512, 512, 6, Bytes(rgba16_scanline_bytes), 16);
    constexpr auto huffman_scratch = std::size_t{256} * 1024;
    const auto inflate_scratch = rgba16.size() + 2 * rgba16_scanline_bytes +
                                 rgba16_scanline_bytes / 2 + 65535 + huffman_scratch;
    require(decode_png(rgba16, {.scratch_bytes = inflate_scratch - 1}).limit_exceeded &&
                success(decode_png(rgba16, {.scratch_bytes = inflate_scratch})).rgb.size() ==
                    6 * pixels,
            "RGBA16 scratch did not account for depth and alpha during inflate");
}

void require_rgb8_mip_equivalence(const DecodedImage &image, const DecodedImage &rgb) {
    for (const auto encoding : {ImageEncoding::srgb, ImageEncoding::linear}) {
        for (const auto wrap : {ImageWrap::repeat, ImageWrap::clamp, ImageWrap::black}) {
            auto result = build_image_pyramid(image, encoding, wrap);
            const auto actual = success(std::move(result));
            const auto expected = success(build_image_pyramid(rgb, encoding, wrap));
            require(actual.storage == ImageStorage::encoded8 && actual.texels == expected.texels &&
                        std::ranges::equal(actual.levels, expected.levels,
                                           [](const auto &a, const auto &b) {
                                               return a.width == b.width && a.height == b.height &&
                                                      a.offset == b.offset;
                                           }),
                    "PNG changed encoded8 mip levels compared with RGB8");
        }
    }
}

void decode_indexed_profiles() {
    // Width 5 makes each sub-byte row end mid-byte. All row-padding bits are set.
    struct Case {
        std::uint8_t depth;
        Bytes scanlines;
        Bytes indices;
    };
    const std::array cases{
        Case{1, {0, 0x5f, 0, 0xa7, 0, 0xcf}, {0, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1}},
        Case{2,
             {0, 0x19, 0x3f, 0, 0x86, 0x7f, 0, 0x61, 0xbf},
             {0, 1, 2, 1, 0, 2, 0, 1, 2, 1, 1, 2, 0, 1, 2}},
        // Rows independently encode None, Sub, Up, Average, and Paeth.
        Case{4,
             {0,    0x01, 0x21, 0x0f, 1,    0x20, 0xf2, 0x0d, 2,    0xf2,
              0xef, 0x10, 3,    0xf8, 0x20, 0xe7, 4,    0x1f, 0xf1, 0x10},
             {0, 1, 2, 1, 0, 2, 0, 1, 2, 1, 1, 2, 0, 1, 2, 0, 1, 2, 1, 0, 2, 0, 1, 2, 1}},
        Case{8,
             {0, 0, 1, 2, 1, 0, 0, 2, 0, 1, 2, 1, 0, 1, 2, 0, 1, 2},
             {0, 1, 2, 1, 0, 2, 0, 1, 2, 1, 1, 2, 0, 1, 2}},
    };
    constexpr std::uint32_t width = 5;
    for (const auto &test : cases) {
        const auto height = static_cast<std::uint32_t>(test.indices.size() / width);
        const Bytes palette = test.depth == 1 ? Bytes{23, 157, 201, 229, 19, 3}
                                              : Bytes{23, 157, 201, 229, 19, 3, 5, 97, 182};
        const Bytes alpha = test.depth == 1 ? Bytes{0} : Bytes{0, 91};
        const auto png = make_indexed_png(width, height, test.depth, test.scanlines, palette);
        Bytes rgb_scanlines;
        Bytes rgb;
        for (std::size_t i = 0; i < test.indices.size(); ++i) {
            if (i % width == 0) {
                rgb_scanlines.push_back(0);
            }
            const auto *sample = palette.data() + std::size_t{test.indices[i]} * 3;
            rgb_scanlines.insert(rgb_scanlines.end(), sample, sample + 3);
            rgb.insert(rgb.end(), sample, sample + 3);
        }
        const auto reference = success(decode_png(make_png(width, height, 2, rgb_scanlines)));
        constexpr std::size_t after_ihdr = 33;
        constexpr std::size_t chunk_overhead = 12;
        const auto after_plte = after_ihdr + chunk_overhead + palette.size();
        for (const auto &bytes : {png, insert_chunk(png, after_plte, "tRNS", {}),
                                  insert_chunk(png, after_plte, "tRNS", alpha)}) {
            const auto image = success(decode_png(bytes));
            require(image.width == width && image.height == height && image.bit_depth == 8 &&
                        image.rgb == rgb,
                    "Indexed PNG changed palette RGB, row padding, filtering, or tRNS handling");
            require_rgb8_mip_equivalence(image, reference);
        }
        Bytes full_palette(3 * (std::size_t{1} << test.depth));
        const Bytes last_color{113, 193, 61};
        std::ranges::copy(last_color, full_palette.end() - 3);
        require(
            success(decode_png(make_indexed_png(1, 1, test.depth, Bytes{0, 0xff}, full_palette)))
                    .rgb == last_color,
            "Indexed PNG rejected the highest index or largest palette for its depth");
    }

    const Bytes palette{25, 132, 207, 91, 221, 19};
    const auto pot =
        success(decode_png(make_indexed_png(4, 2, 1, Bytes{0, 0x5f, 0, 0xaf}, palette)));
    const auto rgb_pot = success(decode_png(
        make_png(4, 2, 2, Bytes{0, 25, 132, 207, 91, 221, 19,  25, 132, 207, 91, 221, 19,
                                0, 91, 221, 19,  25, 132, 207, 91, 221, 19,  25, 132, 207})));
    require_rgb8_mip_equivalence(pot, rgb_pot);
}

void decode_packed_grayscale_profiles() {
    // Each odd-width image contains every sample code. Its manually authored
    // rows use None, Sub, Up, Average, and Paeth, with all padding bits set.
    struct Case {
        std::uint8_t depth;
        Bytes scanlines;
        Bytes gray8;
    };
    const std::array cases{
        Case{1,
             {0, 0x57, 1, 0xaf, 2, 0xc0, 3, 0x78, 4, 0xa8},
             {0, 255, 0,   255, 0,   255, 0,   255, 0,   255, 0,   255, 255,
              0, 255, 255, 0,   255, 0,   255, 0,   255, 0,   255, 0}},
        Case{2,
             {0, 0x1b, 0x3f, 1, 0xe4, 0x1b, 2, 0xb0, 0x00, 3, 0x9a, 0x8e, 4, 0xcd, 0xc0},
             {0, 85,  170, 255, 0,  255, 170, 85,  0,   255, 170, 85, 85,
              0, 255, 255, 170, 85, 0,   85,  170, 255, 0,   85,  0}},
        Case{4,
             {0,    0x01, 0x23, 0x4f, 1,    0xfe, 0xde, 0xe3, 2,    0x6b,
              0x7c, 0xc0, 3,    0xfe, 0x0f, 0xb6, 4,    0x79, 0xde, 0xbb},
             {0,   17,  34, 51, 68, 255, 238, 221, 204, 187, 102, 153, 85,
              136, 119, 51, 34, 85, 68,  17,  170, 187, 136, 153, 0}},
    };
    constexpr std::uint32_t width = 5, height = 5;
    constexpr std::size_t after_ihdr = 33;
    for (const auto &test : cases) {
        Bytes rgb_scanlines;
        Bytes rgb;
        for (std::size_t i = 0; i < test.gray8.size(); ++i) {
            if (i % width == 0) {
                rgb_scanlines.push_back(0);
            }
            rgb_scanlines.insert(rgb_scanlines.end(), 3, test.gray8[i]);
            rgb.insert(rgb.end(), 3, test.gray8[i]);
        }
        const auto reference = success(decode_png(make_png(width, height, 2, rgb_scanlines)));
        const auto png = make_png(width, height, 0, test.scanlines, test.depth, 0, "gAMA");
        // Sample 1 is nonblack at every depth. The unused high bits of its key
        // may be set, and keyed pixels retain the same reflectance.
        for (const auto &bytes : {png, insert_chunk(png, after_ihdr, "tRNS", {0, 1}),
                                  insert_chunk(png, after_ihdr, "tRNS", {0xff, 0xf1})}) {
            const auto image = success(decode_png(bytes));
            require(image.width == width && image.height == height && image.bit_depth == 8 &&
                        image.rgb == rgb,
                    "Packed grayscale changed sample expansion, filtering, padding, or tRNS");
            require_rgb8_mip_equivalence(image, reference);
        }
    }

    const auto pot = success(decode_png(make_png(4, 2, 0, Bytes{0, 0x1b, 0, 0xe4}, 2)));
    const DecodedImage rgb_pot{4, 2, {0,   0,   0,   85,  85,  85,  170, 170, 170, 255, 255, 255,
                                      255, 255, 255, 170, 170, 170, 85,  85,  85,  0,   0,   0}};
    require_rgb8_mip_equivalence(pot, rgb_pot);
}

void packed_decode_limits() {
    constexpr auto palette_scratch = std::size_t{2} * 1024;
    constexpr auto huffman_scratch = std::size_t{256} * 1024;
    const Bytes palette{15, 131, 217};
    const auto check_limits = [](const Bytes &png, std::size_t scratch, std::size_t retained) {
        const auto short_scratch = decode_png(png, {.scratch_bytes = scratch - 1});
        const auto short_retained = decode_png(png, {.retained_bytes = retained - 1});
        require(!short_scratch.value && short_scratch.limit_exceeded &&
                    !short_scratch.unsupported && !short_retained.value &&
                    short_retained.limit_exceeded && !short_retained.unsupported,
                "Packed PNG decoder accepted an undersized budget");
        const auto exact =
            success(decode_png(png, {.retained_bytes = retained, .scratch_bytes = scratch}));
        require(exact.bit_depth == 8 && exact.rgb.size() == retained,
                "Packed PNG decoder retained a non-RGB8 payload");
    };
    // Inflate dominates for tiny inputs. A 5x3 two-bit image needs two data bytes per row.
    const Bytes small_scanlines{0, 0, 0x3f, 0, 0, 0x3f, 0, 0, 0x3f};
    const auto small = make_indexed_png(5, 3, 2, small_scanlines, palette);
    check_limits(small,
                 palette_scratch + small.size() + 2 * small_scanlines.size() +
                     small_scanlines.size() / 2 + 65535 + huffman_scratch,
                 45);

    // Expansion dominates for one-bit data at this size. Each row has seven
    // padding bits that would reference a missing palette entry if read as pixels.
    constexpr std::uint32_t width = 513, height = 513;
    constexpr auto pixels = std::size_t{width} * height;
    constexpr std::size_t scanline_size = 66;
    Bytes large_scanlines(scanline_size * height);
    for (std::size_t end = scanline_size - 1; end < large_scanlines.size(); end += scanline_size) {
        large_scanlines[end] = 0x7f;
    }
    const auto large = make_indexed_png(width, height, 1, large_scanlines, palette);
    check_limits(large, palette_scratch + (pixels + 7) / 8 + 3 * pixels, 3 * pixels);

    const auto small_gray = make_png(5, 3, 0, small_scanlines, 2);
    check_limits(small_gray,
                 small_gray.size() + 2 * small_scanlines.size() + small_scanlines.size() / 2 +
                     65535 + huffman_scratch,
                 45);
    // In the nonindexed path, the RGB8 copy dominates scratch for this fixture.
    const auto large_gray = make_png(width, height, 0, large_scanlines, 1);
    check_limits(large_gray, 6 * pixels, 3 * pixels);
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
    const auto no_palette = make_png(1, 1, 3, Bytes{0, 0});
    const auto indexed = insert_palette(no_palette, after_ihdr, palette);
    malformed(insert_chunk(indexed, after_ihdr, "tRNS", {0}));
    malformed(insert_chunk(indexed, after_ihdr + 15, "tRNS", {0, 0}));
    malformed(insert_chunk(indexed, indexed.size() - 12, "tRNS", {0}));
    const auto indexed_alpha = insert_chunk(indexed, after_ihdr + 15, "tRNS", {});
    malformed(insert_chunk(indexed_alpha, after_ihdr + 15, "tRNS", {0}));
    malformed(insert_palette(indexed, after_ihdr, palette));
    malformed(insert_palette(no_palette, no_palette.size() - 12, palette));
    malformed(make_indexed_png(1, 1, 8, Bytes{0, 0}, Bytes(std::size_t{3} * 257)));
    malformed(make_indexed_png(1, 1, 8, Bytes{0, 0}, {}));
    malformed(make_indexed_png(1, 1, 8, Bytes{0, 0}, {1, 2, 3, 4}));
    malformed(make_indexed_png(1, 1, 16, Bytes{0, 0, 0}, palette));
    malformed(make_indexed_png(5, 1, 2, Bytes{5, 0, 0}, palette));    // Invalid filter.
    malformed(make_indexed_png(5, 1, 2, Bytes{0, 0}, palette));       // Truncated row.
    malformed(make_indexed_png(5, 1, 2, Bytes{0, 0, 0, 0}, palette)); // Oversized row.
    const auto packed_gray = make_png(5, 1, 0, Bytes{0, 0x1b, 0x3f}, 2);
    const Bytes gray_key{0, 1};
    const auto keyed_gray = insert_chunk(packed_gray, after_ihdr, "tRNS", gray_key);
    malformed(insert_chunk(packed_gray, after_ihdr, "tRNS", {1}));
    malformed(insert_chunk(packed_gray, after_ihdr, "tRNS", {0, 1, 0}));
    malformed(insert_chunk(keyed_gray, after_ihdr, "tRNS", gray_key));
    malformed(insert_chunk(packed_gray, packed_gray.size() - 12, "tRNS", gray_key));
    auto bad_key_crc = keyed_gray;
    bad_key_crc[after_ihdr + 10] ^= 1u;
    malformed(bad_key_crc);
    malformed(insert_palette(packed_gray, after_ihdr, palette));
    malformed(make_png(5, 1, 0, Bytes{5, 0x1b, 0x3f}, 2));    // Invalid filter.
    malformed(make_png(5, 1, 0, Bytes{0, 0x1b}, 2));          // Truncated row.
    malformed(make_png(5, 1, 0, Bytes{0, 0x1b, 0x3f, 0}, 2)); // Oversized row.
    malformed(make_png(1, 1, 0, Bytes{0, 0}, 3));             // Illegal grayscale depth.
    for (const auto depth : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{4}, std::uint8_t{8}}) {
        // In this 1x3 image, only the final pixel uses an out-of-range index.
        const auto late_index = make_indexed_png(1, 3, depth, Bytes{0, 0, 0, 0, 0, 0xff}, palette);
        const auto result = decode_png(late_index);
        require(!result.value && !result.unsupported && !result.limit_exceeded &&
                    result.diagnostic.find("pixel index exceeds palette size") != std::string::npos,
                "Indexed PNG did not diagnose an out-of-range index in its last row");
    }

    for (const auto &unsupported :
         {make_png(1, 1, 0, Bytes{0, 0x10}, 4, 0, "acTL"),
          insert_chunk(indexed, after_ihdr, "acTL", {0, 0, 0, 1, 0, 0, 0, 0}),
          make_png(1, 1, 2, Bytes{0, 11, 22, 33}, 8, 0, "acTL"),
          make_png(1, 1, 2, Bytes{0, 1, 2, 3, 4, 5, 6}, 16, 0, "acTL")}) {
        const auto result = decode_png(unsupported);
        require(!result.value && result.unsupported && !result.limit_exceeded,
                "Unsupported PNG profile was accepted or marked malformed");
        malformed(Bytes(unsupported.begin(), unsupported.end() - 1));
    }
    const auto rgb16 = make_png(1, 1, 2, Bytes{0, 1, 2, 3, 4, 5, 6}, 16);
    malformed(Bytes(rgb16.begin(), rgb16.begin() + after_ihdr));
    malformed(make_png(1, 1, 2, Bytes{0, 1, 2, 3, 4, 5}, 16));
    malformed(make_png(1, 1, 2, Bytes{0, 1, 2, 3, 4, 5, 6, 7}, 16));

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

// Test writer follows the seven PNG passes directly, with the "None" row filter.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Bytes adam7_scanlines(unsigned width, unsigned height, unsigned channels, unsigned depth,
                      std::span<const std::uint16_t> source) {
    constexpr std::array passes{std::array{0u, 0u, 8u, 8u}, std::array{4u, 0u, 8u, 8u},
                                std::array{0u, 4u, 4u, 8u}, std::array{2u, 0u, 4u, 4u},
                                std::array{0u, 2u, 2u, 4u}, std::array{1u, 0u, 2u, 2u},
                                std::array{0u, 1u, 1u, 2u}};
    Bytes rows;
    for (const auto &[x0, y0, dx, dy] : passes) {
        if (x0 >= width)
            continue;
        for (unsigned y = y0; y < height; y += dy) {
            rows.push_back(0);
            unsigned bit = 0;
            for (unsigned x = x0; x < width; x += dx) {
                const auto index = (y * width + x) * channels;
                for (const auto sample : source.subspan(index, channels)) {
                    if (depth == 16) {
                        rows.push_back(static_cast<std::uint8_t>(sample >> 8));
                        rows.push_back(static_cast<std::uint8_t>(sample));
                    } else {
                        if (bit == 0)
                            rows.push_back(0);
                        rows.back() |= static_cast<std::uint8_t>(sample << (8 - bit - depth));
                        bit = (bit + depth) % 8;
                    }
                }
            }
        }
    }
    return rows;
}

void decode_adam7_profiles() {
    constexpr auto palette =
        std::array<std::uint8_t, 12>{32, 8, 91, 255, 2, 20, 0, 37, 189, 134, 1, 5};
    struct Profile {
        unsigned mode, depth, channels;
    };
    constexpr std::array profiles{Profile{0, 1, 1},  Profile{0, 2, 1},  Profile{0, 4, 1},
                                  Profile{0, 8, 1},  Profile{0, 16, 1}, Profile{2, 8, 3},
                                  Profile{2, 16, 3}, Profile{3, 1, 1},  Profile{3, 2, 1},
                                  Profile{3, 4, 1},  Profile{3, 8, 1},  Profile{4, 8, 2},
                                  Profile{4, 16, 2}, Profile{6, 8, 4},  Profile{6, 16, 4}};
    for (const auto &[mode, depth, channels] : profiles) {
        for (const auto &[width, height] :
             {std::array{1u, 1u}, std::array{1u, 9u}, std::array{9u, 1u}, std::array{11u, 13u}}) {
            const unsigned max_sample = (1u << depth) - 1;
            const unsigned colors = std::min(1u << depth, 4u);
            std::vector<std::uint16_t> source(std::size_t{width} * height * channels);
            for (std::size_t i = 0; i < source.size(); ++i) {
                const auto patterned = (i * 149 + i / 3 * 53 + 1) & max_sample;
                source[i] = static_cast<std::uint16_t>(mode == 3 ? patterned % colors : patterned);
            }
            const auto scanlines = adam7_scanlines(width, height, channels, depth, source);
            auto png = make_png(width, height, static_cast<std::uint8_t>(mode), scanlines,
                                static_cast<std::uint8_t>(depth), 1);
            if (mode == 3)
                png =
                    insert_chunk(std::move(png), 33, "PLTE",
                                 Bytes(palette.begin(), palette.begin() + 3 * std::size_t{colors}));
            const auto image = success(decode_png(png));
            Bytes rgb, alpha;
            const auto append_sample = [&](Bytes &out, unsigned value) {
                if (depth == 16)
                    out.push_back(static_cast<std::uint8_t>(value >> 8));
                out.push_back(
                    static_cast<std::uint8_t>(depth < 8 ? value * 255 / max_sample : value));
            };
            for (std::size_t i = 0; i < source.size(); i += channels) {
                if (mode == 3) {
                    const auto *entry = palette.data() + 3 * std::size_t{source[i]};
                    rgb.insert(rgb.end(), entry, entry + 3);
                } else {
                    for (unsigned c = 0; c < 3; ++c)
                        append_sample(rgb, source[i + (channels < 3 ? 0 : c)]);
                    if (mode == 6)
                        append_sample(alpha, source[i + 3]);
                }
            }
            const auto expected_channels = mode == 3 ? 3u : (mode == 4 ? 1u : channels);
            require(image.width == width && image.height == height &&
                        image.bit_depth == std::max(8u, depth) &&
                        image.source_channels == expected_channels && image.rgb == rgb &&
                        image.alpha == alpha,
                    "Adam7 pixel layout, expanded colors, or native channel identity differs");
        }
    }
    // One missing pass row and an invalid filter byte in the final pass are malformed.
    require(!decode_png(make_png(2, 2, 0, Bytes{0, 17, 0, 18}, 8, 1)).value &&
                !decode_png(make_png(2, 2, 0, Bytes{0, 17, 0, 18, 5, 19, 20}, 8, 1)).value,
            "Adam7 accepted malformed scanlines");
    const auto indexed = insert_chunk(make_png(2, 2, 3, Bytes{0, 0, 0, 0, 0, 0x40}, 2, 1), 33,
                                      "PLTE", Bytes{1, 2, 3});
    require(decode_png(indexed).diagnostic.find("index exceeds palette size") != std::string::npos,
            "Adam7 did not validate a palette index in the final pass");

    const auto rows =
        adam7_scanlines(11, 13, 4, 16, std::vector<std::uint16_t>(std::size_t{11} * 13 * 4, 87));
    const auto rgba = make_png(11, 13, 6, rows, 16, 1, "PLTE");
    const auto scratch = rgba.size() + rows.size() * 2 + rows.size() / 2 + 65535 +
                         std::size_t{256} * 1024 + std::size_t{2} * 256 * 4;
    const auto retained = 11u * 13u * 8u;
    require(decode_png(rgba, {.retained_bytes = retained - 1}).limit_exceeded &&
                decode_png(rgba, {.scratch_bytes = scratch - 1}).limit_exceeded &&
                decode_png(rgba, {.retained_bytes = retained, .scratch_bytes = scratch}).value,
            "Adam7 scratch or retained bytes did not include truecolor alpha and PLTE");
}

void decode_jpeg_profiles() {
    const auto directory = std::filesystem::path(__FILE__).parent_path() / "fixtures/image";
    const auto read = [&](std::string_view name) {
        std::ifstream input(directory / name, std::ios::binary);
        require(input.good(), "Could not open JPEG test fixture");
        return Bytes(std::istreambuf_iterator<char>(input), {});
    };
    struct Case {
        std::string_view name;
        std::uint32_t channels;
        std::uint64_t decoded_hash;
    };
    constexpr std::array cases{
        Case{"baseline-ycbcr.jpg", 3, 0xfe62f13d28cf9135},
        Case{"progressive-ycbcr.jpg", 3, 0xfe62f13d28cf9135},
        Case{"progressive-mixed-dc.jpg", 3, 0xfe62f13d28cf9135},
        Case{"baseline-rgb.jpg", 3, 0x2fa873fff074c333},
        Case{"progressive-gray.jpg", 1, 0xa5a287805fcc30c9},
    };
    for (const auto &test : cases) {
        const auto bytes = read(test.name);
        const auto image = success(decode_jpeg(bytes));
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto byte : image.rgb)
            hash = (hash ^ byte) * 1099511628211ull;
        require(image.width == 19 && image.height == 17 && image.bit_depth == 8 && image.jpeg &&
                    image.source_channels == test.channels && image.alpha.empty() &&
                    hash == test.decoded_hash,
                "JPEG decoded bytes differ from PBRT's pinned decoder");
        const auto authored_linear =
            success(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::repeat));
        const auto authored_gamma =
            success(build_image_pyramid(image, ImageEncoding::gamma, ImageWrap::repeat, {}, 2));
        require(authored_linear.encoding == ImageEncoding::srgb &&
                    authored_gamma.texels == authored_linear.texels,
                "Explicit JPEG encoding overrode pinned effective sRGB");
    }

    const auto bytes = read("baseline-ycbcr.jpg");
    const auto marker = [&](std::uint8_t code) {
        const auto match = std::ranges::search(bytes, std::array<std::uint8_t, 2>{0xff, code});
        require(!match.empty(), "Missing JPEG marker in test fixture");
        return static_cast<std::size_t>(match.begin() - bytes.begin()) + 1;
    };
    const auto expect_invalid = [&](const Bytes &bad) {
        const auto result = decode_jpeg(bad);
        require(!result.value && !result.limit_exceeded && !result.unsupported,
                "Malformed JPEG was accepted or misclassified");
    };

    auto malformed = bytes;
    malformed.pop_back();
    expect_invalid(malformed);
    malformed = bytes;
    malformed.push_back(0);
    expect_invalid(malformed);
    malformed = bytes;
    malformed[marker(0xd0)] = 0xd1;
    expect_invalid(malformed);
    malformed = bytes;
    const auto scan = marker(0xda);
    const auto length = static_cast<std::size_t>(bytes[scan + 1]) * 256 + bytes[scan + 2];
    malformed.erase(malformed.begin() + static_cast<std::ptrdiff_t>(scan + 1 + length),
                    malformed.end() - 2);
    expect_invalid(malformed); // Complete EOI framing with no entropy.
    malformed = bytes;
    malformed[marker(0xc4) + 4] = 255; // Oversized Huffman symbol array.
    expect_invalid(malformed);
    malformed = bytes;
    malformed[scan + 5] = 0x33; // Missing DC and AC tables.
    expect_invalid(malformed);
    malformed = bytes;
    malformed[marker(0xc0)] = 0xc1;
    require(decode_jpeg(malformed).unsupported, "Unsupported JPEG SOF was accepted");
    malformed = bytes;
    malformed[marker(0xc0) + 8] = 4;
    require(decode_jpeg(malformed).unsupported, "CMYK was accepted");

    malformed.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(scan - 1));
    for (unsigned i = 0; i < 257; ++i)
        malformed.insert(malformed.end(), {0xff, 0xda, 0, 8, 1, 1, 0, 0, 63, 0});
    malformed.insert(malformed.end(), {0xff, 0xd9});
    const auto too_many_scans = decode_jpeg(malformed);
    require(too_many_scans.limit_exceeded &&
                too_many_scans.diagnostic.find("256-scan") != std::string::npos,
            "JPEG ignored its scan count limit");
    constexpr auto retained = 19u * 17u * 3u;
    require(decode_jpeg(bytes, {.max_dimension = 18}).limit_exceeded &&
                decode_jpeg(bytes, {.retained_bytes = retained - 1}).limit_exceeded &&
                decode_jpeg(bytes, {.retained_bytes = retained}).value &&
                decode_jpeg(bytes, {.scratch_bytes = retained}).limit_exceeded &&
                decode_jpeg(Bytes(16 * 1024 * 1024 + 1)).limit_exceeded,
            "JPEG ignored its axis, byte, or work budget");
}

void append_jpeg_segment(Bytes &jpeg, std::uint8_t marker, std::span<const std::uint8_t> data) {
    const auto size = data.size() + 2;
    jpeg.insert(jpeg.end(), {0xff, marker, static_cast<std::uint8_t>(size >> 8),
                             static_cast<std::uint8_t>(size)});
    jpeg.insert(jpeg.end(), data.begin(), data.end());
}

void jpeg_dct_work_limit() {
    Bytes jpeg{0xff, 0xd8};
    append_jpeg_segment(jpeg, 0xdb, Bytes(65, 1));
    // The quantization table ID is 1, and all 64 quantization values are 1.
    append_jpeg_segment(jpeg, 0xc2, Bytes{8, 16, 0, 16, 0, 1, 1, 0x11, 1});
    Bytes dc{0, 1};
    dc.insert(dc.end(), 16, 0);
    append_jpeg_segment(jpeg, 0xc4, dc); // Huffman bits 0 -> DC difference 0.
    Bytes ac{0x10, 1, 1};
    ac.insert(ac.end(), 14, 0);
    ac.insert(ac.end(), {0xe0, 0x30}); // 0 -> 14-bit EOB count, 10 -> three-bit count.
    append_jpeg_segment(jpeg, 0xc4, ac);

    constexpr unsigned blocks = 4096 * 4096 / 64;
    append_jpeg_segment(jpeg, 0xda, Bytes{1, 1, 0, 0, 0, 0});
    jpeg.insert(jpeg.end(), blocks / 8, 0); // DC scans consume a bit per block.

    // All AC values are zero. Eight 32767-block EOB runs and one eight-block run
    // encode the complete scan in 125 bits. Stuff each entropy byte of 0xff.
    std::vector<bool> bits;
    for (unsigned i = 0; i < 8; ++i) {
        bits.push_back(false);
        bits.insert(bits.end(), 14, true);
    }
    bits.insert(bits.end(), {true, false, false, false, false, true, true, true});
    Bytes entropy;
    for (std::size_t i = 0; i < bits.size(); i += 8) {
        std::uint8_t byte = 0;
        for (std::size_t j = 0; j < 8; ++j)
            byte = static_cast<std::uint8_t>(2 * byte + static_cast<unsigned>(bits[i + j]));
        entropy.push_back(byte);
        if (byte == 0xff)
            entropy.push_back(0);
    }
    for (std::uint8_t coefficient = 1; coefficient <= 16; ++coefficient) {
        append_jpeg_segment(jpeg, 0xda, Bytes{1, 1, 0, coefficient, coefficient, 0});
        jpeg.insert(jpeg.end(), entropy.begin(), entropy.end());
    }
    jpeg.insert(jpeg.end(), {0xff, 0xd9});
    const auto result = decode_jpeg(jpeg);
    require(result.limit_exceeded && result.diagnostic.find("DCT block-work") != std::string::npos,
            "JPEG ignored its DCT block limit across complete progressive scans");
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

void deflate_allocation_limit() {
    const auto fixture = [](std::uint32_t width, std::uint32_t height) {
        // A fixed-Huffman DEFLATE block produces zero scanlines without storing them.
        const auto scanline_bytes = height * (width * 8 + 1);
        Bytes zlib{0x78, 0x01};
        unsigned bit = 0;
        auto write_code = [&](unsigned code, unsigned length) {
            // Huffman codes are written most significant bit first, into byte bits 0 through 7.
            for (unsigned i = length; i > 0; --i) {
                if (bit == 0) {
                    zlib.push_back(0);
                }
                zlib.back() |= static_cast<std::uint8_t>(((code >> (i - 1)) & 1u) << bit);
                bit = (bit + 1) % 8;
            }
        };
        write_code(0b110, 3); // BFINAL=1, BTYPE=01 in DEFLATE bit order.
        write_code(0x30, 8);  // Literal zero.
        auto remaining = scanline_bytes - 1;
        for (; remaining >= 258; remaining -= 258) {
            write_code(0xc5, 8); // Length 258.
            write_code(0, 5);    // Distance 1.
        }
        for (; remaining > 0; --remaining) {
            write_code(0x30, 8);
        }
        write_code(0, 7); // End of block.
        append_u32(zlib, ((scanline_bytes % 65521u) << 16) | 1u);
        auto png = make_png(width, height, 6, Bytes{}, 16);
        png.resize(33);
        append_chunk(png, "IDAT", zlib);
        append_chunk(png, "IEND");
        return png;
    };
    require(success(decode_png(fixture(33, 1))).rgb == Bytes(std::size_t{33} * 6),
            "Fixed-Huffman zero scanline fixture did not decode");
    // The predicted scratch and retained costs fit. LodePNG's Huffman decoder can still
    // request growth beyond its per-allocation cap while reserving space for the next code.
    const auto result = decode_png(fixture(3072, 4096));
    require(!result.value && !result.unsupported && result.limit_exceeded &&
                result.diagnostic.find("could not allocate") != std::string::npos,
            "PNG16 decoder allocation failure was not reported as a resource limit");
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
    require(pyramid.storage == ImageStorage::encoded8, "8-bit PNG mip storage changed");
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

DecodedImage gray16_strip(std::span<const std::uint16_t> values) {
    DecodedImage image{static_cast<std::uint32_t>(values.size()), 1, {}, 16};
    for (const auto value : values) {
        const auto high = static_cast<std::uint8_t>(value >> 8);
        const auto low = static_cast<std::uint8_t>(value);
        image.rgb.insert(image.rgb.end(), {high, low, high, low, high, low});
    }
    return image;
}

void require_half_strip(const ImagePyramid &pyramid, std::span<const std::uint16_t> halves) {
    require(pyramid.storage == ImageStorage::linear_half && pyramid.levels.size() == 3 &&
                pyramid.texels.size() == 2 * halves.size(),
            "Unexpected half-float mip pyramid layout");
    for (std::uint32_t level = 0, width = 4, offset = 0; level < 3; ++level) {
        const auto &actual = pyramid.levels[level];
        require(actual.width == width && actual.height == 1 && actual.offset == offset,
                "Half-float mip offsets must count words");
        offset += width * 2;
        width /= 2;
    }
    for (std::size_t i = 0; i < halves.size(); ++i) {
        require(pyramid.texels[2 * i] == halves[i] * 0x10001u &&
                    pyramid.texels[2 * i + 1] == halves[i],
                "Half-float gray mip texel changed");
    }
}

void mip_pyramid_16_bit() {
    const auto image = gray16_strip(std::array<std::uint16_t, 4>{1, 1, 255, 0x1235});
    const auto pyramid =
        success(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::clamp));
    // Skipping the source half round or cascading stored halves both yield 0x24ce at the top.
    require_half_strip(pyramid, std::array<std::uint16_t, 7>{0x0100, 0x0100, 0x1bf8, 0x2c8d, 0x0100,
                                                             0x28cd, 0x24cd});

    const auto npot = gray16_strip(std::array<std::uint16_t, 3>{65535, 1, 16448});
    const auto clamp = success(build_image_pyramid(npot, ImageEncoding::linear, ImageWrap::clamp));
    const auto repeat =
        success(build_image_pyramid(npot, ImageEncoding::linear, ImageWrap::repeat));
    // Clamp resize overshoots one at its first texel. Repeat clips an undershoot to zero.
    require_half_strip(clamp, std::array<std::uint16_t, 7>{0x3c3d, 0x3554, 0x1fdd, 0x3442, 0x3992,
                                                           0x3061, 0x36ab});
    require_half_strip(
        repeat, std::array<std::uint16_t, 7>{0x3bec, 0x35cb, 0, 0x3576, 0x3968, 0x3176, 0x36c6});

    // The rational PBRT transfer gives blue 0x1bc9 where the sRGB power law gives 0x1bc8.
    const auto srgb = success(build_image_pyramid({1, 1, {0, 1, 0, 2, 0x0c, 0x69}, 16},
                                                  ImageEncoding::srgb, ImageWrap::clamp));
    require(srgb.encoding == ImageEncoding::srgb && srgb.storage == ImageStorage::linear_half &&
                srgb.texels == std::vector<std::uint32_t>{0x00280014, 0x1bc9},
            "Continuous sRGB16 decode or binary16 channel packing changed");
    require(image_linear_rgb(srgb, 0.5f, 0.5f, 0, 0, 0, 0, ImageFilter::point) ==
                std::array{20 * 0x1p-24f, 40 * 0x1p-24f, 0x1.f24p-9f},
            "sRGB16 lookup lost subnormals or decoded linear texels twice");

    constexpr auto retained = 3 * sizeof(ImageMipLevel) + 14 * sizeof(std::uint32_t);
    require(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::clamp,
                                {.retained_bytes = retained - 1})
                    .limit_exceeded &&
                build_image_pyramid(image, ImageEncoding::linear, ImageWrap::clamp,
                                    {.retained_bytes = retained})
                    .value.has_value(),
            "Half-float mip payload ignored its retained byte boundary");
    constexpr auto scratch = 6 * sizeof(std::array<float, 3>);
    require(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::clamp,
                                {.scratch_bytes = scratch - 1})
                    .limit_exceeded &&
                build_image_pyramid(image, ImageEncoding::linear, ImageWrap::clamp,
                                    {.scratch_bytes = scratch})
                    .value.has_value(),
            "Half-float mip scratch did not account for both float working levels");
    for (const auto &malformed :
         {DecodedImage{1, 1, {0, 1, 2}, 16}, DecodedImage{1, 1, Bytes(7), 16},
          DecodedImage{1, 1, Bytes(6), 12}}) {
        const auto result = build_image_pyramid(malformed, ImageEncoding::linear, ImageWrap::clamp);
        require(!result.value && !result.unsupported && !result.limit_exceeded &&
                    !result.diagnostic.empty(),
                "Invalid decoded bit depth or RGB16 payload length was accepted");
    }
}

void npot_black_and_native_view() {
    const std::array<std::uint8_t, 9> gray{251, 26, 109, 31, 123, 120, 185, 44, 65};
    DecodedImage image{.width = 3, .height = 3, .rgb = {}, .source_channels = 1};
    for (const auto sample : gray)
        image.rgb.insert(image.rgb.end(), 3, sample);
    auto black = success(build_image_pyramid(image, ImageEncoding::linear, ImageWrap::black));
    require(black.levels.size() == 3 && black.levels[0].width == 4 && black.levels[0].height == 4,
            "Black NPOT resize did not produce a complete mip chain");
    const auto sinc = [](double x) {
        return x == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
    };
    const auto weight = [&](double x) { return std::abs(x) <= 2 ? sinc(x) * sinc(x / 2) : 0; };
    const auto weight_sum = [&](double x) {
        double sum = 0;
        for (int source = -2; source < 5; ++source)
            sum += weight(source - x);
        return sum;
    };
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            const double cx = (x + 0.5) * 3 / 4 - 0.5;
            const double cy = (y + 0.5) * 3 / 4 - 0.5;
            double sum = 0;
            // The normalization includes zero taps outside the original source.
            for (unsigned sy = 0; sy < 3; ++sy)
                for (unsigned sx = 0; sx < 3; ++sx)
                    sum += gray[3 * sy + sx] * weight(sx - cx) * weight(sy - cy);
            const auto quantized =
                std::lround(std::clamp(sum / (weight_sum(cx) * weight_sum(cy)), 0.0, 255.0));
            require(black.texels[4 * y + x] == 0x010101u * static_cast<unsigned>(quantized),
                    "Black NPOT mip base differs from independent zero-tap convolution");
        }
    }

    const auto native = success(
        build_image_base(image, ImageEncoding::linear, ImageWrap::black, {.scratch_bytes = 0}));
    require(native.native_base && native.levels.size() == 1 && native.levels[0].width == 3 &&
                native.levels[0].height == 3 && native.texels[0] == 0xfbfbfbu,
            "Native image view resampled the source");
    require(image_base_linear_rgb(native, 0.5f, 0.5f) ==
                std::array{123.0f / 255, 123.0f / 255, 123.0f / 255},
            "Native image view did not use source pixel centers");
}

void scalar_channel_and_gamma() {
    const Bytes scanlines{0, 51, 102, 153, 25,  51, 102, 153, 85,
                          0, 51, 102, 153, 127, 51, 102, 153, 255};
    auto rgba = success(decode_png(make_png(2, 2, 6, scanlines)));
    const auto mip = success(build_image_pyramid(rgba, ImageEncoding::linear, ImageWrap::clamp));
    require(mip.source_channels == 4 && mip.texels.front() == 0x19996633u &&
                mip.texels.back() == 0x7b996633u,
            "Native alpha packing or downsampling differs from pinned PBRT");
    const auto value = [&](ImageFilter filter, float width) {
        return image_linear_scalar(mip, 0.25f, 0.25f, width, 0, 0, width, filter);
    };
    require(value(ImageFilter::point, 0) == 51.0f / 255 &&
                value(ImageFilter::bilinear, 0) == 25.0f / 255 &&
                value(ImageFilter::trilinear, 0) == 25.0f / 255 &&
                value(ImageFilter::ewa, 0) == 25.0f / 255 &&
                std::abs(value(ImageFilter::ewa, 0.3f).value_or(NAN) - 0.200000033f) < 1e-7f &&
                value(ImageFilter::bilinear, 0.5f) == 51.0f / 255 &&
                value(ImageFilter::ewa, 4) == 51.0f / 255,
            "Scalar imagemap selected the wrong channel for its filter and footprint");
    require(image_linear_rgb(mip, 0.25f, 0.25f, 0, 0, 0, 0) ==
                std::array{51.0f / 255, 102.0f / 255, 153.0f / 255},
            "Spectrum sampling used RGBA alpha");

    std::ranges::fill(rgba.alpha, 255);
    const auto opaque = success(build_image_pyramid(rgba, ImageEncoding::linear, ImageWrap::clamp));
    require(opaque.source_channels == 3 &&
                image_linear_scalar(opaque, 0.25f, 0.25f, 0, 0, 0, 0) == 0.4f,
            "Opaque RGBA did not use the RGB average");

    const auto rgba16 = success(decode_png(make_png(
        2, 1, 6,
        Bytes{0, 0x80, 0, 0x80, 0, 0x80, 0, 0x40, 0, 0x80, 0, 0x80, 0, 0x80, 0, 0xff, 0xff}, 16)));
    const auto half =
        success(build_image_pyramid(rgba16, ImageEncoding::gamma, ImageWrap::clamp, {}, 2));
    require(half.source_channels == 4 && !half.custom_transfer &&
                image_linear_scalar(half, 0.25f, 0.5f, 0, 0, 0, 0) == 0.0625f,
            "Linear-half packing lost the gamma-decoded true alpha");

    const auto gray = success(decode_png(make_png(4, 1, 0, Bytes{0, 0, 51, 128, 255})));
    const auto gamma =
        success(build_image_pyramid(gray, ImageEncoding::gamma, ImageWrap::clamp, {}, 2));
    // PBRT gamma's 1024-slot inverse LUT maps source code 51 through code 50.
    require_levels(
        gamma, std::array{ImageMipLevel{4, 1, 0}, ImageMipLevel{2, 1, 4}, ImageMipLevel{1, 1, 6}},
        Bytes{0, 50, 128, 255, 36, 202, 145});
    const auto gamma_sample =
        image_linear_scalar(gamma, 0.25f, 0.25f, 0.3f, 0, 0, 0.3f, ImageFilter::trilinear);
    require(gamma.source_channels == 1 && gamma.custom_transfer &&
                std::abs(gamma_sample.value_or(NAN) - 0.0997371376f) < 1e-7f,
            "Gamma forward samples or scalar trilinear mip blend differs from pinned PBRT");
    const auto base =
        success(build_image_base(gray, ImageEncoding::gamma, ImageWrap::clamp, {}, 2));
    const auto native_gamma = image_base_linear_rgb(base, 0.375f, 0.5f);
    require(base.texels[1] == 0x333333u && native_gamma &&
                std::abs(native_gamma->front() - 0.04f) < 1e-7f,
            "Native gamma view rounded source codes using the mip encoding");
    constexpr auto retained = 3 * sizeof(ImageMipLevel) + 7 * sizeof(std::uint32_t) + 1024;
    require(build_image_pyramid(gray, ImageEncoding::gamma, ImageWrap::clamp,
                                {.retained_bytes = retained - 1}, 2)
                    .limit_exceeded &&
                !build_image_pyramid(gray, ImageEncoding::gamma, ImageWrap::clamp, {}, 0).value &&
                !build_image_pyramid(gray, ImageEncoding::gamma, ImageWrap::clamp, {},
                                     std::numeric_limits<float>::infinity())
                     .value,
            "Gamma validation or transfer-table byte accounting failed");
}

void lookup_16_bit() {
    using Rgb = std::array<float, 3>;
    // The same 0/1 signals are stored at independently chosen scales in each half channel.
    ImagePyramid bytes{ImageEncoding::linear,
                       ImageWrap::repeat,
                       {{4, 1, 0}, {2, 1, 4}, {1, 1, 6}},
                       {0xff00ff, 0x00ffff, 0xff0000, 0x00ff00, 0xffffff, 0x0000ff, 0xffff00}};
    ImagePyramid halves{ImageEncoding::srgb,
                        ImageWrap::repeat,
                        {{4, 1, 0}, {2, 1, 8}, {1, 1, 12}},
                        {0x00003c00, 1, 0x38003c00, 0, 0, 1, 0x38000000, 0, 0x38003c00, 1,
                         0x00003c00, 0, 0x38000000, 1},
                        ImageStorage::linear_half};
    constexpr Rgb scales{1, 0.5f, 0x1p-24f};
    for (const auto wrap : {ImageWrap::repeat, ImageWrap::clamp, ImageWrap::black}) {
        bytes.wrap = halves.wrap = wrap;
        for (const auto filter : {ImageFilter::point, ImageFilter::bilinear, ImageFilter::trilinear,
                                  ImageFilter::ewa}) {
            for (const float s : {-0.125f, 0.125f, 0.625f, 1.125f}) {
                for (const float width : {0.0f, 0.15f, 0.3f, 0.8f, 2.0f}) {
                    const auto encoded =
                        image_linear_rgb(bytes, s, 0.5f, width, 0, 0, width, filter);
                    const auto linear =
                        image_linear_rgb(halves, s, 0.5f, width, 0, 0, width, filter);
                    if (!encoded || !linear) {
                        throw std::runtime_error("A valid half-float mip sample failed");
                    }
                    for (std::size_t c = 0; c < scales.size(); ++c) {
                        require(std::abs((*linear)[c] / scales[c] - (*encoded)[c]) <= 1e-6f,
                                "Filtering or wrapping changed with half-float mip storage");
                    }
                }
            }
        }
    }
    const auto sample = [](const ImagePyramid &image, ImageFilter filter, float width) {
        return image_linear_rgb(image, 0.125f, 0.5f, width, 0, 0, width, filter);
    };
    auto malformed = halves;
    malformed.texels.pop_back();
    for (const auto filter :
         {ImageFilter::point, ImageFilter::bilinear, ImageFilter::trilinear, ImageFilter::ewa}) {
        require(!sample(malformed, filter, 2), "A wide lookup accepted a truncated half texel");
    }
    require(!sample(malformed, ImageFilter::trilinear, 0.3f) &&
                !sample(malformed, ImageFilter::ewa, 0.8f) &&
                sample(malformed, ImageFilter::point, 0) == Rgb{1, 0, 0x1p-24f},
            "Half-float payload bounds must validate only the selected mip levels");
    malformed = halves;
    malformed.levels[0].offset = 1;
    require(!sample(malformed, ImageFilter::point, 0), "An unaligned half texel was accepted");
    malformed = halves;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    malformed.storage = static_cast<ImageStorage>(-1);
    require(!sample(malformed, ImageFilter::bilinear, 0), "An invalid mip storage was accepted");
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

void trilinear_lookup() {
    using Rgb = std::array<float, 3>;
    // Explicit encoded texels isolate filtering from pyramid generation.
    const ImagePyramid image{
        ImageEncoding::srgb,
        ImageWrap::black,
        {{8, 1, 0}, {4, 1, 8}, {2, 1, 12}, {1, 1, 14}},
        {0x000000, 0x0000ff, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0xff0000,
         0x000000, 0x000000, 0x000000, 0x00ff00, 0x000000, 0x800080},
    };
    const auto sample = [&image](float half_width, float s = 0.25f) {
        return image_linear_rgb(image, s, 0.5f, 0, 0, -half_width, 0, ImageFilter::trilinear);
    };
    const auto half_value = std::pow((128.0f / 255.0f + 0.055f) / 1.055f, 2.4f);
    require(sample(0) == Rgb{0.5f, 0, 0}, "Trilinear magnification did not bilerp the base");
    require(sample(0.0625f * std::numbers::sqrt2_v<float>) == Rgb{0.5f, 0, 0},
            "Trilinear blended the base LOD interval");
    require(sample(0.125f) == Rgb{0, 0, 0.5f}, "Wrong trilinear result at integer LOD one");
    const auto interior = sample(0.125f * std::exp2(0.25f));
    require(interior && (*interior)[0] == 0 && std::abs((*interior)[1] - 0.25f) < 1e-6f &&
                std::abs((*interior)[2] - 0.375f) < 1e-6f,
            "Wrong trilinear weights between interior mips");
    const float fractional_width = 0.25f * std::numbers::sqrt2_v<float>;
    const auto blended = sample(fractional_width);
    // LOD 2.5 blends equal weights. At s=0.25 the 1x1 lookup contributes
    // three quarters of its decoded value because the fourth quarter is black.
    require(blended && std::abs((*blended)[0] - 0.375f * half_value) < 1e-6f &&
                std::abs((*blended)[1] - 0.5f) < 1e-6f &&
                std::abs((*blended)[2] - 0.375f * half_value) < 1e-6f,
            "Trilinear did not blend two bilinear samples in linear RGB");
    require(sample(fractional_width, -2.0f) == Rgb{}, "Upper 1x1 mip ignored black wrap");
    require(sample(0.5f, -2.0f) == Rgb{image_transfer_table(ImageEncoding::srgb)[128], 0,
                                       image_transfer_table(ImageEncoding::srgb)[128]},
            "Trilinear did not return the top texel directly at the coarsest LOD");

    auto linear = image;
    linear.encoding = ImageEncoding::linear;
    // At s=1.25, repeat samples the green lower texel and clamp samples the black one.
    // Both wrap modes sample the sole upper texel at full strength.
    for (const auto wrap : {ImageWrap::repeat, ImageWrap::clamp}) {
        linear.wrap = wrap;
        const auto result = image_linear_rgb(linear, 1.25f, 0.5f, fractional_width, 0, 0, 0,
                                             ImageFilter::trilinear);
        const float green = wrap == ImageWrap::repeat ? 0.5f : 0.0f;
        require(result && std::abs((*result)[0] - 64.0f / 255.0f) < 1e-6f &&
                    std::abs((*result)[1] - green) < 1e-6f &&
                    std::abs((*result)[2] - 64.0f / 255.0f) < 1e-6f,
                "Wrong trilinear result for linear encoding with repeat or clamp");
    }

    const auto singleton =
        success(build_image_pyramid({1, 1, {255, 0, 0}}, ImageEncoding::linear, ImageWrap::black));
    require(image_linear_rgb(singleton, 0, 0, 0, 0, 0, 0, ImageFilter::trilinear) ==
                    Rgb{0.25f, 0, 0} &&
                image_linear_rgb(singleton, 0, 0, 0.5f, 0, 0, 0, ImageFilter::trilinear) ==
                    Rgb{1, 0, 0},
            "Singleton black wrap must follow the footprint");

    auto malformed = image;
    malformed.levels.back().offset = static_cast<std::uint32_t>(image.texels.size());
    require(!image_linear_rgb(malformed, 0.25f, 0.5f, fractional_width, 0, 0, 0,
                              ImageFilter::trilinear),
            "Trilinear accepted an out-of-bounds upper mip");
    require(image_linear_rgb(malformed, 0.25f, 0.5f, fractional_width, 0, 0, 0) == Rgb{0, 1, 0},
            "Bilinear must only read the selected mip");
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto invalid_filter = static_cast<ImageFilter>(-1);
    require(!image_linear_rgb(image, 0.25f, 0.5f, 0.5f, 0, 0, 0, invalid_filter),
            "Direct top-level lookup accepted an unknown filter");
}

void point_lookup() {
    using Rgb = std::array<float, 3>;
    constexpr Rgb red{1, 0, 0}, green{0, 1, 0}, blue{0, 0, 1}, white{1, 1, 1};
    ImagePyramid image{ImageEncoding::linear,
                       ImageWrap::repeat,
                       {{4, 1, 0}, {2, 1, 4}, {1, 1, 6}},
                       {0x0000ff, 0x00ff00, 0xff0000, 0xffffff, 0x0080ff, 0x00ffff, 0xff00ff}};
    const auto sample = [&image](float s, float t = 0.5f, float half_width = 0) {
        return image_linear_rgb(image, s, t, 0, 0, 0, -half_width, ImageFilter::point);
    };
    require(sample(-0.25f) == blue && sample(0.25f) == green,
            "Point lookup did not round signed ties away from zero");
    require(sample(std::nextafter(0.25f, 0.0f)) == red &&
                sample(std::nextafter(0.25f, 1.0f)) == green,
            "Point lookup changed texels on the wrong side of a tie");
    require(sample(0) == white && sample(1) == red && sample(0x1p-24f) == red &&
                sample(std::nextafter(1.0f, 0.0f)) == white,
            "Point repeat must wrap the rounded index, including at exact UV endpoints");
    require(sample(0.5f, 0.5f, 0.25f) == Rgb{1, 1, 0} &&
                sample(0.5f, 0.5f, 0.25f * std::numbers::sqrt2_v<float>) == Rgb{1, 1, 0},
            "Point lookup did not select one texel at integer and fractional LODs");
    ImagePyramid lods{ImageEncoding::linear,
                      ImageWrap::repeat,
                      {{16, 1, 0}, {8, 1, 16}, {4, 1, 24}, {2, 1, 28}, {1, 1, 30}},
                      std::vector<std::uint32_t>(31)};
    lods.texels[25] = 0x00ff00;
    lods.texels[28] = 0x0000ff;
    const auto lod_sample = [&lods](float width) {
        return image_linear_rgb(lods, 0.25f, 0.5f, 0.5f * width, 0, 0, 0, ImageFilter::point);
    };
    require(lod_sample(0.5f) == red, "Point lookup missed the exact higher mip transition");
    constexpr float width_below_transition = 0x1.fffffcp-2f;
    // Force a separate rounding in the reference. The logf result may vary by libm.
    const volatile float log_width = std::log(width_below_transition) * 1.4426950408889634f;
    const auto expected = 4.0f + log_width >= 3.0f ? red : green;
    require(lod_sample(width_below_transition) == expected,
            "Point LOD did not round the logarithm term before adding the coarsest mip");
    const float huge = std::numeric_limits<float>::max();
    for (const auto s : {0x1.fffffep28f, 0x1p29f, 0x1.000002p29f, huge}) {
        require(sample(s) == red && sample(-s) == red,
                "Large point repeat coordinate did not safely wrap to zero");
    }

    require(sample(0.25f, 0.5f, 0.3f) == Rgb{1, 128.0f / 255.0f, 0},
            "Point lookup did not use linear transfer decoding");
    image.encoding = ImageEncoding::srgb;
    const auto srgb_mid = std::pow((128.0f / 255.0f + 0.055f) / 1.055f, 2.4f);
    const auto decoded = sample(0.25f, 0.5f, 0.3f);
    require(decoded && (*decoded)[0] == 1 && std::abs((*decoded)[1] - srgb_mid) < 1e-6f &&
                (*decoded)[2] == 0,
            "Point lookup did not use sRGB transfer decoding");

    image.wrap = ImageWrap::clamp;
    require(sample(0) == red && sample(1) == white && sample(-huge, huge) == red &&
                sample(huge, -huge) == white,
            "Point clamp did not retain the first and last texels");
    image.wrap = ImageWrap::black;
    require(sample(0) == Rgb{} && sample(1) == Rgb{} && sample(-huge, huge) == Rgb{} &&
                sample(huge, -huge) == Rgb{} && sample(0x1p-24f) == red &&
                sample(std::nextafter(1.0f, 0.0f)) == white,
            "Point black wrapping did not preserve exact and adjacent UV edges");
    require(sample(huge, -huge, 0.5f) == Rgb{1, 0, 1},
            "Wide point footprint did not bypass black wrapping");

    const ImagePyramid singleton{ImageEncoding::linear, ImageWrap::black, {{1, 1, 0}}, {0x0000ff}};
    require(image_linear_rgb(singleton, 0, 0.5f, 0, 0, 0, 0, ImageFilter::point) == Rgb{} &&
                image_linear_rgb(singleton, 0.5f, 0.5f, 0, 0, 0, 0, ImageFilter::point) == red &&
                image_linear_rgb(singleton, 0, 0.5f, 0.5f, 0, 0, 0, ImageFilter::point) == red,
            "Standalone 1x1 point black wrapping did not follow the footprint");

    image.levels.back().offset = 7;
    require(sample(0.5f, 0.5f, 0.3f) == Rgb{1, 1, 0} && !sample(0.5f, 0.5f, 0.5f),
            "Point lookup must only read and validate the selected level");
    image.levels[1].offset = 6;
    require(!sample(0.5f, 0.5f, 0.3f), "Point lookup accepted an invalid selected mip");
    require(!sample(std::numeric_limits<float>::infinity()),
            "Point lookup accepted a nonfinite mapped coordinate");
}

void ewa_lookup() {
    using Rgb = std::array<float, 3>;
    ImagePyramid image{ImageEncoding::srgb,
                       ImageWrap::repeat,
                       {{16, 4, 0}, {8, 2, 64}, {4, 1, 80}, {2, 1, 84}, {1, 1, 86}},
                       std::vector<std::uint32_t>(87)};
    for (unsigned i = 0; i < image.texels.size(); ++i) {
        image.texels[i] = (i * 0x03182fu) & 0xffffffu;
    }
    constexpr float huge = std::numeric_limits<float>::max();
    constexpr float tiny = std::numeric_limits<float>::min();
    struct Case {
        ImageWrap wrap;
        float s;
        float t;
        float dsdx;
        float dtdx;
        float dsdy;
        float dtdy;
        float cap;
        std::string_view label;
    };
    const std::array cases{
        Case{ImageWrap::repeat, 0.283f, 0.41f, 0.019f, 0.037f, 0.46f, -0.21f, 8, "axis order"},
        Case{ImageWrap::repeat, 0.283f, 0.41f, 0.46f, -0.21f, 0.019f, 0.037f, 3, "cap"},
        Case{ImageWrap::repeat, -0.041f, 1.125f, 0.31f, -0.26f, 0.043f, -0.036f, 16,
             "negative center and nearly collinear axes"},
        Case{ImageWrap::repeat, 0.223f, 0.58f, 0.375f, -0.125f, 0.09375f, -0.03125f, 8,
             "collinear nonzero axes"},
        Case{ImageWrap::repeat, 0.223f, 0.58f, 0.375f, -0.125f, 0, 0, 8, "zero minor"},
        Case{ImageWrap::repeat, 0.459f, 0.722f, 0.03f, 0.05f, -0.016f, 0.014f, 8, "LOD zero"},
        Case{ImageWrap::repeat, 0.283f, 0.41f, 0.23f, 0.07f, 0, -0.087f, 8, "first LOD blend"},
        Case{ImageWrap::repeat, -65.12890625f, 71.87890625f, 0.23f, 0.07f, 0, -0.087f, 8,
             "signed distant repeat phase"},
        Case{ImageWrap::clamp, -65.12890625f, 0.519f, 0.23f, 0.07f, 0, -0.087f, 8,
             "distant low clamp phase"},
        Case{ImageWrap::clamp, 65.12890625f, 0.519f, 0.23f, 0.07f, 0, -0.087f, 8,
             "distant high clamp phase"},
        Case{ImageWrap::clamp, 1.03f, 0.519f, 0.23f, 0.07f, 0, -0.087f, 8, "near clamp edge"},
        Case{ImageWrap::black, -0.11f, 0.476f, 0.23f, 0.07f, 0, -0.087f, 8, "black overlap"},
        Case{ImageWrap::black, -3.0f, 0.476f, 0.23f, 0.07f, 0, -0.087f, 8, "black outside"},
        Case{ImageWrap::black, huge, -huge, 0.23f, 0.07f, 0, -0.087f, 8, "black huge"},
        Case{ImageWrap::clamp, huge, 0.37f, 0.23f, 0.07f, 0, -0.087f, 8, "clamp huge"},
        Case{ImageWrap::repeat, huge, -huge, 0.23f, 0.07f, 0, -0.087f, 8, "repeat huge"},
        Case{ImageWrap::black, -0.165f, 0.291f, 8.0f, 0.3f, 0, -1.0f, 16, "actual top"},
        Case{ImageWrap::black, -0.165f, 0.291f, 8.0f, 0.3f, 0, -1.44f, 16, "top blend"},
        Case{ImageWrap::black, huge, huge, 0, -2.0f, 4.0f, 0, 8, "wide boundary"},
        Case{ImageWrap::black, huge, huge, huge, -huge, tiny, tiny, 16, "wide overflow guard"},
        Case{ImageWrap::black, 0.223f, 0.58f, huge, -huge, 0, 0, 16, "zero before wide"},
        Case{ImageWrap::repeat, 0.43f, 0.53f, 0.46f, -0.21f, -tiny, tiny, 8, "cap with tiny minor"},
    };
    for (const auto &test : cases) {
        image.wrap = test.wrap;
        const auto got = image_linear_rgb(image, test.s, test.t, test.dsdx, test.dtdx, test.dsdy,
                                          test.dtdy, ImageFilter::ewa, test.cap);
        const auto expected = rypath::test::ewa_reference(
            image, test.s, test.t, test.dsdx, test.dtdx, test.dsdy, test.dtdy, test.cap);
        if (!got) {
            throw std::runtime_error(std::string(test.label));
        }
        const auto actual = *got;
        for (std::size_t c = 0; c < expected.size(); ++c) {
            require(std::abs(actual[c] - expected[c]) <= 2e-6, test.label);
        }
    }
    const ImagePyramid singleton{ImageEncoding::linear, ImageWrap::black, {{1, 1, 0}}, {0x0000ff}};
    const auto sample = [&](float minor) {
        return image_linear_rgb(singleton, -8.0f, 0.5f, 2.0f, 0, 0, minor, ImageFilter::ewa);
    };
    require(sample(0.5f) == Rgb{} && sample(1.0f) == Rgb{} && sample(2.0f) == Rgb{1, 0, 0},
            "Black 1x1 EWA must only bypass wrapping past the final mip");
    const auto half = sample(std::numbers::sqrt2_v<float>);
    require(half && std::abs((*half)[0] - 0.5f) <= 2e-6f,
            "The final black EWA mip must blend with the direct wide return");
    auto malformed = image;
    malformed.levels[1].height = 4;
    require(!image_linear_rgb(malformed, 0.25f, 0.5f, 0.06f, 0, 0, 0.04f, ImageFilter::ewa),
            "A malformed adjacent mip must fail even when its blend weight is zero");
    malformed = image;
    malformed.levels.back().width = 2;
    require(!image_linear_rgb(malformed, 0.25f, 0.5f, 2, 0, 0, 2, ImageFilter::ewa),
            "A direct wide EWA lookup must validate the top dimensions");
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    require(!image_linear_rgb(image, 0, 0, 1, inf, 0, 1, ImageFilter::ewa) &&
                !image_linear_rgb(image, inf, 0, 1, 0, 0, 1, ImageFilter::ewa) &&
                !image_linear_rgb(image, 0, 0, 1, 0, 0, 1, ImageFilter::ewa, nan),
            "EWA must reject nonfinite needed inputs");
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        require(argc == 2, "Expected a temporary test directory argument");
        decode_profiles(argv[1]);
        decode_adam7_profiles();
        decode_jpeg_profiles();
        jpeg_dct_work_limit();
        decode_16_bit_profiles();
        decode_indexed_profiles();
        decode_packed_grayscale_profiles();
        packed_decode_limits();
        decode_errors();
        deflate_work_limit();
        deflate_allocation_limit();
        mip_pyramid();
        mip_pyramid_16_bit();
        npot_black_and_native_view();
        scalar_channel_and_gamma();
        lookup_16_bit();
        lookup();
        trilinear_lookup();
        point_lookup();
        ewa_lookup();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
