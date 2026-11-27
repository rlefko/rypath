// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace duplication_oracle {
constexpr std::uint16_t missing = 0xffffu;
struct Pixel {
    std::array<std::uint32_t, 4> birth{};
    std::array<std::uint32_t, 8> current{};
    bool valid = true;
    bool null = false;
};
struct Map {
    std::vector<std::uint16_t> counts;
    std::array<std::uint32_t, 291> histogram{};
    std::uint64_t sum = 0;
};
inline void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
inline bool same(const Pixel &a, const Pixel &b, unsigned mode) {
    if (mode == 3)
        return a.current == b.current;
    const unsigned words = mode == 1 ? 3 : 4;
    for (unsigned k = 0; k < words; ++k)
        if (a.birth[k] != b.birth[k])
            return false;
    return true;
}
// Independent all-pairs finite-coordinate definition. No halo load, tiling,
// clamping, production label helper, hash or radius-offset loop is used.
inline Map count(const std::vector<Pixel> &pixels, unsigned width, unsigned height, unsigned mode) {
    require(width && height && std::uint64_t(width) * height == pixels.size(),
            "Independent map dimensions");
    require(mode >= 1 && mode <= 3, "Independent comparison mode");
    Map result;
    result.counts.resize(pixels.size(), missing);
    for (unsigned p = 0; p < pixels.size(); ++p) {
        if (!pixels[p].valid) {
            ++result.histogram[pixels[p].null ? 289 : 290];
            continue;
        }
        unsigned count = 0;
        for (unsigned q = 0; q < pixels.size(); ++q) {
            if (p == q || !pixels[q].valid)
                continue;
            const long long dx =
                static_cast<long long>(q % width) - static_cast<long long>(p % width);
            const long long dy =
                static_cast<long long>(q / width) - static_cast<long long>(p / width);
            if (dx >= -8 && dx <= 8 && dy >= -8 && dy <= 8 && same(pixels[p], pixels[q], mode))
                ++count;
        }
        require(count <= 288, "Independent map center exclusion bound");
        result.counts[p] = std::uint16_t(count);
        ++result.histogram[count];
        result.sum += count;
    }
    return result;
}
inline std::vector<Pixel> fixture(unsigned width, unsigned height, unsigned pattern) {
    std::vector<Pixel> result(width * height);
    for (unsigned p = 0; p < result.size(); ++p) {
        const unsigned x = p % width, y = p / width;
        auto &v = result[p];
        const unsigned family = pattern == 0   ? 0
                                : pattern == 1 ? p
                                : pattern == 2 ? x
                                               : (x + y) % 2;
        v.birth = {family, 0, 0, 0};
        v.current = {family, 0, 0, 0, 0, 0, 0, 0};
    }
    return result;
}
} // namespace duplication_oracle
