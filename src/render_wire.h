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

enum : Word {
    success = 0,
    emergency_depth = 1,
    invalid_arithmetic = 2,
    invalid_hit = 3,
    boundary_exhaustion = 4
};
enum : Word { demo_model = 0, pbrt_model = 1 };
enum : Word { box_filter = 0, triangle_filter = 1 };
enum : Word {
    bilinear_texture_filter = 0,
    trilinear_texture_filter = 1,
    point_texture_filter = 2,
    ewa_texture_filter = 3,
};
enum : Word {
    srgb8_texel_format = 0,
    linear8_texel_format = 1,
    linear_half_texel_format = 2,
    linear_float_texel_format = 3
};
enum : Word { independent_sampler = 0, sobol_sampler = 1 };
enum : Word {
    sampled_material = 0,
    constant_material = 1,
    rgb_sigmoid_material = 2,
    image_material = 3,
    blackbody_material = 4,
    graph_material = 5
};
enum : Word { no_emission = 0xffffffffu };
enum : Word { no_normals = 0xffffffffu };
enum : Word { environment_grid_limit = 128 };
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
    Word offset; // Offset in texel words.
};
struct ImageTexture {
    Word level_begin;
    Word level_count;
    Word wrap;
    Word texel_format;
    float uscale;
    float vscale;
    float udelta;
    float vdelta;
    float scale;
    Word invert;
    Word filter;
    float max_anisotropy;
    Word transfer_offset = 0;
    Word channels = 3;
    Word gltf = 0;
    Word wrap_t = 10497;
    Word min_filter = 9987;
    Word mag_filter = 9729;
};
struct TextureParameters {
    Word texture_count;
    Word level_count;
    Word texel_word_count;
    Word uv_count;
    Word frame_count;
    float spp_scale;
    float world_from_camera[9];
    float camera_from_world[9];
    Point minimum_dx;
    Point minimum_dy;
    Word transfer_count = 512;
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
    // Constant reflectance, or bounded C2/temperature for blackbody reflectance.
    float scalar_value;
    float coefficients[3];
    Word image_texture;
    Word texture_root = 0xffffffffu;
    Word bump_root = 0xffffffffu;
    Word normal_map = 0xffffffffu;
    Word scattering = 0; // 0: diffuse, 1: conductor, 2: dielectric
    Word eta_begin = 0;
    Word eta_count = 0;
    Word k_begin = 0;
    Word k_count = 0;
    Word optical_extensions = 0; // bit 0: eta, bit 1: k
    float u_roughness = 0;
    float v_roughness = 0;
    Word u_roughness_root = 0xffffffffu;
    Word v_roughness_root = 0xffffffffu;
    Word remap_roughness = 1;
    Word auxiliary = 0xffffffffu; // Word offset in the material-data buffer.
};
struct GltfMaterial {
    float base_color[4];
    float metallic, roughness, normal_scale, alpha_cutoff;
    Word alpha_mode, double_sided;
    Word base_color_texture, metallic_roughness_texture, normal_texture, emissive_texture;
    float emissive[3];
};
struct MaterialAuxiliary {
    Word eta_root, k_root, conductor_reflectance_root;
    float transmittance;
    Word transmittance_root;
    float transmission_scale;
    Word transmission_scale_root;
    Word interface_eta_begin, interface_eta_count, interface_eta_extend, interface_eta_root;
    float interface_u_roughness, interface_v_roughness;
    Word interface_u_roughness_root, interface_v_roughness_root;
    float thickness;
    Word thickness_root;
    float albedo;
    Word albedo_root;
    float g;
    Word g_root;
    Word max_depth, samples;
    Word first_material, second_material;
    float mix_amount;
    Word mix_amount_root;
    Word table_offset;
    GltfMaterial gltf;
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
    Word alpha_root = 0xffffffffu;
    Word invisible_emitter = 0;
    Word inside_medium = 0xffffffffu;
    Word outside_medium = 0xffffffffu;
};
struct Medium {
    float sigma_a[471];
    float sigma_s[471];
    float emission[471];
    float g;
};
enum : Word { point_light = 0, spot_light = 1, goniometric_light = 2, projection_light = 3 };
struct DistantLight {
    Point direction;
    float radiance[471];
};
struct PositionalLight {
    Point position;
    float intensity[471];
    Word kind;
    float light_from_world[9];
    float cos_falloff_start;
    float cos_falloff_end;
    Word image_texture;
    float image_scale;
    float inv_tan_half_fov;
    float screen_half_width;
    float screen_half_height;
};
struct AreaEmission {
    float radiance[471];
    Word two_sided;
    Word image_texture = 0xffffffffu;
    float image_scale = 1;
    Word gltf_material = 0xffffffffu;
};
struct SurfaceVertex {
    Point position;
    Point normal;
    Point tangent;
    float tangent_sign = 1;
};
struct SurfaceTransform {
    float shape[9];
    float shape_normal[9];
    float placement[9];
    float placement_normal[9];
    Word vertex_begin;
    Word vertex_count;
    Word has_normals;
    Word has_tangents;
    Word authored_reverse;
    Word shape_reverse_winding;
    Point definition_translation;
};
struct TextureGraphParameters {
    Word node_count;
    Word knot_count;
    Word alpha_enabled;
    Word environment_image;
    float environment_scale;
    float environment_from_world[9];
    float world_from_environment[9];
    Word environment_width;
    Word environment_cdf_count;
    Word surface_vertex_count;
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
    Word material_data_words;
    Word medium_begin;
    Word medium_count;
    Word camera_medium = 0xffffffffu;
    Word boundary_limit = 4096;
    Word positional_begin;
    Word positional_count;
    Word distant_begin;
    Word distant_count;
    float distant_scene_radius;
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
static_assert(sizeof(ImageTexture) == 72 && alignof(ImageTexture) == 4 &&
                  __builtin_offsetof(ImageTexture, texel_format) == 12 &&
                  __builtin_offsetof(ImageTexture, uscale) == 16 &&
                  __builtin_offsetof(ImageTexture, filter) == 40 &&
                  __builtin_offsetof(ImageTexture, max_anisotropy) == 44,
              "image texture layout");
static_assert(sizeof(TextureParameters) == 124 && alignof(TextureParameters) == 4 &&
                  __builtin_offsetof(TextureParameters, world_from_camera) == 24 &&
                  __builtin_offsetof(TextureParameters, minimum_dx) == 96,
              "texture parameters layout");
static_assert(sizeof(Material) == 124 && alignof(Material) == 4 &&
                  __builtin_offsetof(Material, knot_begin) == 32 &&
                  __builtin_offsetof(Material, knot_count) == 36 &&
                  __builtin_offsetof(Material, kind) == 40 &&
                  __builtin_offsetof(Material, scalar_value) == 44 &&
                  __builtin_offsetof(Material, coefficients) == 48 &&
                  __builtin_offsetof(Material, image_texture) == 60 &&
                  __builtin_offsetof(Material, scattering) == 76 &&
                  __builtin_offsetof(Material, eta_begin) == 80 &&
                  __builtin_offsetof(Material, u_roughness) == 100 &&
                  __builtin_offsetof(Material, remap_roughness) == 116,
              "render material layout");
static_assert(sizeof(GltfMaterial) == 68 && sizeof(MaterialAuxiliary) == 180 &&
                  alignof(MaterialAuxiliary) == 4,
              "material auxiliary layout");
static_assert(sizeof(SpectralKnot) == 8 && alignof(SpectralKnot) == 4 &&
                  __builtin_offsetof(SpectralKnot, value) == 4,
              "render spectral knot layout");
static_assert(sizeof(Instance) == 108 && alignof(Instance) == 4 &&
                  __builtin_offsetof(Instance, material) == 60 &&
                  __builtin_offsetof(Instance, reverse_orientation) == 64 &&
                  __builtin_offsetof(Instance, emission) == 68 &&
                  __builtin_offsetof(Instance, normal_begin) == 72 &&
                  __builtin_offsetof(Instance, patch_begin) == 76 &&
                  __builtin_offsetof(Instance, patch_count) == 80 &&
                  __builtin_offsetof(Instance, uv_begin) == 84 &&
                  __builtin_offsetof(Instance, frame_begin) == 88,
              "render instance layout");
static_assert(sizeof(Medium) == 5656 && alignof(Medium) == 4 &&
                  __builtin_offsetof(Medium, g) == 5652,
              "homogeneous medium layout");
static_assert(sizeof(PositionalLight) == 1964 && alignof(PositionalLight) == 4 &&
                  __builtin_offsetof(PositionalLight, intensity) == 12 &&
                  __builtin_offsetof(PositionalLight, kind) == 1896 &&
                  __builtin_offsetof(PositionalLight, light_from_world) == 1900 &&
                  __builtin_offsetof(PositionalLight, cos_falloff_start) == 1936 &&
                  __builtin_offsetof(PositionalLight, image_texture) == 1944 &&
                  __builtin_offsetof(PositionalLight, image_scale) == 1948 &&
                  __builtin_offsetof(PositionalLight, inv_tan_half_fov) == 1952 &&
                  __builtin_offsetof(PositionalLight, screen_half_width) == 1956 &&
                  __builtin_offsetof(PositionalLight, screen_half_height) == 1960,
              "positional light layout");
static_assert(sizeof(DistantLight) == 1896 && alignof(DistantLight) == 4 &&
                  __builtin_offsetof(DistantLight, radiance) == 12,
              "distant light layout");
static_assert(sizeof(AreaEmission) == 1900 && alignof(AreaEmission) == 4 &&
                  __builtin_offsetof(AreaEmission, two_sided) == 1884,
              "render area emission layout");
static_assert(sizeof(SurfaceVertex) == 40 && sizeof(SurfaceTransform) == 180 &&
                  sizeof(TextureGraphParameters) == 104,
              "texture graph and surface layout");
static_assert(sizeof(Emitter) == 12 && alignof(Emitter) == 4 &&
                  __builtin_offsetof(Emitter, primitive) == 4 &&
                  __builtin_offsetof(Emitter, kind) == 8,
              "render emitter layout");
static_assert(sizeof(Parameters) == 224 && alignof(Parameters) == 4 &&
                  __builtin_offsetof(Parameters, distant_begin) == 212 &&
                  __builtin_offsetof(Parameters, distant_count) == 216 &&
                  __builtin_offsetof(Parameters, distant_scene_radius) == 220 &&
                  __builtin_offsetof(Parameters, positional_begin) == 204 &&
                  __builtin_offsetof(Parameters, positional_count) == 208 &&
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
