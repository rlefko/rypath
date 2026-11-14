// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "image_texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace rypath::test {

using ReferenceRgb = std::array<double, 3>;
using ReferenceUv = std::array<double, 2>;

inline double ewa_reference_decode(unsigned byte, ImageEncoding encoding) {
    const double encoded = byte / 255.0;
    return encoding == ImageEncoding::linear
               ? encoded
               : (encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4));
}

inline int ewa_reference_wrap(int coordinate, int size, ImageWrap wrap) {
    if (wrap == ImageWrap::repeat) {
        return (coordinate % size + size) % size;
    }
    return std::clamp(coordinate, 0, size - 1);
}

inline ReferenceRgb ewa_reference_texel(const ImagePyramid &image, const ImageMipLevel &mip, int x,
                                        int y) {
    const auto width = static_cast<int>(mip.width);
    const auto height = static_cast<int>(mip.height);
    if (image.wrap == ImageWrap::black && (x < 0 || x >= width || y < 0 || y >= height)) {
        return {};
    }
    const auto column = ewa_reference_wrap(x, width, image.wrap);
    const auto row = ewa_reference_wrap(y, height, image.wrap);
    const auto packed = image.texels[mip.offset + row * width + column];
    ReferenceRgb rgb{};
    for (unsigned c = 0; c < rgb.size(); ++c) {
        rgb[c] = ewa_reference_decode((packed >> (8 * c)) & 255u, image.encoding);
    }
    return rgb;
}

inline double ewa_reference_center(double uv, double size, ImageWrap wrap) {
    if (std::abs(uv) > 64.0 && wrap != ImageWrap::black) {
        // Remove the integer part while retaining the sign of the fractional phase.
        double whole = 0;
        const double phase = std::modf(uv, &whole);
        if (wrap == ImageWrap::repeat) {
            return phase * size - 0.5;
        }
        return (uv < 0 ? -64.0 : size + 64.0) + phase * size - 0.5;
    }
    return uv * size - 0.5;
}

inline void ewa_reference_add(ReferenceRgb &sum, const ReferenceRgb &sample, double weight) {
    for (std::size_t c = 0; c < sum.size(); ++c) {
        sum[c] += weight * sample[c];
    }
}

// Exhaust a fixed square larger than the supported ellipse, then test the
// quadratic form of the inverse covariance in double precision.
inline ReferenceRgb ewa_reference_level(const ImagePyramid &image, std::size_t index,
                                        ReferenceUv st, ReferenceUv major, ReferenceUv minor,
                                        bool bilinear) {
    if (index >= image.levels.size()) {
        return ewa_reference_texel(image, image.levels.back(), 0, 0);
    }
    const auto &mip = image.levels[index];
    const double width = mip.width;
    const double height = mip.height;
    const double s = ewa_reference_center(st[0], width, image.wrap);
    const double t = ewa_reference_center(st[1], height, image.wrap);
    constexpr int margin = 40;
    if (image.wrap == ImageWrap::black &&
        (s < -margin || s > width - 1 + margin || t < -margin || t > height - 1 + margin)) {
        return {};
    }
    const auto x0 = static_cast<int>(std::floor(s));
    const auto y0 = static_cast<int>(std::floor(t));
    ReferenceRgb sum{};
    if (bilinear) {
        const double sx = s - x0;
        const double sy = t - y0;
        ewa_reference_add(sum, ewa_reference_texel(image, mip, x0, y0), (1 - sx) * (1 - sy));
        ewa_reference_add(sum, ewa_reference_texel(image, mip, x0 + 1, y0), sx * (1 - sy));
        ewa_reference_add(sum, ewa_reference_texel(image, mip, x0, y0 + 1), (1 - sx) * sy);
        ewa_reference_add(sum, ewa_reference_texel(image, mip, x0 + 1, y0 + 1), sx * sy);
        return sum;
    }
    const double a = 1 + width * width * (major[0] * major[0] + minor[0] * minor[0]);
    const double b = width * height * (major[0] * major[1] + minor[0] * minor[1]);
    const double c = 1 + height * height * (major[1] * major[1] + minor[1] * minor[1]);
    const double determinant = a * c - b * b;
    double total_weight = 0;
    for (int y = y0 - margin; y <= y0 + margin; ++y) {
        for (int x = x0 - margin; x <= x0 + margin; ++x) {
            const double dx = x - s;
            const double dy = y - t;
            const double r2 = (c * dx * dx - 2 * b * dx * dy + a * dy * dy) / determinant;
            if (r2 < 1) {
                const double weight_index = std::min(std::floor(r2 * 128), 127.0);
                const double weight = std::exp(-2 * weight_index / 127) - std::exp(-2.0);
                total_weight += weight;
                ewa_reference_add(sum, ewa_reference_texel(image, mip, x, y), weight);
            }
        }
    }
    for (auto &channel : sum) {
        channel /= total_weight;
    }
    return sum;
}

// Inputs are already UV-mapped and V-flipped. The pyramid contains the bytes
// used by the sampler. Transfer decoding and EWA weights are generated here.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline ReferenceRgb ewa_reference(const ImagePyramid &image, double s, double t, double dsdx,
                                  double dtdx, double dsdy, double dtdy,
                                  double max_anisotropy = 8) {
    ReferenceUv major{dsdx, dtdx};
    ReferenceUv minor{dsdy, dtdy};
    double longer = std::hypot(major[0], major[1]);
    double shorter = std::hypot(minor[0], minor[1]);
    if (longer < shorter) {
        std::swap(major, minor);
        std::swap(longer, shorter);
    }
    const ReferenceUv st{s, t};
    if (shorter == 0) {
        return ewa_reference_level(image, 0, st, {}, {}, true);
    }
    const double adjusted = std::max(shorter, longer / max_anisotropy);
    if (adjusted >= 2) {
        return ewa_reference_level(image, image.levels.size(), st, {}, {}, false);
    }
    for (auto &component : minor) {
        component = component / shorter * adjusted;
    }
    const double lod =
        std::max(0.0, static_cast<double>(image.levels.size() - 1) + std::log2(adjusted));
    const auto lower = static_cast<std::size_t>(std::floor(lod));
    const double fraction = lod - std::floor(lod);
    ReferenceRgb output{};
    ewa_reference_add(output, ewa_reference_level(image, lower, st, major, minor, false),
                      1 - fraction);
    ewa_reference_add(output, ewa_reference_level(image, lower + 1, st, major, minor, false),
                      fraction);
    return output;
}

} // namespace rypath::test
