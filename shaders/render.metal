// SPDX-License-Identifier: GPL-3.0-only

#include <metal_raytracing>
#include <metal_stdlib>

#include "../src/render_wire.h"
#include "../src/sobol_probe_wire.h"
#include "../src/sobol_shared.h"
#include "alpha_hash.h"
#include "conductor.h"
#include "image_lighting.h"
#include "image_texture.h"
#include "intersection.h"
#include "masked_intersection.h"
#include "surface_detail.h"
#include "texture_footprint.h"
#include "texture_graph.h"

using namespace metal;
using namespace metal::raytracing;
namespace wire = rypath::render_wire;
namespace sobol = rypath::sobol;

constant float pi = 3.14159265358979323846f;
constant uint all_visible = 0xffu;
// Scene specialization keeps graph stacks and manual traversal out of scenes
// that do not use those features, including the original diffuse baseline.
constant bool graph_enabled [[function_constant(0)]];
constant bool alpha_enabled [[function_constant(1)]];
constant bool surface_detail_enabled [[function_constant(2)]];
constant bool environment_image_enabled [[function_constant(3)]];
constant bool area_image_enabled [[function_constant(4)]];
constant bool conductor_enabled [[function_constant(5)]];

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

static float blackbody_reflectance(float exponent_factor, float wavelength) {
    if (!(wavelength >= 360.0f && wavelength <= 830.0f)) {
        return 0.0f;
    }
    if (!(exponent_factor > 0.0f && isfinite(exponent_factor))) {
        return NAN;
    }
    const float x = exponent_factor / wavelength;
    // The host caps C2/T at 128*830. At x >= 128 the normalized result is
    // below half the smallest binary32 subnormal throughout the visible range.
    if (x >= 128.0f) {
        return 0.0f;
    }
    // C2 / (2.8977721e6 nm K), using pinned PBRT's h, c, k and Wien constant.
    constexpr float x_peak = 4.965114268247028f;
    constexpr float log_peak_factor = -0.007001607544495994f; // log(1-exp(-x_peak))
    // Metal has no expm1. This factored series avoids subtracting nearly equal
    // values for hot spectra; its first omitted relative term is x^5/720.
    const float factor =
        x < 0.0625f
            ? x * (1.0f + x * (-0.5f + x * (1.0f / 6.0f + x * (-1.0f / 24.0f + x / 120.0f))))
            : 1.0f - exp(-x);
    // Combine exponents before exp: an intermediate exp(x_peak-x) can flush
    // even when the final normalized reflectance is a normal binary32 value.
    const float log_rho = 5.0f * log(x / x_peak) + x_peak - x + log_peak_factor - log(factor);
    return clamp(exp(log_rho), 0.0f, 1.0f);
}

kernel void rypath_probe_blackbody(const device float2 *queries [[buffer(0)]],
                                   device float *results [[buffer(1)]],
                                   constant uint &count [[buffer(2)]],
                                   uint lane [[thread_position_in_grid]]) {
    if (lane < count) {
        results[lane] = blackbody_reflectance(queries[lane].x, queries[lane].y);
    }
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

namespace graph_wire = rypath::texture_graph_wire;

static wire::Point pack_point(float3 p) { return {p.x, p.y, p.z}; }

struct GraphImageSampler {
    const device wire::ImageTexture *textures;
    const device wire::TextureLevel *levels;
    const device wire::Word *texels;
    const device float *transfer;
    const device float *spectra;
    constant wire::TextureParameters *parameters;
    bool operator()(uint index, bool spectrum, graph_wire::Mapped2D mapped, float wavelength,
                    thread float &value) const {
        if (index >= parameters->texture_count)
            return false;
        const device auto &texture = textures[index];
        const float2 st(mapped.s, mapped.t), dx(mapped.dsdx, mapped.dtdx),
            dy(mapped.dsdy, mapped.dtdy);
        if (spectrum) {
            float3 rgb = rypath_image::lookup(texture, levels, texels, transfer, *parameters, st,
                                              dx, dy, true);
            rgb *= texture.scale;
            if (texture.invert != 0u)
                rgb = 1.0f - rgb;
            value = rypath_image::spectrum(rgb, wavelength, spectra);
        } else {
            value = rypath_image::lookup_scalar(texture, levels, texels, transfer, *parameters, st,
                                                dx, dy, true);
            if (!isfinite(value))
                return false;
            value *= texture.scale;
            if (!isfinite(value))
                return false;
            if (texture.invert != 0u)
                value = max(0.0f, 1.0f - value);
        }
        return isfinite(value);
    }
};
struct GraphRgbSampler {
    const device float *table;
    bool operator()(float r, float g, float b, float wavelength, thread float &value) const {
        value = rypath_image::spectrum(float3(r, g, b), wavelength, table);
        return isfinite(value);
    }
};
struct GraphResources {
    const device graph_wire::Node *nodes;
    const device wire::SpectralKnot *knots;
    constant wire::TextureGraphParameters *parameters;
    GraphImageSampler images;
    // Share the bounded evaluator across shading, masks, and bump differences.
    // Inlining its stack at every query also overwhelms instrumented compilation.
    __attribute__((noinline)) bool evaluate(uint root, graph_wire::Context context,
                                            float wavelength, thread float &value) const {
        if (!graph_enabled)
            return false;
        return rypath::texture_graph::evaluate(nodes, parameters->node_count, knots,
                                               parameters->knot_count, root, context, wavelength,
                                               images, GraphRgbSampler{images.spectra}, value);
    }
};

static bool
surface_texture_context(const device wire::Instance &instance, uint primitive, uint kind,
                        float2 hit_uv, thread const SurfacePoint &point,
                        const device wire::Point *positions, const device wire::Triangle *triangles,
                        const device wire::Patch *patches, const device wire::Point2 *uvs,
                        const device wire::TextureFrame *frames,
                        constant wire::TextureParameters &tp, constant wire::Parameters &p,
                        thread const wire::Sample &sample, float3 primary_direction,
                        bool footprints, thread graph_wire::Context &context,
                        thread float3 &geometric_dpdu, thread float3 &geometric_dpdv) {
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
            return false;
        if (!rypath_texture_triangle_uv(float3(1.0f - (hit_uv.x + hit_uv.y), hit_uv.x, hit_uv.y), a,
                                        b, c, st) ||
            instance.frame_begin >= tp.frame_count ||
            primitive >= tp.frame_count - instance.frame_begin)
            return false;
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
                return false;
        }
        const float3 du = texture_world_vector(instance, mix(v[1] - v[0], v[3] - v[2], hit_uv.y));
        const float3 dv = texture_world_vector(instance, mix(v[2] - v[0], v[3] - v[1], hit_uv.x));
        local_p = mix(mix(v[0], v[1], hit_uv.x), mix(v[2], v[3], hit_uv.x), hit_uv.y);
        if (!rypath_texture_patch_frame(hit_uv, du, dv, instance.uv_begin != 0xffffffffu, uv[0],
                                        uv[1], uv[2], uv[3], st, dpdu, dpdv))
            return false;
    }
    float3 dpdx(0), dpdy(0);
    // Rebase the affine translation before adding local coordinates. Subtracting
    // the camera from an already-rounded world point loses small surface offsets.
    const float3 relative_p =
        texture_world_vector(instance, local_p) +
        (float3(instance.matrix[3], instance.matrix[7], instance.matrix[11]) - unpack(p.origin));
    if (!all(isfinite(relative_p)))
        return false;
    bool primary = false;
    if (footprints && sample.scatterings == 0u) {
        float3 rx, ry;
        texture_camera_rays(p, sample, rx, ry);
        if (!all(isfinite(rx)) || !all(isfinite(ry)))
            return false;
        primary = rypath_texture_primary_dp(relative_p, point.geometric_normal, primary_direction,
                                            rx, ry, tp.spp_scale, dpdx, dpdy);
    }
    if (footprints && !primary) {
        rypath_texture_approximate_dp(relative_p, point.geometric_normal,
                                      texture_matrix(tp.world_from_camera),
                                      texture_matrix(tp.camera_from_world), unpack(tp.minimum_dx),
                                      unpack(tp.minimum_dy), tp.spp_scale, dpdx, dpdy);
    }
    float2 dstdx(0), dstdy(0);
    if (footprints && !rypath_texture_uv_derivatives(dpdu, dpdv, dpdx, dpdy, dstdx, dstdy))
        return false;
    context = {pack_point(relative_p),
               pack_point(point.geometric_normal),
               pack_point(dpdx),
               pack_point(dpdy),
               st.x,
               st.y,
               dstdx.x,
               dstdx.y,
               dstdy.x,
               dstdy.y};
    geometric_dpdu = dpdu;
    geometric_dpdv = dpdv;
    return true;
}

struct SurfaceAlpha {
    const device wire::Instance *instances;
    const device wire::Point *positions;
    const device wire::Triangle *triangles;
    const device wire::Patch *patches;
    const device wire::Point *normals;
    const device wire::Point2 *uvs;
    const device wire::TextureFrame *frames;
    const device wire::SurfaceVertex *vertices;
    const device wire::SurfaceTransform *transforms;
    constant wire::Parameters *parameters;
    thread const wire::Sample *sample;
    GraphResources graph;
    ray incoming;

    MaskedAlphaResult operator()(uint ordinal, uint geometry, uint primitive,
                                 intersection_type type, float2 uv, float t) const {
        if (ordinal >= parameters->instance_count || geometry != 0 || !isfinite(t))
            return {false, false};
        const device auto &instance = instances[ordinal];
        const bool patch = type == intersection_type::bounding_box;
        const uint kind = patch ? wire::patch_primitive : wire::triangle_primitive;
        if (primitive >= (patch ? instance.patch_count : instance.triangle_count))
            return {false, false};
        if (instance.alpha_root == 0xffffffffu)
            return {true, true};
        SurfacePoint point;
        if (!surface_point(uv, kind, instance, primitive, positions, triangles, patches, normals,
                           parameters->normal_count, PointSource::native_hit, point))
            return {false, false};
        graph_wire::Context context{};
        float3 dpdu, dpdv;
        if (!surface_texture_context(instance, primitive, kind, uv, point, positions, triangles,
                                     patches, uvs, frames, *graph.images.parameters, *parameters,
                                     *sample, incoming.direction, false, context, dpdu, dpdv))
            return {false, false};
        // PBRT tests shape alpha before ObjectInstance transforms the accepted
        // interaction. Reconstruct that definition-frame point directly to
        // preserve small coordinates under large placement translations.
        const device auto &xf = transforms[ordinal];
        if (xf.vertex_begin > graph.parameters->surface_vertex_count ||
            xf.vertex_count > graph.parameters->surface_vertex_count - xf.vertex_begin)
            return {false, false};
        float3 local[4];
        for (uint i = 0; i < (patch ? 4u : 3u); ++i) {
            const uint vertex_index =
                patch ? patches[instance.patch_begin + primitive].vertices[i]
                      : triangles[instance.triangle_begin + primitive].vertices[i];
            if (vertex_index >= xf.vertex_count)
                return {false, false};
            local[i] = unpack(vertices[xf.vertex_begin + vertex_index].position);
        }
        const float3 local_p =
            patch ? mix(mix(local[0], local[1], uv.x), mix(local[2], local[3], uv.x), uv.y)
                  : fma(float3(uv.y), local[2] - local[0],
                        fma(float3(uv.x), local[1] - local[0], local[0]));
        const float3 definition =
            rypath_surface_detail::transform(xf.shape, local_p) + unpack(xf.definition_translation);
        const float3 n = unpack(context.n);
        float3 definition_n(dot(float3(xf.placement[0], xf.placement[3], xf.placement[6]), n),
                            dot(float3(xf.placement[1], xf.placement[4], xf.placement[7]), n),
                            dot(float3(xf.placement[2], xf.placement[5], xf.placement[8]), n));
        if (!all(isfinite(definition)) ||
            !rypath_surface_detail::unit_vector(definition_n, definition_n))
            return {false, false};
        context.p = pack_point(definition);
        context.n = pack_point(definition_n);
        float alpha;
        if (!graph.evaluate(instance.alpha_root, context, sample->wavelength, alpha))
            return {false, false};
        return {alpha >= 1 ||
                    (alpha > 0 && alpha_uniform(incoming.origin, incoming.direction, t) <= alpha),
                true};
    }
};

static SurfaceHit visible_shadow(thread MixedIntersector &trace, ray incoming,
                                 instance_acceleration_structure scene,
                                 MixedIntersectionTable table, thread SurfaceAlpha &alpha) {
    if (alpha_enabled) {
        alpha.incoming = incoming;
        return trace_masked_surface(incoming, scene, all_visible, alpha, true);
    }
    const auto hit = trace_shadow(trace, incoming, scene, all_visible, table);
    return {hit.type, 0, 0, 0, 0, {0, 0}, true};
}

struct BumpEvaluator {
    GraphResources graph;
    uint root;
    float wavelength;
    bool operator()(graph_wire::Context context, thread float &value) const {
        return graph.evaluate(root, context, wavelength, value);
    }
};

static bool apply_surface_detail(
    uint ordinal, const device wire::Instance &instance, uint primitive, uint kind, float2 uv,
    const device wire::Triangle *triangles, const device wire::Patch *patches,
    const device wire::Point2 *texture_uvs, const device wire::SurfaceVertex *vertices,
    const device wire::SurfaceTransform *transforms, const device wire::Material &material,
    GraphResources graph, graph_wire::Context context, float wavelength, thread SurfacePoint &point,
    thread rypath_surface_detail::Frame &frame, bool retain_frame) {
    const device auto &xf = transforms[ordinal];
    if (xf.vertex_begin > graph.parameters->surface_vertex_count ||
        xf.vertex_count > graph.parameters->surface_vertex_count - xf.vertex_begin)
        return false;
    const bool is_patch = kind == wire::patch_primitive;
    const uint count = is_patch ? 4u : 3u;
    uint indices[4];
    float2 st[4] = {float2(0, 0), float2(1, 0), is_patch ? float2(0, 1) : float2(1, 1),
                    float2(1, 1)};
    for (uint i = 0; i < count; ++i) {
        indices[i] = is_patch ? patches[instance.patch_begin + primitive].vertices[i]
                              : triangles[instance.triangle_begin + primitive].vertices[i];
        if (instance.uv_begin != 0xffffffffu &&
            !texture_vertex_uv(texture_uvs, instance.uv_begin, indices[i],
                               graph.images.parameters->uv_count, st[i]))
            return false;
    }
    if (!(is_patch ? rypath_surface_detail::patch(vertices, xf, indices, uv, st, frame)
                   : rypath_surface_detail::triangle(vertices, xf, indices, uv, st, frame)))
        return false;
    point.geometric_normal = frame.geometric_normal;
    if (material.normal_map != 0xffffffffu) {
        if (material.normal_map >= graph.images.parameters->texture_count)
            return false;
        const float3 rgb = rypath_image::lookup_base(
            graph.images.textures[material.normal_map], graph.images.levels, graph.images.texels,
            graph.images.transfer, *graph.images.parameters, float2(context.u, 1.0f - context.v));
        return rypath_surface_detail::normal_map(frame, rgb, point.geometric_normal,
                                                 point.shading_normal,
                                                 retain_frame ? &frame : nullptr);
    }
    if (material.bump_root != 0xffffffffu)
        return rypath_surface_detail::bump_map(frame, context, point.geometric_normal,
                                               BumpEvaluator{graph, material.bump_root, wavelength},
                                               point.shading_normal,
                                               retain_frame ? &frame : nullptr);
    point.shading_normal = frame.n;
    return true;
}

// Only certified binary alpha can reach an emitter. Constant-zero alpha keeps
// PBRT's special invisible light behavior; an all-zero image emits nothing.
static bool area_radiance(const device wire::AreaEmission &emission,
                          const device wire::Instance &instance, uint primitive, uint kind,
                          float2 uv, thread const SurfacePoint &point, SurfaceAlpha resources,
                          rypath_image_light::Images images, thread float &radiance) {
    radiance = emission.radiance[dense_index(resources.sample->wavelength)];
    const bool masked =
        alpha_enabled && instance.alpha_root != 0xffffffffu && instance.invisible_emitter == 0u;
    if (!masked && !(area_image_enabled && emission.image_texture != 0xffffffffu))
        return true;
    graph_wire::Context context{};
    float3 dpdu, dpdv;
    if (!surface_texture_context(instance, primitive, kind, uv, point, resources.positions,
                                 resources.triangles, resources.patches, resources.uvs,
                                 resources.frames, *resources.graph.images.parameters,
                                 *resources.parameters, *resources.sample,
                                 resources.incoming.direction, false, context, dpdu, dpdv))
        return false;
    if (masked) {
        float alpha;
        if (!resources.graph.evaluate(instance.alpha_root, context, resources.sample->wavelength,
                                      alpha) ||
            !(alpha == 0.0f || alpha == 1.0f))
            return false;
        if (alpha == 0.0f) {
            radiance = 0;
            return true;
        }
    }
    if (area_image_enabled && emission.image_texture != 0xffffffffu)
        radiance = rypath_image_light::area_radiance(
            images, emission.image_texture, emission.image_scale, float2(context.u, context.v),
            resources.sample->wavelength);
    return isfinite(radiance) && radiance >= 0;
}

static float power_weight(float a, float b) {
    const float scale = max(a, b);
    if (!(scale > 0) || !isfinite(scale))
        return NAN;
    a /= scale;
    b /= scale;
    return a * a / (a * a + b * b);
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

// Conductor spectra share the immutable knot buffer, with source-specific tails.
static float optical_value(const device wire::SpectralKnot *knots, uint begin, uint count,
                           uint total, bool extend, float wavelength) {
    if (count < 2 || begin > total || count > total - begin)
        return NAN;
    const device auto *curve = knots + begin;
    if (extend) {
        if (wavelength < curve[0].wavelength)
            return curve[0].value;
        if (wavelength > curve[count - 1].wavelength)
            return curve[count - 1].value;
    }
    return pbrt_reflectance(curve, count, wavelength);
}

struct ConductorFrame {
    float3 x, y, z;
    float3 local(float3 v) const { return float3(dot(x, v), dot(y, v), dot(z, v)); }
    float3 world(float3 v) const { return v.x * x + v.y * y + v.z * z; }
};

static bool conductor_frame(rypath_surface_detail::Frame frame, thread ConductorFrame &result) {
    if (!rypath_surface_detail::unit_vector(frame.n, result.z) ||
        !rypath_surface_detail::unit_vector(frame.dpdu, result.x))
        return false;
    const float projection = dot(result.x, result.z);
    if (!isfinite(projection) || abs(projection) > 64.0f * FLT_EPSILON)
        return false;
    if (!rypath_surface_detail::unit_vector(result.x - projection * result.z, result.x))
        return false;
    result.y = cross(result.z, result.x);
    return all(isfinite(result.y));
}

// Continuous light proposals. Zero f/PDF is a valid unsupported direction;
// arithmetic failure is distinct and is rejected with the whole sample batch.
static float conductor_direct(float f_cos, float bsdf_pdf, float light_pdf, bool invisible) {
    if (!(f_cos >= 0) || !(bsdf_pdf >= 0) || !(light_pdf > 0) || !isfinite(f_cos) ||
        !isfinite(bsdf_pdf) || !isfinite(light_pdf))
        return NAN;
    if (f_cos == 0)
        return 0;
    return (f_cos / light_pdf) * (invisible ? 1.0f : power_weight(light_pdf, bsdf_pdf));
}

static float area_light_pdf(float area, float light_cosine, float distance_squared, uint slots) {
    const float inverse_pdf = (float(slots) * area) * light_cosine;
    if (!(inverse_pdf > 0) || !isfinite(inverse_pdf) || !(distance_squared >= FLT_MIN) ||
        !isfinite(distance_squared))
        return NAN;
    return distance_squared / inverse_pdf;
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
                              const device wire::ImageTexture *image_textures [[buffer(17)]],
                              const device wire::TextureLevel *image_levels [[buffer(18)]],
                              const device wire::Word *image_texels [[buffer(19)]],
                              const device float *image_transfer [[buffer(20)]],
                              const device float *image_spectra [[buffer(21)]],
                              constant wire::TextureParameters &texture_p [[buffer(22)]],
                              constant wire::TextureGraphParameters &graph_p [[buffer(25)]],
                              const device float *illuminant [[buffer(29)]],
                              uint lane [[thread_position_in_grid]]) {
    if (lane >= p.lane_count) {
        return;
    }
    ulong index = 0;
    wire::Sample sample = begin_sample(p, tables, lane, index);
    if (p.model == wire::pbrt_model && !all(isfinite(primary_ray(p, sample).direction))) {
        sample.status = wire::invalid_arithmetic;
    }
    if (environment_image_enabled) {
        const rypath_image_light::Images images{image_textures, image_levels,  image_texels,
                                                image_transfer, image_spectra, illuminant,
                                                &texture_p};
        sample.radiance = rypath_image_light::environment_radiance(
            images, graph_p, primary_ray(p, sample).direction, sample.wavelength);
    } else {
        sample.radiance = sky_radiance(environment, p, sample.wavelength);
    }
    if (!isfinite(sample.radiance) || sample.radiance < 0)
        sample.status = wire::invalid_arithmetic;
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
                                const device graph_wire::Node *graph_nodes [[buffer(23)]],
                                const device wire::SpectralKnot *graph_knots [[buffer(24)]],
                                constant wire::TextureGraphParameters &graph_p [[buffer(25)]],
                                const device wire::SurfaceVertex *surface_vertices [[buffer(26)]],
                                const device wire::SurfaceTransform *surface_transforms
                                [[buffer(27)]],
                                const device float *environment_cdf [[buffer(28)]],
                                const device float *illuminant [[buffer(29)]],
                                uint lane [[thread_position_in_grid]]) {
    if (lane >= p.lane_count) {
        return;
    }
    ulong index = 0;
    wire::Sample sample = begin_sample(p, tables, lane, index);
    ray incoming = primary_ray(p, sample);
    const float3 primary_direction = incoming.direction;
    const GraphResources graph{
        graph_nodes,
        graph_knots,
        &graph_p,
        {image_textures, image_levels, image_texels, image_transfer, image_spectra, &texture_p}};
    SurfaceAlpha alpha{
        instances,      positions,        triangles,          patches, normals, texture_uvs,
        texture_frames, surface_vertices, surface_transforms, &p,      &sample, graph,
        incoming};
    const rypath_image_light::Images light_images{image_textures, image_levels,  image_texels,
                                                  image_transfer, image_spectra, illuminant,
                                                  &texture_p};
    MixedIntersector closest;
    closest.set_triangle_cull_mode(triangle_cull_mode::none);
    MixedIntersector shadow;
    shadow.set_triangle_cull_mode(triangle_cull_mode::none);
    shadow.accept_any_intersection(true);
    float throughput = 1.0f;
    float escape_weight = 1.0f;
    float previous_bsdf_pdf = 0.0f;
    bool previous_delta = false;
    bool previous_conductor = false;
    float3 previous_area_reference = incoming.origin;
    const float E = sky_radiance(environment, p, sample.wavelength);

    while (true) {
        if (!all(isfinite(incoming.direction)) || !isfinite(throughput) ||
            !isfinite(sample.radiance)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        alpha.incoming = incoming;
        const auto hit = alpha_enabled ? trace_masked_surface(incoming, scene, all_visible, alpha)
                                       : trace_surface(closest, incoming, scene, all_visible,
                                                       intersection_table);
        if (!hit.valid) {
            sample.status = wire::invalid_hit;
            break;
        }
        if (hit.type == intersection_type::none) {
            float Le = E;
            if (environment_image_enabled) {
                Le = rypath_image_light::environment_radiance(
                    light_images, graph_p, incoming.direction, sample.wavelength);
                if (sample.scatterings != 0u && !previous_delta) {
                    const float pdf = rypath_image_light::environment_pdf(graph_p, environment_cdf,
                                                                          incoming.direction);
                    escape_weight = power_weight(previous_bsdf_pdf, pdf);
                }
            }
            sample.radiance += throughput * escape_weight * Le;
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
            float Le;
            if (!area_radiance(emission, instance, hit.primitive_id, kind, hit_uv, point, alpha,
                               light_images, Le)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            if (Le > 0.0f && (emission.two_sided != 0u ||
                              dot(point.geometric_normal, -incoming.direction) >= 0.0f)) {
                float weight = 1.0f;
                if (bounce != 0u && !previous_delta) {
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
                        patch_hit && light_cosine == 0.0f ? 1.0f
                        : previous_conductor
                            ? power_weight(previous_bsdf_pdf,
                                           area_light_pdf(point.inverse_area_pdf, light_cosine,
                                                          distance_squared, p.emitter_slots))
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
        const bool conductor = conductor_enabled && material.scattering == 1u;
        const bool context_material = conductor || material.kind == wire::image_material ||
                                      material.kind == wire::graph_material ||
                                      material.bump_root != 0xffffffffu ||
                                      material.normal_map != 0xffffffffu;
        if (context_material && !emissive &&
            !surface_point(hit_uv, kind, instance, hit.primitive_id, positions, triangles, patches,
                           normals, p.normal_count, PointSource::native_hit, point)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        graph_wire::Context texture_context{};
        float3 geometric_dpdu(0), geometric_dpdv(0);
        if ((conductor || material.kind == wire::graph_material ||
             material.bump_root != 0xffffffffu || material.normal_map != 0xffffffffu) &&
            !surface_texture_context(instance, hit.primitive_id, kind, hit_uv, point, positions,
                                     triangles, patches, texture_uvs, texture_frames, texture_p, p,
                                     sample, primary_direction, true, texture_context,
                                     geometric_dpdu, geometric_dpdv)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        float rho = NAN;
        if (conductor) {
            rho = 1.0f;
        } else if (p.model == wire::demo_model && material.kind == wire::sampled_material) {
            rho = demo_reflectance(material, p, sample.wavelength);
        } else if (p.model == wire::pbrt_model && material.kind == wire::sampled_material &&
                   material.knot_count >= 2 && material.knot_begin <= p.knot_count &&
                   material.knot_count <= p.knot_count - material.knot_begin) {
            rho = pbrt_reflectance(knots + material.knot_begin, material.knot_count,
                                   sample.wavelength);
        } else if (p.model == wire::pbrt_model && material.kind == wire::constant_material) {
            rho = material.scalar_value;
        } else if (p.model == wire::pbrt_model && material.kind == wire::blackbody_material) {
            rho = blackbody_reflectance(material.scalar_value, sample.wavelength);
        } else if (p.model == wire::pbrt_model && material.kind == wire::rgb_sigmoid_material) {
            rho = sigmoid_reflectance(material, sample.wavelength);
        } else if (p.model == wire::pbrt_model && material.kind == wire::image_material &&
                   material.image_texture < texture_p.texture_count) {
            rho = image_reflectance(instance, hit.primitive_id, kind, hit_uv, point, positions,
                                    triangles, patches, texture_uvs, texture_frames,
                                    image_textures[material.image_texture], image_levels,
                                    image_texels, image_transfer, image_spectra, texture_p, p,
                                    sample, primary_direction);
        } else if (p.model == wire::pbrt_model && material.kind == wire::graph_material) {
            if (!graph.evaluate(material.texture_root, texture_context, sample.wavelength, rho)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            rho = clamp(rho, 0.0f, 1.0f);
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
        const bool detail = surface_detail_enabled && (material.bump_root != 0xffffffffu ||
                                                       material.normal_map != 0xffffffffu);
        rypath_surface_detail::Frame shading_frame;
        if ((detail || conductor) &&
            !apply_surface_detail(hit.instance_id, instance, hit.primitive_id, kind, hit_uv,
                                  triangles, patches, texture_uvs, surface_vertices,
                                  surface_transforms, material, graph, texture_context,
                                  sample.wavelength, point, shading_frame, conductor)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const bool authored_normals =
            instance.normal_begin != wire::no_normals || detail || conductor;
        if (!authored_normals && bounce >= p.emergency_limit) {
            sample.status = wire::emergency_depth;
            break;
        }
        if (!emissive && !context_material &&
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
        rypath_conductor::Parameters conductor_parameters{};
        ConductorFrame frame{};
        float3 wo(0);
        if (conductor) {
            float u = material.u_roughness, v = material.v_roughness;
            if ((material.u_roughness_root != 0xffffffffu &&
                 !graph.evaluate(material.u_roughness_root, texture_context, sample.wavelength,
                                 u)) ||
                (material.v_roughness_root != 0xffffffffu &&
                 !graph.evaluate(material.v_roughness_root, texture_context, sample.wavelength,
                                 v))) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            const float eta =
                optical_value(knots, material.eta_begin, material.eta_count, p.knot_count,
                              (material.optical_extensions & 1u) != 0u, sample.wavelength);
            const float k =
                optical_value(knots, material.k_begin, material.k_count, p.knot_count,
                              (material.optical_extensions & 2u) != 0u, sample.wavelength);
            if (!rypath_conductor::prepare(eta, k, u, v, material.remap_roughness != 0u,
                                           conductor_parameters) ||
                !conductor_frame(shading_frame, frame)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            wo = frame.local(-incoming.direction);
        }
        const bool delta = conductor && conductor_parameters.smooth;
        const bool directional_offset = authored_normals || patch_hit;
        const float3 area_reference =
            directional_offset ? point.position : spawn_origin(incoming, hit.distance, point, n);
        if (!all(isfinite(area_reference))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const uint4 light_words = draw_words(p, tables, sample, index, 2 + 2 * bounce);
        float3 light_direction;
        float light_pdf = 1.0f / (4.0f * pi);
        float light_radiance = E;
        const float2 light_u(sample_uniform(p, light_words.x), sample_uniform(p, light_words.y));
        if (environment_image_enabled) {
            if (!rypath_image_light::sample_environment(graph_p, environment_cdf, light_u,
                                                        light_direction, light_pdf)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            light_radiance = rypath_image_light::environment_radiance(
                light_images, graph_p, light_direction, sample.wavelength);
        } else {
            light_direction = sphere(light_u.x, light_u.y);
        }
        if (!isfinite(light_radiance) || light_radiance < 0) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const float light_cosine = dot(n, light_direction);
        if (!delta && light_radiance > 0.0f && light_cosine > 0.0f) {
            const float3 origin = directional_offset
                                      ? spawn_toward(incoming, hit.distance, point, light_direction)
                                      : area_reference;
            if (!all(isfinite(origin))) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            const auto visibility = visible_shadow(shadow, ray(origin, light_direction), scene,
                                                   intersection_table, alpha);
            if (!visibility.valid) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            if (visibility.type == intersection_type::none) {
                float factor;
                if (conductor) {
                    const auto evaluated = rypath_conductor::evaluate(conductor_parameters, wo,
                                                                      frame.local(light_direction));
                    factor = evaluated.valid ? conductor_direct(evaluated.f * light_cosine,
                                                                evaluated.pdf, light_pdf, false)
                                             : NAN;
                } else {
                    factor = environment_image_enabled
                                 ? (light_cosine / pi) / light_pdf *
                                       power_weight(light_pdf, light_cosine / pi)
                                 : (4.0f * light_cosine) * light_weight(light_cosine);
                }
                if (!isfinite(factor)) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                sample.radiance += throughput * rho * factor * light_radiance;
            }
        }

        const uint4 bsdf_words = draw_words(p, tables, sample, index, 1 + 2 * bounce);
        // A null slot intentionally contributes zero. This gives every emitter
        // an exact dyadic PMF and avoids altering the BSDF and sky streams.
        const uint selected =
            p.sampler_kind == wire::sobol_sampler
                ? (p.emitter_slots == 1 ? 0 : bsdf_words.w >> (clz(p.emitter_slots) + 1))
                : bsdf_words.w & (p.emitter_slots - 1u);
        if (!delta && selected < p.emitter_count) {
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
            float Le = emission.radiance[dense_index(sample.wavelength)];
            // A coplanar connection has zero geometric emitter cosine.
            const bool same_triangle = !patch_hit && !patch_light &&
                                       emitter.instance == hit.instance_id &&
                                       emitter.primitive == hit.primitive_id;
            if ((Le > 0.0f || (area_image_enabled && emission.image_texture != 0xffffffffu)) &&
                !same_triangle) {
                SurfacePoint target;
                const float2 light_uv = patch_light ? patch_sample_uv(light_words.z, light_words.w)
                                                    : triangle_uv(light_words.z, light_words.w);
                if (!surface_point(light_uv, emitter.kind, light_instance, emitter.primitive,
                                   positions, triangles, patches, normals, p.normal_count,
                                   PointSource::area_sample, target)) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                if (!area_radiance(emission, light_instance, emitter.primitive, emitter.kind,
                                   light_uv, target, alpha, light_images, Le)) {
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
                    if (Le > 0.0f && receiver_cosine > 0.0f && emitter_cosine != 0.0f &&
                        (emission.two_sided != 0u || emitter_cosine > 0.0f)) {
                        float factor;
                        if (conductor) {
                            const auto evaluated = rypath_conductor::evaluate(
                                conductor_parameters, wo, frame.local(direction));
                            factor = evaluated.valid
                                         ? conductor_direct(
                                               evaluated.f * receiver_cosine, evaluated.pdf,
                                               area_light_pdf(target.inverse_area_pdf,
                                                              abs(emitter_cosine), distance_squared,
                                                              p.emitter_slots),
                                               light_instance.invisible_emitter != 0u)
                                         : NAN;
                        } else {
                            factor = light_instance.invisible_emitter != 0u
                                         ? (float(p.emitter_slots) * target.inverse_area_pdf) *
                                               abs(emitter_cosine) * (receiver_cosine / pi) /
                                               distance_squared
                                         : area_weights(target.inverse_area_pdf,
                                                        abs(emitter_cosine), receiver_cosine / pi,
                                                        distance_squared, p.emitter_slots)
                                               .direct_factor;
                        }
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
                        const auto visibility =
                            visible_shadow(shadow, connection, scene, intersection_table, alpha);
                        if (!visibility.valid) {
                            sample.status = wire::invalid_arithmetic;
                            break;
                        }
                        if (visibility.type == intersection_type::none) {
                            sample.radiance += (throughput * rho) * (factor * Le);
                        }
                    }
                }
            }
        }
        float3 wi;
        float continuation_pdf = 0;
        if (conductor) {
            const auto continuation = rypath_conductor::sample(
                conductor_parameters, wo,
                float2(sample_uniform(p, bsdf_words.x), sample_uniform(p, bsdf_words.y)));
            if (!continuation.valid) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            if (!continuation.accepted)
                break;
            wi = frame.world(continuation.wi);
            if (!rypath_surface_detail::unit_vector(wi, wi)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            throughput *= continuation.weight;
            continuation_pdf = continuation.pdf;
        } else {
            wi = hemisphere(n, sample_uniform(p, bsdf_words.x), sample_uniform(p, bsdf_words.y));
            throughput *= rho;
        }
        ++sample.scatterings;
        if (sample.scatterings >= 3) {
            if (bsdf_words.z >= 0xc0000000u) {
                break;
            }
            throughput /= 0.75f;
        }
        const float cosine = dot(n, wi);
        if (!(cosine > 0.0f)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        previous_bsdf_pdf = conductor ? continuation_pdf : cosine / pi;
        previous_delta = delta;
        previous_conductor = conductor;
        escape_weight = delta       ? 1.0f
                        : conductor ? power_weight(previous_bsdf_pdf, 1.0f / (4.0f * pi))
                                    : bsdf_weight(cosine);
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
