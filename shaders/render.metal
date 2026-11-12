// SPDX-License-Identifier: GPL-3.0-only

#include <metal_raytracing>
#include <metal_stdlib>

#include "../src/render_wire.h"

using namespace metal;
using namespace metal::raytracing;
namespace wire = rypath::render_wire;

constant float pi = 3.14159265358979323846f;
constant uint all_visible = 0xffu;

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

static wire::Sample begin_sample(constant wire::Parameters &p, uint lane) {
    const uint pixel = p.pixel_begin + lane % p.pixel_count;
    const uint sample_low = p.sample_low + lane / p.pixel_count;
    const uint sample_high = p.sample_high + uint(sample_low < p.sample_low);
    const uint4 words =
        philox(uint4(pixel, sample_low, sample_high, 0), uint2(p.seed_low, p.seed_high));
    return wire::Sample{wire::success,
                        pixel,
                        sample_low,
                        sample_high,
                        360.0f + 470.0f * u01(words.z),
                        0.0f,
                        0,
                        {words.x, words.y, words.z, words.w}};
}

static ray primary_ray(constant wire::Parameters &p, thread const wire::Sample &sample) {
    const float x = (float(sample.pixel % p.width) + u01(sample.random_words[0])) / float(p.width);
    const float y = (float(sample.pixel / p.width) + u01(sample.random_words[1])) / float(p.height);
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

static float sky_radiance(const device float *environment, constant wire::Parameters &p,
                          float wavelength) {
    if (p.model == wire::demo_model) {
        return 1.0f;
    }
    const uint nm = uint(wavelength);
    const uint nearest = nm + uint(wavelength - float(nm) >= 0.5f);
    return environment[nearest - 360u];
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

static bool spawn_point(ray incoming, float t, float2 uv, float3 p0, float3 p1, float3 p2,
                        const device wire::Instance &instance, thread float3 &n,
                        thread float3 &out) {
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
    n = cross_edges / sqrt(normal_squared);
    if (instance.reverse_orientation != 0) {
        n = -n;
    }
    if (dot(n, incoming.direction) > 0.0f) {
        n = -n;
    }
    const float3 local = p0 + (uv.x * e1 + uv.y * e2);
    const float3 world = float3(dot(ax, local), dot(ay, local), dot(az, local)) + shift;
    // The barycentric allowance comes from the bounded geometry fixtures. Metal
    // does not specify a universal native-traversal error bound.
    constexpr float barycentric_allowance = 64.0f * 0x1.0p-23f;
    const float3 extent = max(abs(e1), max(abs(e2), abs(e2 - e1)));
    const float max_extent = max(extent.x, max(extent.y, extent.z));
    // Allow for native intersection arithmetic across axes, even for a plane
    // whose vertices have an exact zero along the normal. This extent-scaled
    // allowance is validated on the bounded fixtures.
    const float traversal_error = gamma_bound(7.0f) * max_extent;
    const float3 local_error = gamma_bound(7.0f) * (abs(p0) + abs(uv.x * e1) + abs(uv.y * e2)) +
                               barycentric_allowance * (abs(e1) + abs(e2)) + traversal_error;
    const float3 error_input = local_error + gamma_bound(4.0f) * abs(local);
    const float3 world_error =
        float3(dot(abs(ax), error_input), dot(abs(ay), error_input), dot(abs(az), error_input)) +
        gamma_bound(4.0f) * abs(shift);
    const float3 ray_position = fma(float3(t), incoming.direction, incoming.origin);
    // The ray evaluation can cancel near the origin. Bound its rounding error
    // from the magnitudes of the operands before cancellation.
    const float3 ray_error =
        gamma_bound(2.0f) * (abs(t * incoming.direction) + abs(incoming.origin));
    const float3 error = world_error + abs(world - ray_position) + ray_error;
    if (!all(isfinite(world)) || !all(isfinite(error)) || !all(isfinite(n))) {
        return false;
    }
    out = round_out(world + dot(abs(n), error) * n, n);
    return all(isfinite(out));
}

kernel void rypath_render_sky(constant wire::Parameters &p [[buffer(0)]],
                              device wire::Sample *output [[buffer(1)]],
                              const device float *environment [[buffer(7)]],
                              uint lane [[thread_position_in_grid]]) {
    if (lane >= p.lane_count) {
        return;
    }
    wire::Sample sample = begin_sample(p, lane);
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
                                uint lane [[thread_position_in_grid]]) {
    if (lane >= p.lane_count) {
        return;
    }
    wire::Sample sample = begin_sample(p, lane);
    ray incoming = primary_ray(p, sample);
    intersector<triangle_data, instancing> closest;
    closest.assume_geometry_type(geometry_type::triangle);
    closest.set_triangle_cull_mode(triangle_cull_mode::none);
    intersector<instancing> shadow;
    shadow.assume_geometry_type(geometry_type::triangle);
    shadow.set_triangle_cull_mode(triangle_cull_mode::none);
    shadow.accept_any_intersection(true);
    float throughput = 1.0f;
    float escape_weight = 1.0f;
    const float E = sky_radiance(environment, p, sample.wavelength);

    while (true) {
        if (!all(isfinite(incoming.direction)) || !isfinite(throughput) ||
            !isfinite(sample.radiance)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const auto hit = closest.intersect(incoming, scene, all_visible);
        if (hit.type == intersection_type::none) {
            sample.radiance += throughput * escape_weight * E;
            break;
        }
        const uint bounce = sample.scatterings;
        if (hit.type != intersection_type::triangle || hit.instance_id >= p.instance_count ||
            hit.geometry_id != 0 || !isfinite(hit.distance) ||
            !all(isfinite(hit.triangle_barycentric_coord))) {
            sample.status = wire::invalid_hit;
            break;
        }

        const device wire::Instance &instance = instances[hit.instance_id];
        if (hit.primitive_id >= instance.triangle_count || instance.material >= p.material_count) {
            sample.status = wire::invalid_hit;
            break;
        }
        // A miss can still contribute at the authored depth. A valid opaque
        // hit at that depth contributes no further direct or indirect light.
        if (p.model == wire::pbrt_model && bounce >= p.max_depth) {
            break;
        }
        const device wire::Material &material = materials[instance.material];
        float rho;
        if (p.model == wire::pbrt_model) {
            if (material.knot_count == 0 || material.knot_begin > p.knot_count ||
                material.knot_count > p.knot_count - material.knot_begin) {
                sample.status = wire::invalid_hit;
                break;
            }
            rho = pbrt_reflectance(knots + material.knot_begin, material.knot_count,
                                   sample.wavelength);
        } else {
            rho = demo_reflectance(material, p, sample.wavelength);
        }
        if (!(rho >= 0.0f && rho <= 1.0f)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        if (rho == 0.0f) {
            break;
        }
        if (bounce >= p.emergency_limit) {
            sample.status = wire::emergency_depth;
            break;
        }
        const wire::Triangle triangle = triangles[instance.triangle_begin + hit.primitive_id];
        const float3 p0 = unpack(positions[instance.vertex_begin + triangle.vertices[0]]);
        const float3 p1 = unpack(positions[instance.vertex_begin + triangle.vertices[1]]);
        const float3 p2 = unpack(positions[instance.vertex_begin + triangle.vertices[2]]);
        float3 n;
        float3 origin;
        if (!spawn_point(incoming, hit.distance, hit.triangle_barycentric_coord, p0, p1, p2,
                         instance, n, origin)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const uint4 base(sample.pixel, sample.sample_low, sample.sample_high, 0);
        const uint2 key(p.seed_low, p.seed_high);
        const uint4 light_words = philox(base + uint4(0, 0, 0, 2 + 2 * bounce), key);
        const float3 light_direction = sphere(u01(light_words.x), u01(light_words.y));
        const float light_cosine = dot(n, light_direction);
        if (E > 0.0f && light_cosine > 0.0f &&
            shadow.intersect(ray(origin, light_direction), scene, all_visible).type ==
                intersection_type::none) {
            sample.radiance +=
                throughput * rho * (4.0f * light_cosine) * light_weight(light_cosine) * E;
        }

        const uint4 bsdf_words = philox(base + uint4(0, 0, 0, 1 + 2 * bounce), key);
        ++sample.scatterings;
        throughput *= rho;
        if (sample.scatterings >= 3) {
            if (bsdf_words.z >= 0xc0000000u) {
                break;
            }
            throughput /= 0.75f;
        }
        const float3 wi = hemisphere(n, u01(bsdf_words.x), u01(bsdf_words.y));
        const float cosine = dot(n, wi);
        if (!(cosine > 0.0f)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        escape_weight = bsdf_weight(cosine);
        incoming = ray(origin, wi);
    }

    if (sample.status == wire::success && (!isfinite(sample.radiance) || sample.radiance < 0.0f)) {
        sample.status = wire::invalid_arithmetic;
    }
    output[lane] = sample;
}
