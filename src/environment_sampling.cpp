// SPDX-License-Identifier: GPL-3.0-only
#include "environment_sampling.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace rypath {

std::optional<std::size_t> environment_sampling_bytes(std::uint32_t width) {
    if (width == 0 || width > image_dimension_ceiling)
        return std::nullopt;
    const auto side = std::size_t(width) + 1;
    if (side > std::numeric_limits<std::size_t>::max() / side)
        return std::nullopt;
    const auto count = side * side;
    if (count > std::numeric_limits<std::uint32_t>::max() / 2 ||
        count > std::numeric_limits<std::size_t>::max() / (2 * sizeof(float)))
        return std::nullopt;
    return count * 2 * sizeof(float);
}

std::optional<EnvironmentSampling> build_environment_sampling(const ImagePyramid &image,
                                                              std::size_t byte_budget,
                                                              std::string &diagnostic) {
    diagnostic.clear();
    if (image.levels.size() != 1 || !image.native_base ||
        image.levels.front().width != image.levels.front().height) {
        diagnostic = "Environment sampling requires a square native image base";
        return std::nullopt;
    }
    const auto width = image.levels.front().width;
    const auto bytes = environment_sampling_bytes(width);
    if (!bytes || *bytes > byte_budget) {
        diagnostic = "Environment sampling tables exceed resource limits";
        return std::nullopt;
    }
    const auto mass = [&](std::uint32_t x, std::uint32_t y) -> std::optional<double> {
        const auto rgb = image_base_texel_linear_rgb(image, x, y);
        if (!rgb)
            return std::nullopt;
        return (double(std::max((*rgb)[0], 0.f)) + double(std::max((*rgb)[1], 0.f)) +
                double(std::max((*rgb)[2], 0.f))) /
               3;
    };
    try {
        EnvironmentSampling result;
        result.width = width;
        const auto side = std::size_t(width) + 1;
        result.compensated_offset = static_cast<std::uint32_t>(side * side);
        result.cdf.assign(*bytes / sizeof(float), 0.f);
        // Keep only O(width) double scratch rather than a double copy of the map.
        std::vector<double> full_rows(width), compensated_rows(width);
        std::vector<double> full_prefix(side), compensated_prefix(side);
        double total = 0;
        for (std::uint32_t y = 0; y < width; ++y) {
            double row = 0;
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto value = mass(x, y);
                if (!value) {
                    diagnostic = "Environment sampling texel is invalid";
                    return std::nullopt;
                }
                row += *value;
            }
            full_rows[y] = row;
            total += row;
        }
        const double average = total / (double(width) * width);
        const auto write_prefix = [&](std::size_t offset, const std::vector<double> &prefix,
                                      double sum) {
            for (std::uint32_t x = 1; x <= width; ++x)
                result.cdf[offset + x] =
                    sum > 0 ? static_cast<float>(prefix[x] / sum) : float(x) / float(width);
            result.cdf[offset + width] = 1.f;
        };
        double compensated_total = 0;
        for (std::uint32_t y = 0; y < width; ++y) {
            full_prefix[0] = compensated_prefix[0] = 0;
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto value = mass(x, y);
                if (!value) {
                    diagnostic = "Environment sampling texel is invalid";
                    return std::nullopt;
                }
                full_prefix[x + 1] = full_prefix[x] + *value;
                compensated_prefix[x + 1] = compensated_prefix[x] + std::max(*value - average, 0.);
            }
            compensated_rows[y] = compensated_prefix.back();
            compensated_total += compensated_rows[y];
            const auto row_offset = side * (y + 1);
            write_prefix(row_offset, full_prefix, full_rows[y]);
            write_prefix(result.compensated_offset + row_offset, compensated_prefix,
                         compensated_rows[y]);
        }
        double full_sum = 0, compensated_sum = 0;
        for (std::uint32_t y = 0; y < width; ++y) {
            full_sum += full_rows[y];
            compensated_sum += compensated_rows[y];
            result.cdf[y + 1] =
                total > 0 ? static_cast<float>(full_sum / total) : float(y + 1) / float(width);
            result.cdf[result.compensated_offset + y + 1] =
                compensated_total > 0 ? static_cast<float>(compensated_sum / compensated_total)
                                      : float(y + 1) / float(width);
        }
        result.cdf[width] = result.cdf[result.compensated_offset + width] = 1.f;
        return result;
    } catch (const std::bad_alloc &) {
        diagnostic = "Environment sampling table allocation failed";
        return std::nullopt;
    }
}

} // namespace rypath
