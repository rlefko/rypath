// SPDX-License-Identifier: GPL-3.0-only

#include "image_output.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <system_error>
#include <unistd.h>

namespace rypath {
namespace {

struct CFReleaseObject {
    void operator()(CFTypeRef object) const { CFRelease(object); }
};

template <typename T> using CFObject = std::unique_ptr<T, CFReleaseObject>;

std::string system_diagnostic(std::string_view operation, int error) {
    return std::string{operation} + ": " +
           std::error_code{error, std::generic_category()}.message();
}

struct PendingImage {
    explicit PendingImage(const std::filesystem::path &target)
        : name((target.parent_path() / temporary_basename(target.filename())).native()),
          fd(mkstemp(name.data())) {}

    PendingImage(const PendingImage &) = delete;
    PendingImage &operator=(const PendingImage &) = delete;

    ~PendingImage() {
        if (fd >= 0) {
            close(fd);
        }
        if (owns_path) {
            unlink(name.c_str());
        }
    }

    std::string name;
    int fd;
    bool owns_path = fd >= 0;
    int write_error = 0;

  private:
    static const char *temporary_basename(const std::filesystem::path &target_name) {
        if (target_name.native().starts_with(".rypath-png.")) {
            return ".rypath-png-alt.XXXXXX";
        }
        return ".rypath-png.XXXXXX";
    }
};

size_t write_image_data(void *info, const void *buffer, size_t count) {
    auto &pending = *static_cast<PendingImage *>(info);
    const auto *data = static_cast<const std::byte *>(buffer);
    size_t offset = 0;
    while (offset < count && pending.write_error == 0) {
        const auto chunk =
            std::min(count - offset, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const auto written = write(pending.fd, data + offset, chunk);
        if (written > 0) {
            offset += static_cast<size_t>(written);
        } else if (written == 0 || errno != EINTR) {
            pending.write_error = written == 0 ? EIO : errno;
        }
    }
    return offset;
}

std::string encode_png(CGImageRef image, const std::filesystem::path &path) {
    PendingImage pending{path};
    if (pending.fd < 0) {
        return system_diagnostic("could not create sibling PNG temporary file", errno);
    }

    const CGDataConsumerCallbacks callbacks{write_image_data, nullptr};
    {
        const CFObject<CGDataConsumer> consumer{CGDataConsumerCreate(&pending, &callbacks)};
        if (!consumer) {
            return "could not create PNG data consumer";
        }
        const CFObject<CGImageDestination> destination{CGImageDestinationCreateWithDataConsumer(
            consumer.get(), CFSTR("public.png"), 1, nullptr)};
        if (!destination) {
            return "could not create PNG image destination";
        }
        CGImageDestinationAddImage(destination.get(), image, nullptr);
        const bool finalized = CGImageDestinationFinalize(destination.get());
        if (pending.write_error != 0) {
            return system_diagnostic("could not write PNG temporary file", pending.write_error);
        }
        if (!finalized) {
            return "could not finalize PNG image";
        }
    }
    const int close_result = close(pending.fd);
    pending.fd = -1;
    if (close_result != 0) {
        return system_diagnostic("could not close PNG temporary file", errno);
    }
    if (rename(pending.name.c_str(), path.c_str()) != 0) {
        return system_diagnostic("could not publish PNG image", errno);
    }
    pending.owns_path = false;
    return {};
}

} // namespace

std::string write_png(const Film &film, const std::filesystem::path &path) {
    @autoreleasepool {
        if (path.empty() || path.native().find('\0') != std::string::npos) {
            return "invalid PNG output path";
        }
        const auto pixels = static_cast<std::uint64_t>(film.width) * film.height;
        if (pixels == 0 || pixels != film.pixels.size() ||
            pixels > std::numeric_limits<size_t>::max() / 3) {
            return "invalid PNG film dimensions";
        }
        std::vector<std::uint8_t> rgb;
        rgb.reserve(static_cast<size_t>(pixels) * 3);
        const auto count = film.pixels.front().count;
        for (const auto &pixel : film.pixels) {
            if (count == 0 || pixel.count != count) {
                return "cannot write a film with incomplete sample counts";
            }
            const auto mean = film_mean(pixel);
            if (!std::ranges::all_of(mean, [](double value) { return std::isfinite(value); })) {
                return "cannot write a film with nonfinite XYZ values";
            }
            const auto linear = linear_rgb(film, pixel);
            if (!std::ranges::all_of(linear, [](double value) { return std::isfinite(value); })) {
                return "cannot write a film with nonfinite linear RGB values";
            }
            const auto channels = display_rgb(film, pixel);
            rgb.insert(rgb.end(), channels.begin(), channels.end());
        }
        const CFObject<CGColorSpace> color_space{CGColorSpaceCreateWithName(kCGColorSpaceSRGB)};
        const CFObject<CGDataProvider> provider{
            CGDataProviderCreateWithData(nullptr, rgb.data(), rgb.size(), nullptr)};
        if (!color_space || !provider) {
            return "could not create PNG color space or pixel buffer";
        }
        const CFObject<CGImage> image{
            CGImageCreate(film.width, film.height, 8, 24, static_cast<size_t>(film.width) * 3,
                          color_space.get(), static_cast<CGBitmapInfo>(kCGImageAlphaNone),
                          provider.get(), nullptr, false, kCGRenderingIntentDefault)};
        if (!image) {
            return "could not create RGB image for PNG output";
        }
        return encode_png(image.get(), path);
    }
}

} // namespace rypath
