// SPDX-License-Identifier: GPL-3.0-only

#include "image_output.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <system_error>
#include <unistd.h>

namespace rypath {
namespace {
std::string system_diagnostic(std::string_view operation, int error) {
    return std::string{operation} + ": " +
           std::error_code{error, std::generic_category()}.message();
}

struct PendingExr {
    explicit PendingExr(const std::filesystem::path &target)
        : name((target.parent_path() / (target.filename().native().starts_with(".rypath-exr.")
                                            ? ".rypath-exr-alt.XXXXXX"
                                            : ".rypath-exr.XXXXXX"))
                   .native()),
          fd(mkstemp(name.data())), owns_path(fd >= 0) {}
    PendingExr(const PendingExr &) = delete;
    PendingExr &operator=(const PendingExr &) = delete;
    ~PendingExr() {
        if (fd >= 0)
            close(fd);
        if (owns_path)
            unlink(name.c_str());
    }
    bool write(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > maximum || bytes.size() > maximum - offset) {
            write_error = EFBIG;
            return false;
        }
        std::size_t done = 0;
        while (done < bytes.size() && write_error == 0) {
            const auto chunk = std::min(
                bytes.size() - done, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const auto written =
                pwrite(fd, bytes.data() + done, chunk, static_cast<off_t>(offset + done));
            if (written > 0)
                done += static_cast<std::size_t>(written);
            else if (written == 0 || errno != EINTR)
                write_error = written == 0 ? EIO : errno;
        }
        return write_error == 0;
    }
    std::string name;
    int fd;
    bool owns_path;
    int write_error = 0;
};
} // namespace

std::string write_exr(const Film &film, const std::filesystem::path &path, ExrPrecision precision,
                      std::span<const std::pair<std::string, std::string>> metadata) {
    if (path.empty() || path.native().find('\0') != std::string::npos)
        return "invalid EXR output path";
    const auto pixels = static_cast<std::uint64_t>(film.width) * film.height;
    if (pixels == 0 || pixels != film.pixels.size() ||
        pixels > std::numeric_limits<std::size_t>::max() / (3 * sizeof(float)))
        return "invalid EXR film dimensions";
    if (!std::isfinite(film.exposure))
        return "cannot write a film with nonfinite exposure";
    std::vector<float> rgb;
    rgb.reserve(static_cast<std::size_t>(pixels) * 3);
    const auto count = film.pixels.front().count;
    for (const auto &pixel : film.pixels) {
        if (count == 0 || pixel.count != count)
            return "cannot write a film with incomplete sample counts";
        const auto finite = [](double value) { return std::isfinite(value); };
        if (!std::ranges::all_of(pixel.sum, finite) ||
            !std::ranges::all_of(pixel.correction, finite) ||
            !std::ranges::all_of(film_mean(pixel), finite))
            return "cannot write a film with nonfinite XYZ values";
        for (const double value : linear_rgb(film, pixel)) {
            if (!std::isfinite(value))
                return "cannot write a film with nonfinite linear RGB values";
            if (std::abs(value) > std::numeric_limits<float>::max())
                return "EXR FLOAT output would overflow";
            if (precision == ExrPrecision::half && std::abs(value) > 65504)
                return "EXR HALF output would overflow";
            rgb.push_back(static_cast<float>(value));
        }
    }
    PendingExr pending{path};
    if (pending.fd < 0)
        return system_diagnostic("could not create sibling EXR temporary file", errno);
    ImageLimits limits;
    limits.max_dimension = std::max(film.width, film.height);
    limits.retained_bytes = rgb.size() * sizeof(float);
    const auto diagnostic = encode_exr(
        {film.width, film.height, rgb, precision, metadata},
        [&](std::uint64_t offset, std::span<const std::uint8_t> bytes) {
            return pending.write(offset, bytes);
        },
        limits);
    if (pending.write_error != 0)
        return system_diagnostic("could not write EXR temporary file", pending.write_error);
    if (!diagnostic.empty())
        return diagnostic;
    const int close_result = close(pending.fd);
    pending.fd = -1;
    if (close_result != 0)
        return system_diagnostic("could not close EXR temporary file", errno);
    if (rename(pending.name.c_str(), path.c_str()) != 0)
        return system_diagnostic("could not publish EXR image", errno);
    pending.owns_path = false;
    return {};
}
} // namespace rypath
