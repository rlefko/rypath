// SPDX-License-Identifier: GPL-3.0-only
#include "image_texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace rypath;
using Bytes = std::vector<std::uint8_t>;
template <typename T> T sampled(std::optional<T> value) {
    if (!value)
        throw std::runtime_error("Expected a finite image lookup result");
    return *value;
}

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T success(ImageResult<T> result) {
    if (!result.value)
        throw std::runtime_error(result.diagnostic);
    return std::move(*result.value);
}
Bytes header(unsigned w = 2, unsigned h = 1, std::string metadata = {}, bool crlf = false) {
    std::string text = "#?RADIANCE\n# inert $(false)\nFORMAT=32-bit_rle_rgbe\n" + metadata +
                       "\n-Y " + std::to_string(h) + " +X " + std::to_string(w) + "\n";
    if (crlf) {
        std::string windows;
        for (char c : text) {
            if (c == '\n')
                windows += '\r';
            windows += c;
        }
        text = std::move(windows);
    }
    return Bytes(text.begin(), text.end());
}
void rgb_vectors() {
    auto bytes = header();
    bytes.insert(bytes.end(), {128, 64, 32, 129, 255, 17, 3, 0});
    auto image = success(decode_hdr(bytes));
    require(image.floating_rgb == std::vector<float>{1, .5f, .25f, 0, 0, 0},
            "RGBE exact conversion");
    require(image.rgb.empty() && image.sample_format == ImageSampleFormat::float32 &&
                image.file_format == ImageFileFormat::hdr &&
                decoded_image_retained_bytes(image) == 24,
            "HDR source format and bytes");
    auto minimum = header(1);
    minimum.insert(minimum.end(), {1, 2, 3, 1});
    require(success(decode_hdr(minimum)).floating_rgb[0] == std::ldexp(1.0f, -135),
            "minimum exponent");
    auto maximum = header(1);
    maximum.insert(maximum.end(), {255, 128, 0, 255});
    require(success(decode_hdr(maximum)).floating_rgb[0] == std::ldexp(255.0f, 119),
            "maximum exponent");
    auto windows = header(
        1, 1,
        "EXPOSURE=1\nCOLORCORR=1 1 1\nPIXASPECT=1\nPRIMARIES=.64 .33 .30 .60 .15 .06 .3127 .3290\n",
        true);
    windows.insert(windows.end(), {128, 0, 0, 129});
    require(success(decode_hdr(windows)).floating_rgb[0] == 1, "CRLF and identity metadata");
    for (std::size_t n = 0; n < bytes.size(); ++n)
        require(!decode_hdr(std::span(bytes).first(n)).value, "truncation accepted");
    bytes.push_back(0);
    require(!decode_hdr(bytes).value, "trailing byte accepted");
    bytes.pop_back();
    require(decode_hdr(bytes, {.max_dimension = 1}).limit_exceeded &&
                decode_hdr(bytes, {.retained_bytes = 23}).limit_exceeded &&
                decode_hdr(bytes, {.scratch_bytes = 7}).limit_exceeded,
            "HDR budgets");
    for (const auto &metadata :
         {"EXPOSURE=2\n", "COLORCORR=1 0 1\n", "PIXASPECT=2\n",
          "PRIMARIES=.64 .33 .30 .60 .15 .06 .333 .333\n", "SOFTWARE=unknown\n"}) {
        auto unsupported = header(1, 1, metadata);
        unsupported.insert(unsupported.end(), {128, 0, 0, 129});
        require(decode_hdr(unsupported).unsupported, "unsupported metadata accepted");
    }
    auto oversized = header(1, 1, std::string(4097, '#') + "\n");
    require(!decode_hdr(oversized).value, "oversized header line accepted");
    oversized = header(1, 1, std::string(65536, '\n'));
    require(!decode_hdr(oversized).value, "oversized header accepted");
    auto legacy = header(1);
    legacy.insert(legacy.end(), {1, 1, 1, 2});
    require(decode_hdr(legacy).unsupported, "legacy RLE accepted");
    auto xyze = header();
    const std::string rgb = "rgbe";
    auto it = std::search(xyze.begin(), xyze.end(), rgb.begin(), rgb.end());
    std::copy_n("xyze", 4, it);
    require(decode_hdr(xyze).unsupported, "XYZE accepted");
}
Bytes rle() {
    auto bytes = header(8, 2);
    for (int y = 0; y < 2; ++y) {
        bytes.insert(bytes.end(),
                     {2, 2, 0, 8, 136, 128, 8, 0, 1, 2, 3, 4, 5, 6, 7, 136, 32, 136, 129});
    }
    return bytes;
}
void rle_vectors() {
    auto bytes = rle();
    auto image = success(decode_hdr(bytes));
    require(image.floating_rgb[0] == 1 && image.floating_rgb[22] == 7.0f / 128 &&
                image.floating_rgb[26] == .25f,
            "RLE runs/literals and row order");
    const auto begin = header(8, 2).size();
    for (std::size_t n = begin; n < bytes.size(); ++n)
        require(!decode_hdr(std::span(bytes).first(n)).value, "RLE truncation accepted");
    bytes[begin + 4] = 0;
    require(!decode_hdr(bytes).value, "zero-progress packet accepted");
    bytes = rle();
    bytes[begin + 4] = 137;
    require(!decode_hdr(bytes).value, "overlong run accepted");
    bytes = rle();
    bytes[begin + 4] = 9;
    require(!decode_hdr(bytes).value, "overlong literal accepted");
    bytes = rle();
    bytes[begin + 3] = 7;
    require(!decode_hdr(bytes).value, "RLE width mismatch accepted");
    auto mixed = header(8, 2);
    mixed.insert(mixed.end(), 32, 0);
    mixed.insert(mixed.end(), {2, 2, 0, 8});
    require(!decode_hdr(mixed).value, "mixed coding accepted");
}
void floating_storage() {
    DecodedImage source{.width = 2,
                        .height = 1,
                        .rgb = {},
                        .sample_format = ImageSampleFormat::float32,
                        .source_channels = 4,
                        .file_format = ImageFileFormat::exr,
                        .floating_rgb = {-2, 70000.125f, .123456789f, 4, 90000.125f, .25f},
                        .floating_alpha = {-1, 3}};
    auto pyramid =
        success(build_image_pyramid(source, ImageEncoding::gamma, ImageWrap::clamp, {}, -1));
    require(pyramid.storage == ImageStorage::linear_float &&
                pyramid.encoding == ImageEncoding::linear && pyramid.texels.size() == 12 &&
                pyramid.source_channels == 4,
            "FLOAT storage/encoding");
    require(std::bit_cast<float>(pyramid.texels[0]) == -2 &&
                std::bit_cast<float>(pyramid.texels[1]) == 70000.125f &&
                std::bit_cast<float>(pyramid.texels[8]) == 1,
            "FLOAT exact base and signed mip");
    require(sampled(image_linear_scalar(pyramid, .25f, .5f, 0, 0, 0, 0, ImageFilter::bilinear)) ==
                -1,
            "RGBA scalar alpha");
    auto native = success(build_image_base(source, ImageEncoding::srgb, ImageWrap::clamp));
    require(native.source_channels == 3 && native.texels[3] == 0 && native.texels[7] == 0,
            "native RGB and cleared unused alpha");
    require(sampled(image_base_texel_linear_rgb(native, 0, 0))[1] == 70000.125f,
            "native FLOAT precision");
    require(sampled(image_linear_rgb(pyramid, .5f, .5f, 0, 0, 0, 0))[0] == 1, "signed bilinear");
    require(
        !build_image_base(source, ImageEncoding::linear, ImageWrap::clamp, {.retained_bytes = 31})
             .value,
        "true FLOAT retained bytes");
    source.floating_rgb[0] = std::numeric_limits<float>::quiet_NaN();
    require(!build_image_base(source, ImageEncoding::linear, ImageWrap::clamp).value,
            "NaN accepted");
    source.floating_rgb[0] = std::numeric_limits<float>::infinity();
    require(!build_image_base(source, ImageEncoding::linear, ImageWrap::clamp).value,
            "infinity accepted");
    source.sample_format = ImageSampleFormat::half;
    source.floating_rgb = {-65504, -std::ldexp(1.0f, -24), -0.0f, 65504, .5f, 1};
    auto half = success(build_image_pyramid(source, ImageEncoding::srgb, ImageWrap::clamp));
    require(half.storage == ImageStorage::linear_half && half.texels[0] == 0x8001fbffu &&
                half.texels[1] == 0xbc008000u,
            "signed HALF encoding");
    // Exact halfway mip averages round to the even binary16 significand.
    source.floating_rgb = {1, -1, std::ldexp(1.0f, -24), 1.0009765625f, -1.0009765625f, 0};
    auto ties = success(build_image_pyramid(source, ImageEncoding::linear, ImageWrap::clamp));
    require(ties.texels[4] == 0xbc003c00u && (ties.texels[5] & 0xffffu) == 0,
            "HALF normal and subnormal ties-to-even");
    source.floating_rgb = {1.0009765625f, -1.0009765625f, 0, 1.001953125f, -1.001953125f, 0};
    ties = success(build_image_pyramid(source, ImageEncoding::linear, ImageWrap::clamp));
    require(ties.texels[4] == 0xbc023c02u, "HALF odd ties advance to even");
    source.floating_rgb[0] = 65520;
    require(!build_image_base(source, ImageEncoding::linear, ImageWrap::clamp).value,
            "HALF overflow accepted");
    source.floating_rgb[0] = 1.0001f;
    require(!build_image_base(source, ImageEncoding::linear, ImageWrap::clamp).value,
            "inexact source HALF accepted");
    source.sample_format = ImageSampleFormat::float32;
    source.width = 3;
    source.source_channels = 3;
    source.floating_alpha.clear();
    source.floating_rgb.assign(9, -2);
    auto npot = success(build_image_pyramid(source, ImageEncoding::linear, ImageWrap::clamp));
    require(npot.levels[0].width == 4 &&
                sampled(image_linear_rgb(npot, .5f, .5f, 0, 0, 0, 0))[0] == 0,
            "NPOT PBRT resize clamps signed output");
    source.width = 2;
    source.sample_format = ImageSampleFormat::float32;
    source.floating_rgb.assign(6, std::numeric_limits<float>::max());
    require(build_image_base(source, ImageEncoding::linear, ImageWrap::clamp).value.has_value(),
            "finite FLOAT extrema lost");
    require(!build_image_pyramid(source, ImageEncoding::linear, ImageWrap::clamp).value,
            "mip overflow accepted");
}
void lookup_overflow() {
    DecodedImage source{.width = 2,
                        .height = 1,
                        .rgb = {},
                        .sample_format = ImageSampleFormat::float32,
                        .floating_rgb = {1.2e38f, 1.2e38f, 1.2e38f, -1.2e38f, -1.2e38f, -1.2e38f}};
    const auto image =
        success(build_image_pyramid(source, ImageEncoding::linear, ImageWrap::clamp));
    require(image_linear_rgb(image, .25f, .5f, 0, 0, 0, 0).has_value(),
            "Finite FLOAT RGB lookup unexpectedly failed");
    require(!image_linear_scalar(image, .25f, .5f, 0, 0, 0, 0),
            "Overflowing FLOAT RGB scalar mean must fail");
    auto corrupted = image;
    corrupted.texels[0] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    require(!image_linear_rgb(corrupted, .25f, .5f, 0, 0, 0, 0), "Nonfinite RGB lookup must fail");
    auto native = success(build_image_base(source, ImageEncoding::linear, ImageWrap::clamp));
    native.texels[0] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    require(!image_base_linear_rgb(native, .25f, .5f) && !image_base_texel_linear_rgb(native, 0, 0),
            "Nonfinite native RGB lookups must fail");
}

} // namespace
int main() {
    try {
        rgb_vectors();
        rle_vectors();
        floating_storage();
        lookup_overflow();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
