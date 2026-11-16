// SPDX-License-Identifier: Apache-2.0
// Adapted from pinned PBRT v4; see third_party/pbrt/sampling.md and LICENSE.txt.
#pragma once
#include "sobol_shared.h"
#if !defined(__METAL_VERSION__)
#include <bit>
#endif
namespace rypath {
namespace qmc {
using Word = sobol::Word;
using Index = sobol::Index;
#if defined(__METAL_VERSION__)
constant Word primes[1000] = {
#else
inline constexpr Word primes[1000] = {
#endif
#include "../third_party/pbrt/primes.inc"
};
inline Index hash2(Word a, Word b) {
    constexpr Index m = 0xc6a4a7935bd1e995ull;
    Index k = ((Index(b) << 32) | a) * m;
    k ^= k >> 47;
    k *= m;
    Index h = (8 * m ^ k) * m;
    h ^= h >> 47;
    h *= m;
    return h ^ (h >> 47);
}
inline Index hash3(Word a, Word b, Word c) {
    constexpr Index m = 0xc6a4a7935bd1e995ull;
    Index k = ((Index(b) << 32) | a) * m;
    k ^= k >> 47;
    k *= m;
    Index h = (12 * m ^ k) * m;
    h ^= c;
    h *= m;
    h ^= h >> 47;
    h *= m;
    return h ^ (h >> 47);
}
inline Word permutation(Word i, Word l, Word p) {
    Word w = l - 1;
    w |= w >> 1;
    w |= w >> 2;
    w |= w >> 4;
    w |= w >> 8;
    w |= w >> 16;
    do {
        i ^= p;
        i *= 0xe170893d;
        i ^= p >> 16;
        i ^= (i & w) >> 4;
        i ^= p >> 8;
        i *= 0x0929eb3f;
        i ^= p >> 23;
        i ^= (i & w) >> 1;
        i *= 1 | p >> 27;
        i *= 0x6935fa69;
        i ^= (i & w) >> 11;
        i *= 0x74dcb303;
        i ^= (i & w) >> 2;
        i *= 0x9e501cc3;
        i ^= (i & w) >> 2;
        i *= 0xc860a3df;
        i &= w;
        i ^= i >> 5;
    } while (i >= l);
    return (i + p) % l;
}

inline Word float_bits(float x) {
#if defined(__METAL_VERSION__)
    return as_type<Word>(x);
#else
    return std::bit_cast<Word>(x);
#endif
}
inline float bits_float(Word x) {
#if defined(__METAL_VERSION__)
    return as_type<float>(x);
#else
    return std::bit_cast<float>(x);
#endif
}
inline float inverse_base(Word base) {
#if defined(__METAL_VERSION__)
    return metal::precise::divide(1.f, float(base));
#else
    return 1.f / float(base);
#endif
}
inline float radical(Word dimension, Index a, Word mode = 0, Word seed = 0) {
    const Word base = primes[dimension];
    const Index limit = ~Index(0) / base - base;
    const float inv = inverse_base(base);
    float inv_m = 1;
    Index reversed = 0;
    Word digit_index = 0;
    const Word owen_hash = Word(sobol::mix_bits(1 + (dimension << 4)));
    while ((mode == 0 ? a != 0
                      : (mode == 1 ? 1.f - float(base - 1) * inv_m < 1.f : 1.f - inv_m < 1.f)) &&
           reversed < limit) {
        const Index next = a / base;
        Word digit = Word(a - next * base);
        if (mode == 1)
            digit = permutation(digit, base, Word(hash3(base, digit_index, seed)));
        else if (mode == 3)
            digit = permutation(digit, base, Word(sobol::mix_bits(owen_hash ^ reversed)));
        reversed = reversed * base + digit;
        inv_m *= inv;
        ++digit_index;
        a = next;
    }
    const float value = float(reversed) * inv_m;
    return value < 0x1.fffffep-1f ? value : 0x1.fffffep-1f;
}
inline Index inverse_radical(Word a, Word base, Word count) {
    Index result = 0;
    for (Word i = 0; i < count; ++i) {
        result = result * base + a % base;
        a /= base;
    }
    return result;
}
inline Word base_scale(Word resolution, Word base) {
    Word scale = 1;
    const Word bound = resolution < 128 ? resolution : 128;
    while (scale < bound)
        scale *= base;
    return scale;
}
inline Word base_exponent(Word scale, Word base) {
    Word e = 0;
    while (scale > 1) {
        scale /= base;
        ++e;
    }
    return e;
}
inline Word inverse_mod(Word a, Word modulus) {
    if (modulus == 1)
        return 0;
    // Scales are bounded by 243; a bounded search avoids signed arithmetic.
    for (Word i = 1; i < modulus; ++i)
        if ((a * i) % modulus == 1)
            return i;
    return 0;
}
inline Index halton_index(Word width, Word height, Word x, Word y, Index sample) {
    const Word sx = base_scale(width, 2), sy = base_scale(height, 3), stride = sx * sy;
    Index offset = 0;
    if (stride > 1) {
        offset = inverse_radical(x % 128, 2, base_exponent(sx, 2)) * sy * inverse_mod(sy, sx);
        offset += inverse_radical(y % 128, 3, base_exponent(sy, 3)) * sx * inverse_mod(sx, sy);
        offset %= stride;
    }
    return offset + sample * stride;
}
inline float halton_camera(Word axis, Word resolution, Index index) {
    const Word scale = base_scale(resolution, axis == 0 ? 2 : 3);
    return radical(axis, index / scale);
}
inline Word scramble(Word value, Word mode, Word hash) {
    if (mode == 0)
        return value;
    if (mode == 1)
        return value ^ hash;
    if (mode == 2) {
        value = sobol::reverse_bits(value);
        value ^= value * 0x3d20adeau;
        value += hash;
        value *= (hash >> 16) | 1;
        value ^= value * 0x05526c56u;
        value ^= value * 0x53a22864u;
        return sobol::reverse_bits(value);
    }
    value ^= (hash & 1) << 31;
    for (Word bit = 1; bit < 32; ++bit) {
        const Word prefix = value & (~Word(0) << (32 - bit));
        if (Word(sobol::mix_bits(prefix ^ hash)) & (Word(1) << bit))
            value ^= Word(1) << (31 - bit);
    }
    return value;
}
inline Index morton(Word x, Word y, Word log_spp, Index sample) {
    Index result = 0;
    for (Word b = 0; b < 26; ++b) {
        result |= Index((x >> b) & 1) << (2 * b);
        result |= Index((y >> b) & 1) << (2 * b + 1);
    }
    return (result << log_spp) | sample;
}
inline Index zindex(Index morton_index, Word dimension, Word exponent, Word log_spp) {
    const Word permutations[24][4] = {
        {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 2, 3, 1}, {0, 3, 2, 1}, {0, 3, 1, 2},
        {1, 0, 2, 3}, {1, 0, 3, 2}, {1, 2, 0, 3}, {1, 2, 3, 0}, {1, 3, 2, 0}, {1, 3, 0, 2},
        {2, 1, 0, 3}, {2, 1, 3, 0}, {2, 0, 1, 3}, {2, 0, 3, 1}, {2, 3, 0, 1}, {2, 3, 1, 0},
        {3, 1, 2, 0}, {3, 1, 0, 2}, {3, 2, 1, 0}, {3, 2, 0, 1}, {3, 0, 2, 1}, {3, 0, 1, 2}};
    const Word odd = log_spp & 1;
    const Word digits = exponent + (log_spp + 1) / 2;
    Index result = 0;
    for (int i = int(digits) - 1; i >= int(odd); --i) {
        const Word shift = 2 * Word(i) - odd;
        const Word digit = Word((morton_index >> shift) & 3);
        const Index higher = morton_index >> (shift + 2);
        const Word p = Word((sobol::mix_bits(higher ^ Word(0x55555555u * dimension)) >> 24) % 24);
        result |= Index(permutations[p][digit]) << shift;
    }
    if (odd)
        result |= (morton_index & 1) ^
                  (sobol::mix_bits((morton_index >> 1) ^ Word(0x55555555u * dimension)) & 1);
    return result;
}
#if defined(__METAL_VERSION__)
#define RYPATH_QMC_DEVICE device
#else
#define RYPATH_QMC_DEVICE
#endif
inline Word zword(const RYPATH_QMC_DEVICE sobol::Tables &tables, Index morton_index, Word dimension,
                  Word axis, bool pair, Word exponent, Word log_spp, Word mode, Word seed) {
    const Index index = zindex(morton_index, dimension, exponent, log_spp);
    const Index hash = hash2(dimension + (pair ? 2 : 1), seed);
    const Word random = pair && axis == 1 ? Word(hash >> 32) : Word(hash);
    return scramble(sobol::word(tables, index, axis, 0, 0), mode, random);
}
#undef RYPATH_QMC_DEVICE
} // namespace qmc
} // namespace rypath
