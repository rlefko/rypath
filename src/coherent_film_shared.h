// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"
namespace rypath_film_jitter {
using Word = rypath_replay::Word;
// Reuses the former emitter_slots word, which old hosts initialize only to
// zero or bit_ceil(emitter_count), never a non-power-of-two tag. No shader uses
// that historical word for light indexing. Exact-tag matching is mandatory:
// testing nonzero would reinterpret legacy emitter counts as sampling modes.
enum : Word { coherent_tag = 0x67666a31u, counter_domain = 0x67666a31u };
inline bool coherent(Word mode) { return mode == coherent_tag; }
inline bool valid_mode(Word mode) {
    return mode == 0 || coherent(mode) || (mode & (mode - 1)) == 0;
}
inline rypath_replay::Words2 words(Word seed_low, Word seed_high, Word sample_low,
                                   Word sample_high) {
    const auto value =
        rypath_replay::philox({0, sample_low, sample_high, counter_domain}, {seed_low, seed_high});
    return {value.x, value.y};
}
static_assert((coherent_tag & (coherent_tag - 1)) != 0,
              "Coherent tag is outside every legacy power-of-two count");
} // namespace rypath_film_jitter
