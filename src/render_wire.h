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
enum : Word { box_filter = 0, triangle_filter = 1 };
enum : Word { independent_sampler = 0, sobol_sampler = 1 };
enum : Word {
    sampled_material = 0,
    constant_material = 1,
    rgb_sigmoid_material = 2,
    image_material = 3
};
enum : Word { no_emission = 0xffffffffu };
enum : Word { no_normals = 0xffffffffu };
enum : Word { triangle_primitive = 1, patch_primitive = 2 };

struct Point {
    float x;
    float y;
    float z;
};
struct Point2 {
    float x;
    float y;
};
struct TextureFrame {
    Point dpdu;
    Point dpdv;
};
struct TextureLevel {
    Word width;
    Word height;
    Word offset;
};
struct ImageTexture {
    Word level_begin;
    Word level_count;
    Word wrap;
    Word encoding;
    float uscale;
    float vscale;
    float udelta;
    float vdelta;
    float scale;
    Word invert;
};
struct TextureParameters {
    Word texture_count;
    Word level_count;
    Word texel_count;
    Word uv_count;
    Word frame_count;
    float spp_scale;
    float world_from_camera[9];
    float camera_from_world[9];
    Point minimum_dx;
    Point minimum_dy;
};
struct Triangle {
    Word vertices[3];
};
struct Patch {
    Word vertices[4];
};
struct Material {
    float reflectance[8];
    Word knot_begin;
    Word knot_count;
    Word kind;
    float constant_value;
    float coefficients[3];
    Word image_texture;
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
    Word normal_begin;
    Word patch_begin;
    Word patch_count;
    Word uv_begin;
    Word frame_begin;
};
struct AreaEmission {
    float radiance[471];
    Word two_sided;
};
struct Emitter {
    Word instance;
    Word primitive;
    Word kind;
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
    Word filter_kind;
    float filter_x_radius;
    float filter_y_radius;
    Word normal_count;
    Word sampler_kind;
    Word randomization;
    Word sobol_scale_exponent;
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
static_assert(sizeof(Patch) == 16 && alignof(Patch) == 4, "render patch layout");
static_assert(sizeof(Point2) == 8 && alignof(Point2) == 4, "texture coordinate layout");
static_assert(sizeof(TextureFrame) == 24 && alignof(TextureFrame) == 4, "texture frame layout");
static_assert(sizeof(TextureLevel) == 12 && alignof(TextureLevel) == 4, "texture level layout");
static_assert(sizeof(ImageTexture) == 40 && alignof(ImageTexture) == 4 &&
                  __builtin_offsetof(ImageTexture, uscale) == 16,
              "image texture layout");
static_assert(sizeof(TextureParameters) == 120 && alignof(TextureParameters) == 4 &&
                  __builtin_offsetof(TextureParameters, world_from_camera) == 24 &&
                  __builtin_offsetof(TextureParameters, minimum_dx) == 96,
              "texture parameters layout");
static_assert(sizeof(Material) == 64 && alignof(Material) == 4 &&
                  __builtin_offsetof(Material, knot_begin) == 32 &&
                  __builtin_offsetof(Material, knot_count) == 36 &&
                  __builtin_offsetof(Material, kind) == 40 &&
                  __builtin_offsetof(Material, constant_value) == 44 &&
                  __builtin_offsetof(Material, coefficients) == 48 &&
                  __builtin_offsetof(Material, image_texture) == 60,
              "render material layout");
static_assert(sizeof(SpectralKnot) == 8 && alignof(SpectralKnot) == 4 &&
                  __builtin_offsetof(SpectralKnot, value) == 4,
              "render spectral knot layout");
static_assert(sizeof(Instance) == 92 && alignof(Instance) == 4 &&
                  __builtin_offsetof(Instance, material) == 60 &&
                  __builtin_offsetof(Instance, reverse_orientation) == 64 &&
                  __builtin_offsetof(Instance, emission) == 68 &&
                  __builtin_offsetof(Instance, normal_begin) == 72 &&
                  __builtin_offsetof(Instance, patch_begin) == 76 &&
                  __builtin_offsetof(Instance, patch_count) == 80 &&
                  __builtin_offsetof(Instance, uv_begin) == 84 &&
                  __builtin_offsetof(Instance, frame_begin) == 88,
              "render instance layout");
static_assert(sizeof(AreaEmission) == 1888 && alignof(AreaEmission) == 4 &&
                  __builtin_offsetof(AreaEmission, two_sided) == 1884,
              "render area emission layout");
static_assert(sizeof(Emitter) == 12 && alignof(Emitter) == 4 &&
                  __builtin_offsetof(Emitter, primitive) == 4 &&
                  __builtin_offsetof(Emitter, kind) == 8,
              "render emitter layout");
static_assert(sizeof(Parameters) == 184 && alignof(Parameters) == 4 &&
                  __builtin_offsetof(Parameters, film_scale) == 48 &&
                  __builtin_offsetof(Parameters, width) == 84 &&
                  __builtin_offsetof(Parameters, lane_count) == 120 &&
                  __builtin_offsetof(Parameters, model) == 128 &&
                  __builtin_offsetof(Parameters, knot_count) == 140 &&
                  __builtin_offsetof(Parameters, emission_count) == 144 &&
                  __builtin_offsetof(Parameters, emitter_count) == 148 &&
                  __builtin_offsetof(Parameters, emitter_slots) == 152 &&
                  __builtin_offsetof(Parameters, filter_kind) == 156 &&
                  __builtin_offsetof(Parameters, filter_x_radius) == 160 &&
                  __builtin_offsetof(Parameters, filter_y_radius) == 164 &&
                  __builtin_offsetof(Parameters, normal_count) == 168 &&
                  __builtin_offsetof(Parameters, sampler_kind) == 172 &&
                  __builtin_offsetof(Parameters, randomization) == 176 &&
                  __builtin_offsetof(Parameters, sobol_scale_exponent) == 180,
              "render parameter layout");
static_assert(sizeof(Sample) == 44 && alignof(Sample) == 4 &&
                  __builtin_offsetof(Sample, wavelength) == 16 &&
                  __builtin_offsetof(Sample, random_words) == 28,
              "render sample layout");

} // namespace render_wire
} // namespace rypath
