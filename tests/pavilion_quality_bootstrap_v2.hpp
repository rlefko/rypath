// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

namespace pavilion_quality_v2 {
struct BootstrapDraw {
    unsigned replicates;
    std::uint64_t seed;
    unsigned draw;
};
// Same per-draw seed-seq/rejection algorithm as v1; only two real arms.
inline std::array<std::vector<unsigned>, 2> bootstrap_counts(const BootstrapDraw &configuration) {
    const unsigned n = configuration.replicates;
    if (n != 64)
        throw std::runtime_error("v2 requires exactly 64 replicates per arm");
    std::array<std::vector<unsigned>, 2> counts{std::vector<unsigned>(n), std::vector<unsigned>(n)};
    for (unsigned backend = 0; backend < 2; ++backend) {
        std::seed_seq sequence{unsigned(configuration.seed), unsigned(configuration.seed >> 32),
                               configuration.draw, backend};
        std::mt19937 rng(sequence);
        const std::uint32_t threshold = std::uint32_t(0u - n) % n;
        for (unsigned i = 0; i < n; ++i) {
            std::uint32_t word;
            do {
                word = rng();
            } while (word < threshold);
            ++counts[backend][word % n];
        }
    }
    return counts;
}
} // namespace pavilion_quality_v2
