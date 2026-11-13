// SPDX-License-Identifier: GPL-3.0-only

#include <metal_raytracing>
#include <metal_stdlib>

#include "../src/render_wire.h"
#include "../src/sobol_probe_wire.h"
#include "../src/sobol_shared.h"
#include "image_texture.h"
#include "intersection.h"
#include "texture_footprint.h"

using namespace metal;
using namespace metal::raytracing;
namespace wire = rypath::render_wire;
namespace sobol = rypath::sobol;

constant float pi = 3.14159265358979323846f;
constant uint all_visible = 0xffu;

kernel void rypath_sobol_probe(const device sobol::Tables &tables [[buffer(0)]],
                               const device sobol::Probe *queries [[buffer(1)]],
                               device sobol::ProbeResult *results [[buffer(2)]],
                               constant uint &count [[buffer(3)]],
                               uint lane [[thread_position_in_grid]]) {
    if (lane >= count) {
        return;
    }
    const auto q = queries[lane];
    const ulong index =
        sobol::index(tables, q.exponent, q.x, q.y, (ulong(q.sample_high) << 32) | q.sample_low);
    const uint value = sobol::word(tables, index, q.dimension, q.mode, q.seed);
    const uint camera = sobol::word(tables, index, 0, sobol::no_randomization, 0);
    results[lane] = {uint(index),
                     uint(index >> 32),
                     value,
                     sobol::dimension_hash(q.dimension, q.seed),
                     sobol::uniform(value),
                     sobol::camera_uniform(camera, q.exponent, q.x)};
}

static float3 unpack(wire::Point p) { return float3(p.x, p.y, p.z); }

static uint4 philox(uint4 counter, uint2 key) {
    // Philox4x32-10, from Random123 by D. E. Shaw Research.
    // See third_party/random123 for the source and license.
    constexpr uint m0 = 0xd2511f53u;
    constexpr uint m1 = 0xcd9e8d57u;
    constexpr uint2 step(0x9e3779b9u, 0xbb67ae85u);
    for (uint round = 0; round < 10; ++round) {
        counter = uint4(mulhi(m1, counter.z) ^ counter.y ^ key.x, m1 * counter.z,
                        mulhi(m0, counter.x) ^ counter.w ^ key.y, m0 * counter.x);
        key += step;
    }
    return counter;
}

static float u01(uint word) { return float(word >> 8) * 0x1.0p-24f; }

static float sample_uniform(constant wire::Parameters &p, uint word) {
    return p.sampler_kind == wire::sobol_sampler ? sobol::uniform(word) : u01(word);
}

static uint4 draw_words(constant wire::Parameters &p, const device sobol::Tables &tables,
                        thread const wire::Sample &sample, ulong index, uint block) {
    if (p.sampler_kind == wire::independent_sampler) {
        return philox(uint4(sample.pixel, sample.sample_low, sample.sample_high, block),
                      uint2(p.seed_low, p.seed_high));
    }
    const uint d = 4 * block;
    return uint4(sobol::word(tables, index, d, p.randomization, p.seed_low),
                 sobol::word(tables, index, d + 1, p.randomization, p.seed_low),
                 sobol::word(tables, index, d + 2, p.randomization, p.seed_low),
                 sobol::word(tables, index, d + 3, p.randomization, p.seed_low));
}

static wire::Sample begin_sample(constant wire::Parameters &p, const device sobol::Tables &tables,
                                 uint lane, thread ulong &index) {
    const uint pixel = p.pixel_begin + lane % p.pixel_count;
    const uint sample_low = p.sample_low + lane / p.pixel_count;
    const uint sample_high = p.sample_high + uint(sample_low < p.sample_low);
    uint4 words;
    if (p.sampler_kind == wire::sobol_sampler) {
        index = sobol::index(tables, p.sobol_scale_exponent, pixel % p.width, pixel / p.width,
                             (ulong(sample_high) << 32) | sample_low);
        words = uint4(sobol::word(tables, index, 0, sobol::no_randomization, 0),
                      sobol::word(tables, index, 1, sobol::no_randomization, 0),
                      sobol::word(tables, index, 2, p.randomization, p.seed_low),
                      sobol::word(tables, index, 3, p.randomization, p.seed_low));
    } else {
        words = philox(uint4(pixel, sample_low, sample_high, 0), uint2(p.seed_low, p.seed_high));
    }
    return wire::Sample{wire::success,
                        pixel,
                        sample_low,
                        sample_high,
                        360.0f + 470.0f * sample_uniform(p, words.z),
                        0.0f,
                        0,
                        {words.x, words.y, words.z, words.w}};
}

static float2 camera_uniforms(constant wire::Parameters &p, thread const wire::Sample &sample) {
    if (p.sampler_kind == wire::sobol_sampler) {
        return float2(sobol::camera_uniform(sample.random_words[0], p.sobol_scale_exponent,
                                            sample.pixel % p.width),
                      sobol::camera_uniform(sample.random_words[1], p.sobol_scale_exponent,
                                            sample.pixel / p.width));
    }
    return float2(u01(sample.random_words[0]), u01(sample.random_words[1]));
}

static float triangle_offset(float u, float radius) {
    // Invert the normalized tent CDF without subtracting nearly equal values
    // around its center. The existing camera uniform grid includes u = 0.
    const float s = 2.0f * u - 1.0f;
    return radius * precise::divide(s, 1.0f + precise::sqrt(1.0f - abs(s)));
}

static ray triangle_primary_ray(constant wire::Parameters &p, thread const wire::Sample &sample) {
    const float2 uv = camera_uniforms(p, sample);
    const float ox = triangle_offset(uv.x, p.filter_x_radius);
    const float oy = triangle_offset(uv.y, p.filter_y_radius);
    const float x = precise::divide((float(sample.pixel % p.width) + 0.5f) + ox, float(p.width));
    const float y = precise::divide((float(sample.pixel / p.width) + 0.5f) + oy, float(p.height));
    const float3 d =
        fma(float3(y), unpack(p.up), fma(float3(x), unpack(p.right), unpack(p.forward)));
    const float scale = max(abs(d.x), max(abs(d.y), abs(d.z)));
    // Use the existing invalid-ray path for unrepresentable camera arithmetic.
    if (!all(isfinite(d)) || !(scale > 0.0f)) {
        return ray(unpack(p.origin), float3(NAN));
    }
    const float3 scaled = precise::divide(d, float3(scale));
    if (!all(isfinite(scaled)) || !any(scaled != 0.0f)) {
        return ray(unpack(p.origin), float3(NAN));
    }
    return ray(unpack(p.origin), normalize(scaled));
}

static ray primary_ray(constant wire::Parameters &p, thread const wire::Sample &sample) {
    if (p.filter_kind == wire::triangle_filter) {
        return triangle_primary_ray(p, sample);
    }
    const float2 uv = camera_uniforms(p, sample);
    const float x = (float(sample.pixel % p.width) + uv.x) / float(p.width);
    const float y = (float(sample.pixel / p.width) + uv.y) / float(p.height);
    if (p.model == wire::pbrt_model) {
        const float3 d = unpack(p.forward) + x * unpack(p.right) + y * unpack(p.up);
        const float scale = max(abs(d.x), max(abs(d.y), abs(d.z)));
        return ray(unpack(p.origin), normalize(d / scale));
    }
    const float aspect = float(p.width) / float(p.height);
    const float3 direction =
        normalize(unpack(p.forward) + (2.0f * x - 1.0f) * aspect * p.film_scale * unpack(p.right) +
                  (1.0f - 2.0f * y) * p.film_scale * unpack(p.up));
    return ray(unpack(p.origin), direction);
}

static void texture_camera_rays(constant wire::Parameters &p, thread const wire::Sample &sample,
                                thread float3 &dx, thread float3 &dy) {
    const float2 uv = camera_uniforms(p, sample);
    float3 d;
    if (p.filter_kind == wire::triangle_filter) {
        const float x = precise::divide((float(sample.pixel % p.width) + 0.5f) +
                                            triangle_offset(uv.x, p.filter_x_radius),
                                        float(p.width));
        const float y = precise::divide((float(sample.pixel / p.width) + 0.5f) +
                                            triangle_offset(uv.y, p.filter_y_radius),
                                        float(p.height));
        d = fma(float3(y), unpack(p.up), fma(float3(x), unpack(p.right), unpack(p.forward)));
    } else {
        const float x = (float(sample.pixel % p.width) + uv.x) / float(p.width);
        const float y = (float(sample.pixel / p.width) + uv.y) / float(p.height);
        d = unpack(p.forward) + x * unpack(p.right) + y * unpack(p.up);
    }
    dx = d + unpack(p.right) / float(p.width);
    dy = d + unpack(p.up) / float(p.height);
}

static float demo_reflectance(const device wire::Material &material, constant wire::Parameters &p,
                              float wavelength) {
    for (uint i = 1; i < 8; ++i) {
        if (wavelength <= p.knots[i]) {
            const float t = (wavelength - p.knots[i - 1]) / (p.knots[i] - p.knots[i - 1]);
            return mix(material.reflectance[i - 1], material.reflectance[i], t);
        }
    }
    return material.reflectance[7];
}

static float pbrt_reflectance(const device wire::SpectralKnot *knots, uint count,
                              float wavelength) {
    if (wavelength < knots[0].wavelength || wavelength > knots[count - 1].wavelength) {
        return 0.0f;
    }
    uint lower = 0;
    uint upper = count - 1;
    while (upper - lower > 1) {
        const uint mid = lower + (upper - lower) / 2;
        if (wavelength < knots[mid].wavelength) {
            upper = mid;
        } else {
            lower = mid;
        }
    }
    if (lower == upper) {
        return knots[lower].value;
    }
    const float a = knots[lower].wavelength;
    const float b = knots[upper].wavelength;
    const float span = b - a;
    // Finite authored endpoints may straddle the entire binary32 range.
    // Halving before subtraction preserves the ratio when the full span overflows.
    const float t = isfinite(span) ? (wavelength - a) / span
                                   : (0.5f * wavelength - 0.5f * a) / (0.5f * b - 0.5f * a);
    return mix(knots[lower].value, knots[upper].value, t);
}

static float sigmoid_reflectance(const device wire::Material &material, float wavelength) {
    const float p = fma(fma(material.coefficients[0], wavelength, material.coefficients[1]),
                        wavelength, material.coefficients[2]);
    if (!isfinite(p)) {
        return NAN;
    }
    // Rescale the ratio before squaring a large polynomial value.
    if (abs(p) <= 1.0f) {
        return 0.5f + 0.5f * (p / sqrt(1.0f + p * p));
    }
    const float inverse = 1.0f / p;
    return 0.5f + 0.5f * copysign(1.0f / sqrt(1.0f + inverse * inverse), p);
}

static uint dense_index(float wavelength) {
    const uint nm = uint(wavelength);
    return nm + uint(wavelength - float(nm) >= 0.5f) - 360u;
}

static float sky_radiance(const device float *environment, constant wire::Parameters &p,
                          float wavelength) {
    if (p.model == wire::demo_model) {
        return 1.0f;
    }
    return environment[dense_index(wavelength)];
}

static float3 sphere(float u, float v) {
    const float z = 1.0f - 2.0f * u;
    const float r = sqrt(max(0.0f, 1.0f - z * z));
    const float phi = 2.0f * pi * v;
    return float3(r * cos(phi), r * sin(phi), z);
}

static float3 hemisphere(float3 n, float u, float v) {
    const float r = sqrt(u);
    const float phi = 2.0f * pi * v;
    const float3 axis = abs(n.z) < 0.999f ? float3(0, 0, 1) : float3(0, 1, 0);
    const float3 t = normalize(cross(axis, n));
    const float3 b = cross(n, t);
    return normalize((r * cos(phi)) * t + (r * sin(phi)) * b + sqrt(1.0f - u) * n);
}

// Relative power weights; p_light = 1/(4*pi) and p_bsdf = cos(theta)/pi.
static float light_weight(float cosine) {
    const float relative = 4.0f * cosine;
    return 1.0f / (1.0f + relative * relative);
}

static float bsdf_weight(float cosine) {
    const float relative = 4.0f * cosine;
    const float square = relative * relative;
    return square / (1.0f + square);
}

static float gamma_bound(float n) {
    constexpr float unit_roundoff = 0x1.0p-24f;
    return n * unit_roundoff / (1.0f - n * unit_roundoff);
}

static float3 round_out(float3 position, float3 n) {
    for (uint i = 0; i < 3; ++i) {
        if (n[i] != 0.0f) {
            position[i] = nextafter(position[i], copysign(INFINITY, n[i]));
        }
    }
    return position;
}

static float triangle_extent(float3 e1, float3 e2) {
    const float3 extent = max(abs(e1), max(abs(e2), abs(e2 - e1)));
    return max(extent.x, max(extent.y, extent.z));
}

struct SurfacePoint {
    float3 position;
    float3 error;
    float3 geometric_normal;
    float3 shading_normal;
    float inverse_area_pdf;
    float world_extent;
    bool patch;
};

enum class PointSource { native_hit, area_sample };

static bool triangle_surface_point(float2 uv, const device wire::Instance &instance, uint primitive,
                                   const device wire::Point *positions,
                                   const device wire::Triangle *triangles,
                                   const device wire::Point *normals, uint normal_count,
                                   PointSource source, thread SurfacePoint &point) {
    point.patch = false;
    const wire::Triangle triangle = triangles[instance.triangle_begin + primitive];
    const float3 p0 = unpack(positions[instance.vertex_begin + triangle.vertices[0]]);
    const float3 p1 = unpack(positions[instance.vertex_begin + triangle.vertices[1]]);
    const float3 p2 = unpack(positions[instance.vertex_begin + triangle.vertices[2]]);
    const float3 ax(instance.matrix[0], instance.matrix[1], instance.matrix[2]);
    const float3 ay(instance.matrix[4], instance.matrix[5], instance.matrix[6]);
    const float3 az(instance.matrix[8], instance.matrix[9], instance.matrix[10]);
    const float3 shift(instance.matrix[3], instance.matrix[7], instance.matrix[11]);
    const float3 e1 = p1 - p0;
    const float3 e2 = p2 - p0;
    const float3 world_e1(dot(ax, e1), dot(ay, e1), dot(az, e1));
    const float3 world_e2(dot(ax, e2), dot(ay, e2), dot(az, e2));
    const float3 cross_edges = cross(world_e1, world_e2);
    const float normal_squared = dot(cross_edges, cross_edges);
    if (!(normal_squared > 0.0f) || !isfinite(normal_squared)) {
        return false;
    }
    const float normal_length = sqrt(normal_squared);
    point.inverse_area_pdf = 0.5f * normal_length;
    point.world_extent = triangle_extent(world_e1, world_e2);
    point.geometric_normal = cross_edges / normal_length;
    if (instance.reverse_orientation != 0) {
        point.geometric_normal = -point.geometric_normal;
    }
    point.shading_normal = point.geometric_normal;
    if (instance.normal_begin != wire::no_normals) {
        const uint begin = instance.normal_begin;
        if (begin >= normal_count || triangle.vertices[0] >= normal_count - begin ||
            triangle.vertices[1] >= normal_count - begin ||
            triangle.vertices[2] >= normal_count - begin) {
            return false;
        }
        const float3 n0 = unpack(normals[begin + triangle.vertices[0]]);
        const float3 n1 = unpack(normals[begin + triangle.vertices[1]]);
        const float3 n2 = unpack(normals[begin + triangle.vertices[2]]);
        const float3 interpolated = (1.0f - (uv.x + uv.y)) * n0 + uv.x * n1 + uv.y * n2;
        if (!all(isfinite(interpolated))) {
            return false;
        }
        const float3 magnitude = abs(interpolated);
        const float scale = max(magnitude.x, max(magnitude.y, magnitude.z));
        if (scale > 0.0f) {
            // Normalize after interpolation and rescale before squaring. Keep
            // the oriented geometric normal at exact tangent and zero ties.
            const float3 scaled = precise::divide(interpolated, float3(scale));
            point.shading_normal = normalize(scaled);
            if (dot(point.geometric_normal, scaled) < 0.0f) {
                point.geometric_normal = -point.geometric_normal;
            }
        }
    }
    const float3 local = p0 + (uv.x * e1 + uv.y * e2);
    point.position = float3(dot(ax, local), dot(ay, local), dot(az, local)) + shift;
    float3 local_error = gamma_bound(7.0f) * (abs(p0) + abs(uv.x * e1) + abs(uv.y * e2));
    if (source == PointSource::native_hit) {
        // These allowances cover error in reconstructing a native hit. A light
        // sample's barycentrics are exact on its sampling grid, so its local
        // error comes from interpolation alone.
        constexpr float barycentric_allowance = 64.0f * 0x1.0p-23f;
        const float traversal_error = gamma_bound(7.0f) * triangle_extent(e1, e2);
        local_error = (local_error + barycentric_allowance * (abs(e1) + abs(e2))) + traversal_error;
    }
    const float3 error_input = local_error + gamma_bound(4.0f) * abs(local);
    point.error =
        float3(dot(abs(ax), error_input), dot(abs(ay), error_input), dot(abs(az), error_input)) +
        gamma_bound(4.0f) * abs(shift);
    return all(isfinite(point.position)) && all(isfinite(point.error)) &&
           all(isfinite(point.geometric_normal)) && all(isfinite(point.shading_normal));
}

// Uniform parameter sampling has area density 1/J. Geometric (u,v) is
// independent of any authored texture coordinates.
static bool patch_surface_point(float2 uv, const device wire::Instance &instance, uint primitive,
                                const device wire::Point *positions,
                                const device wire::Patch *patches,
                                const device wire::Point *normals, uint normal_count,
                                thread SurfacePoint &point) {
    point.patch = true;
    const wire::Patch patch = patches[instance.patch_begin + primitive];
    float3 p[4];
    for (uint i = 0; i < 4; ++i) {
        p[i] = unpack(positions[instance.vertex_begin + patch.vertices[i]]);
    }
    const float3 ax(instance.matrix[0], instance.matrix[1], instance.matrix[2]);
    const float3 ay(instance.matrix[4], instance.matrix[5], instance.matrix[6]);
    const float3 az(instance.matrix[8], instance.matrix[9], instance.matrix[10]);
    const float3 du = mix(p[1] - p[0], p[3] - p[2], uv.y);
    const float3 dv = mix(p[2] - p[0], p[3] - p[1], uv.x);
    const float3 wu(dot(ax, du), dot(ay, du), dot(az, du));
    const float3 wv(dot(ax, dv), dot(ay, dv), dot(az, dv));
    const float3 c = cross(wu, wv);
    const float squared = dot(c, c);
    if (!(squared >= FLT_MIN) || !isfinite(squared)) {
        return false;
    }
    point.inverse_area_pdf = sqrt(squared);
    point.geometric_normal = c / point.inverse_area_pdf;
    if (instance.reverse_orientation != 0) {
        point.geometric_normal = -point.geometric_normal;
    }
    point.shading_normal = point.geometric_normal;
    if (instance.normal_begin != wire::no_normals) {
        float3 n[4];
        for (uint i = 0; i < 4; ++i) {
            if (instance.normal_begin >= normal_count ||
                patch.vertices[i] >= normal_count - instance.normal_begin) {
                return false;
            }
            n[i] = unpack(normals[instance.normal_begin + patch.vertices[i]]);
        }
        const float3 interpolated = mix(mix(n[0], n[2], uv.y), mix(n[1], n[3], uv.y), uv.x);
        if (!all(isfinite(interpolated))) {
            return false;
        }
        const float3 a = abs(interpolated);
        const float scale = max(a.x, max(a.y, a.z));
        if (scale > 0) {
            const float3 scaled = precise::divide(interpolated, float3(scale));
            point.shading_normal = normalize(scaled);
            if (dot(point.geometric_normal, scaled) < 0) {
                point.geometric_normal = -point.geometric_normal;
            }
        }
    }
    // Retain interpolation and affine roundoff in a second float. A bound
    // proportional to absolute translated coordinates at ordinary precision
    // can move a small surface across a nearby light before tracing starts.
    BilinearNumber local[3];
    float3 magnitude = abs(p[0]) + abs(p[1]) + abs(p[2]) + abs(p[3]);
    for (uint axis = 0; axis < 3; ++axis) {
        const auto a =
            BilinearNumber(p[0][axis]) + (BilinearNumber(p[2][axis]) - p[0][axis]) * uv.y;
        const auto b =
            BilinearNumber(p[1][axis]) + (BilinearNumber(p[3][axis]) - p[1][axis]) * uv.y;
        local[axis] = a + (b - a) * uv.x;
    }
    const bool identity = instance.matrix[3] == 0 && instance.matrix[7] == 0 &&
                          instance.matrix[11] == 0 && all(ax == float3(1, 0, 0)) &&
                          all(ay == float3(0, 1, 0)) && all(az == float3(0, 0, 1));
    for (uint axis = 0; axis < 3; ++axis) {
        BilinearNumber world(instance.matrix[axis * 4 + 3]);
        float bound = abs(instance.matrix[axis * 4 + 3]);
        for (uint j = 0; j < 3; ++j) {
            world = world + local[j] * instance.matrix[axis * 4 + j];
            bound += magnitude[j] * abs(instance.matrix[axis * 4 + j]);
        }
        point.position[axis] = world.high;
        // Pair operations discard second-order roundoff; 64 eps^2 covers
        // the nested lerps and matrix sums. Native spawning also encloses
        // the discrepancy to the committed ray's point.
        // Native instancing rounds the inverse ray transform independently
        // of this forward reconstruction. Keep its local-coordinate allowance
        // for nonidentity maps; host validation bounds its displacement.
        const float traversal = identity ? 0.0f : 0x1p-20f * bound;
        point.error[axis] = abs(world.low) + 0x1p-40f * bound + traversal + 128.0f * FLT_MIN;
    }
    point.world_extent = 0;
    for (uint i = 1; i < 4; ++i) {
        const float3 e = p[i] - p[0];
        const float3 w = abs(float3(dot(ax, e), dot(ay, e), dot(az, e)));
        point.world_extent = max(point.world_extent, max(w.x, max(w.y, w.z)));
    }
    return all(isfinite(point.position)) && all(isfinite(point.error)) &&
           all(isfinite(point.geometric_normal)) && all(isfinite(point.shading_normal));
}

static bool surface_point(float2 uv, uint kind, const device wire::Instance &instance,
                          uint primitive, const device wire::Point *positions,
                          const device wire::Triangle *triangles, const device wire::Patch *patches,
                          const device wire::Point *normals, uint normal_count, PointSource source,
                          thread SurfacePoint &point) {
    if (kind == wire::patch_primitive) {
        return patch_surface_point(uv, instance, primitive, positions, patches, normals,
                                   normal_count, point);
    }
    return triangle_surface_point(uv, instance, primitive, positions, triangles, normals,
                                  normal_count, source, point);
}

static float3x3 texture_matrix(constant float *m) {
    return float3x3(float3(m[0], m[3], m[6]), float3(m[1], m[4], m[7]), float3(m[2], m[5], m[8]));
}

static float3 texture_world_vector(const device wire::Instance &instance, float3 v) {
    return float3(dot(float3(instance.matrix[0], instance.matrix[1], instance.matrix[2]), v),
                  dot(float3(instance.matrix[4], instance.matrix[5], instance.matrix[6]), v),
                  dot(float3(instance.matrix[8], instance.matrix[9], instance.matrix[10]), v));
}

static bool texture_vertex_uv(const device wire::Point2 *uvs, uint begin, uint vertex_index,
                              uint count, thread float2 &uv) {
    if (begin >= count || vertex_index >= count - begin)
        return false;
    const auto value = uvs[begin + vertex_index];
    uv = float2(value.x, value.y);
    return all(isfinite(uv));
}

static float
image_reflectance(const device wire::Instance &instance, uint primitive, uint kind, float2 hit_uv,
                  thread const SurfacePoint &point, const device wire::Point *positions,
                  const device wire::Triangle *triangles, const device wire::Patch *patches,
                  const device wire::Point2 *uvs, const device wire::TextureFrame *frames,
                  const device wire::ImageTexture &texture, const device wire::TextureLevel *levels,
                  const device wire::Word *texels, const device float *transfer,
                  const device float *rgb_table, constant wire::TextureParameters &tp,
                  constant wire::Parameters &p, thread const wire::Sample &sample,
                  float3 primary_direction) {
    float2 st;
    float3 dpdu, dpdv, local_p;
    if (kind == wire::triangle_primitive) {
        const auto triangle = triangles[instance.triangle_begin + primitive];
        const float3 p0 = unpack(positions[instance.vertex_begin + triangle.vertices[0]]);
        const float3 p1 = unpack(positions[instance.vertex_begin + triangle.vertices[1]]);
        const float3 p2 = unpack(positions[instance.vertex_begin + triangle.vertices[2]]);
        local_p = fma(float3(hit_uv.y), p2 - p0, fma(float3(hit_uv.x), p1 - p0, p0));
        float2 a(0, 0), b(1, 0), c(1, 1);
        if (instance.uv_begin != 0xffffffffu &&
            (!texture_vertex_uv(uvs, instance.uv_begin, triangle.vertices[0], tp.uv_count, a) ||
             !texture_vertex_uv(uvs, instance.uv_begin, triangle.vertices[1], tp.uv_count, b) ||
             !texture_vertex_uv(uvs, instance.uv_begin, triangle.vertices[2], tp.uv_count, c)))
            return NAN;
        if (!rypath_texture_triangle_uv(float3(1.0f - (hit_uv.x + hit_uv.y), hit_uv.x, hit_uv.y), a,
                                        b, c, st) ||
            instance.frame_begin >= tp.frame_count ||
            primitive >= tp.frame_count - instance.frame_begin)
            return NAN;
        const auto frame = frames[instance.frame_begin + primitive];
        dpdu = unpack(frame.dpdu);
        dpdv = unpack(frame.dpdv);
    } else {
        const auto patch = patches[instance.patch_begin + primitive];
        float3 v[4];
        float2 uv[4] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
        for (uint i = 0; i < 4; ++i) {
            v[i] = unpack(positions[instance.vertex_begin + patch.vertices[i]]);
            if (instance.uv_begin != 0xffffffffu &&
                !texture_vertex_uv(uvs, instance.uv_begin, patch.vertices[i], tp.uv_count, uv[i]))
                return NAN;
        }
        const float3 du = texture_world_vector(instance, mix(v[1] - v[0], v[3] - v[2], hit_uv.y));
        const float3 dv = texture_world_vector(instance, mix(v[2] - v[0], v[3] - v[1], hit_uv.x));
        local_p = mix(mix(v[0], v[1], hit_uv.x), mix(v[2], v[3], hit_uv.x), hit_uv.y);
        if (!rypath_texture_patch_frame(hit_uv, du, dv, instance.uv_begin != 0xffffffffu, uv[0],
                                        uv[1], uv[2], uv[3], st, dpdu, dpdv))
            return NAN;
    }
    float3 dpdx, dpdy;
    // Rebase the affine translation before adding local coordinates. Subtracting
    // the camera from an already-rounded world point loses small surface offsets.
    const float3 relative_p =
        texture_world_vector(instance, local_p) +
        (float3(instance.matrix[3], instance.matrix[7], instance.matrix[11]) - unpack(p.origin));
    if (!all(isfinite(relative_p)))
        return NAN;
    bool primary = false;
    if (sample.scatterings == 0u) {
        float3 rx, ry;
        texture_camera_rays(p, sample, rx, ry);
        if (!all(isfinite(rx)) || !all(isfinite(ry)))
            return NAN;
        primary = rypath_texture_primary_dp(relative_p, point.geometric_normal, primary_direction,
                                            rx, ry, tp.spp_scale, dpdx, dpdy);
    }
    if (!primary) {
        rypath_texture_approximate_dp(relative_p, point.geometric_normal,
                                      texture_matrix(tp.world_from_camera),
                                      texture_matrix(tp.camera_from_world), unpack(tp.minimum_dx),
                                      unpack(tp.minimum_dy), tp.spp_scale, dpdx, dpdy);
    }
    float2 dstdx, dstdy;
    if (!rypath_texture_uv_derivatives(dpdu, dpdv, dpdx, dpdy, dstdx, dstdy))
        return NAN;
    float3 rgb = rypath_image::lookup(texture, levels, texels, transfer, tp, st, dstdx, dstdy);
    rgb *= texture.scale;
    if (texture.invert != 0u)
        rgb = 1.0f - rgb;
    return rypath_image::spectrum(rgb, sample.wavelength, rgb_table);
}

static float2 patch_sample_uv(uint u, uint v) {
    // Independent 23-bit midpoint coordinates stay strictly within the square.
    return (float2(u >> 9, v >> 9) + 0.5f) * 0x1.0p-23f;
}

static float3 spawn_origin(ray incoming, float t, SurfacePoint point, float3 n) {
    float3 ray_position = fma(float3(t), incoming.direction, incoming.origin);
    // Preserve the existing triangle allowance; patch reconstruction retains
    // its roundoff explicitly so a large translation does not inflate offsets.
    float3 ray_error = gamma_bound(2.0f) * (abs(t * incoming.direction) + abs(incoming.origin));
    if (point.patch) {
        for (uint axis = 0; axis < 3; ++axis) {
            const auto value = BilinearNumber(t) * incoming.direction[axis] + incoming.origin[axis];
            ray_position[axis] = value.high;
            ray_error[axis] =
                abs(value.low) +
                0x1p-40f * (abs(t * incoming.direction[axis]) + abs(incoming.origin[axis])) +
                128.0f * FLT_MIN;
        }
    }
    const float3 error = point.error + abs(point.position - ray_position) + ray_error;
    return round_out(point.position + dot(abs(n), error) * n, n);
}

static float3 spawn_toward(ray incoming, float t, SurfacePoint point, float3 direction) {
    const float3 ng = point.geometric_normal;
    return spawn_origin(incoming, t, point, dot(ng, direction) < 0.0f ? -ng : ng);
}

// Reflect independent midpoint coordinates into the triangle. The unequal
// grids make the seam unreachable and preserve three positive binary32 weights.
static float2 triangle_uv(uint u_word, uint v_word) {
    constexpr uint grid = 1u << 24;
    uint u = 2u * (u_word >> 9) + 1u;
    uint v = 4u * (v_word >> 10) + 2u;
    if (u + v > grid) {
        u = grid - u;
        v = grid - v;
    }
    return float2(u, v) * 0x1.0p-24f;
}

struct AreaWeights {
    float direct_factor;
    float hit_weight;
};

static AreaWeights area_weights(float area, float light_cosine, float bsdf_pdf,
                                float distance_squared, uint slots) {
    // p_bsdf / p_light = (slots * area * |cos_light| * p_bsdf) / r^2.
    // Work with the smaller ratio so neither a large PDF nor its square overflows.
    const float a = (float(slots) * area) * light_cosine * bsdf_pdf;
    const float b = distance_squared;
    if (!(a >= FLT_MIN && b >= FLT_MIN) || !isfinite(a) || !isfinite(b)) {
        return {NAN, NAN};
    }
    const float ratio = min(a, b) / max(a, b);
    if (!(ratio >= FLT_MIN)) {
        return {NAN, NAN};
    }
    const float square = ratio * ratio;
    const float denominator = 1.0f + square;
    // When a <= b, the square is the numerator of the hit contribution.
    // When a > b, its underflow only rounds the denominator to one.
    const float hit_weight =
        a > b ? 1.0f / denominator : (square >= FLT_MIN ? square / denominator : NAN);
    return {ratio / denominator, hit_weight};
}

static float3 shadow_segment_direction(float3 origin, float3 delta, SurfacePoint target,
                                       float signed_cosine) {
    const float3 n = signed_cosine > 0.0f ? target.geometric_normal : -target.geometric_normal;
    // Include connection-ray arithmetic as well as the sampled point's error.
    // Traversal of the connection still needs an empirical allowance. Use the
    // world-space extent so stretching a local tangent axis cannot enlarge
    // the normal offset after the instance transform cancels that stretch.
    const float3 error = target.error + gamma_bound(2.0f) * (abs(origin) + abs(delta)) +
                         gamma_bound(7.0f) * target.world_extent;
    const float3 endpoint = round_out(target.position + dot(abs(n), error) * n, n);
    return endpoint - origin;
}

kernel void rypath_render_sky(constant wire::Parameters &p [[buffer(0)]],
                              device wire::Sample *output [[buffer(1)]],
                              const device float *environment [[buffer(7)]],
                              const device sobol::Tables &tables [[buffer(12)]],
                              uint lane [[thread_position_in_grid]]) {
    if (lane >= p.lane_count) {
        return;
    }
    ulong index = 0;
    wire::Sample sample = begin_sample(p, tables, lane, index);
    if (p.model == wire::pbrt_model && !all(isfinite(primary_ray(p, sample).direction))) {
        sample.status = wire::invalid_arithmetic;
    }
    sample.radiance = sky_radiance(environment, p, sample.wavelength);
    output[lane] = sample;
}

kernel void rypath_render_paths(constant wire::Parameters &p [[buffer(0)]],
                                device wire::Sample *output [[buffer(1)]],
                                instance_acceleration_structure scene [[buffer(2)]],
                                const device wire::Instance *instances [[buffer(3)]],
                                const device wire::Point *positions [[buffer(4)]],
                                const device wire::Triangle *triangles [[buffer(5)]],
                                const device wire::Material *materials [[buffer(6)]],
                                const device float *environment [[buffer(7)]],
                                const device wire::SpectralKnot *knots [[buffer(8)]],
                                const device wire::AreaEmission *emissions [[buffer(9)]],
                                const device wire::Emitter *emitters [[buffer(10)]],
                                const device wire::Point *normals [[buffer(11)]],
                                const device sobol::Tables &tables [[buffer(12)]],
                                const device wire::Patch *patches [[buffer(13)]],
                                MixedIntersectionTable intersection_table [[buffer(14)]],
                                const device wire::Point2 *texture_uvs [[buffer(15)]],
                                const device wire::TextureFrame *texture_frames [[buffer(16)]],
                                const device wire::ImageTexture *image_textures [[buffer(17)]],
                                const device wire::TextureLevel *image_levels [[buffer(18)]],
                                const device wire::Word *image_texels [[buffer(19)]],
                                const device float *image_transfer [[buffer(20)]],
                                const device float *image_spectra [[buffer(21)]],
                                constant wire::TextureParameters &texture_p [[buffer(22)]],
                                uint lane [[thread_position_in_grid]]) {
    if (lane >= p.lane_count) {
        return;
    }
    ulong index = 0;
    wire::Sample sample = begin_sample(p, tables, lane, index);
    ray incoming = primary_ray(p, sample);
    const float3 primary_direction = incoming.direction;
    MixedIntersector closest;
    closest.set_triangle_cull_mode(triangle_cull_mode::none);
    MixedIntersector shadow;
    shadow.set_triangle_cull_mode(triangle_cull_mode::none);
    shadow.accept_any_intersection(true);
    float throughput = 1.0f;
    float escape_weight = 1.0f;
    float previous_bsdf_pdf = 0.0f;
    float3 previous_area_reference = incoming.origin;
    const float E = sky_radiance(environment, p, sample.wavelength);

    while (true) {
        if (!all(isfinite(incoming.direction)) || !isfinite(throughput) ||
            !isfinite(sample.radiance)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const auto hit = trace_surface(closest, incoming, scene, all_visible, intersection_table);
        if (hit.type == intersection_type::none) {
            sample.radiance += throughput * escape_weight * E;
            break;
        }
        const uint bounce = sample.scatterings;
        const bool patch_hit = hit.type == intersection_type::bounding_box;
        if (!hit.valid || (!patch_hit && hit.type != intersection_type::triangle) ||
            hit.instance_id >= p.instance_count || !isfinite(hit.distance)) {
            sample.status = wire::invalid_hit;
            break;
        }

        const device wire::Instance &instance = instances[hit.instance_id];
        const uint kind = patch_hit ? wire::patch_primitive : wire::triangle_primitive;
        const uint primitive_count = patch_hit ? instance.patch_count : instance.triangle_count;
        if (hit.geometry_id != 0 || hit.primitive_id >= primitive_count ||
            instance.material >= p.material_count) {
            sample.status = wire::invalid_hit;
            break;
        }
        const float2 hit_uv = hit.coordinates;
        if (!all(isfinite(hit_uv))) {
            sample.status = wire::invalid_hit;
            break;
        }
        SurfacePoint point;
        const bool emissive = instance.emission != wire::no_emission;
        if (emissive) {
            if (instance.emission >= p.emission_count || p.emitter_count == 0u) {
                sample.status = wire::invalid_hit;
                break;
            }
            if (!surface_point(hit_uv, kind, instance, hit.primitive_id, positions, triangles,
                               patches, normals, p.normal_count, PointSource::native_hit, point)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            const device wire::AreaEmission &emission = emissions[instance.emission];
            const float Le = emission.radiance[dense_index(sample.wavelength)];
            if (Le > 0.0f && (emission.two_sided != 0u ||
                              dot(point.geometric_normal, -incoming.direction) >= 0.0f)) {
                float weight = 1.0f;
                if (bounce != 0u) {
                    // Use the previous area proposal's reference point, which
                    // can differ from the displaced traversal ray origin.
                    const float3 delta = point.position - previous_area_reference;
                    const float distance_squared = dot(delta, delta);
                    if (!(distance_squared >= FLT_MIN) || !isfinite(distance_squared)) {
                        sample.status = wire::invalid_arithmetic;
                        break;
                    }
                    const float light_cosine =
                        abs(dot(point.geometric_normal, -delta / sqrt(distance_squared)));
                    // Pinned PBRT reports zero light PDF at an exactly tangent
                    // endpoint; the BSDF technique then receives full weight.
                    weight =
                        patch_hit && light_cosine == 0.0f
                            ? 1.0f
                            : area_weights(point.inverse_area_pdf, light_cosine, previous_bsdf_pdf,
                                           distance_squared, p.emitter_slots)
                                  .hit_weight;
                }
                if (!isfinite(weight)) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                sample.radiance += throughput * (weight * Le);
            }
        }
        // Emission at this endpoint has already contributed at authored depth.
        if (p.model == wire::pbrt_model && bounce >= p.max_depth) {
            break;
        }
        const device wire::Material &material = materials[instance.material];
        if (material.kind == wire::image_material && !emissive &&
            !surface_point(hit_uv, kind, instance, hit.primitive_id, positions, triangles, patches,
                           normals, p.normal_count, PointSource::native_hit, point)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        float rho = NAN;
        if (p.model == wire::demo_model && material.kind == wire::sampled_material) {
            rho = demo_reflectance(material, p, sample.wavelength);
        } else if (p.model == wire::pbrt_model && material.kind == wire::sampled_material &&
                   material.knot_count >= 2 && material.knot_begin <= p.knot_count &&
                   material.knot_count <= p.knot_count - material.knot_begin) {
            rho = pbrt_reflectance(knots + material.knot_begin, material.knot_count,
                                   sample.wavelength);
        } else if (p.model == wire::pbrt_model && material.kind == wire::constant_material) {
            rho = material.constant_value;
        } else if (p.model == wire::pbrt_model && material.kind == wire::rgb_sigmoid_material) {
            rho = sigmoid_reflectance(material, sample.wavelength);
        } else if (p.model == wire::pbrt_model && material.kind == wire::image_material &&
                   material.image_texture < texture_p.texture_count) {
            rho = image_reflectance(instance, hit.primitive_id, kind, hit_uv, point, positions,
                                    triangles, patches, texture_uvs, texture_frames,
                                    image_textures[material.image_texture], image_levels,
                                    image_texels, image_transfer, image_spectra, texture_p, p,
                                    sample, primary_direction);
        } else {
            sample.status = wire::invalid_hit;
            break;
        }
        if (!(rho >= 0.0f && rho <= 1.0f)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        if (rho == 0.0f) {
            break;
        }
        const bool authored_normals = instance.normal_begin != wire::no_normals;
        if (!authored_normals && bounce >= p.emergency_limit) {
            sample.status = wire::emergency_depth;
            break;
        }
        if (!emissive && material.kind != wire::image_material &&
            !surface_point(hit_uv, kind, instance, hit.primitive_id, positions, triangles, patches,
                           normals, p.normal_count, PointSource::native_hit, point)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const float incident_cosine = dot(point.shading_normal, incoming.direction);
        if (authored_normals && incident_cosine == 0.0f) {
            break;
        }
        if (bounce >= p.emergency_limit) {
            sample.status = wire::emergency_depth;
            break;
        }
        const float3 n = incident_cosine > 0.0f ? -point.shading_normal : point.shading_normal;
        const bool directional_offset = authored_normals || patch_hit;
        const float3 area_reference =
            directional_offset ? point.position : spawn_origin(incoming, hit.distance, point, n);
        if (!all(isfinite(area_reference))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const uint4 light_words = draw_words(p, tables, sample, index, 2 + 2 * bounce);
        const float3 light_direction =
            sphere(sample_uniform(p, light_words.x), sample_uniform(p, light_words.y));
        const float light_cosine = dot(n, light_direction);
        if (E > 0.0f && light_cosine > 0.0f) {
            const float3 origin = directional_offset
                                      ? spawn_toward(incoming, hit.distance, point, light_direction)
                                      : area_reference;
            if (!all(isfinite(origin))) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            if (trace_shadow(shadow, ray(origin, light_direction), scene, all_visible,
                             intersection_table)
                    .type == intersection_type::none) {
                sample.radiance +=
                    throughput * rho * (4.0f * light_cosine) * light_weight(light_cosine) * E;
            }
        }

        const uint4 bsdf_words = draw_words(p, tables, sample, index, 1 + 2 * bounce);
        // A null slot intentionally contributes zero. This gives every emitter
        // an exact dyadic PMF and avoids altering the BSDF and sky streams.
        const uint selected =
            p.sampler_kind == wire::sobol_sampler
                ? (p.emitter_slots == 1 ? 0 : bsdf_words.w >> (clz(p.emitter_slots) + 1))
                : bsdf_words.w & (p.emitter_slots - 1u);
        if (selected < p.emitter_count) {
            const wire::Emitter emitter = emitters[selected];
            if (emitter.instance >= p.instance_count) {
                sample.status = wire::invalid_hit;
                break;
            }
            const device wire::Instance &light_instance = instances[emitter.instance];
            const bool patch_light = emitter.kind == wire::patch_primitive;
            const uint light_count =
                patch_light ? light_instance.patch_count : light_instance.triangle_count;
            if ((!patch_light && emitter.kind != wire::triangle_primitive) ||
                emitter.primitive >= light_count || light_instance.emission >= p.emission_count) {
                sample.status = wire::invalid_hit;
                break;
            }
            const device wire::AreaEmission &emission = emissions[light_instance.emission];
            const float Le = emission.radiance[dense_index(sample.wavelength)];
            // A coplanar connection has zero geometric emitter cosine.
            const bool same_triangle = !patch_hit && !patch_light &&
                                       emitter.instance == hit.instance_id &&
                                       emitter.primitive == hit.primitive_id;
            if (Le > 0.0f && !same_triangle) {
                SurfacePoint target;
                const float2 light_uv = patch_light ? patch_sample_uv(light_words.z, light_words.w)
                                                    : triangle_uv(light_words.z, light_words.w);
                if (!surface_point(light_uv, emitter.kind, light_instance, emitter.primitive,
                                   positions, triangles, patches, normals, p.normal_count,
                                   PointSource::area_sample, target)) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                const float3 delta = target.position - area_reference;
                const float distance_squared = dot(delta, delta);
                const bool same_patch = patch_hit && patch_light &&
                                        emitter.instance == hit.instance_id &&
                                        emitter.primitive == hit.primitive_id;
                if (!isfinite(distance_squared) ||
                    (!(distance_squared >= FLT_MIN) && !same_patch)) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                // Two samples of the same patch can round to one endpoint.
                // Its degenerate connection contributes zero; retain the BSDF
                // continuation and all noncoincident curved self-lighting.
                if (distance_squared >= FLT_MIN) {
                    const float3 direction = delta / sqrt(distance_squared);
                    const float receiver_cosine = dot(n, direction);
                    const float emitter_cosine = dot(target.geometric_normal, -direction);
                    if (receiver_cosine > 0.0f && emitter_cosine != 0.0f &&
                        (emission.two_sided != 0u || emitter_cosine > 0.0f)) {
                        const float factor =
                            area_weights(target.inverse_area_pdf, abs(emitter_cosine),
                                         receiver_cosine / pi, distance_squared, p.emitter_slots)
                                .direct_factor;
                        const float3 origin =
                            directional_offset
                                ? spawn_toward(incoming, hit.distance, point, direction)
                                : area_reference;
                        const float3 shadow_delta =
                            directional_offset ? target.position - origin : delta;
                        const float endpoint_cosine =
                            directional_offset ? dot(target.geometric_normal, -shadow_delta)
                                               : emitter_cosine;
                        const float3 segment =
                            shadow_segment_direction(origin, shadow_delta, target, endpoint_cosine);
                        const float progress = dot(segment, direction);
                        if (!isfinite(factor) || !all(isfinite(segment)) || !(progress > 0.0f) ||
                            !isfinite(progress)) {
                            sample.status = wire::invalid_arithmetic;
                            break;
                        }
                        const ray connection(origin, segment, 0.0f, 0x1.fffffep-1f);
                        if (trace_shadow(shadow, connection, scene, all_visible, intersection_table)
                                .type == intersection_type::none) {
                            sample.radiance += (throughput * rho) * (factor * Le);
                        }
                    }
                }
            }
        }
        ++sample.scatterings;
        throughput *= rho;
        if (sample.scatterings >= 3) {
            if (bsdf_words.z >= 0xc0000000u) {
                break;
            }
            throughput /= 0.75f;
        }
        const float3 wi =
            hemisphere(n, sample_uniform(p, bsdf_words.x), sample_uniform(p, bsdf_words.y));
        const float cosine = dot(n, wi);
        if (!(cosine > 0.0f)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        escape_weight = bsdf_weight(cosine);
        previous_bsdf_pdf = cosine / pi;
        previous_area_reference = area_reference;
        const float3 origin =
            directional_offset ? spawn_toward(incoming, hit.distance, point, wi) : area_reference;
        if (!all(isfinite(origin))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        incoming = ray(origin, wi);
    }

    if (sample.status == wire::success && (!isfinite(sample.radiance) || sample.radiance < 0.0f)) {
        sample.status = wire::invalid_arithmetic;
    }
    output[lane] = sample;
}
