// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#if !defined(__METAL_VERSION__)
#include <cstdint>
#endif

namespace rypath {
namespace render_wire {

#if defined(__METAL_VERSION__)
using Word = uint;
#else
using Word = std::uint32_t;
#endif

enum : Word { success = 0, emergency_depth = 1, invalid_arithmetic = 2, invalid_hit = 3 };

struct Point {
    float x;
    float y;
    float z;
};
struct Triangle {
    Word vertices[3];
};
struct Material {
    float reflectance[8];
};
struct Instance {
    float matrix[12];
    Word vertex_begin;
    Word triangle_begin;
    Word triangle_count;
    Word material;
};
struct Parameters {
    Point origin;
    Point forward;
    Point right;
    Point up;
    float film_scale;
    float knots[8];
    Word width;
    Word height;
    Word pixel_begin;
    Word pixel_count;
    Word sample_low;
    Word sample_high;
    Word seed_low;
    Word seed_high;
    Word emergency_limit;
    Word lane_count;
    Word instance_count;
};
struct Sample {
    Word status;
    Word pixel;
    Word sample_low;
    Word sample_high;
    float wavelength;
    float radiance;
    Word scatterings;
    Word random_words[4];
};

static_assert(sizeof(Word) == 4 && sizeof(float) == 4, "render wire scalar width");
static_assert(sizeof(Point) == 12 && alignof(Point) == 4, "render point layout");
static_assert(sizeof(Triangle) == 12 && alignof(Triangle) == 4, "render triangle layout");
static_assert(sizeof(Material) == 32 && alignof(Material) == 4, "render material layout");
static_assert(sizeof(Instance) == 64 && alignof(Instance) == 4 &&
                  __builtin_offsetof(Instance, material) == 60,
              "render instance layout");
static_assert(sizeof(Parameters) == 128 && alignof(Parameters) == 4 &&
                  __builtin_offsetof(Parameters, film_scale) == 48 &&
                  __builtin_offsetof(Parameters, width) == 84 &&
                  __builtin_offsetof(Parameters, lane_count) == 120,
              "render parameter layout");
static_assert(sizeof(Sample) == 44 && alignof(Sample) == 4 &&
                  __builtin_offsetof(Sample, wavelength) == 16 &&
                  __builtin_offsetof(Sample, random_words) == 28,
              "render sample layout");

} // namespace render_wire
} // namespace rypath
