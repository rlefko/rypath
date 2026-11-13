// SPDX-License-Identifier: Apache-2.0
// Adapted from pbrt, Copyright (c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/sobol.md and third_party/pbrt/LICENSE.txt.

#pragma once

#if defined(__METAL_VERSION__)
#define RYPATH_SOBOL_DEVICE device
#else
#include <cstddef>
#include <cstdint>
#define RYPATH_SOBOL_DEVICE
#endif

namespace rypath {
namespace sobol {

#if defined(__METAL_VERSION__)
using Word = uint;
using Index = ulong;
#else
using Word = std::uint32_t;
using Index = std::uint64_t;
#endif

enum : Word {
    dimension_count = 1024,
    matrix_size = 52,
    max_scale_exponent = 25,
    no_randomization = 0,
    permutedigits = 1,
    fastowen = 2,
    owen = 3
};

struct Tables {
    Word directions[dimension_count * matrix_size];
    Index intervals[max_scale_exponent * matrix_size];
    Index inverse[(max_scale_exponent + 1) * matrix_size];
};

static_assert(sizeof(Word) == 4 && sizeof(Index) == 8, "Sobol word and index sizes");
static_assert(sizeof(Tables) == 234208 && alignof(Tables) == 8 &&
#if defined(__METAL_VERSION__)
                  __builtin_offsetof(Tables, directions) == 0 &&
                  __builtin_offsetof(Tables, intervals) == 212992 &&
                  __builtin_offsetof(Tables, inverse) == 223392,
#else
                  offsetof(Tables, directions) == 0 && offsetof(Tables, intervals) == 212992 &&
                  offsetof(Tables, inverse) == 223392,
#endif
              "Sobol table layout");

// The caller bounds m, pixel coordinates, sample and dimension before lookup.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline Index index(const RYPATH_SOBOL_DEVICE Tables &tables, Word m, Word x, Word y, Index sample) {
    if (m == 0) {
        return sample;
    }

    const Word row = (m - 1) * matrix_size;
    Index result = sample << (2 * m);
    Index delta = 0;
    for (Word column = row; sample != 0; ++column, sample >>= 1) {
        if (sample & 1) {
            delta ^= tables.intervals[column];
        }
    }
    Index pixel_bits = ((Index(x) << m) | y) ^ delta;
    for (Word column = row; pixel_bits != 0; ++column, pixel_bits >>= 1) {
        if (pixel_bits & 1) {
            result ^= tables.inverse[column];
        }
    }
    return result;
}

// The eight-byte input to PBRT's MurmurHash64A is LE32(dimension) || LE32(seed).
inline Word dimension_hash(Word dimension, Word seed_bits) {
    const Index multiplier = 0xc6a4a7935bd1e995ull;
    Index block = ((Index(seed_bits) << 32) | dimension) * multiplier;
    block ^= block >> 47;
    block *= multiplier;
    Index hash = (8 * multiplier ^ block) * multiplier;
    hash ^= hash >> 47;
    hash *= multiplier;
    return Word(hash ^ (hash >> 47));
}

inline Word reverse_bits(Word value) {
    value = (value << 16) | (value >> 16);
    value = ((value & 0x00ff00ffu) << 8) | ((value & 0xff00ff00u) >> 8);
    value = ((value & 0x0f0f0f0fu) << 4) | ((value & 0xf0f0f0f0u) >> 4);
    value = ((value & 0x33333333u) << 2) | ((value & 0xccccccccu) >> 2);
    return ((value & 0x55555555u) << 1) | ((value & 0xaaaaaaaau) >> 1);
}

inline Index mix_bits(Index value) {
    value ^= value >> 31;
    value *= 0x7fb5d329728ea185ull;
    value ^= value >> 27;
    value *= 0x81dadef4bc2dd44dull;
    return value ^ (value >> 33);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline Word word(const RYPATH_SOBOL_DEVICE Tables &tables, Index sample_index, Word dimension,
                 Word mode, Word seed_bits) {
    Word value = 0;
    for (Word column = dimension * matrix_size; sample_index != 0; ++column, sample_index >>= 1) {
        if (sample_index & 1) {
            value ^= tables.directions[column];
        }
    }
    if (mode == no_randomization) {
        return value;
    }

    const Word hash = dimension_hash(dimension, seed_bits);
    if (mode == permutedigits) {
        return value ^ hash;
    }
    if (mode == fastowen) {
        value = reverse_bits(value);
        value ^= value * 0x3d20adeau;
        value += hash;
        value *= (hash >> 16) | 1;
        value ^= value * 0x05526c56u;
        value ^= value * 0x53a22864u;
        return reverse_bits(value);
    }

    value ^= (hash & 1) << 31;
    for (Word bit = 1; bit < 32; ++bit) {
        const Word prefix = value & (~Word(0) << (32 - bit));
        if (Word(mix_bits(prefix ^ hash)) & (Word(1) << bit)) {
            value ^= Word(1) << (31 - bit);
        }
    }
    return value;
}

inline float uniform(Word value) {
    const float unit = float(value) * 0x1p-32f;
    return unit < 0x1.fffffep-1f ? unit : 0x1.fffffep-1f;
}

inline float camera_uniform(Word global_word, Word m, Word pixel) {
    const float local = uniform(global_word) * float(Word(1) << m) - float(pixel);
    if (local < 0.0f) {
        return 0.0f;
    }
    return local < 0x1.fffffep-1f ? local : 0x1.fffffep-1f;
}

} // namespace sobol
} // namespace rypath

#undef RYPATH_SOBOL_DEVICE
