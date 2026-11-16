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

enum class ImageEncoding { srgb, linear, gamma };
enum class ImageStorage { encoded8, linear_half, linear_float };
enum class ImageSampleFormat { unorm8, unorm16, half, float32 };
enum class ImageFileFormat { png, jpeg, exr, hdr };
enum class ImageWrap { repeat, clamp, black };
enum class ImageFilter { bilinear, trilinear, point, ewa };

inline constexpr std::uint32_t image_dimension_ceiling = 8192;
inline constexpr std::size_t image_file_ceiling = std::size_t{128} * 1024 * 1024;
inline constexpr std::size_t image_retained_ceiling = std::size_t{2} * 1024 * 1024 * 1024;
inline constexpr std::size_t image_scratch_ceiling = std::size_t{1} * 1024 * 1024 * 1024;

struct ImageLimits {
    std::uint32_t max_dimension = 4096;
    std::size_t retained_bytes = 256 * 1024 * 1024;
    std::size_t scratch_bytes = 256 * 1024 * 1024;
    std::size_t file_bytes = 16 * 1024 * 1024;
    // Per-allocation ceiling, independent of the conservative peak scratch estimate.
    std::size_t png_allocation_bytes = 128 * 1024 * 1024;
};

constexpr bool valid_image_limits(const ImageLimits &limits) {
    return limits.max_dimension > 0 && limits.max_dimension <= image_dimension_ceiling &&
           limits.file_bytes <= image_file_ceiling &&
           limits.retained_bytes <= image_retained_ceiling &&
           limits.scratch_bytes <= image_scratch_ceiling &&
           limits.png_allocation_bytes <= image_scratch_ceiling;
}

struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
    ImageSampleFormat sample_format = ImageSampleFormat::unorm8;
    std::uint32_t source_channels = 3; // PBRT default drops gray/palette alpha; glTF retains it.
    std::vector<std::uint8_t> alpha{}; // Unassociated alpha, in the same byte order as RGB.
    ImageFileFormat file_format = ImageFileFormat::png;
    // Floating sources retain exact decoded samples; HALF values widen exactly.
    // Byte and floating representations are mutually exclusive.
    std::vector<float> floating_rgb{};
    std::vector<float> floating_alpha{};
};

constexpr std::size_t image_storage_words(ImageStorage storage) {
    return storage == ImageStorage::linear_float ? 4 : storage == ImageStorage::linear_half ? 2 : 1;
}

inline std::size_t decoded_image_retained_bytes(const DecodedImage &image) {
    return image.rgb.size() + image.alpha.size() +
           (image.floating_rgb.size() + image.floating_alpha.size()) * sizeof(float);
}

struct ImageMipLevel {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t offset; // Offset in uint32_t words.
};

struct ImagePyramid {
    ImageEncoding encoding;
    ImageWrap wrap;
    std::vector<ImageMipLevel> levels;
    std::vector<std::uint32_t> texels;
    ImageStorage storage = ImageStorage::encoded8;
    std::uint32_t source_channels = 3; // Any retained alpha uses the high byte or half.
    std::shared_ptr<const std::array<float, 256>> custom_transfer{};
    bool native_base = false;
    bool gltf = false;
};

template <typename T> struct ImageResult {
    std::optional<T> value;
    std::string diagnostic;
    bool unsupported = false;
    bool limit_exceeded = false;
};

ImageResult<DecodedImage> decode_png(std::span<const std::uint8_t> bytes,
                                     const ImageLimits &limits = {},
                                     bool preserve_all_alpha = false);
ImageResult<DecodedImage> decode_jpeg(std::span<const std::uint8_t> bytes,
                                      const ImageLimits &limits = {});
ImageResult<DecodedImage> decode_exr(std::span<const std::uint8_t> bytes,
                                     const ImageLimits &limits = {});
ImageResult<DecodedImage> decode_hdr(std::span<const std::uint8_t> bytes,
                                     const ImageLimits &limits = {});
ImageEncoding image_effective_encoding(const DecodedImage &image, ImageEncoding authored);
ImageResult<ImagePyramid> build_image_pyramid(const DecodedImage &image, ImageEncoding encoding,
                                              ImageWrap wrap, const ImageLimits &limits = {},
                                              float gamma = 1);
ImageResult<ImagePyramid> build_image_base(const DecodedImage &image, ImageEncoding encoding,
                                           ImageWrap wrap, const ImageLimits &limits = {},
                                           float gamma = 1);
// Native PBRT goniometric Y map: gray is retained; RGB is averaged in decoded
// linear space and rounded back to the source storage. Alpha is ignored.
ImageResult<ImagePyramid> build_goniometric_image_base(const DecodedImage &image,
                                                       const ImageLimits &limits = {});
const std::array<float, 256> &image_transfer_table(ImageEncoding encoding);
std::optional<std::array<float, 3>> image_linear_rgb(const ImagePyramid &image, float s, float t,
                                                     float dsdx, float dtdx, float dsdy, float dtdy,
                                                     ImageFilter filter = ImageFilter::bilinear,
                                                     float max_anisotropy = 8);
std::optional<float> image_linear_scalar(const ImagePyramid &image, float s, float t, float dsdx,
                                         float dtdx, float dsdy, float dtdy,
                                         ImageFilter filter = ImageFilter::bilinear,
                                         float max_anisotropy = 8);
std::optional<std::array<float, 3>> image_base_linear_rgb(const ImagePyramid &image, float s,
                                                          float t);
std::optional<std::array<float, 3>> image_base_texel_linear_rgb(const ImagePyramid &image,
                                                                std::uint32_t x, std::uint32_t y);

// OpenGL sampler enums retained verbatim from the glTF sampler.
struct GltfSampler {
    std::uint32_t wrap_s = 10497;
    std::uint32_t wrap_t = 10497;
    std::uint32_t min_filter = 9987;
    std::uint32_t mag_filter = 9729;
};

ImageResult<ImagePyramid> build_gltf_image_pyramid(const DecodedImage &image,
                                                   ImageEncoding encoding,
                                                   const ImageLimits &limits = {});

struct ImageTexture {
    std::shared_ptr<const ImagePyramid> image;
    float uscale = 1;
    float vscale = 1;
    float udelta = 0;
    float vdelta = 0;
    float scale = 1;
    bool invert = false;
    ImageFilter filter = ImageFilter::bilinear;
    float max_anisotropy = 8;
    std::optional<GltfSampler> gltf_sampler{};
};

// Native top-left glTF UVs and derivatives; no PBRT mapping or V flip.
std::optional<std::array<float, 4>> gltf_image_rgba(const ImageTexture &texture, float s, float t,
                                                    float dsdx, float dtdx, float dsdy, float dtdy);

} // namespace rypath
