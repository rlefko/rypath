// SPDX-License-Identifier: Apache-2.0
// Typed MurmurHash64A and PCG32 adapted from PBRT v4 b0bdf461.
// Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt.
#pragma once
#if !defined(__METAL_VERSION__)
#include <bit>
#include <cstdint>
#endif
namespace rypath_layered_rng {
#if defined(__METAL_VERSION__)
using Word = uint;
using Wide = ulong;
#else
using Word = std::uint32_t;
using Wide = std::uint64_t;
#endif
inline Word bits(float v) {
#if defined(__METAL_VERSION__)
    return as_type<Word>(v);
#else
    return std::bit_cast<Word>(v);
#endif
}
inline Wide block(Wide k) {
    k *= 0xc6a4a7935bd1e995ull;
    k ^= k >> 47;
    return k * 0xc6a4a7935bd1e995ull;
}
inline Wide finish(Wide h) {
    h ^= h >> 47;
    h *= 0xc6a4a7935bd1e995ull;
    return h ^ (h >> 47);
}
// PBRT hashes packed argument bytes, including a signed 32-bit seed and
// 12-byte Vector3f. Metal float3 has 16-byte storage and must not be memcpy'd.
inline Wide hash3(Word a, Word b, Word c) {
    constexpr Wide m = 0xc6a4a7935bd1e995ull;
    Wide h = (12 * m ^ block(Wide(a) | (Wide(b) << 32))) * m;
    return finish((h ^ c) * m);
}
inline Wide hash4(Word a, Word b, Word c, Word d) {
    constexpr Wide m = 0xc6a4a7935bd1e995ull;
    Wide h = (16 * m ^ block(Wide(a) | (Wide(b) << 32))) * m;
    return finish((h ^ block(Wide(c) | (Wide(d) << 32))) * m);
}
inline Wide direction(float x, float y, float z) { return hash3(bits(x), bits(y), bits(z)); }
inline Wide seeded_direction(int seed, float x, float y, float z) {
    return hash4(Word(seed), bits(x), bits(y), bits(z));
}
inline Wide sample(float uc, float x, float y) { return hash3(bits(uc), bits(x), bits(y)); }
struct PCG {
    Wide state, inc;
    PCG(Wide sequence, Wide offset) : state(0), inc((sequence << 1) | 1) {
        next_word();
        state += offset;
        next_word();
    }
    Word next_word() {
        Wide old = state;
        state = old * 0x5851f42d4c957f2dull + inc;
        Word x = Word(((old >> 18) ^ old) >> 27), r = Word(old >> 59);
        return (x >> r) | (x << ((~r + 1u) & 31u));
    }
    void advance(Wide delta) {
        Wide mult = 0x5851f42d4c957f2dull, plus = inc, acc_mult = 1, acc_plus = 0;
        while (delta > 0) {
            if (delta & 1) {
                acc_mult *= mult;
                acc_plus = acc_plus * mult + plus;
            }
            plus = (mult + 1) * plus;
            mult *= mult;
            delta >>= 1;
        }
        state = acc_mult * state + acc_plus;
    }
    float next() {
        float v = float(next_word()) * 0x1p-32f;
        return v < 0x1.fffffep-1f ? v : 0x1.fffffep-1f;
    }
};
} // namespace rypath_layered_rng
