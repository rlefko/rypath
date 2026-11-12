// SPDX-License-Identifier: GPL-3.0-only

#include "image_output.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {

namespace fs = std::filesystem;

struct CFReleaseObject {
    void operator()(CFTypeRef object) const { CFRelease(object); }
};

template <typename T> using CFObject = std::unique_ptr<T, CFReleaseObject>;

void require(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string{description});
    }
}

struct TestDirectory {
    TestDirectory() : path(fs::temp_directory_path() / "rypath-png-test.XXXXXX") {
        auto name = path.native();
        require(mkdtemp(name.data()) != nullptr, "could not create test directory");
        path = std::move(name);
    }

    TestDirectory(const TestDirectory &) = delete;
    TestDirectory &operator=(const TestDirectory &) = delete;

    ~TestDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

std::string read_file(const fs::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool has_color_tag(std::string_view png) {
    constexpr size_t signature_size = 8;
    constexpr size_t chunk_overhead = 12; // Length, type, and CRC.
    for (size_t offset = signature_size;
         offset <= png.size() && png.size() - offset >= chunk_overhead;) {
        std::uint32_t length = 0;
        for (size_t byte = 0; byte < 4; ++byte) {
            length = (length << 8) | static_cast<unsigned char>(png[offset + byte]);
        }
        if (length > png.size() - offset - chunk_overhead) {
            return false;
        }
        const auto type = png.substr(offset + 4, 4);
        if (type == "sRGB" || type == "iCCP") {
            return true;
        }
        offset += chunk_overhead + length;
    }
    return false;
}

void check_png(const fs::path &path, const rypath::Film &film) {
    require(has_color_tag(read_file(path)), "PNG lacks a serialized color space tag");
    const auto &name = path.native();
    const CFObject<const __CFURL> url{CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8 *>(name.data()), static_cast<CFIndex>(name.size()),
        false)};
    require(static_cast<bool>(url), "could not create PNG URL");
    const CFObject<CGImageSource> source{CGImageSourceCreateWithURL(url.get(), nullptr)};
    require(static_cast<bool>(source), "could not open written PNG");
    const auto type = CGImageSourceGetType(source.get());
    require(type && CFEqual(type, CFSTR("public.png")) && CGImageSourceGetCount(source.get()) == 1,
            "expected a single PNG image");
    const CFObject<CGImage> decoded{CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr)};
    require(decoded && CGImageGetWidth(decoded.get()) == film.width &&
                CGImageGetHeight(decoded.get()) == film.height &&
                CGImageGetBitsPerComponent(decoded.get()) == 8,
            "unexpected decoded dimensions or channel depth");

    const CFObject<CGColorSpace> srgb{CGColorSpaceCreateWithName(kCGColorSpaceSRGB)};
    require(static_cast<bool>(srgb), "could not create sRGB color space");
    const auto image_space = CGImageGetColorSpace(decoded.get());
    require(image_space != nullptr, "PNG has no RGB color space");
    const CFObject<const __CFString> image_space_name{CGColorSpaceCopyName(image_space)};
    const CFObject<const __CFData> actual_profile{CGColorSpaceCopyICCData(image_space)};
    const CFObject<const __CFData> srgb_profile{CGColorSpaceCopyICCData(srgb.get())};
    require(
        (image_space_name && CFEqual(image_space_name.get(), kCGColorSpaceSRGB)) ||
            (actual_profile && srgb_profile && CFEqual(actual_profile.get(), srgb_profile.get())),
        "decoded color space is not sRGB");

    std::vector<std::uint8_t> rgba(film.pixels.size() * 4);
    const CFObject<CGContext> context{CGBitmapContextCreate(
        rgba.data(), film.width, film.height, 8, static_cast<size_t>(film.width) * 4, srgb.get(),
        static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrder32Big)};
    require(static_cast<bool>(context), "could not create RGB decode buffer");
    CGContextDrawImage(context.get(), CGRectMake(0, 0, film.width, film.height), decoded.get());
    constexpr std::array<std::array<std::uint8_t, 3>, 4> expected{{
        {0, 0, 0},
        {253, 0, 9},
        {110, 230, 31},
        {0, 75, 222},
    }};
    for (size_t index = 0; index < expected.size(); ++index) {
        const std::array actual{rgba[index * 4], rgba[index * 4 + 1], rgba[index * 4 + 2]};
        require(actual == expected[index],
                "decoded RGB differs from the expected display transform");
    }
}

void test_write() {
    const TestDirectory directory;
    const auto output = directory.path / "render with spaces.png";
    auto film = rypath::make_film(2, 2);
    film.pixels = {
        {{0, 0, 0}, {}, 2},
        {{0.78, 0.38, 0.03}, {0.02, 0.02, 0.01}, 2},
        {{0.7, 1.2, 0.22}, {}, 2},
        {{0.3, 0.2, 1.4}, {}, 2},
    };
    const auto original = film;
    std::ofstream{output} << "existing output";
    const auto diagnostic = rypath::write_png(film, output);
    require(diagnostic.empty(), diagnostic);
    require(film == original, "PNG export mutated the raw film");
    check_png(output, film);

    const auto original_png = read_file(output);
    require(!original_png.empty(), "PNG output is empty");
    film.pixels[0].count = 0;
    require(!rypath::write_png(film, output).empty(), "incomplete film was published");
    require(read_file(output) == original_png, "failed publication changed existing PNG");

    const auto occupied = directory.path / "occupied.png";
    fs::create_directory(occupied);
    const auto marker = occupied / "keep";
    std::ofstream{marker} << "keep me";
    const auto publish_error = rypath::write_png(original, occupied);
    require(publish_error.starts_with("could not publish PNG image"),
            "expected rename over the occupied destination to fail");
    require(read_file(marker) == "keep me", "failed rename changed the existing destination");
    for (const auto &entry : fs::directory_iterator{directory.path}) {
        require(entry.path() == output || entry.path() == occupied,
                "PNG export left a temporary file");
    }
}

} // namespace

int main() {
    @autoreleasepool {
        try {
            test_write();
            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
