// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#if !defined(__METAL_VERSION__)
#include <cstdint>
#endif

namespace rypath {
namespace geometry_wire {

#if defined(__METAL_VERSION__)
using Word = uint;
#else
using Word = std::uint32_t;
#endif

// Standard Metal acceleration structures support eight visibility mask bits.
enum : Word { all_visible = 0xffu };
enum : Word { miss = 0u, triangle = 1u, bilinear_patch = 2u, invalid_hit = 3u };

// These corners are copied into the acceleration structure as per-primitive data.
struct Patch {
    float corners[4][3];
};

struct Ray {
    float origin[3];
    float direction[3];
    float t_min;
    float t_max;
};

struct Closest {
    Word kind;
    Word instance;
    Word geometry;
    Word primitive;
    float t;
    float u;
    float v;
};

static_assert(sizeof(Word) == 4 && sizeof(float) == 4, "geometry wire scalar width");
static_assert(sizeof(Patch) == 48 && alignof(Patch) == 4, "geometry wire patch size and alignment");
static_assert(sizeof(Ray) == 32 && alignof(Ray) == 4, "geometry wire ray size and alignment");
static_assert(__builtin_offsetof(Ray, direction) == 12 && __builtin_offsetof(Ray, t_min) == 24 &&
                  __builtin_offsetof(Ray, t_max) == 28,
              "geometry wire ray offsets");
static_assert(sizeof(Closest) == 28 && alignof(Closest) == 4,
              "geometry wire closest size and alignment");
static_assert(__builtin_offsetof(Closest, instance) == 4 &&
                  __builtin_offsetof(Closest, geometry) == 8 &&
                  __builtin_offsetof(Closest, primitive) == 12 &&
                  __builtin_offsetof(Closest, t) == 16 && __builtin_offsetof(Closest, u) == 20 &&
                  __builtin_offsetof(Closest, v) == 24,
              "geometry wire closest offsets");

} // namespace geometry_wire
} // namespace rypath
