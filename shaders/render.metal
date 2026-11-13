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

static float triangle_offset(float u, float radius) {
    // Invert the normalized tent CDF without subtracting nearly equal values
    // around its center. The existing camera uniform grid includes u = 0.
    const float s = 2.0f * u - 1.0f;
    return radius * precise::divide(s, 1.0f + precise::sqrt(1.0f - abs(s)));
}

static ray triangle_primary_ray(constant wire::Parameters &p, thread const wire::Sample &sample) {
    const float ox = triangle_offset(u01(sample.random_words[0]), p.filter_x_radius);
    const float oy = triangle_offset(u01(sample.random_words[1]), p.filter_y_radius);
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
    float area;
    float world_extent;
};

enum class PointSource { native_hit, area_sample };

static bool surface_point(float2 uv, const device wire::Instance &instance, uint primitive,
                          const device wire::Point *positions,
                          const device wire::Triangle *triangles, const device wire::Point *normals,
                          uint normal_count, PointSource source, thread SurfacePoint &point) {
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
    point.area = 0.5f * normal_length;
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

static float3 spawn_origin(ray incoming, float t, SurfacePoint point, float3 n) {
    const float3 ray_position = fma(float3(t), incoming.direction, incoming.origin);
    // The ray evaluation can cancel near the origin. Bound its rounding error
    // from the magnitudes of the operands before cancellation.
    const float3 ray_error =
        gamma_bound(2.0f) * (abs(t * incoming.direction) + abs(incoming.origin));
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
                                const device wire::AreaEmission *emissions [[buffer(9)]],
                                const device wire::Emitter *emitters [[buffer(10)]],
                                const device wire::Point *normals [[buffer(11)]],
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
    float previous_bsdf_pdf = 0.0f;
    float3 previous_area_reference = incoming.origin;
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
        SurfacePoint point;
        const bool emissive = instance.emission != wire::no_emission;
        if (emissive) {
            if (instance.emission >= p.emission_count || p.emitter_count == 0u) {
                sample.status = wire::invalid_hit;
                break;
            }
            if (!surface_point(hit.triangle_barycentric_coord, instance, hit.primitive_id,
                               positions, triangles, normals, p.normal_count,
                               PointSource::native_hit, point)) {
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
                    weight = area_weights(point.area, light_cosine, previous_bsdf_pdf,
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
        if (!emissive &&
            !surface_point(hit.triangle_barycentric_coord, instance, hit.primitive_id, positions,
                           triangles, normals, p.normal_count, PointSource::native_hit, point)) {
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
        const float3 area_reference =
            authored_normals ? point.position : spawn_origin(incoming, hit.distance, point, n);
        if (!all(isfinite(area_reference))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const uint4 base(sample.pixel, sample.sample_low, sample.sample_high, 0);
        const uint2 key(p.seed_low, p.seed_high);
        const uint4 light_words = philox(base + uint4(0, 0, 0, 2 + 2 * bounce), key);
        const float3 light_direction = sphere(u01(light_words.x), u01(light_words.y));
        const float light_cosine = dot(n, light_direction);
        if (E > 0.0f && light_cosine > 0.0f) {
            const float3 origin = authored_normals
                                      ? spawn_toward(incoming, hit.distance, point, light_direction)
                                      : area_reference;
            if (!all(isfinite(origin))) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            if (shadow.intersect(ray(origin, light_direction), scene, all_visible).type ==
                intersection_type::none) {
                sample.radiance +=
                    throughput * rho * (4.0f * light_cosine) * light_weight(light_cosine) * E;
            }
        }

        const uint4 bsdf_words = philox(base + uint4(0, 0, 0, 1 + 2 * bounce), key);
        // A null slot intentionally contributes zero. This gives every emitter
        // an exact dyadic PMF and avoids altering the BSDF and sky streams.
        const uint selected = bsdf_words.w & (p.emitter_slots - 1u);
        if (selected < p.emitter_count) {
            const wire::Emitter emitter = emitters[selected];
            if (emitter.instance >= p.instance_count) {
                sample.status = wire::invalid_hit;
                break;
            }
            const device wire::Instance &light_instance = instances[emitter.instance];
            if (emitter.primitive >= light_instance.triangle_count ||
                light_instance.emission >= p.emission_count) {
                sample.status = wire::invalid_hit;
                break;
            }
            const device wire::AreaEmission &emission = emissions[light_instance.emission];
            const float Le = emission.radiance[dense_index(sample.wavelength)];
            // A coplanar connection has zero geometric emitter cosine.
            const bool same_triangle =
                emitter.instance == hit.instance_id && emitter.primitive == hit.primitive_id;
            if (Le > 0.0f && !same_triangle) {
                SurfacePoint target;
                if (!surface_point(triangle_uv(light_words.z, light_words.w), light_instance,
                                   emitter.primitive, positions, triangles, normals, p.normal_count,
                                   PointSource::area_sample, target)) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                const float3 delta = target.position - area_reference;
                const float distance_squared = dot(delta, delta);
                if (!(distance_squared >= FLT_MIN) || !isfinite(distance_squared)) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                const float3 direction = delta / sqrt(distance_squared);
                const float receiver_cosine = dot(n, direction);
                const float emitter_cosine = dot(target.geometric_normal, -direction);
                if (receiver_cosine > 0.0f && emitter_cosine != 0.0f &&
                    (emission.two_sided != 0u || emitter_cosine > 0.0f)) {
                    const float factor =
                        area_weights(target.area, abs(emitter_cosine), receiver_cosine / pi,
                                     distance_squared, p.emitter_slots)
                            .direct_factor;
                    const float3 origin =
                        authored_normals ? spawn_toward(incoming, hit.distance, point, direction)
                                         : area_reference;
                    const float3 shadow_delta = authored_normals ? target.position - origin : delta;
                    const float endpoint_cosine = authored_normals
                                                      ? dot(target.geometric_normal, -shadow_delta)
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
                    if (shadow.intersect(connection, scene, all_visible).type ==
                        intersection_type::none) {
                        sample.radiance += (throughput * rho) * (factor * Le);
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
        const float3 wi = hemisphere(n, u01(bsdf_words.x), u01(bsdf_words.y));
        const float cosine = dot(n, wi);
        if (!(cosine > 0.0f)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        escape_weight = bsdf_weight(cosine);
        previous_bsdf_pdf = cosine / pi;
        previous_area_reference = area_reference;
        const float3 origin =
            authored_normals ? spawn_toward(incoming, hit.distance, point, wi) : area_reference;
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
