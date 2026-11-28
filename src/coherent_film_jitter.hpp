// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "coherent_film_shared.h"
#include <array>
#include <cstdint>
#include <stdexcept>
namespace rypath {
// A separate counter block preserves per-pixel wavelength/path/lens streams.
// Correlation across pixels changes variance, not each uniform box marginal,
// under the same pseudorandom-stream assumption as independent sampling.
inline std::array<std::uint32_t, 2> coherent_film_words(std::uint64_t seed, std::uint64_t sample) {
    const auto w = rypath_film_jitter::words(std::uint32_t(seed), std::uint32_t(seed >> 32),
                                             std::uint32_t(sample), std::uint32_t(sample >> 32));
    return {w.x, w.y};
}
inline std::array<float, 2> coherent_film_offset(const std::array<std::uint32_t, 2> &words) {
    return {rypath_replay::uniform(words[0]) - .5f, rypath_replay::uniform(words[1]) - .5f};
}
// Column-major homogeneous projection for the actual shifted ray grid. A ray
// sample at center+jitter corresponds to an image-feature shift of -jitter.
inline std::array<float, 16> coherent_film_projection(std::array<float, 16> projection,
                                                      std::uint32_t width, std::uint32_t height,
                                                      std::array<float, 2> jitter) {
    if (!width || !height)
        throw std::invalid_argument("Jitter projection needs positive dimensions");
    if (jitter[0] == 0 && jitter[1] == 0)
        return projection;
    const float x = -2 * jitter[0] / float(width), y = 2 * jitter[1] / float(height);
    for (unsigned column = 0; column < 4; ++column) {
        projection[column * 4] += x * projection[column * 4 + 3];
        projection[column * 4 + 1] += y * projection[column * 4 + 3];
    }
    return projection;
}
} // namespace rypath
