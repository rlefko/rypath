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
enum : Word { demo_model = 0, pbrt_model = 1 };
enum : Word { sampled_material = 0, constant_material = 1, rgb_sigmoid_material = 2 };
enum : Word { no_emission = 0xffffffffu };

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
    Word knot_begin;
    Word knot_count;
    Word kind;
    float constant_value;
    float coefficients[3];
};
struct SpectralKnot {
    float wavelength;
    float value;
};
struct Instance {
    float matrix[12];
    Word vertex_begin;
    Word triangle_begin;
    Word triangle_count;
    Word material;
    Word reverse_orientation;
    Word emission;
};
struct AreaEmission {
    float radiance[471];
    Word two_sided;
};
struct Emitter {
    Word instance;
    Word primitive;
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
    Word model;
    Word max_depth;
    Word material_count;
    Word knot_count;
    Word emission_count;
    Word emitter_count;
    Word emitter_slots;
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
static_assert(sizeof(Material) == 60 && alignof(Material) == 4 &&
                  __builtin_offsetof(Material, knot_begin) == 32 &&
                  __builtin_offsetof(Material, knot_count) == 36 &&
                  __builtin_offsetof(Material, kind) == 40 &&
                  __builtin_offsetof(Material, constant_value) == 44 &&
                  __builtin_offsetof(Material, coefficients) == 48,
              "render material layout");
static_assert(sizeof(SpectralKnot) == 8 && alignof(SpectralKnot) == 4 &&
                  __builtin_offsetof(SpectralKnot, value) == 4,
              "render spectral knot layout");
static_assert(sizeof(Instance) == 72 && alignof(Instance) == 4 &&
                  __builtin_offsetof(Instance, material) == 60 &&
                  __builtin_offsetof(Instance, reverse_orientation) == 64 &&
                  __builtin_offsetof(Instance, emission) == 68,
              "render instance layout");
static_assert(sizeof(AreaEmission) == 1888 && alignof(AreaEmission) == 4 &&
                  __builtin_offsetof(AreaEmission, two_sided) == 1884,
              "render area emission layout");
static_assert(sizeof(Emitter) == 8 && alignof(Emitter) == 4 &&
                  __builtin_offsetof(Emitter, primitive) == 4,
              "render emitter layout");
static_assert(sizeof(Parameters) == 156 && alignof(Parameters) == 4 &&
                  __builtin_offsetof(Parameters, film_scale) == 48 &&
                  __builtin_offsetof(Parameters, width) == 84 &&
                  __builtin_offsetof(Parameters, lane_count) == 120 &&
                  __builtin_offsetof(Parameters, model) == 128 &&
                  __builtin_offsetof(Parameters, knot_count) == 140 &&
                  __builtin_offsetof(Parameters, emission_count) == 144 &&
                  __builtin_offsetof(Parameters, emitter_count) == 148 &&
                  __builtin_offsetof(Parameters, emitter_slots) == 152,
              "render parameter layout");
static_assert(sizeof(Sample) == 44 && alignof(Sample) == 4 &&
                  __builtin_offsetof(Sample, wavelength) == 16 &&
                  __builtin_offsetof(Sample, random_words) == 28,
              "render sample layout");

} // namespace render_wire
} // namespace rypath
