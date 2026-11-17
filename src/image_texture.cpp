// SPDX-License-Identifier: Apache-2.0
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Mipmap filtering and color encoding adapted for rypath. See third_party/pbrt/README.md.

#include "image_texture.hpp"
#include "png_allocator.hpp"

#include "ewa_weights.h"

#include <jpeg.hpp>
#include <lodepng.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace rypath {
namespace {

constexpr std::uint32_t max_image_dimension = image_dimension_ceiling;

using Rgb = std::array<float, 3>;
using Rgba = std::array<float, 4>;

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
    return valid_image_limits(limits) && width > 0 && height > 0 && width <= limits.max_dimension &&
           height <= limits.max_dimension;
}

bool valid_profile(ImageEncoding encoding, ImageWrap wrap) {
    return (encoding == ImageEncoding::srgb || encoding == ImageEncoding::linear ||
            encoding == ImageEncoding::gamma) &&
           (wrap == ImageWrap::repeat || wrap == ImageWrap::clamp || wrap == ImageWrap::black);
}

std::size_t png_scanline_bytes(unsigned width, unsigned height, const LodePNGInfo &info) {
    const auto bpp = lodepng_get_bpp(&info.color);
    const auto pass_bytes = [bpp](unsigned w, unsigned h) -> std::size_t {
        return w == 0 || h == 0 ? 0 : ((static_cast<std::size_t>(w) * bpp + 7) / 8 + 1) * h;
    };
    if (info.interlace_method == 0) {
        return pass_bytes(width, height);
    }
    constexpr std::array<unsigned, 7> x_start{0, 4, 0, 2, 0, 1, 0};
    constexpr std::array<unsigned, 7> y_start{0, 0, 4, 0, 2, 0, 1};
    constexpr std::array<unsigned, 7> x_step{8, 8, 4, 4, 2, 2, 1};
    constexpr std::array<unsigned, 7> y_step{8, 8, 8, 4, 4, 2, 2};
    std::size_t total = 0;
    for (std::size_t pass = 0; pass < x_start.size(); ++pass) {
        total += pass_bytes((width + x_step[pass] - 1 - x_start[pass]) / x_step[pass],
                            (height + y_step[pass] - 1 - y_start[pass]) / y_step[pass]);
    }
    return total;
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
                                             const LodePNGColorMode &color, bool &has_palette) {
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
            has_palette = true;
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

// IEEE binary16 round-to-nearest, ties-to-even, including signed subnormals.
std::uint32_t encode_half(float value) {
    constexpr auto fraction_mask = 0x007fffffu;
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = (bits >> 16) & 0x8000u;
    const auto exponent = (bits >> 23) & 0xffu;
    const auto fraction = bits & fraction_mask;
    if (exponent >= 143u)
        return sign | 0x7c00u;
    if (exponent < 102u) {
        return sign; // Below half of the smallest binary16 subnormal.
    }
    if (exponent <= 112u) {
        return sign | round_significand(fraction | (1u << 23), 126u - exponent);
    }
    // A carry from rounding the fraction advances the exponent.
    return sign | (((exponent - 112u) << 10) + round_significand(fraction, 13));
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

struct Transfer {
    ImageEncoding encoding;
    float gamma;
    std::array<std::uint8_t, 1024> inverse{};

    Transfer(ImageEncoding encoding, float gamma) : encoding(encoding), gamma(gamma) {
        if (encoding == ImageEncoding::gamma) {
            for (std::size_t i = 0; i < inverse.size(); ++i) {
                const float v = static_cast<float>(i) / static_cast<float>(inverse.size() - 1);
                inverse[i] = static_cast<std::uint8_t>(
                    std::clamp(255.0f * std::pow(v, 1.0f / gamma) + 0.5f, 0.0f, 255.0f));
            }
        }
    }

    float decode(float value) const {
        if (encoding == ImageEncoding::srgb)
            return decode_srgb(value);
        if (encoding == ImageEncoding::gamma)
            return std::pow(value, gamma);
        return value;
    }

    std::uint32_t encode_byte(float value) const {
        value = std::clamp(value, 0.0f, 1.0f);
        if (encoding == ImageEncoding::srgb)
            return static_cast<std::uint32_t>(std::round(255.0f * encode_srgb(value)));
        if (encoding == ImageEncoding::gamma)
            return inverse[static_cast<std::size_t>(value *
                                                    static_cast<float>(inverse.size() - 1))];
        return static_cast<std::uint32_t>(std::min(255.0f, 255.0f * value + 0.5f));
    }
};

const std::array<float, 256> &transfer_table(const ImagePyramid &image) {
    return image.custom_transfer ? *image.custom_transfer : image_transfer_table(image.encoding);
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

std::uint32_t read_sample(std::span<const std::uint8_t> bytes, std::size_t index, bool half) {
    if (!half)
        return bytes[index];
    return (static_cast<unsigned>(bytes[index * 2]) << 8) | bytes[index * 2 + 1];
}

struct SourcePixels {
    const DecodedImage &image;
    const Transfer &color;
    const std::array<float, 256> &transfer;
    bool is_half;

    float decode(std::uint32_t sample) const {
        if (!is_half)
            return transfer[sample];
        // PBRT rounds the source to half before any float resize or downsample.
        return decode_half(encode_half(color.decode(static_cast<float>(sample) / 65535.0f)));
    }

    template <std::size_t N> std::array<float, N> pixel(std::size_t index) const {
        std::array<float, N> result{};
        if (image.sample_format == ImageSampleFormat::half ||
            image.sample_format == ImageSampleFormat::float32) {
            std::copy_n(image.floating_rgb.data() + index * 3, 3, result.data());
            if constexpr (N == 4)
                result[3] = image.floating_alpha[index];
            return result;
        }
        for (std::size_t c = 0; c < 3; ++c)
            result[c] = decode(read_sample(image.rgb, index * 3 + c, is_half));
        if constexpr (N == 4)
            result[3] = decode(read_sample(image.alpha, index, is_half));
        return result;
    }
};

int remap(int coordinate, int size, ImageWrap wrap) {
    assert(size > 0);
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

bool outside(int coordinate, int size) { return coordinate < 0 || coordinate >= size; }

template <typename Pixel>
void resize_row(const SourcePixels &source, int y, const std::vector<ResampleWeight> &horizontal,
                ImageWrap wrap, std::span<Pixel> destination) {
    const auto width = static_cast<int>(source.image.width);
    const auto height = static_cast<int>(source.image.height);
    if (wrap == ImageWrap::black && outside(y, height)) {
        std::ranges::fill(destination, Pixel{});
        return;
    }
    const auto row = pixel_count(source.image.width, remap(y, height, wrap));
    for (std::size_t x = 0; x < horizontal.size(); ++x) {
        const auto &sample = horizontal[x];
        Pixel pixel{};
        for (int tap = 0; tap < 4; ++tap) {
            const int in_x = sample.first + tap;
            if (wrap == ImageWrap::black && outside(in_x, width))
                continue;
            const auto in_pixel = source.pixel<std::tuple_size_v<Pixel>>(
                row + static_cast<std::size_t>(remap(in_x, width, wrap)));
            for (std::size_t channel = 0; channel < pixel.size(); ++channel)
                pixel[channel] += sample.weights[tap] * in_pixel[channel];
        }
        destination[x] = pixel;
    }
}

template <typename Pixel>
std::vector<Pixel> resize_image(const SourcePixels &source, std::uint32_t width,
                                std::uint32_t height, ImageWrap wrap) {
    const auto horizontal = resample_weights(source.image.width, width);
    const auto vertical = resample_weights(source.image.height, height);
    std::vector<Pixel> output(pixel_count(width, height));

    // A four-row ring keeps both separable passes within the scratch budget.
    std::vector<Pixel> rows(pixel_count(width, 4));
    int last_row = vertical.front().first - 1;
    auto row_slot = [](int y) { return static_cast<std::uint32_t>(y) % 4u; };
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto &sample = vertical[y];
        while (last_row < sample.first + 3) {
            ++last_row;
            auto destination_row =
                std::span(rows).subspan(pixel_count(width, row_slot(last_row)), width);
            resize_row(source, last_row, horizontal, wrap, destination_row);
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            auto &pixel = output[pixel_count(width, y) + x];
            for (int tap = 0; tap < 4; ++tap) {
                const auto &source_pixel = rows[width * row_slot(sample.first + tap) + x];
                for (std::size_t channel = 0; channel < pixel.size(); ++channel)
                    pixel[channel] += sample.weights[tap] * source_pixel[channel];
            }
            for (auto &channel : pixel) {
                if (!std::isfinite(channel))
                    throw std::runtime_error("Image resize overflowed finite FLOAT range");
                channel = std::max(0.0f, channel);
            }
        }
    }
    return output;
}

template <typename Pixel>
std::vector<Pixel> downsample(std::span<const Pixel> input, const ImageMipLevel &current,
                              const ImageMipLevel &next) {
    std::vector<Pixel> output(pixel_count(next.width, next.height));
    for (std::uint32_t y = 0; y < next.height; ++y) {
        const auto row = pixel_count(current.width, 2 * y);
        const auto next_row = row + (current.height > 1 ? current.width : 0);
        for (std::uint32_t x = 0; x < next.width; ++x) {
            const auto column = 2 * x;
            const auto next_column = column + (current.width > 1 ? 1 : 0);
            auto &pixel = output[pixel_count(next.width, y) + x];
            for (std::size_t c = 0; c < pixel.size(); ++c) {
                pixel[c] = (input[row + column][c] + input[row + next_column][c] +
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
    std::uint32_t channels;

    Rgb read(std::size_t index, bool alpha = false) const {
        if (word_stride == 1) {
            return alpha ? Rgb{transfer[texels[index] >> 24], 0, 0}
                         : decode_byte_texel(texels[index], transfer);
        }
        if (word_stride == 4) {
            return alpha ? Rgb{std::bit_cast<float>(texels[index * 4 + 3]), 0, 0}
                         : Rgb{std::bit_cast<float>(texels[index * 4]),
                               std::bit_cast<float>(texels[index * 4 + 1]),
                               std::bit_cast<float>(texels[index * 4 + 2])};
        }
        const auto rg = texels[index * 2];
        const auto b = texels[index * 2 + 1];
        return alpha ? Rgb{decode_half(b >> 16), 0, 0}
                     : Rgb{decode_half(rg), decode_half(rg >> 16), decode_half(b)};
    }

    Rgb texel(int x, int y, bool alpha = false) const {
        if (wrap == ImageWrap::black && (x < 0 || y < 0 || x >= width || y >= height)) {
            return {};
        }
        const int index = remap(y, height, wrap) * width + remap(x, width, wrap);
        return read(static_cast<std::size_t>(index), alpha);
    }
};

std::optional<MipSampler> mip_sampler(const ImagePyramid &image, std::size_t index) {
    const auto &level = image.levels[index];
    // Construction checks the selected profile. Lookup must accept every
    // supported retained image, including the explicit large-scene profile.
    if (level.width == 0 || level.height == 0 || level.width > image_dimension_ceiling ||
        level.height > image_dimension_ceiling) {
        return std::nullopt;
    }
    const std::size_t word_stride = image_storage_words(image.storage);
    const auto words = pixel_count(level.width, level.height) * word_stride;
    if (level.offset % word_stride != 0 || level.offset > image.texels.size() ||
        words > image.texels.size() - level.offset) {
        return std::nullopt;
    }
    return MipSampler{std::span(image.texels).subspan(level.offset, words),
                      transfer_table(image),
                      static_cast<int>(level.width),
                      static_cast<int>(level.height),
                      image.wrap,
                      word_stride,
                      image.source_channels};
}

struct TexturePoint {
    float x;
    float y;
};

Rgb bilerp(const MipSampler &mip, TexturePoint center, bool scalar = false) {
    const auto ix = static_cast<int>(std::floor(center.x));
    const auto iy = static_cast<int>(std::floor(center.y));
    const float dx = center.x - static_cast<float>(ix);
    const float dy = center.y - static_cast<float>(iy);
    const bool alpha = scalar && mip.channels == 4;
    const auto a = mip.texel(ix, iy, alpha);
    const auto b = mip.texel(ix + 1, iy, alpha);
    const auto c = mip.texel(ix, iy + 1, alpha);
    const auto d = mip.texel(ix + 1, iy + 1, alpha);
    Rgb rgb{};
    for (std::size_t channel = 0; channel < rgb.size(); ++channel) {
        rgb[channel] = (1.0f - dx) * (1.0f - dy) * a[channel] + dx * (1.0f - dy) * b[channel] +
                       (1.0f - dx) * dy * c[channel] + dx * dy * d[channel];
    }
    if (scalar && mip.channels == 3)
        rgb[0] = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
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

std::optional<Rgb> ewa_bilerp(const ImagePyramid &image, TexturePoint st, bool scalar) {
    const auto mip = ewa_mip(image, 0);
    if (!mip) {
        return std::nullopt;
    }
    if (image.wrap == ImageWrap::black && (outside_black_axis(st.x, 1.0f, mip->width) ||
                                           outside_black_axis(st.y, 1.0f, mip->height))) {
        return Rgb{};
    }
    return bilerp(
        *mip, {ewa_center(st.x, mip->width, image.wrap), ewa_center(st.y, mip->height, image.wrap)},
        scalar);
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
                           TexturePoint dy, float max_anisotropy, bool scalar) {
    if (!(max_anisotropy >= 1.0f && max_anisotropy <= 16.0f) ||
        image.levels.size() > std::bit_width(max_image_dimension)) {
        return std::nullopt;
    }
    const float scale_x = largest_component(dx);
    const float scale_y = largest_component(dy);
    const bool finite_center = std::isfinite(st.x) && std::isfinite(st.y);
    if (scale_x == 0.0f || scale_y == 0.0f) {
        return finite_center ? ewa_bilerp(image, st, scalar) : std::nullopt;
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

ImageResult<DecodedImage> decode_png(std::span<const std::uint8_t> bytes, const ImageLimits &limits,
                                     bool preserve_all_alpha) {
    if (!valid_image_limits(limits) || bytes.size() > limits.file_bytes) {
        return budget_error<DecodedImage>("PNG exceeds the configured input limit");
    }
    PngAllocationScope allocation_scope(
        std::min(limits.png_allocation_bytes, limits.scratch_bytes));
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
    bool has_palette = false;
    if (auto result = inspect_png_chunks(bytes, color, has_palette); !result.diagnostic.empty()) {
        return result;
    }
    const auto depth = color.bitdepth;
    const bool indexed = color.colortype == LCT_PALETTE;
    const bool rgba = color.colortype == LCT_RGBA;
    // In glTF mode ask LodePNG for RGBA even for gray, palette and color-key inputs.
    const bool retain_rgba = rgba || preserve_all_alpha;
    const bool gray = color.colortype == LCT_GREY || color.colortype == LCT_GREY_ALPHA;
    const auto rgb_depth = std::max(8u, depth);
    const std::size_t pixels = pixel_count(width, height);
    const auto source_bpp = lodepng_get_bpp(&color);
    const std::size_t sample_bytes = rgb_depth / 8;
    const std::size_t rgb_bytes = pixels * 3 * sample_bytes;
    const std::size_t alpha_bytes = retain_rgba ? pixels * sample_bytes : 0;
    const std::size_t retained = rgb_bytes + alpha_bytes;
    const std::size_t raw_bytes = (pixels * source_bpp + 7) / 8;
    const std::size_t scanline_bytes = png_scanline_bytes(width, height, png.state.info_png);
    if (retained > limits.retained_bytes) {
        return budget_error<DecodedImage>("PNG exceeds the decoded image byte budget");
    }
    // Include both old/new inflate buffers if realloc moves the allocation, plus IDAT
    // and tables. The Adam7 passes unfilter and remove padding in the scanline buffer.
    constexpr std::size_t max_block_bytes = 65535;
    constexpr auto huffman_scratch_bytes = std::size_t{256} * 1024;
    const auto inflate_scratch = bytes.size() + scanline_bytes * 2 + scanline_bytes / 2 +
                                 max_block_bytes + huffman_scratch_bytes;
    const auto scratch = std::max({inflate_scratch, scanline_bytes + raw_bytes,
                                   raw_bytes + retained, indexed ? 0 : 2 * retained}) +
                         (has_palette ? std::size_t{2} * 256 * 4 : 0);
    if (scratch > limits.scratch_bytes) {
        return budget_error<DecodedImage>("PNG exceeds the decoder scratch byte budget");
    }
    png.state.decoder.zlibsettings.max_output_size = scanline_bytes;
    if (preserve_all_alpha) {
        png.state.info_raw.colortype = LCT_RGBA;
        png.state.info_raw.bitdepth = rgb_depth;
    } else if (indexed || rgba) {
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
    DecodedImage image{.width = width,
                       .height = height,
                       .rgb = {},
                       .sample_format =
                           rgb_depth == 16 ? ImageSampleFormat::unorm16 : ImageSampleFormat::unorm8,
                       .source_channels = retain_rgba ? 4u : (gray ? 1u : 3u)};
    try {
        image.rgb.resize(rgb_bytes);
        image.alpha.resize(alpha_bytes);
    } catch (const std::bad_alloc &) {
        return budget_error<DecodedImage>("PNG decoder could not allocate required memory");
    }
    if (indexed && !preserve_all_alpha) {
        // LodePNG removes row padding and interlacing, leaving a single MSB-first stream.
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
    } else if (retain_rgba) {
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            const auto *input = raw.get() + pixel * 4 * sample_bytes;
            std::copy_n(input, 3 * sample_bytes, image.rgb.data() + pixel * 3 * sample_bytes);
            std::copy_n(input + 3 * sample_bytes, sample_bytes,
                        image.alpha.data() + pixel * sample_bytes);
        }
    } else {
        // Conversion removes grayscale alpha and transparency keys without compositing.
        std::copy_n(raw.get(), rgb_bytes, image.rgb.data());
    }
    return {.value = std::move(image), .diagnostic = {}};
}

ImageEncoding image_effective_encoding(const DecodedImage &image, ImageEncoding authored) {
    if (image.sample_format == ImageSampleFormat::half ||
        image.sample_format == ImageSampleFormat::float32)
        return ImageEncoding::linear;
    return image.file_format == ImageFileFormat::jpeg ? ImageEncoding::srgb : authored;
}

ImageResult<DecodedImage> decode_jpeg(std::span<const std::uint8_t> bytes,
                                      const ImageLimits &limits) {
    if (!valid_image_limits(limits) || bytes.size() > limits.file_bytes)
        return budget_error<DecodedImage>("JPEG exceeds the configured input limit");

    auto decoded = jpeg::decode(bytes, {.max_dimension = limits.max_dimension,
                                        .retained_bytes = limits.retained_bytes,
                                        .scratch_bytes = limits.scratch_bytes});
    switch (decoded.error) {
    case jpeg::Error::none:
        return {.value = DecodedImage{.width = decoded.width,
                                      .height = decoded.height,
                                      .rgb = std::move(decoded.rgb),
                                      .source_channels = decoded.channels,
                                      .file_format = ImageFileFormat::jpeg},
                .diagnostic = {}};
    case jpeg::Error::invalid:
        return {.value = {},
                .diagnostic = std::string("Invalid JPEG: ") +
                              (decoded.detail ? decoded.detail : "entropy decode failed")};
    case jpeg::Error::unsupported:
        return {.value = {}, .diagnostic = decoded.detail, .unsupported = true};
    default:
        return budget_error<DecodedImage>(decoded.detail);
    }
}

namespace {

template <std::size_t N>
void append_texel(ImagePyramid &pyramid, const std::array<float, N> &pixel, const Transfer &color) {
    for (float value : pixel) {
        if (!std::isfinite(value) || (pyramid.storage == ImageStorage::linear_half &&
                                      (encode_half(value) & 0x7c00u) == 0x7c00u))
            throw std::runtime_error("Image filtering overflowed its finite storage range");
    }
    if (pyramid.storage == ImageStorage::linear_float) {
        for (float value : pixel)
            pyramid.texels.push_back(std::bit_cast<std::uint32_t>(value));
        if constexpr (N == 3)
            pyramid.texels.push_back(0);
    } else if (pyramid.storage == ImageStorage::linear_half) {
        pyramid.texels.push_back(encode_half(pixel[0]) | (encode_half(pixel[1]) << 16));
        auto last = encode_half(pixel[2]);
        if constexpr (N == 4)
            last |= encode_half(pixel[3]) << 16;
        pyramid.texels.push_back(last);
    } else {
        auto word = pack_rgb(color.encode_byte(pixel[0]), color.encode_byte(pixel[1]),
                             color.encode_byte(pixel[2]));
        if constexpr (N == 4)
            word |= color.encode_byte(pixel[3]) << 24;
        pyramid.texels.push_back(word);
    }
}

template <typename Pixel>
void generate_mips(ImagePyramid &pyramid, const SourcePixels &source, ImageMipLevel level,
                   int level_count) {
    std::vector<Pixel> working;
    if (source.image.width != level.width || source.image.height != level.height) {
        working = resize_image<Pixel>(source, level.width, level.height, pyramid.wrap);
    } else {
        working.resize(pixel_count(level.width, level.height));
        for (std::size_t i = 0; i < working.size(); ++i)
            working[i] = source.pixel<std::tuple_size_v<Pixel>>(i);
    }
    for (int i = 0; i < level_count; ++i) {
        pyramid.levels.push_back(level);
        for (const auto &pixel : working)
            append_texel(pyramid, pixel, source.color);
        if (i + 1 < level_count) {
            const ImageMipLevel next{std::max(1u, level.width / 2), std::max(1u, level.height / 2),
                                     static_cast<std::uint32_t>(pyramid.texels.size())};
            working = downsample(std::span<const Pixel>(working), level, next);
            level = next;
        }
    }
}

std::array<float, 256> gamma_transfer(float gamma) {
    std::array<float, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i)
        table[i] = std::pow(static_cast<float>(i) / 255.0f, gamma);
    return table;
}

bool nonopaque_alpha(const SourcePixels &source) {
    if (!source.image.floating_alpha.empty())
        return std::ranges::any_of(source.image.floating_alpha, [](float v) { return v != 1; });
    if (source.image.alpha.empty())
        return false;
    const auto count = pixel_count(source.image.width, source.image.height);
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = read_sample(source.image.alpha, i, source.is_half);
        if (value != (source.is_half ? 65535u : 255u) && source.decode(value) != 1.0f)
            return true;
    }
    return false;
}

ImageResult<ImagePyramid> build_image(const DecodedImage &image, ImageEncoding authored,
                                      ImageWrap wrap, const ImageLimits &limits, float gamma,
                                      bool native, bool goniometric = false) {
    const auto encoding = image_effective_encoding(image, authored);
    if (!valid_profile(encoding, wrap) ||
        (encoding == ImageEncoding::gamma && !(gamma > 0 && std::isfinite(gamma)))) {
        return {.value = {}, .diagnostic = "Invalid image encoding or wrap mode"};
    }
    if (!dimensions_fit(image.width, image.height, limits)) {
        return budget_error<ImagePyramid>("Image exceeds the dimension limit");
    }
    const bool floating = image.sample_format == ImageSampleFormat::half ||
                          image.sample_format == ImageSampleFormat::float32;
    const bool is_half = image.sample_format == ImageSampleFormat::unorm16 ||
                         image.sample_format == ImageSampleFormat::half;
    if (!floating && image.sample_format != ImageSampleFormat::unorm8 && !is_half)
        return {.value = {}, .diagnostic = "Invalid decoded image sample format"};
    const auto storage = image.sample_format == ImageSampleFormat::float32
                             ? ImageStorage::linear_float
                         : is_half ? ImageStorage::linear_half
                                   : ImageStorage::encoded8;
    const auto pixels = pixel_count(image.width, image.height);
    const auto sample_bytes = is_half ? 2u : 1u;
    const auto alpha_samples = image.source_channels == 4 ? pixels : 0;
    if (pixels == 0 || image.rgb.size() != (floating ? 0 : pixels * 3 * sample_bytes) ||
        image.alpha.size() != (floating ? 0 : alpha_samples * sample_bytes) ||
        image.floating_rgb.size() != (floating ? pixels * 3 : 0) ||
        image.floating_alpha.size() != (floating ? alpha_samples : 0) ||
        (image.source_channels != 1 && image.source_channels != 3 && image.source_channels != 4))
        return {.value = {}, .diagnostic = "Image dimensions do not match the decoded channels"};
    for (const auto &samples :
         {std::span<const float>(image.floating_rgb),
          goniometric ? std::span<const float>{} : std::span<const float>(image.floating_alpha)}) {
        for (float value : samples) {
            if (!std::isfinite(value) || (goniometric && value < 0) ||
                (image.sample_format == ImageSampleFormat::half &&
                 decode_half(encode_half(value)) != value))
                return {.value = {}, .diagnostic = "Invalid finite decoded image sample"};
        }
    }
    const auto width = native ? image.width : std::bit_ceil(image.width);
    const auto height = native ? image.height : std::bit_ceil(image.height);
    const bool needs_resize = image.width != width || image.height != height;
    if (!dimensions_fit(width, height, limits)) {
        return budget_error<ImagePyramid>("Resized image exceeds the dimension limit");
    }

    const auto level_count = native ? 1 : std::bit_width(std::max(width, height));
    std::size_t total_pixels = 0;
    std::uint32_t level_width = width;
    std::uint32_t level_height = height;
    for (int i = 0; i < level_count; ++i) {
        total_pixels += pixel_count(level_width, level_height);
        level_width = std::max(1u, level_width / 2);
        level_height = std::max(1u, level_height / 2);
    }
    const bool custom = !floating && !is_half && encoding == ImageEncoding::gamma;
    const auto total_words = image_storage_words(storage) * total_pixels;
    const std::size_t retained = total_words * sizeof(std::uint32_t) +
                                 level_count * sizeof(ImageMipLevel) +
                                 (custom ? sizeof(std::array<float, 256>) : 0);
    if (retained > limits.retained_bytes) {
        return budget_error<ImagePyramid>("Image exceeds the mip pyramid byte budget");
    }

    const Transfer color(encoding, gamma);
    const auto gamma_table = custom ? gamma_transfer(gamma) : std::array<float, 256>{};
    const SourcePixels source{image, color, custom ? gamma_table : image_transfer_table(encoding),
                              is_half};
    const bool alpha = !native && nonopaque_alpha(source);
    const auto pixel_size = alpha ? sizeof(Rgba) : sizeof(Rgb);
    const auto base_bytes = native ? 0 : pixel_count(width, height) * pixel_size;
    const auto mip_scratch =
        base_bytes +
        (level_count > 1
             ? pixel_count(std::max(1u, width / 2), std::max(1u, height / 2)) * pixel_size
             : 0);
    const auto resize_scratch = needs_resize
                                    ? base_bytes + (width + height) * sizeof(ResampleWeight) +
                                          pixel_count(width, 4) * pixel_size
                                    : 0;
    if (std::max(mip_scratch, resize_scratch) > limits.scratch_bytes) {
        return budget_error<ImagePyramid>("Image exceeds the mip generation scratch byte budget");
    }

    ImagePyramid pyramid{.encoding = encoding,
                         .wrap = wrap,
                         .levels = {},
                         .texels = {},
                         .storage = storage,
                         .source_channels = goniometric ? 1u
                                            : alpha     ? 4u
                                                        : (image.source_channels == 1 ? 1u : 3u),
                         .native_base = native};
    try {
        if (custom)
            pyramid.custom_transfer = std::make_shared<const std::array<float, 256>>(gamma_table);
        pyramid.levels.reserve(level_count);
        pyramid.texels.reserve(total_words);
        const ImageMipLevel level{width, height, 0};
        if (native) {
            pyramid.levels.push_back(level);
            for (std::size_t i = 0; i < pixels; ++i) {
                if (goniometric && image.source_channels != 1) {
                    const auto rgb = source.pixel<3>(i);
                    // PBRT ImageChannelValues::Average sums in binary32 before
                    // division; do not replace this with an overflow-safe mean.
                    const float scalar = ((0.0f + rgb[0]) + rgb[1] + rgb[2]) / 3.0f;
                    if (!std::isfinite(scalar) || std::fpclassify(scalar) == FP_SUBNORMAL)
                        throw std::runtime_error(
                            "Goniometric scalar is nonfinite or binary32 subnormal");
                    append_texel(pyramid, Rgb{scalar, scalar, scalar}, color);
                } else if (is_half || floating) {
                    auto rgb = source.pixel<3>(i);
                    if (goniometric) {
                        const float scalar = rgb[0];
                        if (std::fpclassify(scalar) == FP_SUBNORMAL)
                            throw std::runtime_error("Goniometric scalar is binary32 subnormal");
                        rgb = {scalar, scalar, scalar};
                    }
                    append_texel(pyramid, rgb, color);
                } else {
                    // Native PBRT consumers read source codes without a mip write round trip.
                    pyramid.texels.push_back(
                        pack_rgb(image.rgb[3 * i], image.rgb[3 * i + 1], image.rgb[3 * i + 2]));
                }
            }
        } else if (alpha) {
            generate_mips<Rgba>(pyramid, source, level, level_count);
        } else {
            generate_mips<Rgb>(pyramid, source, level, level_count);
        }
    } catch (const std::runtime_error &error) {
        return {.value = {}, .diagnostic = error.what()};
    } catch (const std::bad_alloc &) {
        return budget_error<ImagePyramid>(
            "Image mip generation could not allocate required memory");
    }
    return {.value = std::move(pyramid), .diagnostic = {}};
}

} // namespace

ImageResult<ImagePyramid> build_image_pyramid(const DecodedImage &image, ImageEncoding encoding,
                                              ImageWrap wrap, const ImageLimits &limits,
                                              float gamma) {
    return build_image(image, encoding, wrap, limits, gamma, false);
}

ImageResult<ImagePyramid> build_image_base(const DecodedImage &image, ImageEncoding encoding,
                                           ImageWrap wrap, const ImageLimits &limits, float gamma) {
    return build_image(image, encoding, wrap, limits, gamma, true);
}

ImageResult<ImagePyramid> build_goniometric_image_base(const DecodedImage &image,
                                                       const ImageLimits &limits) {
    if (image.width != image.height)
        return {.value = {}, .diagnostic = "A goniometric light image must be square"};
    return build_image(image, ImageEncoding::srgb, ImageWrap::clamp, limits, 1, true, true);
}

const std::array<float, 256> &image_transfer_table(ImageEncoding encoding) {
    return encoding == ImageEncoding::srgb ? srgb_table : linear_table;
}

namespace {

bool valid_sampling_image(const ImagePyramid &image) {
    return valid_profile(image.encoding, image.wrap) && !image.levels.empty() &&
           (image.storage == ImageStorage::linear_half ||
            image.storage == ImageStorage::linear_float ||
            (image.storage == ImageStorage::encoded8 &&
             (image.encoding != ImageEncoding::gamma || image.custom_transfer))) &&
           (image.source_channels == 1 || image.source_channels == 3 || image.source_channels == 4);
}

Rgb bilerp_bounded(const MipSampler &mip, float s, float t, bool scalar = false) {
    return bilerp(mip,
                  {bounded_coordinate(s, mip.wrap) * static_cast<float>(mip.width) - 0.5f,
                   bounded_coordinate(t, mip.wrap) * static_cast<float>(mip.height) - 0.5f},
                  scalar);
}

std::optional<Rgb> image_lookup(const ImagePyramid &image, float s, float t, float dsdx, float dtdx,
                                float dsdy, float dtdy, ImageFilter filter, float max_anisotropy,
                                bool scalar) {
    if (!valid_sampling_image(image) || image.native_base || !std::isfinite(dsdx) ||
        !std::isfinite(dtdx) || !std::isfinite(dsdy) || !std::isfinite(dtdy)) {
        return std::nullopt;
    }
    if (filter == ImageFilter::ewa) {
        return ewa_rgb(image, {s, t}, {dsdx, dtdx}, {dsdy, dtdy}, max_anisotropy, scalar);
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
        return bilerp_bounded(*mip, s, t, scalar);
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

std::optional<Rgb> finite_lookup(std::optional<Rgb> value) {
    if (!value ||
        !std::ranges::all_of(*value, [](float channel) { return std::isfinite(channel); }))
        return std::nullopt;
    return value;
}

} // namespace

std::optional<Rgb> image_linear_rgb(const ImagePyramid &image, float s, float t, float dsdx,
                                    float dtdx, float dsdy, float dtdy, ImageFilter filter,
                                    float max_anisotropy) {
    return finite_lookup(
        image_lookup(image, s, t, dsdx, dtdx, dsdy, dtdy, filter, max_anisotropy, false));
}

std::optional<float> image_linear_scalar(const ImagePyramid &image, float s, float t, float dsdx,
                                         float dtdx, float dsdy, float dtdy, ImageFilter filter,
                                         float max_anisotropy) {
    auto result = image_lookup(image, s, t, dsdx, dtdx, dsdy, dtdy, filter, max_anisotropy, true);
    return result && std::isfinite((*result)[0]) ? std::optional<float>((*result)[0])
                                                 : std::nullopt;
}

std::optional<Rgb> image_base_linear_rgb(const ImagePyramid &image, float s, float t) {
    if (!valid_sampling_image(image) || !image.native_base || image.levels.size() != 1 ||
        !std::isfinite(s) || !std::isfinite(t))
        return std::nullopt;
    const auto mip = mip_sampler(image, 0);
    return finite_lookup(mip ? std::optional<Rgb>(bilerp_bounded(*mip, s, t)) : std::nullopt);
}

std::optional<Rgb> image_base_texel_linear_rgb(const ImagePyramid &image, std::uint32_t x,
                                               std::uint32_t y) {
    if (!valid_sampling_image(image) || !image.native_base || image.levels.size() != 1)
        return std::nullopt;
    const auto mip = mip_sampler(image, 0);
    if (!mip || x >= static_cast<unsigned>(mip->width) || y >= static_cast<unsigned>(mip->height))
        return std::nullopt;
    return finite_lookup(mip->read(pixel_count(static_cast<unsigned>(mip->width), y) + x));
}

ImageResult<ImagePyramid> build_gltf_image_pyramid(const DecodedImage &image,
                                                   ImageEncoding encoding,
                                                   const ImageLimits &limits) {
    if (encoding != ImageEncoding::linear && encoding != ImageEncoding::srgb)
        return {.value = {}, .diagnostic = "glTF images require linear or sRGB encoding"};
    if (!dimensions_fit(image.width, image.height, limits))
        return budget_error<ImagePyramid>("glTF image exceeds the dimension limit");
    const bool wide = image.sample_format == ImageSampleFormat::unorm16;
    if (!wide && image.sample_format != ImageSampleFormat::unorm8)
        return {.value = {}, .diagnostic = "glTF images require unsigned normalized samples"};
    const auto pixels = pixel_count(image.width, image.height);
    const auto bytes = wide ? 2u : 1u;
    const bool alpha = image.source_channels == 4;
    if (pixels == 0 || image.rgb.size() != pixels * 3 * bytes ||
        image.alpha.size() != (alpha ? pixels * bytes : 0) || !image.floating_rgb.empty() ||
        !image.floating_alpha.empty() ||
        (image.source_channels != 1 && image.source_channels != 3 && !alpha))
        return {.value = {}, .diagnostic = "glTF image dimensions do not match decoded channels"};
    std::size_t total = 0, count = 0;
    for (auto w = image.width, h = image.height;;
         w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
        total += pixel_count(w, h);
        ++count;
        if (w == 1 && h == 1)
            break;
    }
    const auto retained = total * sizeof(Rgba) + count * sizeof(ImageMipLevel);
    if (retained > limits.retained_bytes)
        return budget_error<ImagePyramid>("glTF image exceeds the mip pyramid byte budget");
    const auto scratch = (pixels + (count > 1 ? pixel_count(std::max(1u, image.width / 2),
                                                            std::max(1u, image.height / 2))
                                              : 0)) *
                         sizeof(Rgba);
    if (scratch > limits.scratch_bytes)
        return budget_error<ImagePyramid>("glTF image exceeds the mip scratch byte budget");
    ImagePyramid pyramid{.encoding = encoding,
                         .wrap = ImageWrap::repeat,
                         .levels = {},
                         .texels = {},
                         .storage = ImageStorage::linear_float,
                         .source_channels = 4,
                         .gltf = true};
    try {
        pyramid.levels.reserve(count);
        pyramid.texels.reserve(total * 4);
        std::vector<Rgba> working(pixels);
        const float denominator = wide ? 65535.0f : 255.0f;
        for (std::size_t i = 0; i < pixels; ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                const float code =
                    static_cast<float>(read_sample(image.rgb, i * 3 + c, wide)) / denominator;
                working[i][c] = encoding == ImageEncoding::srgb ? decode_srgb(code) : code;
            }
            // Alpha is unassociated coverage data, independent of the RGB transfer.
            working[i][3] =
                alpha ? static_cast<float>(read_sample(image.alpha, i, wide)) / denominator : 1.0f;
        }
        auto w = image.width, h = image.height;
        for (;;) {
            pyramid.levels.push_back({w, h, static_cast<std::uint32_t>(pyramid.texels.size())});
            for (const auto &pixel : working)
                for (float value : pixel)
                    pyramid.texels.push_back(std::bit_cast<std::uint32_t>(value));
            if (w == 1 && h == 1)
                break;
            const auto nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
            std::vector<Rgba> next(pixel_count(nw, nh));
            // Area box resampling covers every source texel, including odd edge rows.
            // Integer overlap numerators avoid floating endpoint slivers.
            for (std::uint32_t y = 0; y < nh; ++y) {
                for (std::uint32_t x = 0; x < nw; ++x) {
                    std::array<double, 4> sum{};
                    for (auto sy = y * h / nh; sy < ((y + 1) * h + nh - 1) / nh; ++sy) {
                        const auto wy =
                            std::min((y + 1) * h, (sy + 1) * nh) - std::max(y * h, sy * nh);
                        for (auto sx = x * w / nw; sx < ((x + 1) * w + nw - 1) / nw; ++sx) {
                            const auto wx =
                                std::min((x + 1) * w, (sx + 1) * nw) - std::max(x * w, sx * nw);
                            const double weight =
                                static_cast<double>(wx) * wy / (static_cast<double>(w) * h);
                            for (std::size_t c = 0; c < 4; ++c)
                                sum[c] += weight * working[pixel_count(w, sy) + sx][c];
                        }
                    }
                    for (std::size_t c = 0; c < 4; ++c)
                        next[pixel_count(nw, y) + x][c] = static_cast<float>(sum[c]);
                }
            }
            working = std::move(next);
            w = nw;
            h = nh;
        }
    } catch (const std::bad_alloc &) {
        return budget_error<ImagePyramid>("glTF image mip allocation failed");
    }
    return {.value = std::move(pyramid), .diagnostic = {}};
}

namespace {

bool gltf_wrap_valid(std::uint32_t wrap) { return wrap == 10497 || wrap == 33071 || wrap == 33648; }

// Coordinate and authored glTF wrap enum have distinct roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
float gltf_coordinate(float coordinate, std::uint32_t wrap) {
    if (wrap == 33071)
        return std::clamp(coordinate, 0.0f, 1.0f);
    // Reduce before multiplying by dimensions, also for huge finite coordinates.
    const float period = wrap == 33648 ? 2.0f : 1.0f;
    float phase = std::fmod(coordinate, period);
    if (wrap == 33648) {
        phase = std::abs(phase);
        return phase > 1 ? 2 - phase : phase;
    }
    if (phase < 0) {
        phase += period;
        // Preserve the last texel for negative values too small to survive 1+s.
        if (phase == period)
            phase = std::nextafter(period, 0.0f);
    }
    return phase;
}

// Integer texel dimension and authored glTF wrap enum are separate units.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int gltf_index(int index, int dimension, std::uint32_t wrap) {
    if (wrap == 10497)
        return (index % dimension + dimension) % dimension;
    // Mirrored coordinates have already been folded; clamp duplicates edge taps.
    return std::clamp(index, 0, dimension - 1);
}

// Mip index and continuous UV coordinates have distinct roles.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::optional<Rgba> gltf_level(const ImagePyramid &image, const GltfSampler &sampler,
                               std::size_t level_index, float s, float t, bool linear) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto &level = image.levels[level_index];
    const auto words = pixel_count(level.width, level.height) * 4;
    if (level.width == 0 || level.height == 0 || level.width > max_image_dimension ||
        level.height > max_image_dimension || level.offset % 4 != 0 ||
        level.offset > image.texels.size() || words > image.texels.size() - level.offset)
        return std::nullopt;
    const auto read = [&](int x, int y) {
        Rgba result{};
        const auto index =
            level.offset +
            4 * (pixel_count(level.width, static_cast<unsigned>(gltf_index(
                                              y, static_cast<int>(level.height), sampler.wrap_t))) +
                 static_cast<unsigned>(
                     gltf_index(x, static_cast<int>(level.width), sampler.wrap_s)));
        for (std::size_t c = 0; c < 4; ++c)
            result[c] = std::bit_cast<float>(image.texels[index + c]);
        return result;
    };
    const float x = gltf_coordinate(s, sampler.wrap_s) * static_cast<float>(level.width);
    const float y = gltf_coordinate(t, sampler.wrap_t) * static_cast<float>(level.height);
    if (!linear)
        return read(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)));
    const float bx = std::floor(x - 0.5f), by = std::floor(y - 0.5f);
    const auto ix = static_cast<int>(bx), iy = static_cast<int>(by);
    const float dx = x - 0.5f - bx, dy = y - 0.5f - by;
    const auto a = read(ix, iy), b = read(ix + 1, iy), c = read(ix, iy + 1),
               d = read(ix + 1, iy + 1);
    Rgba result{};
    for (std::size_t channel = 0; channel < 4; ++channel)
        result[channel] = (1 - dy) * ((1 - dx) * a[channel] + dx * b[channel]) +
                          dy * ((1 - dx) * c[channel] + dx * d[channel]);
    return result;
}

} // namespace

std::optional<Rgba> gltf_image_rgba(const ImageTexture &texture, float s, float t, float dsdx,
                                    float dtdx, float dsdy, float dtdy) {
    if (!texture.image || !texture.gltf_sampler || !texture.image->gltf ||
        texture.image->storage != ImageStorage::linear_float || texture.image->levels.empty() ||
        texture.image->levels.size() > std::bit_width(max_image_dimension) || !std::isfinite(s) ||
        !std::isfinite(t) || !std::isfinite(dsdx) || !std::isfinite(dtdx) || !std::isfinite(dsdy) ||
        !std::isfinite(dtdy))
        return std::nullopt;
    const auto &image = *texture.image;
    const auto &sampler = *texture.gltf_sampler;
    if (!gltf_wrap_valid(sampler.wrap_s) || !gltf_wrap_valid(sampler.wrap_t) ||
        (sampler.mag_filter != 9728 && sampler.mag_filter != 9729) ||
        (sampler.min_filter != 9728 && sampler.min_filter != 9729 &&
         (sampler.min_filter < 9984 || sampler.min_filter > 9987)))
        return std::nullopt;
    const auto &base = image.levels.front();
    // Match the shader's binary32 footprint before the logarithm. In particular,
    // a rounded 1/NPOT-dimension derivative must land on the same min/mag boundary.
    const float gradient_scale =
        std::max({std::abs(dsdx), std::abs(dtdx), std::abs(dsdy), std::abs(dtdy)});
    float lambda = 32;
    if (gradient_scale < 8192) {
        const auto length = [](float x, float y) {
            // Separate products also prevent contraction at a transition boundary.
            const volatile float xx = x * x, yy = y * y;
            return std::sqrt(xx + yy);
        };
        const float width = static_cast<float>(base.width),
                    height = static_cast<float>(base.height);
        const float rho =
            std::max(length(dsdx * width, dtdx * height), length(dsdy * width, dtdy * height));
        lambda = rho > 0 ? std::log2(rho) : -std::numeric_limits<float>::infinity();
    }
    // Explicit c=0 min/mag transition, permitted by modern GL and GLES sampling.
    // Mip-nearest half ties choose the lower mip: ceil(lambda + .5) - 1.
    const auto filter = lambda <= 0 ? sampler.mag_filter : sampler.min_filter;
    std::optional<Rgba> result;
    if (filter == 9728 || filter == 9729) {
        result = gltf_level(image, sampler, 0, s, t, filter == 9729);
    } else {
        const float lod = std::clamp(lambda, 0.0f, static_cast<float>(image.levels.size() - 1));
        const bool linear = filter == 9985 || filter == 9987;
        if (filter == 9984 || filter == 9985) {
            const auto level =
                static_cast<std::size_t>(std::max(0.0f, std::ceil(lod + 0.5f) - 1.0f));
            result = gltf_level(image, sampler, level, s, t, linear);
        } else {
            const auto lower = static_cast<std::size_t>(std::floor(lod));
            const auto upper = std::min(lower + 1, image.levels.size() - 1);
            auto a = gltf_level(image, sampler, lower, s, t, linear);
            const auto b = gltf_level(image, sampler, upper, s, t, linear);
            if (a && b) {
                const auto weight = lod - static_cast<float>(lower);
                for (std::size_t c = 0; c < 4; ++c)
                    (*a)[c] = (1 - weight) * (*a)[c] + weight * (*b)[c];
                result = a;
            }
        }
    }
    if (result && std::ranges::any_of(*result, [](float v) { return !std::isfinite(v); }))
        return std::nullopt;
    return result;
}

} // namespace rypath
