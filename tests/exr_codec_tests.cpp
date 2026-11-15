// SPDX-License-Identifier: GPL-3.0-only
#include "exr_image.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace rypath;
using Bytes = std::vector<std::uint8_t>;
void require(bool test, std::string_view message) {
    if (!test)
        throw std::runtime_error(std::string(message));
}
Bytes read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "fixture open failed");
    return Bytes(std::istreambuf_iterator<char>(input), {});
}
std::size_t attribute(const Bytes &bytes, std::string_view name) {
    std::size_t at = 8;
    while (at < bytes.size() && bytes[at]) {
        const auto start = at;
        while (at < bytes.size() && bytes[at])
            ++at;
        const std::string_view found(reinterpret_cast<const char *>(bytes.data() + start),
                                     at - start);
        ++at;
        while (at < bytes.size() && bytes[at])
            ++at;
        ++at;
        require(at + 4 <= bytes.size(), "test malformed attribute");
        std::uint32_t size = 0;
        for (int i = 0; i < 4; ++i)
            size |= std::uint32_t(bytes[at + i]) << (8 * i);
        at += 4;
        if (found == name)
            return at;
        at += size;
    }
    throw std::runtime_error("test attribute not found");
}
DecodedImage success(ImageResult<DecodedImage> result) {
    if (!result.value)
        throw std::runtime_error(result.diagnostic);
    return std::move(*result.value);
}
Bytes encode(const ExrImageView &image, const ImageLimits &limits = {}) {
    Bytes result;
    auto error = encode_exr(
        image,
        [&](std::uint64_t offset, std::span<const std::uint8_t> data) {
            require(offset + data.size() < std::size_t{1024} * 1024,
                    "unexpected encoded output size");
            result.resize(std::max(result.size(), static_cast<std::size_t>(offset + data.size())));
            std::copy(data.begin(), data.end(),
                      result.begin() + static_cast<std::ptrdiff_t>(offset));
            return true;
        },
        limits);
    if (!error.empty())
        throw std::runtime_error(error);
    return result;
}
void fixtures(const std::filesystem::path &directory) {
    for (const char *name : {"none", "rle", "zips", "zip", "piz", "pxr24", "b44", "b44a", "dwaa",
                             "dwab", "gray", "tile-origin"}) {
        const auto bytes = read(directory / (std::string(name) + ".exr"));
        const auto expected = read(directory / (std::string(name) + ".f32"));
        auto image = success(decode_exr(bytes));
        const bool gray = std::string_view(name) == "gray",
                   rgba = std::string_view(name) == "tile-origin";
        require(image.width == 19 && image.height == 17, "fixture data window extent");
        require(image.source_channels == (gray   ? 1u
                                          : rgba ? 4u
                                                 : 3u),
                "fixture source channel count");
        require(image.sample_format ==
                    (rgba ? ImageSampleFormat::float32 : ImageSampleFormat::half),
                "fixture sample format");
        require(image.file_format == ImageFileFormat::exr && image.rgb.empty() &&
                    image.alpha.empty(),
                "fixture floating representation");
        require(expected.size() == std::size_t{19} * 17 *
                                       (gray   ? 1
                                        : rgba ? 4
                                               : 3) *
                                       sizeof(float),
                "fixture oracle extent");
        for (std::size_t i = 0; i < std::size_t{19} * 17; ++i)
            for (int c = 0; c < (rgba ? 4 : 3); ++c) {
                std::uint32_t bits = 0;
                const auto offset = (i * (gray ? 1 : rgba ? 4 : 3) + (gray ? 0 : c)) * 4;
                for (int b = 0; b < 4; ++b)
                    bits |= std::uint32_t(expected[offset + b]) << (b * 8);
                const float actual =
                    c == 3 ? image.floating_alpha[i] : image.floating_rgb[i * 3 + c];
                require(std::bit_cast<std::uint32_t>(actual) == bits,
                        "independent OpenEXR decoded sample mismatch");
            }
    }
    for (const char *name : {"mixed", "uint", "wide"}) {
        const auto result = decode_exr(read(directory / (std::string(name) + ".exr")));
        require(!result.value && result.unsupported, "unsupported fixture classification");
    }
    for (const char *name : {"nonfinite.exr", "alpha-nonfinite.exr"}) {
        const auto invalid = decode_exr(read(directory / name));
        require(!invalid.value && !invalid.unsupported, "nonfinite fixture invalid");
    }
}
void malformed(const std::filesystem::path &directory) {
    const auto input = read(directory / "zip.exr");
    for (std::size_t n = 0; n < input.size(); ++n)
        require(!decode_exr(std::span(input).first(n)).value, "truncated EXR accepted");
    auto changed = input;
    changed[0] = 0;
    require(!decode_exr(changed).value, "bad magic accepted");
    for (unsigned flag : {8u, 16u}) {
        changed = input;
        changed[5] |= flag;
        require(decode_exr(changed).unsupported, "deep/multipart flag classification");
    }
    changed = input;
    const auto compression = attribute(input, "compression");
    for (unsigned codec : {10u, 11u, 12u, 13u}) {
        changed[compression] = static_cast<std::uint8_t>(codec);
        require(decode_exr(changed).unsupported, "new codec classification");
    }
    changed = input;
    const auto channels = attribute(input, "channels");
    changed[channels] = 'X';
    require(decode_exr(changed).unsupported, "extra/partial channels accepted");
    // A valid larger display window is metadata, not retained pixel expansion.
    changed = input;
    auto put32 = [](Bytes &data, std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            data[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    // Shrinking the declared row makes libdeflate report insufficient output
    // capacity, which Core maps to OUT_OF_MEMORY without rejecting an allocator
    // request. This exercises resource classification without system exhaustion.
    put32(changed, attribute(changed, "dataWindow") + 8, 12);
    const auto decoder_resource = decode_exr(changed);
    require(!decoder_resource.value && decoder_resource.limit_exceeded &&
                decoder_resource.diagnostic.find("memory") != std::string::npos,
            "Core allocation error without a budget rejection must remain a resource failure");
    changed = input;
    put32(changed, attribute(changed, "displayWindow") + 8, 8000);
    require(success(decode_exr(changed)).width == 19,
            "display window expanded or constrained data window");
    changed = input;
    put32(changed, attribute(changed, "dataWindow") + 8, 5000);
    ImageLimits generous;
    generous.max_dimension = 8192;
    require(decode_exr(changed, generous).limit_exceeded, "global decode dimension cap ignored");
    auto tile = read(directory / "tile-origin.exr");
    put32(tile, attribute(tile, "tiles"), 5000);
    require(decode_exr(tile).limit_exceeded, "tile header dimension cap ignored");
    tile = read(directory / "tile-origin.exr");
    tile[attribute(tile, "tiles") + 8] = 1;
    require(decode_exr(tile).unsupported, "mipmap tiles accepted");
    // Rename the optional color-space attribute without changing header offsets.
    changed = input;
    const std::string chroma = "chromaticities";
    const auto name_at = std::search(changed.begin(), changed.end(), chroma.begin(), chroma.end());
    require(name_at != changed.end(), "chromaticities attr not found");
    *(name_at + static_cast<std::ptrdiff_t>(chroma.size() - 1)) = 'X';
    require(bool(decode_exr(changed).value), "absent chromaticities must default to sRGB");
    changed = input;
    changed[channels + 10] = 2; // First channel xSampling.
    require(!decode_exr(changed).value, "subsampled channels accepted");
}
void output() {
    const std::array<float, 12> rgb{-2, 0, 0.25f, 1, 2, 8, 65504, 0.5f, -0.5f, 0.125f, 0.0625f, 0};
    const std::array<std::pair<std::string, std::string>, 1> attributes{
        {{"rypath:test", "linear RGB"}}};
    for (auto precision : {ExrPrecision::half, ExrPrecision::float32}) {
        ExrImageView view{2, 2, rgb, precision, attributes};
        auto bytes = encode(view);
        require(bytes[attribute(bytes, "compression")] == 3, "output must use ZIP");
        const auto metadata = attribute(bytes, "rypath:test");
        require(std::memcmp(bytes.data() + metadata, "linear RGB", 10) == 0,
                "custom string metadata");
        const auto image = success(decode_exr(bytes));
        require(std::equal(rgb.begin(), rgb.end(), image.floating_rgb.begin()), "HDR roundtrip");
    }
    const std::array<float, 9> rounding{0x1p-25f,       0x1.8p-24f,         -0.f,
                                        1.f + 0x1p-11f, 1.f + 3 * 0x1p-11f, -1.f - 0x1p-11f,
                                        0x1p-24f,       0x1p-14f,           65504.f};
    const std::array<float, 9> rounded{0.f,  0x1p-23f, -0.f,     1.f,    1.f + 0x1p-9f,
                                       -1.f, 0x1p-24f, 0x1p-14f, 65504.f};
    const auto converted = success(decode_exr(encode({3, 1, rounding, ExrPrecision::half})));
    for (std::size_t i = 0; i < rounded.size(); ++i)
        require(std::bit_cast<std::uint32_t>(converted.floating_rgb[i]) ==
                    std::bit_cast<std::uint32_t>(rounded[i]),
                "HALF ties/subnormal conversion");
    ImageLimits tiny;
    tiny.scratch_bytes = 0;
    require(
        !encode_exr(
             {2, 2, rgb}, [](std::uint64_t, std::span<const std::uint8_t>) { return true; }, tiny)
             .empty(),
        "output scratch limit");
    auto fail = [](std::uint64_t, std::span<const std::uint8_t>) { return false; };
    require(!encode_exr({2, 2, rgb}, fail).empty(), "failed output callback accepted");
    auto throws = [](std::uint64_t, std::span<const std::uint8_t>) -> bool {
        throw std::runtime_error("failure");
    };
    require(!encode_exr({2, 2, rgb}, throws).empty(), "throwing callback accepted");
    auto bad = rgb;
    bad[0] = std::numeric_limits<float>::infinity();
    require(!encode_exr({2, 2, bad}, fail).empty(), "nonfinite output accepted");
    bad[0] = 65520;
    require(!encode_exr({2, 2, bad, ExrPrecision::half}, fail).empty(), "HALF overflow accepted");
    const std::array<std::pair<std::string, std::string>, 1> reserved{{{"compression", "oops"}}};
    require(encode_exr({2, 2, rgb, ExrPrecision::float32, reserved}, fail).find("reserved") !=
                std::string::npos,
            "reserved metadata accepted");
    const std::array<std::pair<std::string, std::string>, 2> duplicate{
        {{"custom", "1"}, {"custom", "2"}}};
    require(encode_exr({2, 2, rgb, ExrPrecision::float32, duplicate}, fail).find("duplicate") !=
                std::string::npos,
            "duplicate metadata accepted");
    // The failing write on call 3 reaches chunk data or offset-table finalization.
    for (int stop : {1, 2, 3, 4, 5, 6, 7, 8}) {
        int calls = 0;
        const auto result =
            encode_exr({2, 2, rgb}, [&](std::uint64_t, std::span<const std::uint8_t>) {
                return ++calls != stop;
            });
        require(calls < stop || !result.empty(), "output failure/finalization ignored");
    }
    bool nested = false;
    auto result = encode_exr({2, 2, rgb}, [&](std::uint64_t, std::span<const std::uint8_t>) {
        if (!nested) {
            nested = true;
            require(success(decode_exr(encode({2, 2, rgb}))).floating_rgb[0] == -2, "nested codec");
        }
        return true;
    });
    require(result.empty() && nested, "nested TLS allocation scope");
}
void budgets(const std::filesystem::path &directory) {
    const auto input = read(directory / "zip.exr");
    ImageLimits limits;
    limits.max_dimension = 18;
    require(decode_exr(input, limits).limit_exceeded, "dimension limit classification");
    limits = {};
    limits.retained_bytes = 19 * 17 * 3 * 4 - 1;
    require(decode_exr(input, limits).limit_exceeded, "retained allocation boundary");
    ++limits.retained_bytes;
    require(bool(decode_exr(input, limits).value), "exact retained boundary rejected");
    limits = {};
    limits.scratch_bytes = 0;
    require(decode_exr(input, limits).limit_exceeded, "header allocation budget");
    for (const char *name : {"zip", "piz", "b44", "dwaa", "dwab"}) {
        const auto bytes = read(directory / (std::string(name) + ".exr"));
        for (std::size_t budget : {128u, 1024u, 4096u, 16384u, 65536u, 262144u, 1048576u}) {
            limits = {};
            limits.scratch_bytes = budget;
            const auto result = decode_exr(bytes, limits);
            require(result.value || result.limit_exceeded, "allocation failure classification");
        }
        // Locate and straddle the exact successful scratch boundary.
        std::size_t low = 0, high = std::size_t{8} * 1024 * 1024;
        while (low + 1 < high) {
            const auto midpoint = low + (high - low) / 2;
            limits = {};
            limits.scratch_bytes = midpoint;
            const auto result = decode_exr(bytes, limits);
            if (result.value)
                high = midpoint;
            else {
                require(result.limit_exceeded, "scratch binary boundary failure");
                low = midpoint;
            }
        }
        limits.scratch_bytes = high;
        require(bool(decode_exr(bytes, limits).value), "exact scratch boundary rejected");
        limits.scratch_bytes = high - 1;
        require(decode_exr(bytes, limits).limit_exceeded, "scratch lower boundary accepted");
        require(bool(decode_exr(bytes).value), "allocator cleanup changed later decode");
    }
    std::vector<std::future<void>> jobs;
    for (int t = 0; t < 6; ++t)
        jobs.push_back(std::async(std::launch::async, [&, t] {
            for (int i = 0; i < 12; ++i) {
                ImageLimits local;
                if ((i + t) % 2)
                    local.scratch_bytes = 1024;
                const auto result = decode_exr(input, local);
                require(local.scratch_bytes == 1024 ? result.limit_exceeded : bool(result.value),
                        "concurrent budget isolation");
            }
        }));
    for (auto &job : jobs)
        job.get();
}
} // namespace
int main(int argc, char **argv) {
    try {
        require(argc == 2, "expected EXR fixture directory argument");
        const std::filesystem::path directory(argv[1]);
        fixtures(directory);
        malformed(directory);
        output();
        budgets(directory);
        std::cout << "EXR: 10 classic codecs, independent samples, tiles/origins, bounded failure "
                     "and output checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
