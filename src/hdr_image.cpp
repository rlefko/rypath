// SPDX-License-Identifier: GPL-3.0-only

#include "image_texture.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <locale>
#include <new>
#include <sstream>
#include <string_view>

namespace rypath {
namespace {
ImageResult<DecodedImage> error(std::string message, bool unsupported = false, bool limit = false) {
    return {.value = {},
            .diagnostic = "HDR: " + message,
            .unsupported = unsupported,
            .limit_exceeded = limit};
}

// Original bounded RGBE implementation. Conversion follows pinned PBRT/stb:
// mantissa * 2^(exponent - 136), with exponent zero representing black.
class Reader {
  public:
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;
    bool line(std::string_view &result) {
        const auto begin = offset;
        while (offset < bytes.size() && bytes[offset] != '\n') {
            if (offset - begin >= 4096 || offset >= 65536)
                return false;
            ++offset;
        }
        if (offset == bytes.size() || offset >= 65536)
            return false;
        result = std::string_view(reinterpret_cast<const char *>(bytes.data() + begin),
                                  offset++ - begin);
        if (!result.empty() && result.back() == '\r')
            result.remove_suffix(1);
        return result.find('\0') == std::string_view::npos;
    }
    bool take(std::uint8_t &v) {
        if (offset == bytes.size())
            return false;
        v = bytes[offset++];
        return true;
    }
};

bool numbers(std::string_view text, std::span<const double> expected, double tolerance = 0) {
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    for (double target : expected) {
        double value;
        if (!(input >> value) || !std::isfinite(value) || std::abs(value - target) > tolerance)
            return false;
    }
    input >> std::ws;
    return input.eof();
}
} // namespace

ImageResult<DecodedImage> decode_hdr(std::span<const std::uint8_t> bytes,
                                     const ImageLimits &limits) {
    if (bytes.size() > std::size_t{16} * 1024 * 1024)
        return error("input exceeds the 16 MiB limit", false, true);
    Reader input{bytes};
    std::string_view line;
    if (!input.line(line) || (line != "#?RADIANCE" && line != "#?RGBE"))
        return error("invalid signature");
    bool format = false;
    for (;;) {
        if (!input.line(line))
            return error("truncated or oversized header");
        if (line.empty())
            break;
        if (line.front() == '#')
            continue;
        const auto equal = line.find('=');
        if (equal == std::string_view::npos)
            continue; // Inert generator/history text.
        const auto key = line.substr(0, equal);
        const auto value = line.substr(equal + 1);
        constexpr std::array<double, 1> identity{1};
        constexpr std::array<double, 3> white{1, 1, 1};
        constexpr std::array<double, 8> srgb{.64, .33, .30, .60, .15, .06, .3127, .3290};
        if (key == "FORMAT") {
            if (format || value != "32-bit_rle_rgbe")
                return error("unsupported or duplicate FORMAT", true);
            format = true;
        } else if (key == "EXPOSURE" || key == "PIXASPECT") {
            if (!numbers(value, identity))
                return error("nonidentity " + std::string(key) + " is unsupported", true);
        } else if (key == "COLORCORR") {
            if (!numbers(value, white))
                return error("nonidentity COLORCORR is unsupported", true);
        } else if (key == "PRIMARIES") {
            if (!numbers(value, srgb, 1e-7))
                return error("non-sRGB PRIMARIES are unsupported", true);
        } else {
            return error("unsupported header assignment: " + std::string(key), true);
        }
    }
    if (!format || !input.line(line))
        return error("missing FORMAT or resolution");
    if (!line.starts_with("-Y "))
        return error("only -Y height +X width orientation is supported", true);
    line.remove_prefix(3);
    std::uint32_t width = 0, height = 0;
    auto y = std::from_chars(line.data(), line.data() + line.size(), height);
    if (y.ec != std::errc{} || std::string_view(y.ptr, line.data() + line.size()).size() < 4 ||
        !std::string_view(y.ptr, line.data() + line.size()).starts_with(" +X "))
        return error("invalid or unsupported resolution", true);
    auto x = std::from_chars(y.ptr + 4, line.data() + line.size(), width);
    if (x.ec != std::errc{} || x.ptr != line.data() + line.size() || !width || !height)
        return error("invalid resolution");
    if (width > std::min(4096u, limits.max_dimension) ||
        height > std::min(4096u, limits.max_dimension))
        return error("dimensions exceed the image limit", false, true);
    const auto pixels = static_cast<std::size_t>(width) * height;
    if (pixels * 3 * sizeof(float) >
            std::min(limits.retained_bytes, std::size_t{256} * 1024 * 1024) ||
        static_cast<std::size_t>(width) * 4 >
            std::min(limits.scratch_bytes, std::size_t{256} * 1024 * 1024))
        return error("decoded image exceeds the byte budget", false, true);
    try {
        DecodedImage image{.width = width,
                           .height = height,
                           .rgb = {},
                           .sample_format = ImageSampleFormat::float32,
                           .file_format = ImageFileFormat::hdr};
        image.floating_rgb.resize(pixels * 3);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4);
        bool modern = false;
        for (std::uint32_t row_index = 0; row_index < height; ++row_index) {
            std::array<std::uint8_t, 4> first{};
            for (auto &v : first)
                if (!input.take(v))
                    return error("truncated scanline");
            const bool marker = width >= 8 && first[0] == 2 && first[1] == 2 && first[2] < 128;
            if (row_index == 0)
                modern = marker;
            if (marker != modern)
                return error("mixed flat and RLE scanlines are unsupported", true);
            if (modern) {
                if ((static_cast<unsigned>(first[2]) << 8 | first[3]) != width)
                    return error("RLE scanline width mismatch");
                for (unsigned channel = 0; channel < 4; ++channel) {
                    for (std::uint32_t pos = 0; pos < width;) {
                        std::uint8_t code, value;
                        if (!input.take(code) || code == 0)
                            return error("truncated or zero-length RLE packet");
                        const auto count = code > 128 ? code - 128u : code;
                        if (count > width - pos)
                            return error("RLE packet exceeds scanline");
                        if (code > 128 && !input.take(value))
                            return error("truncated RLE run");
                        for (unsigned i = 0; i < count; ++i) {
                            if (code <= 128 && !input.take(value))
                                return error("truncated RLE literal");
                            row[(pos++ * 4) + channel] = value;
                        }
                    }
                }
            } else {
                std::copy(first.begin(), first.end(), row.begin());
                for (std::size_t i = 4; i < row.size(); ++i)
                    if (!input.take(row[i]))
                        return error("truncated flat pixels");
            }
            for (std::uint32_t col = 0; col < width; ++col) {
                const auto *rgbe = row.data() + static_cast<std::size_t>(col) * 4;
                if (!modern && rgbe[0] == 1 && rgbe[1] == 1 && rgbe[2] == 1)
                    return error("legacy RGBE RLE is unsupported", true);
                for (unsigned c = 0; c < 3; ++c)
                    image
                        .floating_rgb[(static_cast<std::size_t>(row_index) * width + col) * 3 + c] =
                        rgbe[3] ? std::ldexp(static_cast<float>(rgbe[c]), int(rgbe[3]) - 136) : 0;
            }
        }
        if (input.offset != bytes.size())
            return error("trailing image bytes");
        return {.value = std::move(image), .diagnostic = {}};
    } catch (const std::bad_alloc &) {
        return error("could not allocate decoded image", false, true);
    }
}
} // namespace rypath
