// SPDX-License-Identifier: GPL-3.0-only

#include "image_texture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

int main(int argc, char **argv) try {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "Expected ENCODING WRAP INPUT.png [u256|source-u256]\n");
        return 1;
    }
    const std::string_view mode = argc == 5 ? argv[4] : "";
    if (argc == 5 && mode != "u256" && mode != "source-u256") {
        std::fprintf(stderr, "Expected u256 or source-u256 probe mode\n");
        return 1;
    }
    const std::string_view encoding(argv[1]);
    const std::string_view wrap(argv[2]);
    if ((encoding != "linear" && encoding != "sRGB") || (wrap != "repeat" && wrap != "clamp")) {
        std::fprintf(stderr, "Expected linear or sRGB encoding and repeat or clamp wrap\n");
        return 1;
    }
    std::ifstream input(argv[3], std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "Could not read %s\n", argv[3]);
        return 1;
    }
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, {}};
    const auto decoded = rypath::decode_png(bytes);
    const bool half = mode.empty();
    if (!decoded.value ||
        decoded.value->sample_format !=
            (half ? rypath::ImageSampleFormat::unorm16 : rypath::ImageSampleFormat::unorm8)) {
        std::fprintf(stderr,
                     half ? "%s: expected a 16-bit PNG (%s)\n"
                          : "%s: expected a PNG decoding to RGB8 (%s)\n",
                     argv[3], decoded.diagnostic.c_str());
        return 1;
    }
    if (mode == "source-u256") {
        const auto &image = *decoded.value;
        std::printf("%u %u\n", image.width, image.height);
        std::size_t offset = 0;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x, offset += 3) {
                std::printf("%s%04x %04x %04x", x ? " " : "", unsigned(image.rgb.at(offset)),
                            unsigned(image.rgb.at(offset + 1)), unsigned(image.rgb.at(offset + 2)));
            }
            std::printf("\n");
        }
        if (offset != image.rgb.size()) {
            std::fprintf(stderr, "%s: unexpected trailing bytes in decoded RGB storage\n", argv[3]);
            return 1;
        }
        return std::ferror(stdout) ? 1 : 0;
    }
    const auto result = rypath::build_image_pyramid(
        *decoded.value,
        encoding == "linear" ? rypath::ImageEncoding::linear : rypath::ImageEncoding::srgb,
        wrap == "repeat" ? rypath::ImageWrap::repeat : rypath::ImageWrap::clamp);
    const auto storage = half ? rypath::ImageStorage::linear_half : rypath::ImageStorage::encoded8;
    if (!result.value || result.value->storage != storage) {
        std::fprintf(stderr,
                     half ? "%s: expected binary16 mip storage (%s)\n"
                          : "%s: expected U256 mip storage (%s)\n",
                     argv[3], result.diagnostic.c_str());
        return 1;
    }
    std::size_t offset = 0;
    for (const auto &level : result.value->levels) {
        if (level.offset != offset) {
            std::fprintf(stderr, "%s: expected separately packed, contiguous mip levels\n",
                         argv[3]);
            return 1;
        }
        std::printf("%u %u\n", level.width, level.height);
        for (std::uint32_t y = 0; y < level.height; ++y) {
            for (std::uint32_t x = 0; x < level.width; ++x) {
                const auto word = result.value->texels.at(offset++);
                if (half) {
                    const auto b = result.value->texels.at(offset++);
                    if (b > 0xffff) {
                        std::fprintf(stderr, "Unexpected bits above the blue channel\n");
                        return 1;
                    }
                    std::printf("%s%04x %04x %04x", x ? " " : "", word & 0xffff, word >> 16, b);
                } else {
                    if (word >> 24 != 0) {
                        std::fprintf(stderr, "Unexpected bits above the blue channel\n");
                        return 1;
                    }
                    std::printf("%s%04x %04x %04x", x ? " " : "", word & 0xff, (word >> 8) & 0xff,
                                (word >> 16) & 0xff);
                }
            }
            std::printf("\n");
        }
    }
    if (offset != result.value->texels.size()) {
        std::fprintf(stderr, "%s: unexpected trailing words in packed mip storage\n", argv[3]);
        return 1;
    }
    return std::ferror(stdout) ? 1 : 0;
} catch (const std::exception &error) {
    std::fprintf(stderr,
                 argc == 4 ? "Could not emit the binary16 mips: %s\n"
                           : "Could not emit RGB codes: %s\n",
                 error.what());
    return 1;
}
