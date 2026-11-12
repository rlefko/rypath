// SPDX-License-Identifier: Apache-2.0
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// Modified for rypath. See third_party/pbrt/README.md.

#include "rgb_spectrum.hpp"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace rypath {
namespace detail {
const char *srgb_table_bytes(std::size_t offset);
} // namespace detail
namespace {

constexpr int resolution = 64;
static_assert(CHAR_BIT == 8 && sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

float table_float(std::size_t index) {
    const char *bytes = detail::srgb_table_bytes(index * 4);
    std::uint32_t bits = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        bits |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[byte])) << (8 * byte);
    }
    return std::bit_cast<float>(bits);
}

const std::array<float, resolution> &z_nodes() {
    static const auto nodes = [] {
        std::array<float, resolution> result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = table_float(i);
        }
        return result;
    }();
    return nodes;
}

float lerp(float t, float a, float b) { return (1.0f - t) * a + t * b; }

} // namespace

std::optional<RgbSpectrum> reconstruct_srgb(const std::array<float, 3> &rgb) {
    if (!std::all_of(rgb.begin(), rgb.end(), [](float value) {
            return std::isfinite(value) && value >= 0 && value <= 1;
        })) {
        return std::nullopt;
    }
    if (rgb[0] == rgb[1] && rgb[1] == rgb[2]) {
        return RgbSpectrum{.is_constant = true, .constant = rgb[0]};
    }

    // Match PBRT's preference for the last channel in a tie.
    int max_channel = 0;
    for (int channel = 1; channel < 3; ++channel) {
        if (rgb[channel] >= rgb[max_channel]) {
            max_channel = channel;
        }
    }
    const float z = rgb[max_channel];
    const float x = rgb[(max_channel + 1) % 3] * (resolution - 1) / z;
    const float y = rgb[(max_channel + 2) % 3] * (resolution - 1) / z;
    const int xi = std::min(static_cast<int>(x), resolution - 2);
    const int yi = std::min(static_cast<int>(y), resolution - 2);
    const auto &nodes = z_nodes();
    // The left node is strictly below z. Restrict the search to interior nodes
    // so that the interpolation interval always has two valid endpoints.
    const auto upper = std::lower_bound(nodes.begin() + 1, nodes.end() - 1, z);
    const int zi = static_cast<int>(upper - nodes.begin() - 1);
    const float dx = x - static_cast<float>(xi);
    const float dy = y - static_cast<float>(yi);
    const float dz = (z - nodes[zi]) / (nodes[zi + 1] - nodes[zi]);

    RgbSpectrum result;
    for (int i = 0; i < 3; ++i) {
        const auto coefficient = [&](int offset_x, int offset_y, int offset_z) {
            const int plane = max_channel * resolution + zi + offset_z;
            const int row = plane * resolution + yi + offset_y;
            const int cell = row * resolution + xi + offset_x;
            return table_float(resolution + 3 * cell + i);
        };
        result.coefficients[i] = lerp(dz,
                                      lerp(dy, lerp(dx, coefficient(0, 0, 0), coefficient(1, 0, 0)),
                                           lerp(dx, coefficient(0, 1, 0), coefficient(1, 1, 0))),
                                      lerp(dy, lerp(dx, coefficient(0, 0, 1), coefficient(1, 0, 1)),
                                           lerp(dx, coefficient(0, 1, 1), coefficient(1, 1, 1))));
    }
    return result;
}

float evaluate_srgb(const RgbSpectrum &spectrum, float wavelength) {
    if (spectrum.is_constant) {
        return spectrum.constant;
    }
    const auto &[a, b, c] = spectrum.coefficients;
    const float polynomial = std::fma(wavelength, std::fma(wavelength, a, b), c);
    if (!std::isfinite(polynomial)) {
        return std::numeric_limits<float>::quiet_NaN();
    }

    // Rescale the ratio before squaring large inputs, keeping the sigmoid bounded.
    if (std::abs(polynomial) <= 1.0f) {
        return 0.5f + 0.5f * (polynomial / std::sqrt(1.0f + polynomial * polynomial));
    }
    const float reciprocal = 1.0f / polynomial;
    return 0.5f +
           0.5f * std::copysign(1.0f / std::sqrt(1.0f + reciprocal * reciprocal), polynomial);
}

const std::array<float, 471> &pbrt_d65() {
    static constexpr std::array<float, 471> values{
#include "../third_party/pbrt/d65.inc"
    };
    return values;
}

} // namespace rypath
