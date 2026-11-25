// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"

namespace rypath_area_stencil {
using Word = rypath_replay::Word;
#if defined(__METAL_VERSION__)
using SignedWide = long;
#else
using SignedWide = long long;
#endif
struct Offset {
    int x, y;
};
struct Offsets {
    Offset first, second;
};
// Exactly 128 distinct integer points on the nonzero Chebyshev-radius-16
// perimeter. Corners belong to just one of the four half-open sides.
inline Offset perimeter(Word index) {
    const int t = int(index & 31u);
    switch ((index >> 5) & 3u) {
    case 0:
        return {-16 + t, -16};
    case 1:
        return {16, -16 + t};
    case 2:
        return {16 - t, 16};
    default:
        return {-16, 16 - t};
    }
}
// A dedicated Philox counter domain; no pixel or replay sample index enters
// this shared frame stencil. Two independent 7-bit lanes retain duplicates.
inline Offsets draw(Word serial_low, Word serial_high, Word seed_low, Word seed_high) {
    const auto words =
        rypath_replay::philox({serial_low, serial_high, 0, 0x73741600u}, {seed_low, seed_high});
    return {perimeter(words.x & 127u), perimeter(words.y & 127u)};
}
inline Word inverse_slot(Word slot) { return slot == 0 ? 0 : ((slot - 1) ^ 1u) + 1; }
inline Offset offset(Offsets offsets, Word slot) {
    if (slot == 0 || slot > 4)
        return {0, 0};
    const auto d = slot <= 2 ? offsets.first : offsets.second;
    return (slot & 1u) ? d : Offset{-d.x, -d.y};
}
inline bool valid(Offset d) {
    return d.x >= -16 && d.x <= 16 && d.y >= -16 && d.y <= 16 &&
           (d.x == -16 || d.x == 16 || d.y == -16 || d.y == 16);
}
// Signed 64-bit coordinates prevent unsigned underflow and preserve reciprocity
// on every in-bounds edge. Out-of-bounds slots map to self, never clamp or wrap.
inline Word neighbor(Word pixel, Word width, Word height, Offsets offsets, Word slot) {
    if (width == 0 || height == 0)
        return pixel;
    const auto d = offset(offsets, slot);
    const SignedWide x = SignedWide(pixel % width) + d.x;
    const SignedWide y = SignedWide(pixel / width) + d.y;
    if (x < 0 || y < 0 || x >= SignedWide(width) || y >= SignedWide(height))
        return pixel;
    return Word(y * SignedWide(width) + x);
}
} // namespace rypath_area_stencil
