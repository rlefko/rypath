// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rypath {

enum class ImageEncoding { srgb, linear };
enum class ImageWrap { repeat, clamp, black };

struct ImageLimits {
    std::uint32_t max_dimension = 4096;
    std::size_t retained_bytes = 256 * 1024 * 1024;
    std::size_t scratch_bytes = 256 * 1024 * 1024;
};

struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
};

struct ImageMipLevel {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t offset;
};

struct ImagePyramid {
    ImageEncoding encoding;
    ImageWrap wrap;
    std::vector<ImageMipLevel> levels;
    std::vector<std::uint32_t> texels;
};

template <typename T> struct ImageResult {
    std::optional<T> value;
    std::string diagnostic;
    bool unsupported = false;
    bool limit_exceeded = false;
};

ImageResult<DecodedImage> decode_png(std::span<const std::uint8_t> bytes,
                                     const ImageLimits &limits = {});
ImageResult<ImagePyramid> build_image_pyramid(const DecodedImage &image, ImageEncoding encoding,
                                              ImageWrap wrap, const ImageLimits &limits = {});
const std::array<float, 256> &image_transfer_table(ImageEncoding encoding);
std::optional<std::array<float, 3>> image_linear_rgb(const ImagePyramid &image, float s, float t,
                                                     float dsdx, float dtdx, float dsdy,
                                                     float dtdy);

struct ImageTexture {
    std::shared_ptr<const ImagePyramid> image;
    float uscale = 1;
    float vscale = 1;
    float udelta = 0;
    float vdelta = 0;
    float scale = 1;
    bool invert = false;
};

} // namespace rypath
