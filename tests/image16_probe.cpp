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
    if (argc != 4) {
        std::fprintf(stderr, "Expected ENCODING WRAP INPUT.png\n");
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
    if (!decoded.value || decoded.value->bit_depth != 16) {
        std::fprintf(stderr, "%s: expected a 16-bit PNG (%s)\n", argv[3],
                     decoded.diagnostic.c_str());
        return 1;
    }
    const auto result = rypath::build_image_pyramid(
        *decoded.value,
        encoding == "linear" ? rypath::ImageEncoding::linear : rypath::ImageEncoding::srgb,
        wrap == "repeat" ? rypath::ImageWrap::repeat : rypath::ImageWrap::clamp);
    if (!result.value || result.value->storage != rypath::ImageStorage::linear_half) {
        std::fprintf(stderr, "%s: expected binary16 mip storage (%s)\n", argv[3],
                     result.diagnostic.c_str());
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
                const auto rg = result.value->texels.at(offset++);
                const auto b = result.value->texels.at(offset++);
                if (b > 0xffff) {
                    std::fprintf(stderr, "Unexpected bits above the blue channel\n");
                    return 1;
                }
                std::printf("%s%04x %04x %04x", x ? " " : "", rg & 0xffff, rg >> 16, b);
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
    std::fprintf(stderr, "Could not emit the binary16 mips: %s\n", error.what());
    return 1;
}
