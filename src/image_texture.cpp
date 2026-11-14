// SPDX-License-Identifier: Apache-2.0
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Mipmap filtering and color encoding adapted for rypath. See third_party/pbrt/README.md.

#include "image_texture.hpp"

#include "ewa_weights.h"

#include <lodepng.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <utility>

namespace rypath {
namespace {

constexpr std::uint32_t max_image_dimension = 4096;
constexpr auto max_png_bytes = std::size_t{16} * 1024 * 1024;
constexpr auto max_image_bytes = std::size_t{256} * 1024 * 1024;

using Rgb = std::array<float, 3>;

struct PngState {
    LodePNGState state;
    PngState() { lodepng_state_init(&state); }
    ~PngState() { lodepng_state_cleanup(&state); }
    PngState(const PngState &) = delete;
    PngState &operator=(const PngState &) = delete;
};

template <typename T> ImageResult<T> budget_error(std::string diagnostic) {
    return {.value = {}, .diagnostic = std::move(diagnostic), .limit_exceeded = true};
}

bool dimensions_fit(std::uint32_t width, std::uint32_t height, const ImageLimits &limits) {
    const auto max_dimension = std::min(limits.max_dimension, max_image_dimension);
    return width <= max_dimension && height <= max_dimension;
}

bool valid_profile(ImageEncoding encoding, ImageWrap wrap) {
    return (encoding == ImageEncoding::srgb || encoding == ImageEncoding::linear) &&
           (wrap == ImageWrap::repeat || wrap == ImageWrap::clamp || wrap == ImageWrap::black);
}

ImageResult<DecodedImage> png_error(std::string diagnostic, bool unsupported = false) {
    return {.value = {}, .diagnostic = std::move(diagnostic), .unsupported = unsupported};
}

ImageResult<DecodedImage> png_error(unsigned code) {
    return png_error(std::string("Invalid PNG: ") + lodepng_error_text(code));
}

bool valid_chunk_type(std::span<const std::uint8_t, 4> type) {
    auto uppercase = [](std::uint8_t byte) { return byte >= 'A' && byte <= 'Z'; };
    return uppercase(type[2]) && std::ranges::all_of(type, [&](std::uint8_t byte) {
               return uppercase(byte) || (byte >= 'a' && byte <= 'z');
           });
}

// The header has already passed lodepng_inspect. Walk the remaining chunks to reject APNG,
// check CRCs for skipped ancillary chunks, and validate the IDAT run and IEND boundary.
ImageResult<DecodedImage> inspect_png_chunks(std::span<const std::uint8_t> bytes,
                                             const LodePNGColorMode &color) {
    constexpr std::size_t header_size = 33;
    constexpr std::size_t chunk_overhead = 12;
    std::size_t compressed_size = 0;
    std::size_t palette_entries = 0;
    bool seen_idat = false;
    bool closed_idat = false;
    bool seen_transparency = false;
    bool animated = false;
    for (std::size_t offset = header_size; bytes.size() - offset >= chunk_overhead;) {
        const auto *chunk = bytes.data() + offset;
        const std::size_t length = lodepng_chunk_length(chunk);
        if (length > bytes.size() - offset - chunk_overhead) {
            return png_error("Truncated PNG chunk");
        }
        offset += length + chunk_overhead;
        if (lodepng_chunk_check_crc(chunk)) {
            return png_error("Invalid PNG chunk CRC");
        }
        if (!valid_chunk_type(std::span<const std::uint8_t, 4>(chunk + 4, 4))) {
            return png_error("Invalid PNG chunk type");
        }
        if (lodepng_chunk_type_equals(chunk, "PLTE")) {
            const bool grayscale = color.colortype == LCT_GREY || color.colortype == LCT_GREY_ALPHA;
            const bool invalid_indexed_size =
                color.colortype == LCT_PALETTE && length / 3 > (1u << color.bitdepth);
            if (seen_idat || palette_entries != 0 || seen_transparency || grayscale ||
                length == 0 || length > 768 || length % 3 != 0 || invalid_indexed_size) {
                return png_error("Invalid PNG palette chunk");
            }
            palette_entries = length / 3;
        } else if (lodepng_chunk_type_equals(chunk, "tRNS")) {
            const bool valid_length = (color.colortype == LCT_GREY && length == 2) ||
                                      (color.colortype == LCT_RGB && length == 6) ||
                                      (color.colortype == LCT_PALETTE && palette_entries != 0 &&
                                       length <= palette_entries);
            if (seen_idat || seen_transparency || !valid_length) {
                return png_error("Invalid PNG transparency chunk");
            }
            seen_transparency = true;
        } else if (lodepng_chunk_type_equals(chunk, "IDAT")) {
            if (closed_idat) {
                return png_error("PNG IDAT chunks must be consecutive");
            }
            seen_idat = true;
            compressed_size += length;
        } else if (lodepng_chunk_type_equals(chunk, "IEND")) {
            if (length != 0 || offset != bytes.size()) {
                return png_error("Invalid PNG end chunk");
            }
            if (compressed_size < 6) {
                return png_error("Missing or truncated PNG image data");
            }
            if (color.colortype == LCT_PALETTE && palette_entries == 0) {
                return png_error("Missing PNG palette chunk");
            }
            if (animated) {
                return png_error("Animated PNG is unsupported", true);
            }
            return {};
        } else {
            if (!lodepng_chunk_ancillary(chunk)) {
                return png_error("Unexpected critical PNG chunk");
            }
            closed_idat = seen_idat;
            animated |= lodepng_chunk_type_equals(chunk, "acTL") ||
                        lodepng_chunk_type_equals(chunk, "fcTL") ||
                        lodepng_chunk_type_equals(chunk, "fdAT");
        }
    }
    return png_error("Missing or truncated PNG end chunk");
}

constexpr std::array<float, 256> linear_table = [] {
    std::array<float, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = static_cast<float>(i) / 255.0f;
    }
    return table;
}();

// The pinned PBRT U256 sRGB transfer table is shared by CPU and Metal lookup.
constexpr std::array<float, 256> srgb_table = {

    0.0000000000f, 0.0003035270f, 0.0006070540f, 0.0009105810f, 0.0012141080f, 0.0015176350f,
    0.0018211619f, 0.0021246888f, 0.0024282159f, 0.0027317430f, 0.0030352699f, 0.0033465356f,
    0.0036765069f, 0.0040247170f, 0.0043914421f, 0.0047769533f, 0.0051815170f, 0.0056053917f,
    0.0060488326f, 0.0065120910f, 0.0069954102f, 0.0074990317f, 0.0080231922f, 0.0085681248f,
    0.0091340570f, 0.0097212177f, 0.0103298230f, 0.0109600937f, 0.0116122449f, 0.0122864870f,
    0.0129830306f, 0.0137020806f, 0.0144438436f, 0.0152085144f, 0.0159962922f, 0.0168073755f,
    0.0176419523f, 0.0185002182f, 0.0193823613f, 0.0202885624f, 0.0212190095f, 0.0221738834f,
    0.0231533647f, 0.0241576303f, 0.0251868572f, 0.0262412224f, 0.0273208916f, 0.0284260381f,
    0.0295568332f, 0.0307134409f, 0.0318960287f, 0.0331047624f, 0.0343398079f, 0.0356013142f,
    0.0368894450f, 0.0382043645f, 0.0395462364f, 0.0409151986f, 0.0423114114f, 0.0437350273f,
    0.0451862030f, 0.0466650836f, 0.0481718220f, 0.0497065634f, 0.0512694679f, 0.0528606549f,
    0.0544802807f, 0.0561284944f, 0.0578054339f, 0.0595112406f, 0.0612460710f, 0.0630100295f,
    0.0648032799f, 0.0666259527f, 0.0684781820f, 0.0703601092f, 0.0722718611f, 0.0742135793f,
    0.0761853904f, 0.0781874284f, 0.0802198276f, 0.0822827145f, 0.0843762159f, 0.0865004659f,
    0.0886556059f, 0.0908417329f, 0.0930589810f, 0.0953074843f, 0.0975873619f, 0.0998987406f,
    0.1022417471f, 0.1046164930f, 0.1070231125f, 0.1094617173f, 0.1119324341f, 0.1144353822f,
    0.1169706732f, 0.1195384338f, 0.1221387982f, 0.1247718409f, 0.1274376959f, 0.1301364899f,
    0.1328683347f, 0.1356333494f, 0.1384316236f, 0.1412633061f, 0.1441284865f, 0.1470272839f,
    0.1499598026f, 0.1529261619f, 0.1559264660f, 0.1589608639f, 0.1620294005f, 0.1651322246f,
    0.1682693958f, 0.1714410931f, 0.1746473908f, 0.1778884083f, 0.1811642349f, 0.1844749898f,
    0.1878207624f, 0.1912016720f, 0.1946178079f, 0.1980693042f, 0.2015562356f, 0.2050787061f,
    0.2086368501f, 0.2122307271f, 0.2158605307f, 0.2195262313f, 0.2232279778f, 0.2269658893f,
    0.2307400703f, 0.2345506549f, 0.2383976579f, 0.2422811985f, 0.2462013960f, 0.2501583695f,
    0.2541521788f, 0.2581829131f, 0.2622507215f, 0.2663556635f, 0.2704978585f, 0.2746773660f,
    0.2788943350f, 0.2831487954f, 0.2874408960f, 0.2917706966f, 0.2961383164f, 0.3005438447f,
    0.3049873710f, 0.3094689548f, 0.3139887452f, 0.3185468316f, 0.3231432438f, 0.3277781308f,
    0.3324515820f, 0.3371636569f, 0.3419144452f, 0.3467040956f, 0.3515326977f, 0.3564002514f,
    0.3613068759f, 0.3662526906f, 0.3712377846f, 0.3762622178f, 0.3813261092f, 0.3864295185f,
    0.3915725648f, 0.3967553079f, 0.4019778669f, 0.4072403014f, 0.4125427008f, 0.4178851545f,
    0.4232677519f, 0.4286905527f, 0.4341537058f, 0.4396572411f, 0.4452012479f, 0.4507858455f,
    0.4564110637f, 0.4620770514f, 0.4677838385f, 0.4735315442f, 0.4793202281f, 0.4851499796f,
    0.4910208881f, 0.4969330430f, 0.5028865933f, 0.5088814497f, 0.5149177909f, 0.5209956765f,
    0.5271152258f, 0.5332764983f, 0.5394796133f, 0.5457245708f, 0.5520114899f, 0.5583404899f,
    0.5647116303f, 0.5711249113f, 0.5775805116f, 0.5840784907f, 0.5906189084f, 0.5972018838f,
    0.6038274169f, 0.6104956269f, 0.6172066331f, 0.6239604354f, 0.6307572126f, 0.6375969648f,
    0.6444797516f, 0.6514056921f, 0.6583748460f, 0.6653873324f, 0.6724432111f, 0.6795425415f,
    0.6866854429f, 0.6938719153f, 0.7011020184f, 0.7083759308f, 0.7156936526f, 0.7230552435f,
    0.7304608822f, 0.7379105687f, 0.7454043627f, 0.7529423237f, 0.7605246305f, 0.7681512833f,
    0.7758223414f, 0.7835379243f, 0.7912980318f, 0.7991028428f, 0.8069523573f, 0.8148466945f,
    0.8227858543f, 0.8307699561f, 0.8387991190f, 0.8468732834f, 0.8549926877f, 0.8631572723f,
    0.8713672161f, 0.8796223402f, 0.8879231811f, 0.8962693810f, 0.9046613574f, 0.9130986929f,
    0.9215820432f, 0.9301108718f, 0.9386858940f, 0.9473065734f, 0.9559735060f, 0.9646862745f,
    0.9734454751f, 0.9822505713f, 0.9911022186f, 1.0000000000f};

template <std::size_t Size> float evaluate_polynomial(float x, const std::array<float, Size> &c) {
    float result = c.back();
    for (std::size_t i = Size - 1; i > 0; --i) {
        result = std::fma(x, result, c[i - 1]);
    }
    return result;
}

float encode_srgb(float value) {
    if (value <= 0.0031308f) {
        return 12.92f * value;
    }
    // Match PBRT's minimax approximation at byte quantization boundaries.
    constexpr std::array numerator{-0.0016829072605308378f, 0.03453868659826638f,
                                   0.7642611304733891f,     2.0041169284241644f,
                                   0.7551545191665577f,     -0.016202083165206348f};
    constexpr std::array denominator{4.178892964897981e-7f, -0.00004375359692957097f,
                                     0.03467195408529984f,  0.6085338522168684f,
                                     1.8970238036421054f,   1.0f};
    const float root = std::sqrt(value);
    return evaluate_polynomial(root, numerator) / evaluate_polynomial(root, denominator) * value;
}

float decode_srgb(float value) {
    if (value <= 0.04045f) {
        return value * (1.0f / 12.92f);
    }
    // Match PBRT's continuous transfer for 16-bit PNG input.
    constexpr std::array numerator{-0.0163933279112946f, -0.7386328024653209f, -11.199318357635072f,
                                   -47.46726633009393f, -36.04572663838034f};
    constexpr std::array denominator{-0.004261480793199332f, -19.140923959601675f,
                                     -59.096406619244426f, -18.225745396846637f, 1.0f};
    return evaluate_polynomial(value, numerator) / evaluate_polynomial(value, denominator) * value;
}

static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == sizeof(std::uint32_t));

std::uint32_t round_significand(std::uint32_t significand, unsigned shift) {
    const auto truncated = significand >> shift;
    const auto halfway = 1u << (shift - 1);
    return (significand + halfway - 1u + (truncated & 1u)) >> shift;
}

// Source and mip values are nonnegative, finite, and well within the binary16 range.
std::uint32_t encode_half(float value) {
    constexpr auto fraction_mask = 0x007fffffu;
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = (bits >> 23) & 0xffu;
    const auto fraction = bits & fraction_mask;
    if (exponent < 102u) {
        return 0; // Below half of the smallest binary16 subnormal.
    }
    if (exponent <= 112u) {
        return round_significand(fraction | (1u << 23), 126u - exponent);
    }
    // A carry from rounding the fraction advances the exponent.
    return ((exponent - 112u) << 10) + round_significand(fraction, 13);
}

float decode_half(std::uint32_t bits) {
    const auto sign = (bits & 0x8000u) << 16;
    const auto exponent = (bits >> 10) & 0x1fu;
    const auto fraction = bits & 0x3ffu;
    if (exponent == 0u) {
        if (fraction == 0u) {
            return std::bit_cast<float>(sign);
        }
        const auto shift = std::countl_zero(fraction) - 21;
        const auto normalized = (fraction << shift) & 0x3ffu;
        return std::bit_cast<float>(sign | ((113u - shift) << 23) | (normalized << 13));
    }
    const auto float_exponent = exponent == 31u ? 255u : exponent + 112u;
    return std::bit_cast<float>(sign | (float_exponent << 23) | (fraction << 13));
}

std::uint32_t encode_byte(float value, ImageEncoding encoding) {
    value = std::clamp(value, 0.0f, 1.0f);
    if (encoding == ImageEncoding::srgb) {
        return static_cast<std::uint32_t>(std::round(255.0f * encode_srgb(value)));
    }
    return static_cast<std::uint32_t>(std::min(255.0f, 255.0f * value + 0.5f));
}

std::uint32_t pack_rgb(std::uint32_t r, std::uint32_t g, std::uint32_t b) {
    return r | (g << 8) | (b << 16);
}

Rgb decode_byte_texel(std::uint32_t texel, const std::array<float, 256> &transfer) {
    return {transfer[texel & 0xffu], transfer[(texel >> 8) & 0xffu],
            transfer[(texel >> 16) & 0xffu]};
}

std::size_t pixel_count(std::uint32_t width, std::uint32_t height) {
    return static_cast<std::size_t>(width) * height;
}

struct SourcePixels {
    std::span<const std::uint8_t> bytes;
    ImageEncoding encoding;
    const std::array<float, 256> &transfer;
    bool is_half;

    Rgb operator[](std::size_t index) const {
        if (!is_half) {
            const auto start = index * 3;
            return {transfer[bytes[start]], transfer[bytes[start + 1]], transfer[bytes[start + 2]]};
        }
        Rgb rgb{};
        for (std::size_t c = 0; c < rgb.size(); ++c) {
            const auto start = index * 6 + c * 2;
            const auto sample = (static_cast<unsigned>(bytes[start]) << 8) | bytes[start + 1];
            const float value = static_cast<float>(sample) / 65535.0f;
            const float linear = encoding == ImageEncoding::srgb ? decode_srgb(value) : value;
            // PBRT rounds the source to half before any float resize or downsample.
            rgb[c] = decode_half(encode_half(linear));
        }
        return rgb;
    }
};

int remap(int coordinate, int size, ImageWrap wrap) {
    if (wrap == ImageWrap::repeat) {
        const int remainder = coordinate % size;
        return remainder < 0 ? remainder + size : remainder;
    }
    return std::clamp(coordinate, 0, size - 1);
}

float sinc(float x) {
    const float argument = std::numbers::pi_v<float> * x;
    return 1.0f - argument * argument == 1.0f ? 1.0f : std::sin(argument) / argument;
}

struct ResampleWeight {
    int first;
    std::array<float, 4> weights;
};

std::vector<ResampleWeight> resample_weights(std::uint32_t old_size, std::uint32_t new_size) {
    std::vector<ResampleWeight> result(new_size);
    for (std::uint32_t i = 0; i < new_size; ++i) {
        constexpr float radius = 2.0f;
        constexpr float tau = 2.0f;
        const float center = (static_cast<float>(i) + 0.5f) * static_cast<float>(old_size) /
                             static_cast<float>(new_size);
        auto &sample = result[i];
        sample.first = static_cast<int>(std::floor(center - radius + 0.5f));
        float sum = 0.0f;
        for (int tap = 0; tap < 4; ++tap) {
            const float x = static_cast<float>(sample.first + tap) + 0.5f - center;
            sample.weights[tap] = std::abs(x) <= radius ? sinc(x) * sinc(x / tau) : 0.0f;
            sum += sample.weights[tap];
        }
        for (auto &weight : sample.weights) {
            weight *= 1.0f / sum;
        }
    }
    return result;
}

void resize_row(const SourcePixels &source, std::uint32_t source_width,
                const std::vector<ResampleWeight> &horizontal, ImageWrap wrap,
                std::span<Rgb> destination) {
    for (std::size_t x = 0; x < horizontal.size(); ++x) {
        const auto &sample = horizontal[x];
        Rgb rgb{};
        for (int tap = 0; tap < 4; ++tap) {
            const int in_x = remap(sample.first + tap, static_cast<int>(source_width), wrap);
            const auto in_rgb = source[static_cast<std::size_t>(in_x)];
            for (std::size_t channel = 0; channel < rgb.size(); ++channel) {
                rgb[channel] += sample.weights[tap] * in_rgb[channel];
            }
        }
        destination[x] = rgb;
    }
}

std::vector<Rgb> resize_image(const DecodedImage &image, std::uint32_t width, std::uint32_t height,
                              ImageEncoding encoding, ImageWrap wrap) {
    const auto &transfer = image_transfer_table(encoding);
    const auto row_bytes = static_cast<std::size_t>(image.width) * 3 * (image.bit_depth / 8);
    const auto horizontal = resample_weights(image.width, width);
    const auto vertical = resample_weights(image.height, height);
    std::vector<Rgb> output(pixel_count(width, height));

    // A four-row ring keeps both separable passes within the scratch budget at 4096 x 4096.
    std::vector<Rgb> rows(pixel_count(width, 4));
    int last_row = vertical.front().first - 1;
    auto row_slot = [](int y) { return static_cast<std::uint32_t>(y) % 4u; };
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto &sample = vertical[y];
        while (last_row < sample.first + 3) {
            ++last_row;
            const auto source_y =
                static_cast<std::uint32_t>(remap(last_row, static_cast<int>(image.height), wrap));
            const SourcePixels source_row{
                std::span(image.rgb).subspan(row_bytes * source_y, row_bytes), encoding, transfer,
                image.bit_depth == 16};
            auto destination_row =
                std::span(rows).subspan(pixel_count(width, row_slot(last_row)), width);
            resize_row(source_row, image.width, horizontal, wrap, destination_row);
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            auto &rgb = output[pixel_count(width, y) + x];
            for (int tap = 0; tap < 4; ++tap) {
                const auto &source_rgb = rows[width * row_slot(sample.first + tap) + x];
                for (std::size_t channel = 0; channel < rgb.size(); ++channel) {
                    rgb[channel] += sample.weights[tap] * source_rgb[channel];
                }
            }
            for (auto &channel : rgb) {
                channel = std::max(0.0f, channel);
            }
        }
    }
    return output;
}

std::vector<Rgb> downsample(std::span<const Rgb> input, const ImageMipLevel &current,
                            const ImageMipLevel &next) {
    std::vector<Rgb> output(pixel_count(next.width, next.height));
    for (std::uint32_t y = 0; y < next.height; ++y) {
        const auto row = pixel_count(current.width, 2 * y);
        const auto next_row = row + (current.height > 1 ? current.width : 0);
        for (std::uint32_t x = 0; x < next.width; ++x) {
            const auto column = 2 * x;
            const auto next_column = column + (current.width > 1 ? 1 : 0);
            auto &rgb = output[pixel_count(next.width, y) + x];
            for (std::size_t c = 0; c < rgb.size(); ++c) {
                rgb[c] = (input[row + column][c] + input[row + next_column][c] +
                          input[next_row + column][c] + input[next_row + next_column][c]) *
                         0.25f;
            }
        }
    }
    return output;
}

float bounded_coordinate(float value, ImageWrap wrap) {
    switch (wrap) {
    case ImageWrap::repeat:
        return value - std::floor(value);
    case ImageWrap::clamp:
        return std::clamp(value, 0.0f, 1.0f);
    case ImageWrap::black:
        return std::clamp(value, -1.0f, 2.0f);
    }
    return 0.0f;
}

int point_coordinate(float value, ImageWrap wrap, int size) {
    if (wrap != ImageWrap::repeat) {
        value = bounded_coordinate(value, wrap);
    }
    const float dimension = static_cast<float>(size);
    const float index = std::round(value * dimension - 0.5f);
    if (wrap == ImageWrap::repeat) {
        // Apply repeat after rounding to preserve PBRT's signed half-texel ties.
        // If scaling overflows, the float's spacing already spans whole periods
        // of the power-of-two mip, so its wrapped index is zero.
        return std::isfinite(index) ? static_cast<int>(std::fmod(index, dimension)) : 0;
    }
    return static_cast<int>(index);
}

float point_log_width(float width) {
    // Force binary32 rounding even when the compiler contracts across statements.
    const volatile float log_width = std::log(width) * 1.4426950408889634f;
    return log_width;
}

struct MipSampler {
    std::span<const std::uint32_t> texels;
    const std::array<float, 256> &transfer;
    int width;
    int height;
    ImageWrap wrap;
    std::size_t word_stride;

    Rgb read(std::size_t index) const {
        if (word_stride == 1) {
            return decode_byte_texel(texels[index], transfer);
        }
        const auto rg = texels[index * 2];
        const auto b = texels[index * 2 + 1];
        return {decode_half(rg), decode_half(rg >> 16), decode_half(b)};
    }

    Rgb texel(int x, int y) const {
        if (wrap == ImageWrap::black && (x < 0 || y < 0 || x >= width || y >= height)) {
            return {};
        }
        const int index = remap(y, height, wrap) * width + remap(x, width, wrap);
        return read(static_cast<std::size_t>(index));
    }
};

std::optional<MipSampler> mip_sampler(const ImagePyramid &image, std::size_t index) {
    const auto &level = image.levels[index];
    if (level.width == 0 || level.height == 0 || !dimensions_fit(level.width, level.height, {})) {
        return std::nullopt;
    }
    const std::size_t word_stride = image.storage == ImageStorage::linear_half ? 2 : 1;
    const auto words = pixel_count(level.width, level.height) * word_stride;
    if (level.offset % word_stride != 0 || level.offset > image.texels.size() ||
        words > image.texels.size() - level.offset) {
        return std::nullopt;
    }
    return MipSampler{std::span(image.texels).subspan(level.offset, words),
                      image_transfer_table(image.encoding),
                      static_cast<int>(level.width),
                      static_cast<int>(level.height),
                      image.wrap,
                      word_stride};
}

struct TexturePoint {
    float x;
    float y;
};

Rgb bilerp(const MipSampler &mip, TexturePoint center) {
    const auto ix = static_cast<int>(std::floor(center.x));
    const auto iy = static_cast<int>(std::floor(center.y));
    const float dx = center.x - static_cast<float>(ix);
    const float dy = center.y - static_cast<float>(iy);
    const auto a = mip.texel(ix, iy);
    const auto b = mip.texel(ix + 1, iy);
    const auto c = mip.texel(ix, iy + 1);
    const auto d = mip.texel(ix + 1, iy + 1);
    Rgb rgb{};
    for (std::size_t channel = 0; channel < rgb.size(); ++channel) {
        rgb[channel] = (1.0f - dx) * (1.0f - dy) * a[channel] + dx * (1.0f - dy) * b[channel] +
                       (1.0f - dx) * dy * c[channel] + dx * dy * d[channel];
    }
    return rgb;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Rgb blend_mips(Rgb lower, const Rgb &upper, float weight) {
    for (std::size_t channel = 0; channel < lower.size(); ++channel) {
        lower[channel] = (1.0f - weight) * lower[channel] + weight * upper[channel];
    }
    return lower;
}

float largest_component(TexturePoint v) { return std::max(std::abs(v.x), std::abs(v.y)); }

float length_squared(TexturePoint v) { return v.x * v.x + v.y * v.y; }

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
float cross(TexturePoint a, TexturePoint b) { return a.x * b.y - a.y * b.x; }

struct ScaledGradient {
    TexturePoint scaled;
    float scaled_length;
    float length;
};

ScaledGradient scaled_gradient(TexturePoint v, float scale) {
    const TexturePoint scaled{v.x / scale, v.y / scale};
    const float scaled_length = std::sqrt(length_squared(scaled));
    return {scaled, scaled_length, scale * scaled_length};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
float ewa_center(float coordinate, int size, ImageWrap wrap) {
    const float dimension = static_cast<float>(size);
    if (std::abs(coordinate) <= 64.0f || wrap == ImageWrap::black) {
        return coordinate * dimension - 0.5f;
    }
    const float phase = std::fmod(coordinate, 1.0f) * dimension - 0.5f;
    if (wrap == ImageWrap::repeat) {
        return phase;
    }
    // Translate by whole texels and keep every sample beyond the same clamp edge.
    const float anchor = coordinate < 0.0f ? -64.0f : dimension + 64.0f;
    return anchor + phase;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool outside_black_axis(float coordinate, float radius, int size) {
    const float dimension = static_cast<float>(size);
    return std::abs(coordinate) > 64.0f && (coordinate < (0.5f - radius) / dimension ||
                                            coordinate > (dimension - 0.5f + radius) / dimension);
}

std::optional<MipSampler> ewa_mip(const ImagePyramid &image, std::size_t index) {
    auto mip = mip_sampler(image, index);
    const auto levels_above = image.levels.size() - 1 - index;
    if (!mip || !std::has_single_bit(static_cast<unsigned>(mip->width)) ||
        !std::has_single_bit(static_cast<unsigned>(mip->height)) ||
        std::max(mip->width, mip->height) != (1 << levels_above)) {
        return std::nullopt;
    }
    return mip;
}

std::optional<Rgb> ewa_bilerp(const ImagePyramid &image, TexturePoint st) {
    const auto mip = ewa_mip(image, 0);
    if (!mip) {
        return std::nullopt;
    }
    if (image.wrap == ImageWrap::black && (outside_black_axis(st.x, 1.0f, mip->width) ||
                                           outside_black_axis(st.y, 1.0f, mip->height))) {
        return Rgb{};
    }
    return bilerp(*mip, {ewa_center(st.x, mip->width, image.wrap),
                         ewa_center(st.y, mip->height, image.wrap)});
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<Rgb> ewa_level(const ImagePyramid &image, std::size_t index, TexturePoint st,
                             TexturePoint major, TexturePoint minor) {
    const auto mip = ewa_mip(image, std::min(index, image.levels.size() - 1));
    if (!mip) {
        return std::nullopt;
    }
    if (index >= image.levels.size()) {
        return mip->read(0);
    }
    const float w = static_cast<float>(mip->width);
    const float h = static_cast<float>(mip->height);
    const TexturePoint u{major.x * w, major.y * h};
    const TexturePoint v{minor.x * w, minor.y * h};
    const TexturePoint radius{std::sqrt(1.0f + u.x * u.x + v.x * v.x),
                              std::sqrt(1.0f + u.y * u.y + v.y * v.y)};
    if (image.wrap == ImageWrap::black && (outside_black_axis(st.x, radius.x, mip->width) ||
                                           outside_black_axis(st.y, radius.y, mip->height))) {
        return Rgb{};
    }
    const TexturePoint center{ewa_center(st.x, mip->width, image.wrap),
                              ewa_center(st.y, mip->height, image.wrap)};
    const float x0 = std::ceil(center.x - radius.x);
    const float y0 = std::ceil(center.y - radius.y);
    const float x1 = std::floor(center.x + radius.x);
    const float y1 = std::floor(center.y + radius.y);
    // Check both the signed bounds and the iteration count before integer conversion.
    constexpr float coordinate_bound = 64.0f * max_image_dimension + 64.0f;
    const float candidates = (x1 - x0 + 1.0f) * (y1 - y0 + 1.0f);
    if (!(x0 >= -coordinate_bound && y0 >= -coordinate_bound && x1 <= coordinate_bound &&
          y1 <= coordinate_bound && x1 >= x0 && y1 >= y0 && candidates <= 4096.0f)) {
        return std::nullopt;
    }
    if (image.wrap == ImageWrap::black && (x1 < 0.0f || y1 < 0.0f || x0 >= w || y0 >= h)) {
        return Rgb{};
    }
    // Expand the covariance determinant as a sum of squares to avoid cancellation
    // when the two texel-space gradients are nearly collinear.
    const float area = cross(u, v);
    const float determinant = 1.0f + length_squared(u) + length_squared(v) + area * area;
    if (!std::isfinite(determinant)) {
        return std::nullopt;
    }
    Rgb sum{};
    float total_weight = 0.0f;
    for (int y = static_cast<int>(y0); y <= static_cast<int>(y1); ++y) {
        for (int x = static_cast<int>(x0); x <= static_cast<int>(x1); ++x) {
            const TexturePoint d{static_cast<float>(x) - center.x,
                                 static_cast<float>(y) - center.y};
            const float ud = cross(u, d);
            const float vd = cross(v, d);
            const float r2 = (length_squared(d) + ud * ud + vd * vd) / determinant;
            if (r2 < 1.0f) {
                const float weight =
                    rypath_ewa::weights[std::min(static_cast<int>(r2 * 128.0f), 127)];
                total_weight += weight;
                const auto sample = mip->texel(x, y);
                for (std::size_t channel = 0; channel < sum.size(); ++channel) {
                    sum[channel] += weight * sample[channel];
                }
            }
        }
    }
    if (!(total_weight > 0.0f) || !std::isfinite(total_weight)) {
        return std::nullopt;
    }
    for (auto &channel : sum) {
        channel /= total_weight;
    }
    return sum;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<Rgb> ewa_rgb(const ImagePyramid &image, TexturePoint st, TexturePoint dx,
                           TexturePoint dy, float max_anisotropy) {
    if (!(max_anisotropy >= 1.0f && max_anisotropy <= 16.0f) ||
        image.levels.size() > std::bit_width(max_image_dimension)) {
        return std::nullopt;
    }
    const float scale_x = largest_component(dx);
    const float scale_y = largest_component(dy);
    const bool finite_center = std::isfinite(st.x) && std::isfinite(st.y);
    if (scale_x == 0.0f || scale_y == 0.0f) {
        return finite_center ? ewa_bilerp(image, st) : std::nullopt;
    }
    // With nonzero gradients and a cap of at most 16, a major length of
    // at least 32 guarantees a post-cap minor length of at least two.
    const auto past_top = image.levels.size();
    if (std::max(scale_x, scale_y) >= 32.0f) {
        return ewa_level(image, past_top, st, dx, dy);
    }
    auto major = scaled_gradient(dx, scale_x);
    auto minor = scaled_gradient(dy, scale_y);
    if (major.length < minor.length) {
        std::swap(major, minor);
        std::swap(dx, dy);
    }
    const float capped_minor_length = major.length / max_anisotropy;
    if (minor.length < capped_minor_length) {
        dy = {(minor.scaled.x / minor.scaled_length) * capped_minor_length,
              (minor.scaled.y / minor.scaled_length) * capped_minor_length};
        minor.length = capped_minor_length;
    }
    if (minor.length >= 2.0f) {
        return ewa_level(image, past_top, st, dx, dy);
    }
    if (!finite_center) {
        return std::nullopt;
    }
    const float lod = std::max(0.0f, static_cast<float>(past_top - 1) + std::log2(minor.length));
    const auto lower_level = std::min(static_cast<std::size_t>(std::floor(lod)), past_top - 1);
    if (lower_level + 1 < past_top) {
        const auto &lower = image.levels[lower_level];
        const auto &upper = image.levels[lower_level + 1];
        if (upper.width != std::max(1u, lower.width / 2u) ||
            upper.height != std::max(1u, lower.height / 2u)) {
            return std::nullopt;
        }
    }
    const auto lower = ewa_level(image, lower_level, st, dx, dy);
    const auto upper = ewa_level(image, lower_level + 1, st, dx, dy);
    if (!lower || !upper) {
        return std::nullopt;
    }
    return blend_mips(*lower, *upper, lod - static_cast<float>(lower_level));
}

} // namespace

ImageResult<DecodedImage> decode_png(std::span<const std::uint8_t> bytes,
                                     const ImageLimits &limits) {
    if (bytes.size() > max_png_bytes) {
        return budget_error<DecodedImage>("PNG exceeds the 16 MiB input limit");
    }
    PngState png;
    unsigned width = 0;
    unsigned height = 0;
    if (const auto error =
            lodepng_inspect(&width, &height, &png.state, bytes.data(), bytes.size())) {
        return png_error(error);
    }
    if (!dimensions_fit(width, height, limits)) {
        return budget_error<DecodedImage>("PNG exceeds the image dimension limit");
    }
    const auto &color = png.state.info_png.color;
    if (auto result = inspect_png_chunks(bytes, color); !result.diagnostic.empty()) {
        return result;
    }
    // lodepng_inspect has already checked the legal depths for each color type.
    const auto depth = color.bitdepth;
    const bool indexed = color.colortype == LCT_PALETTE;
    if ((!indexed && depth != 8 && depth != 16) || png.state.info_png.interlace_method != 0) {
        return png_error("PNG must be noninterlaced indexed color (1, 2, 4, or 8 bits), or "
                         "8- or 16-bit gray, gray-alpha, RGB, or RGBA",
                         true);
    }
    const auto rgb_depth = indexed ? 8u : depth;
    const std::size_t pixels = pixel_count(width, height);
    const auto source_bpp = lodepng_get_bpp(&color);
    const std::size_t rgb_bytes = pixels * 3 * (rgb_depth / 8);
    const std::size_t raw_bytes = (pixels * source_bpp + 7) / 8;
    const std::size_t row_bytes = (static_cast<std::size_t>(width) * source_bpp + 7) / 8;
    const std::size_t scanline_bytes = (row_bytes + 1) * height;
    if (rgb_bytes > std::min(limits.retained_bytes, max_image_bytes)) {
        return budget_error<DecodedImage>("PNG exceeds the decoded image byte budget");
    }
    // LodePNG reserves the exact scanline size. On a malformed stream it can attempt one
    // growth of 1.5 times this size plus at most one stored DEFLATE block (65535 bytes).
    // Include both old/new buffers if realloc moves the allocation, plus IDAT and tables.
    constexpr std::size_t max_block_bytes = 65535;
    constexpr auto huffman_scratch_bytes = std::size_t{256} * 1024;
    const auto inflate_scratch = bytes.size() + scanline_bytes * 2 + scanline_bytes / 2 +
                                 max_block_bytes + huffman_scratch_bytes;
    auto scratch = std::max({inflate_scratch, scanline_bytes + raw_bytes, raw_bytes + rgb_bytes});
    if (indexed) {
        // Count both of LodePNG's 256-entry RGBA palettes in every stage. Expansion
        // writes straight to the retained RGB vector while the packed indices are live.
        scratch += std::size_t{2} * 256 * 4;
    } else {
        // The decoder's RGB buffer remains live while copying into the retained vector.
        scratch = std::max(scratch, 2 * rgb_bytes);
    }
    if (scratch > std::min(limits.scratch_bytes, max_image_bytes)) {
        return budget_error<DecodedImage>("PNG exceeds the decoder scratch byte budget");
    }
    png.state.decoder.zlibsettings.max_output_size = scanline_bytes;
    if (indexed) {
        png.state.decoder.color_convert = 0;
    } else {
        png.state.info_raw.colortype = LCT_RGB;
        png.state.info_raw.bitdepth = rgb_depth;
    }
    unsigned char *data = nullptr;
    const auto error =
        lodepng_decode(&data, &width, &height, &png.state, bytes.data(), bytes.size());
    const std::unique_ptr<unsigned char, decltype(&std::free)> raw(data, &std::free);
    if (error == LODEPNG_ERROR_BLOCK_LIMIT) {
        return budget_error<DecodedImage>(lodepng_error_text(error));
    }
    constexpr unsigned lodepng_allocation_failed = 83;
    if (error == lodepng_allocation_failed) {
        return budget_error<DecodedImage>("PNG decoder could not allocate required memory");
    }
    if (error != 0) {
        return png_error(error);
    }
    if (indexed) {
        DecodedImage image{width, height, {}, rgb_depth};
        try {
            image.rgb.resize(rgb_bytes);
        } catch (const std::bad_alloc &) {
            return budget_error<DecodedImage>("PNG decoder could not allocate required memory");
        }
        // LodePNG removes scanline padding, leaving a single MSB-first index stream.
        const auto mask = (1u << depth) - 1;
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            const auto bit = pixel * depth;
            const auto shift = 8 - depth - (bit % 8);
            const std::size_t index = (raw.get()[bit / 8] >> shift) & mask;
            if (index >= color.palettesize) {
                return png_error("Invalid PNG: pixel index exceeds palette size");
            }
            // The fourth palette byte is alpha. Keep the original RGB even at alpha zero.
            std::copy_n(color.palette + 4 * index, 3, image.rgb.data() + 3 * pixel);
        }
        return {.value = std::move(image), .diagnostic = {}};
    }
    // Conversion removes alpha without compositing and replicates grayscale into RGB.
    DecodedImage image{width, height, {raw.get(), raw.get() + rgb_bytes}, rgb_depth};
    return {.value = std::move(image), .diagnostic = {}};
}

ImageResult<ImagePyramid> build_image_pyramid(const DecodedImage &image, ImageEncoding encoding,
                                              ImageWrap wrap, const ImageLimits &limits) {
    if (!valid_profile(encoding, wrap)) {
        return {.value = {}, .diagnostic = "Invalid image encoding or wrap mode"};
    }
    if (!dimensions_fit(image.width, image.height, limits)) {
        return budget_error<ImagePyramid>("Image exceeds the dimension limit");
    }
    if (image.bit_depth != 8 && image.bit_depth != 16) {
        return {.value = {}, .diagnostic = "Invalid decoded image bit depth"};
    }
    const bool is_half = image.bit_depth == 16;
    const auto pixels = pixel_count(image.width, image.height);
    if (pixels == 0 || image.rgb.size() != pixels * 3 * (image.bit_depth / 8)) {
        return {.value = {}, .diagnostic = "Image dimensions do not match the RGB buffer"};
    }
    const auto width = std::bit_ceil(image.width);
    const auto height = std::bit_ceil(image.height);
    const bool needs_resize = image.width != width || image.height != height;
    if (needs_resize && wrap == ImageWrap::black) {
        return {.value = {},
                .diagnostic = "Non-power-of-two PNGs require repeat or clamp wrapping",
                .unsupported = true};
    }
    if (!dimensions_fit(width, height, limits)) {
        return budget_error<ImagePyramid>("Resized image exceeds the dimension limit");
    }

    const auto level_count = std::bit_width(std::max(width, height));
    std::size_t total_pixels = 0;
    std::uint32_t level_width = width;
    std::uint32_t level_height = height;
    for (int i = 0; i < level_count; ++i) {
        total_pixels += pixel_count(level_width, level_height);
        level_width = std::max(1u, level_width / 2);
        level_height = std::max(1u, level_height / 2);
    }
    const auto total_words = is_half ? 2 * total_pixels : total_pixels;
    const std::size_t retained =
        total_words * sizeof(std::uint32_t) + level_count * sizeof(ImageMipLevel);
    if (retained > std::min(limits.retained_bytes, max_image_bytes)) {
        return budget_error<ImagePyramid>("Image exceeds the mip pyramid byte budget");
    }

    const auto base_bytes = pixel_count(width, height) * sizeof(Rgb);
    const auto mip_scratch =
        base_bytes +
        (level_count > 1
             ? pixel_count(std::max(1u, width / 2), std::max(1u, height / 2)) * sizeof(Rgb)
             : 0);
    const auto resize_scratch = needs_resize
                                    ? base_bytes + (width + height) * sizeof(ResampleWeight) +
                                          pixel_count(width, 4) * sizeof(Rgb)
                                    : 0;
    if (std::max(mip_scratch, resize_scratch) > std::min(limits.scratch_bytes, max_image_bytes)) {
        return budget_error<ImagePyramid>("Image exceeds the mip generation scratch byte budget");
    }

    ImagePyramid pyramid{.encoding = encoding,
                         .wrap = wrap,
                         .levels = {},
                         .texels = {},
                         .storage = is_half ? ImageStorage::linear_half : ImageStorage::encoded8};
    pyramid.levels.reserve(level_count);
    pyramid.texels.reserve(total_words);
    auto level = ImageMipLevel{width, height, 0};
    std::vector<Rgb> working;
    if (needs_resize) {
        working = resize_image(image, width, height, encoding, wrap);
    } else {
        const SourcePixels source{image.rgb, encoding, image_transfer_table(encoding), is_half};
        working.resize(pixels);
        for (std::size_t i = 0; i < pixels; ++i) {
            working[i] = source[i];
        }
    }
    for (int i = 0; i < level_count; ++i) {
        pyramid.levels.push_back(level);
        for (const auto &rgb : working) {
            if (is_half) {
                const auto rg = encode_half(rgb[0]) | (encode_half(rgb[1]) << 16);
                pyramid.texels.push_back(rg);
                pyramid.texels.push_back(encode_half(rgb[2]));
            } else {
                pyramid.texels.push_back(pack_rgb(encode_byte(rgb[0], encoding),
                                                  encode_byte(rgb[1], encoding),
                                                  encode_byte(rgb[2], encoding)));
            }
        }
        if (i + 1 < level_count) {
            const ImageMipLevel next{std::max(1u, level.width / 2), std::max(1u, level.height / 2),
                                     static_cast<std::uint32_t>(pyramid.texels.size())};
            working = downsample(working, level, next);
            level = next;
        }
    }
    return {.value = std::move(pyramid), .diagnostic = {}};
}

const std::array<float, 256> &image_transfer_table(ImageEncoding encoding) {
    return encoding == ImageEncoding::srgb ? srgb_table : linear_table;
}

std::optional<Rgb> image_linear_rgb(const ImagePyramid &image, float s, float t, float dsdx,
                                    float dtdx, float dsdy, float dtdy, ImageFilter filter,
                                    float max_anisotropy) {
    if (!valid_profile(image.encoding, image.wrap) || image.levels.empty() ||
        (image.storage != ImageStorage::encoded8 && image.storage != ImageStorage::linear_half) ||
        !std::isfinite(dsdx) || !std::isfinite(dtdx) || !std::isfinite(dsdy) ||
        !std::isfinite(dtdy)) {
        return std::nullopt;
    }
    if (filter == ImageFilter::ewa) {
        return ewa_rgb(image, {s, t}, {dsdx, dtdx}, {dsdy, dtdy}, max_anisotropy);
    }
    if ((filter != ImageFilter::bilinear && filter != ImageFilter::trilinear &&
         filter != ImageFilter::point) ||
        !std::isfinite(s) || !std::isfinite(t)) {
        return std::nullopt;
    }
    const float half_width =
        std::max({std::abs(dsdx), std::abs(dtdx), std::abs(dsdy), std::abs(dtdy)});
    // Widths of at least one already select the top mip. Bound the width before doubling.
    const float width = std::max(2.0f * std::min(half_width, 0.5f), 1e-8f);
    const auto coarsest = image.levels.size() - 1;
    const float lod = filter == ImageFilter::point
                          ? static_cast<float>(coarsest) + point_log_width(width)
                          : static_cast<float>(coarsest) + std::log(width) * 1.4426950408889634f;
    const bool wide = lod >= static_cast<float>(coarsest);
    const auto sample = [&](std::size_t index) -> std::optional<Rgb> {
        const auto mip = mip_sampler(image, index);
        if (!mip) {
            return std::nullopt;
        }
        if (wide) {
            return mip->read(0);
        }
        if (filter == ImageFilter::point) {
            const int x = point_coordinate(s, image.wrap, mip->width);
            const int y = point_coordinate(t, image.wrap, mip->height);
            return mip->texel(x, y);
        }
        return bilerp(*mip,
                      {bounded_coordinate(s, image.wrap) * static_cast<float>(mip->width) - 0.5f,
                       bounded_coordinate(t, image.wrap) * static_cast<float>(mip->height) - 0.5f});
    };
    const auto selected =
        wide ? coarsest : static_cast<std::size_t>(std::max(0.0f, std::floor(lod)));
    auto rgb = sample(selected);
    // PBRT uses base-level bilerp throughout the first LOD interval.
    if (!rgb || filter != ImageFilter::trilinear || wide || selected == 0) {
        return rgb;
    }
    const auto upper = sample(selected + 1);
    if (!upper) {
        return std::nullopt;
    }
    return blend_mips(*rgb, *upper, lod - static_cast<float>(selected));
}

} // namespace rypath
