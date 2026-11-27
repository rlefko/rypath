// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"
namespace rypath_enhanced_duplication {
using Word = rypath_replay::Word;
enum : Word {
    version = 1,
    absent = 0xffffffffu,
    missing = 0xffffu,
    radius = 8,
    denominator = 288,
    histogram_bins = 291,
    null_bin = 289,
    unknown_bin = 290
};
enum Mode : Word { off = 0, initial_seed = 1, initial_atom = 2, current = 3 };
struct Label {
    Word fresh_pixel, sample_low, sample_high, initial_j;
};
struct Key {
    Word words[8];
};
inline Label invalid_label() { return {absent, 0, 0, 0}; }
inline bool valid_label(Label x, Word pixels) {
    return x.fresh_pixel < pixels && x.initial_j < 0xfffffffeu;
}
inline bool canonical_invalid(Label x) {
    return x.fresh_pixel == absent && !x.sample_low && !x.sample_high && !x.initial_j;
}
inline bool equal_label(Label a, Label b, Word mode) {
    return (mode == initial_seed || mode == initial_atom) && a.fresh_pixel == b.fresh_pixel &&
           a.sample_low == b.sample_low && a.sample_high == b.sample_high &&
           (mode == initial_seed || a.initial_j == b.initial_j);
}
#if defined(__METAL_VERSION__)
#define RYPATH_DUP_THREAD thread
#else
#define RYPATH_DUP_THREAD
#endif
inline Key current_key(RYPATH_DUP_THREAD const rypath_replay::ReplayState &u, Word j) {
    return {{u.tail_key[0], u.tail_key[1], u.film[0], u.film[1], u.lens[0], u.lens[1], u.wavelength,
             j}};
}
inline Key label_key(Label x) {
    return {{x.fresh_pixel, x.sample_low, x.sample_high, x.initial_j, 0, 0, 0, 0}};
}
inline bool equal_key(Key a, Key b, Word mode) {
    const Word count = mode == current        ? 8u
                       : mode == initial_atom ? 4u
                       : mode == initial_seed ? 3u
                                              : 0u;
    if (!count)
        return false;
    for (Word i = 0; i < count; ++i)
        if (a.words[i] != b.words[i])
            return false;
    return true;
}
#undef RYPATH_DUP_THREAD
static_assert(sizeof(Label) == 16 && sizeof(Key) == 32, "Exact diagnostic scalar keys");
} // namespace rypath_enhanced_duplication
