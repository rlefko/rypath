// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"
// Our stochastic evaluation of the four existing pairwise-MIS terms. Atlas
// selection is independent of transport and categorical reservoir entropy.
// Conditional on frozen temporal reservoirs and atlas poses, each of the two
// term sums is estimated with one uniform index and exact inverse probability4.
namespace rypath_stochastic_spatial {
using Word = rypath_replay::Word;
enum : Word { law_version = 1, atlas_count = 4 };
struct Selection {
    Word forward = 0, canonical = 0; // 1-based; zero is the disabled sentinel.
};
inline bool valid(Selection s) {
    return s.forward >= 1 && s.forward <= atlas_count && s.canonical >= 1 &&
           s.canonical <= atlas_count;
}
inline Selection draw(Word serial_low, Word serial_high, Word seed_low, Word seed_high) {
    const rypath_replay::Words2 key{seed_low ^ 0x73707031u, seed_high ^ 0x61746c31u};
    // Distinct counters, exact uniform top-two-bit reduction, no rejection or
    // sample-dependent admission. One shared index per frame preserves each
    // atlas's involution at the reverse lookup pixel.
    return {1u + (rypath_replay::philox({serial_low, serial_high, 0u, 0u}, key).x >> 30),
            1u + (rypath_replay::philox({serial_low, serial_high, 0u, 1u}, key).x >> 30)};
}
inline Word evaluations(Selection s) { return valid(s) ? 1u + Word(s.forward != s.canonical) : 0u; }
} // namespace rypath_stochastic_spatial
