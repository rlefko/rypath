// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Test-only independent integer construction. No production atlas, replay RNG,
// endpoint, transform or lookup helpers participate in these expected results.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
namespace atlas_oracle {
using Word = std::uint32_t;
using Entry = std::uint16_t;
inline void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
inline std::size_t pixel_count(unsigned width, unsigned height) {
    const auto count = std::uint64_t(width) * height;
    require(count <= std::numeric_limits<Word>::max(), "Independent 32-bit pixel ID extent");
    return std::size_t(count);
}
inline std::array<Word, 4> philox(std::array<Word, 4> v, std::array<Word, 2> key) {
    for (unsigned round = 0; round < 10; ++round) {
        const std::uint64_t a = std::uint64_t(v[0]) * 0xd2511f53u;
        const std::uint64_t b = std::uint64_t(v[2]) * 0xcd9e8d57u;
        v = {Word(b >> 32) ^ v[1] ^ key[0], Word(b), Word(a >> 32) ^ v[3] ^ key[1], Word(a)};
        key[0] += 0x9e3779b9u;
        key[1] += 0xbb67ae85u;
    }
    return v;
}
inline std::array<std::array<unsigned, 4>, 24> permutations() {
    std::array<std::array<unsigned, 4>, 24> result{};
    std::array<unsigned, 4> p{0, 1, 2, 3};
    unsigned n = 0;
    do {
        result[n++] = p;
    } while (std::next_permutation(p.begin(), p.end()));
    require(n == 24, "Independent permutation count");
    return result;
}
inline Word rank(unsigned atlas, unsigned pass, unsigned block) {
    for (Word retry = 0;; ++retry) {
        const auto value = philox({block, pass, atlas, retry},
                                  {0x50414952u ^ 0x32425348u, 0x41544c31u ^ 0x5546464cu})[0];
        if (value >= 16)
            return value % 24;
        require(retry != 0xffffffffu, "Independent rank retry exhaustion");
    }
}
inline Entry pack(int dx, int dy) {
    return Entry((unsigned(dx) & 255) | ((unsigned(dy) & 255) << 8));
}
inline std::array<int, 2> decode(Entry entry) {
    const int x = entry & 255, y = entry >> 8;
    return {x < 128 ? x : x - 256, y < 128 ? y : y - 256};
}
inline int modulo(long long x, unsigned width) {
    const auto r = x % width;
    return int(r < 0 ? r + width : r);
}
inline std::vector<Entry> endpoints(const std::vector<Word> &labels, unsigned width) {
    require(width >= 2 && width % 2 == 0 && labels.size() == pixel_count(width, width),
            "Independent endpoint extent");
    std::vector<std::array<unsigned, 2>> positions(labels.size() / 2);
    std::vector<unsigned> counts(positions.size());
    for (unsigned p = 0; p < labels.size(); ++p) {
        require(labels[p] < positions.size() && counts[labels[p]] < 2,
                "Independent endpoint third/out-of-range occurrence");
        positions[labels[p]][counts[labels[p]]++] = p;
    }
    std::vector<Entry> result(labels.size());
    for (unsigned label = 0; label < positions.size(); ++label) {
        require(counts[label] == 2, "Independent endpoint missing occurrence");
        const auto p = positions[label][0], q = positions[label][1];
        int dx = int(q % width) - int(p % width), dy = int(q / width) - int(p / width);
        if (dx > int(width / 2))
            dx -= int(width);
        if (dx < -int(width / 2))
            dx += int(width);
        if (dy > int(width / 2))
            dy -= int(width);
        if (dy < -int(width / 2))
            dy += int(width);
        result[p] = pack(dx, dy);
        result[q] = pack(-dx, -dy);
    }
    return result;
}
inline std::vector<Word> labels(unsigned width, unsigned atlas, unsigned passes = 128) {
    require(width >= 2 && width % 2 == 0, "Independent shuffle extent");
    std::vector<Word> labels(pixel_count(width, width));
    for (unsigned p = 0; p < labels.size(); ++p)
        labels[p] = p / 2;
    const auto table = permutations();
    for (unsigned pass = 0; pass < passes; ++pass) {
        std::vector<unsigned> coverage(width <= 8 ? labels.size() : 0);
        const unsigned parity = pass % 2;
        for (unsigned by = 0; by < width / 2; ++by)
            for (unsigned bx = 0; bx < width / 2; ++bx) {
                std::array<std::size_t, 4> pixel{};
                std::array<unsigned, 4> old{};
                for (unsigned j = 0; j < 4; ++j) {
                    pixel[j] = std::size_t((2 * by + parity + j / 2) % width) * width +
                               (2 * bx + parity + j % 2) % width;
                    old[j] = labels[pixel[j]];
                    if (!coverage.empty())
                        ++coverage[pixel[j]];
                }
                const auto permutation = table[rank(atlas, pass, by * (width / 2) + bx)];
                for (unsigned j = 0; j < 4; ++j)
                    labels[pixel[j]] = old[permutation[j]];
            }
        if (!coverage.empty()) {
            for (auto count : coverage)
                require(count == 1, "Independent parity blocks partition torus including seam");
            std::vector<unsigned> counts(labels.size() / 2);
            for (auto label : labels) {
                require(label < counts.size(), "Independent label bounds");
                ++counts[label];
            }
            for (auto count : counts)
                require(count == 2, "Every shuffle retains exactly two endpoints per label");
        }
    }
    return labels;
}
inline std::vector<Entry> generate(unsigned width, unsigned atlas, unsigned passes = 128) {
    return endpoints(labels(width, atlas, passes), width);
}
// D4 as an explicit signed matrix; bit0 reflects X, bit1 reflects Y, bit2
// transposes. Apply signs before transpose and invert with the matrix transpose.
inline std::array<long long, 2> transform(long long x, long long y, unsigned orientation) {
    x *= orientation & 1 ? -1 : 1;
    y *= orientation & 2 ? -1 : 1;
    return orientation & 4 ? std::array<long long, 2>{y, x} : std::array<long long, 2>{x, y};
}
inline std::array<int, 2> inverse(int x, int y, unsigned orientation) {
    if (orientation & 4)
        std::swap(x, y);
    return {x * (orientation & 1 ? -1 : 1), y * (orientation & 2 ? -1 : 1)};
}
// Build finite-image pairs by explicitly translating tile endpoint pairs and
// transforming BOTH endpoints. This oracle never performs a per-pixel lookup.
inline std::vector<unsigned> lifted_pairs(const std::vector<Entry> &entries, unsigned L,
                                          unsigned width, unsigned height, unsigned orientation,
                                          unsigned ox, unsigned oy) {
    std::vector<unsigned> result(pixel_count(width, height));
    for (unsigned p = 0; p < result.size(); ++p)
        result[p] = p;
    const int reach = int(std::max(width, height) / L) + 3;
    for (unsigned t = 0; t < entries.size(); ++t) {
        const auto d = decode(entries[t]);
        const unsigned partner =
            unsigned(modulo(int(t / L) + d[1], L)) * L + modulo(int(t % L) + d[0], L);
        if (t >= partner)
            continue;
        for (int ty = -reach; ty <= reach; ++ty)
            for (int tx = -reach; tx <= reach; ++tx) {
                const int x = int(t % L) - int(ox) + int(L) * tx,
                          y = int(t / L) - int(oy) + int(L) * ty;
                const auto p = inverse(x, y, orientation),
                           q = inverse(x + d[0], y + d[1], orientation);
                const auto inside = [&](auto v) {
                    return v[0] >= 0 && v[1] >= 0 && v[0] < int(width) && v[1] < int(height);
                };
                if (inside(p) && inside(q)) {
                    const std::size_t pi = std::size_t(p[1]) * width + unsigned(p[0]),
                                      qi = std::size_t(q[1]) * width + unsigned(q[0]);
                    require(result[pi] == pi && result[qi] == qi,
                            "Independent transformed endpoint pairs are disjoint");
                    result[pi] = unsigned(qi);
                    result[qi] = unsigned(pi);
                }
            }
    }
    return result;
}
inline unsigned neighbor(const std::vector<Entry> &entries, unsigned tile_width, unsigned width,
                         unsigned height, unsigned pixel, unsigned orientation, unsigned ox,
                         unsigned oy) {
    const auto t = transform(pixel % width, pixel / width, orientation);
    const auto index =
        std::size_t(modulo(t[1] + oy, tile_width)) * tile_width + modulo(t[0] + ox, tile_width);
    const auto d = decode(entries.at(index));
    const auto lifted = inverse(d[0], d[1], orientation);
    const long long x = static_cast<long long>(pixel % width) + lifted[0],
                    y = static_cast<long long>(pixel / width) + lifted[1];
    return x < 0 || y < 0 || x >= width || y >= height ? pixel : unsigned(y * width + x);
}
} // namespace atlas_oracle
namespace atlas_oracle {
inline std::vector<Entry> handwritten(unsigned width, unsigned candidate) {
    std::vector<Entry> entries(pixel_count(width, width));
    for (unsigned y = 0; y < width; ++y)
        for (unsigned x = 0; x < width; ++x) {
            if (candidate == 0 || (candidate == 1 && y % 2 == 0))
                entries[std::size_t(y) * width + x] = pack(x % 2 ? -1 : 1, 0);
            else if (candidate == 1)
                entries[std::size_t(y) * width + x] = pack(x % 2 ? 1 : -1, 0);
            else if (candidate == 2)
                entries[std::size_t(y) * width + x] = pack(0, y % 2 ? -1 : 1);
            else
                entries[std::size_t(y) * width + x] = pack(x % 2 ? -1 : 1, y % 2 ? -1 : 1);
        }
    return entries;
}
} // namespace atlas_oracle
