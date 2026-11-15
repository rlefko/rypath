// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace graph_probe {

constexpr int query_count = 96;
constexpr std::array uv{1.7f, -.8f, .1f, -.4f};
constexpr std::array vs{.3f, -.6f, .9f};
constexpr std::array vt{-.4f, -.1f, .7f};
constexpr float planar_ds = .23f, planar_dt = -.4f;
constexpr std::array translate{.1f, .2f, -.3f};
constexpr std::array stretch{2.0f, .5f, -1.0f};
constexpr std::array direction{.6f, .8f, 0.0f};
constexpr std::array color{.2f, .5f, .8f};
constexpr float temperature = 6500.0f;
constexpr std::string_view named_spectrum = "metal-Cu-eta";
constexpr float a = .25f, b = .82f, c = -.35f, d = 1.275f;
constexpr int octaves = 8;
constexpr float roughness = .55f, marble_scale = 1.42f, marble_variation = .28f;
constexpr std::array filter_names{"point", "bilinear", "trilinear", "ewa"};

struct Input {
    std::array<float, 3> p, dx, dy, n;
    float u, v, dudx, dvdx, dudy, dvdy, wavelength;
};

inline Input query(int index) {
    const auto signed_fraction = [index](int step, int shift) {
        return static_cast<float>((index * step + shift) % 101 - 50) / 50.0f;
    };
    constexpr std::array footprints{0.0f, .03f, .18f, 1.5f};
    constexpr std::array<std::array<float, 3>, 4> normals{
        {{0, 0, 1}, {-1, 0, 0}, direction, {0, -1, 0}}};
    const auto footprint = footprints[static_cast<std::size_t>(index) % footprints.size()];
    // Offset the axes so none of the mapped positions exactly hits a pole.
    return {
        {signed_fraction(17, 23) + .037f, signed_fraction(37, 11) - .019f, signed_fraction(31, 19)},
        {.061f * footprint, -.12f * footprint, .09f * footprint},
        {-.22f * footprint, .031f * footprint, .08f * footprint},
        normals[static_cast<std::size_t>(index / 4) % normals.size()],
        1.3f * signed_fraction(61, 23),
        1.3f * signed_fraction(67, 31),
        .31f * footprint,
        .17f * footprint,
        -.07f * footprint,
        .101f * footprint,
        365.0f + 457.0f * static_cast<float>((index * 53) % 97) / 96.0f};
}

template <typename... Floats> void row(int index, std::string_view key, Floats... values) {
    std::cout << index << ' ' << key;
    ((std::cout << ' ' << values), ...);
    std::cout << '\n';
}

} // namespace graph_probe
