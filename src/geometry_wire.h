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
enum : Word { miss = 0u, triangle = 1u, bilinear_patch = 2u, invalid_hit = 3u, analytic = 4u };

struct Triangle {
    float corners[3][3];
};

// These corners are copied into the acceleration structure as per-primitive data.
struct Patch {
    float corners[4][3];
};

struct Analytic {
    Word kind;
    Word subtype;
    float radius;
    float z_min;
    float z_max;
    float phi_max;
    float inner_radius;
    float height;
};

// Native primary triangle queries consume only this prefix. Compensated
// geometry and its supplementary native hint AS retain ProceduralRecord below.
enum : Word { native_metadata_contract = 2u };
struct NativeTriangleTag {
    Word kind;
};
static_assert(sizeof(NativeTriangleTag) == 4 && alignof(NativeTriangleTag) == 4,
              "Native primary triangle tag ABI");

// The first word tags each native bounding-box primitive. A miss tag preserves
// the slot of a proven zero-area triangle without offering it to the predicate.
// The existing Patch shading ABI and record size remain unchanged.
struct ProceduralRecord {
    Word kind;
    union {
        Patch patch;
        Triangle triangle;
        Analytic analytic;
    };
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
static_assert(sizeof(Triangle) == 36 && alignof(Triangle) == 4, "geometry wire triangle layout");
static_assert(sizeof(Patch) == 48 && alignof(Patch) == 4, "geometry wire patch size and alignment");
static_assert(sizeof(ProceduralRecord) == 52 && alignof(ProceduralRecord) == 4 &&
                  __builtin_offsetof(ProceduralRecord, patch) == 4 &&
                  __builtin_offsetof(ProceduralRecord, triangle) == 4,
              "geometry wire procedural record");
static_assert(sizeof(Analytic) == 32 && alignof(Analytic) == 4 &&
                  __builtin_offsetof(Analytic, radius) == 8,
              "geometry wire analytic layout");
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
