// SPDX-License-Identifier: Apache-2.0
// Bidirectional path construction and balance MIS adapted from pbrt-v4.
// Copyright (c) 1998–2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt. Packet scheduling and storage are rypath adaptations.
#pragma once
#include "../src/packet_spectrum_shared.h"
#include "../src/packet_wire.h"
#include "packet_scattering.h"

namespace rypath_bdpt {
namespace storage = rypath::packet_wire;
using Wavelengths = rypath_packet_spectrum::Wavelengths;
using Mode = rypath_packet::TransportMode;
enum : uint { camera_vertex, surface_vertex, light_vertex, environment_vertex };
struct Vertex {
    SurfacePoint point;
    ScatteringFrame frame;
    rypath_packet::Prepared bsdf;
    float4 beta;
    float3 wo;
    float2 uv;
    float pdf_fwd, pdf_rev;
    uint type, instance, primitive, kind, light, seed;
    uint path_id, sampled_prev_id, sampled_next_id;
    bool pair_valid;
    float3 sampled_wo_local, sampled_wi_local;
    bool delta, connectible, delta_light;
};
static_assert(sizeof(Vertex) <= storage::vertex_bytes && alignof(Vertex) <= 16,
              "Packet vertex exceeds checked opaque workspace slot");
struct Resources {
    SurfaceAlpha alpha;
    const device wire::SpectralKnot *knots;
    const device wire::AreaEmission *emissions;
    const device wire::Emitter *emitters;
    const device float *environment;
    const device float *environment_cdf;
    rypath_image_light::Images images;
    const device sobol::Tables *tables;
    instance_acceleration_structure scene;
    MixedIntersectionTable intersection_table;
};
inline device Vertex &path_vertex(device uchar *path, uint i) {
    return *reinterpret_cast<device Vertex *>(path + i * storage::vertex_bytes);
}
inline float remap_zero(float p) { return p == 0 ? 1 : p; }
inline bool nonzero(float4 v) { return any(v > 0); }
inline bool finite_nonnegative(float4 v) { return all(isfinite(v)) && all(v >= 0); }
inline uint light_count(constant wire::Parameters &p) { return p.light_count; }
inline bool infinite(const thread Vertex &v) { return v.type == environment_vertex; }
inline bool on_surface(const thread Vertex &v) { return any(v.point.geometric_normal != 0); }
inline float convert_density(float pdf, const thread Vertex &from, const thread Vertex &to) {
    if (infinite(to))
        return pdf;
    const float3 d = to.point.position - from.point.position;
    const float d2 = dot(d, d);
    if (!(d2 > 0))
        return 0;
    if (on_surface(to))
        pdf *= abs(dot(to.point.geometric_normal, d * rsqrt(d2)));
    return pdf / d2;
}
inline float3 offset(const thread Vertex &v, float3 direction) {
    if (!on_surface(v))
        return v.point.position;
    const float3 n = dot(direction, v.point.geometric_normal) < 0 ? -v.point.geometric_normal
                                                                  : v.point.geometric_normal;
    return round_out(v.point.position + dot(abs(n), v.point.error) * n, n);
}
inline float2 uniform2(constant wire::Parameters &p, uint2 words) {
    return float2(sample_uniform(p, words.x), sample_uniform(p, words.y));
}
inline uint4 random(const thread Resources &r, const thread wire::Sample &sample, ulong index,
                    uint block) {
    return draw_words(*r.alpha.parameters, *r.tables, sample, index, block);
}
inline float4 environment_radiance(const thread Resources &r, float3 direction,
                                   const thread Wavelengths &lambda) {
    float4 value;
    for (uint i = 0; i < 4; ++i)
        value[i] = environment_image_enabled
                       ? rypath_image_light::environment_radiance(
                             r.images, *r.alpha.graph.parameters, direction, lambda.lambda[i])
                       : sky_radiance(r.environment, *r.alpha.parameters, lambda.lambda[i]);
    return value;
}
inline float environment_pdf(const thread Resources &r, float3 direction) {
    return environment_image_enabled
               ? rypath_image_light::environment_pdf(*r.alpha.graph.parameters, r.environment_cdf,
                                                     direction, !r.alpha.parameters->bidirectional)
               : (r.alpha.parameters->bidirectional ? 1 / (4 * pi) : 0);
}
inline bool sample_environment(const thread Resources &r, float2 u, thread float3 &direction,
                               thread float &pdf) {
    if (environment_image_enabled)
        return rypath_image_light::sample_environment(*r.alpha.graph.parameters, r.environment_cdf,
                                                      u, direction, pdf,
                                                      !r.alpha.parameters->bidirectional);
    if (!r.alpha.parameters->bidirectional) {
        direction = float3(0, 0, 1);
        pdf = 0;
        return true;
    }
    direction = sphere(u.x, u.y);
    pdf = 1 / (4 * pi);
    return true;
}

// Perspective We/PDF_We use the full film support, including filter overscan.
inline bool camera_projection(constant wire::Parameters &p, float3 origin, float3 direction,
                              thread float2 &raster, thread float &cosine) {
    cosine = dot(direction, unpack(p.camera_normal));
    if (!(cosine > 0))
        return false;
    const float distance = p.lens_radius > 0 ? p.focal_distance : 1;
    const float3 focus =
        origin - unpack(p.origin) + direction * (distance * p.camera_plane_depth / cosine);
    const float3 plane = focus / distance - unpack(p.forward);
    const float3 dx = unpack(p.right), dy = unpack(p.up);
    const float xx = dot(dx, dx), yy = dot(dy, dy), xy = dot(dx, dy);
    const float determinant = xx * yy - xy * xy;
    raster = float2((dot(plane, dx) * yy - dot(plane, dy) * xy) / determinant,
                    (dot(plane, dy) * xx - dot(plane, dx) * xy) / determinant);
    raster *= float2(p.width, p.height);
    return all(isfinite(raster)) && raster.x >= .5f - p.filter_x_radius &&
           raster.y >= .5f - p.filter_y_radius &&
           raster.x < float(p.width) - .5f + p.filter_x_radius &&
           raster.y < float(p.height) - .5f + p.filter_y_radius;
}
inline float camera_pdf(constant wire::Parameters &p, float3 origin, float3 direction) {
    float2 raster;
    float cosine;
    return camera_projection(p, origin, direction, raster, cosine)
               ? 1 / (p.camera_image_area * cosine * cosine * cosine)
               : 0;
}
inline bool camera_sample(constant wire::Parameters &p, const thread Vertex &reference, float2 u,
                          thread Vertex &sampled, thread float2 &raster, thread float3 &direction) {
    // The existing lens mapping is shared with primary rays.
    // lens_point consumes sampler representations, so use the same concentric
    // mapping here directly on already-decoded uniforms.
    const float2 q = 2 * u - 1;
    float2 disk(0);
    if (any(q != 0)) {
        const bool x = abs(q.x) > abs(q.y);
        const float radius = x ? q.x : q.y;
        const float angle = x ? (pi / 4) * q.y / q.x : pi / 2 - (pi / 4) * q.x / q.y;
        disk = radius * float2(cos(angle), sin(angle));
    }
    sampled = {};
    sampled.type = camera_vertex;
    sampled.path_id = 0xffffffffu;
    sampled.connectible = true;
    sampled.point.position =
        unpack(p.origin) + p.lens_radius * (disk.x * unpack(p.lens_x) + disk.y * unpack(p.lens_y));
    sampled.point.geometric_normal = unpack(p.camera_normal);
    sampled.point.shading_normal = sampled.point.geometric_normal;
    const float3 delta = sampled.point.position - reference.point.position;
    const float d2 = dot(delta, delta);
    if (!(d2 > 0))
        return false;
    direction = delta * rsqrt(d2);
    float cosine;
    if (!camera_projection(p, sampled.point.position, -direction, raster, cosine))
        return false;
    // We / SampleWi.pdf; lens area cancels even for the pinhole convention.
    sampled.beta = float4(1 / (p.camera_image_area * cosine * cosine * cosine * d2));
    return finite_nonnegative(sampled.beta);
}
inline bool surface(const thread Resources &r, float2 uv, uint instance_id, uint primitive,
                    uint kind, PointSource source, thread SurfacePoint &point) {
    const constant auto &p = *r.alpha.parameters;
    return instance_id < p.instance_count &&
           surface_point(uv, kind, r.alpha.instances[instance_id], primitive, r.alpha.positions,
                         r.alpha.triangles, r.alpha.patches, r.alpha.normals, p.normal_count,
                         source, point, r.alpha.material_data);
}
inline float2 sample_area_uv(const thread Resources &r, const thread wire::Emitter &emitter,
                             float2 u) {
    auto alpha = r.alpha;
    return rypath_light_geometry::area_uv_sample(alpha, emitter, u);
}
inline float area_pdf(const thread Resources &r, const thread Vertex &v) {
    auto alpha = r.alpha;
    const wire::Emitter emitter{v.instance, v.primitive, v.kind};
    return rypath_light_geometry::area_pdf(alpha, emitter, v.uv, v.point);
}
inline bool emitted(thread Resources &r, const thread Vertex &v, float3 direction,
                    const thread Wavelengths &lambda, thread float4 &radiance) {
    const constant auto &p = *r.alpha.parameters;
    if (infinite(v)) {
        radiance = environment_radiance(r, direction, lambda);
        return finite_nonnegative(radiance);
    }
    radiance = 0;
    if (v.instance >= p.instance_count)
        return true;
    const device auto &instance = r.alpha.instances[v.instance];
    if (instance.emission == wire::no_emission)
        return true;
    const device auto &emission = r.emissions[instance.emission];
    if (!emission.two_sided && dot(v.point.geometric_normal, direction) <= 0)
        return true;
    wire::Sample sample = *r.alpha.sample;
    auto alpha = r.alpha;
    alpha.sample = &sample;
    for (uint i = 0; i < 4; ++i) {
        sample.wavelength = lambda.lambda[i];
        auto point = v.point;
        float lane_radiance;
        if (!area_radiance(emission, instance, v.primitive, v.kind, v.uv, point, alpha, r.images,
                           lane_radiance))
            return false;
        radiance[i] = lane_radiance;
    }
    return finite_nonnegative(radiance);
}
inline float4 positional_radiance(const thread Resources &r, uint index, float3 wi,
                                  const thread Wavelengths &lambda) {
    if (!positional_lights_enabled)
        return float4(0);
    const device auto *light = positional_record(r.alpha.material_data, *r.alpha.parameters, index);
    if (!light)
        return float4(NAN);
    float4 value;
    for (uint i = 0; i < 4; ++i) {
        float intensity = light->intensity[dense_index(lambda.lambda[i])];
        if (!positional_intensity(*light, wi, r.images, lambda.lambda[i], intensity))
            return float4(NAN);
        value[i] = intensity;
    }
    return value;
}
inline float4 distant_radiance(const thread Resources &r, uint index,
                               const thread Wavelengths &lambda) {
    if (!distant_lights_enabled)
        return float4(0);
    const device auto *light = distant_record(r.alpha.material_data, *r.alpha.parameters, index);
    if (!light)
        return float4(NAN);
    return float4(light->radiance[dense_index(lambda.lambda[0])],
                  light->radiance[dense_index(lambda.lambda[1])],
                  light->radiance[dense_index(lambda.lambda[2])],
                  light->radiance[dense_index(lambda.lambda[3])]);
}
inline float light_origin_pdf(const thread Resources &r, const thread Vertex &v,
                              const thread Vertex &next) {
    const constant auto &p = *r.alpha.parameters;
    const float selection = rypath_light_sampling::pmf(
        p, r.alpha.material_data, {next.point.position, next.point.shading_normal}, v.light);
    if (infinite(v)) {
        if (v.delta_light)
            return 0;
        return selection * environment_pdf(r, -v.point.geometric_normal);
    }
    return on_surface(v) ? selection * area_pdf(r, v) : selection;
}
inline float light_direction_pdf(const thread Resources &r, const thread Vertex &v,
                                 const thread Vertex &next) {
    const constant auto &p = *r.alpha.parameters;
    const float3 delta = next.point.position - v.point.position;
    const float distance2 = infinite(v) ? 1 : dot(delta, delta);
    if (!(distance2 > 0))
        return 0;
    const float3 direction = infinite(v) ? v.point.geometric_normal : delta * rsqrt(distance2);
    float pdf;
    if (infinite(v)) {
        pdf = 1 / (pi * p.scene_radius * p.scene_radius);
    } else {
        pdf = 1 / (4 * pi);
        if (!on_surface(v) && v.light >= p.emitter_count &&
            v.light < p.emitter_count + p.positional_count) {
            const auto light =
                positional_record(r.alpha.material_data, p, v.light - p.emitter_count);
            if (!light)
                return NAN;
            pdf = rypath_light_emission::pdf(*light, r.alpha.material_data, direction);
        }
        if (on_surface(v)) {
            const device auto &instance = r.alpha.instances[v.instance];
            const bool two_sided = r.emissions[instance.emission].two_sided;
            const float cosine = dot(v.point.geometric_normal, direction);
            pdf = two_sided ? abs(cosine) / (2 * pi) : max(0.0f, cosine) / pi;
        }
        pdf /= distance2;
    }
    if (on_surface(next))
        pdf *= abs(dot(next.point.geometric_normal, direction));
    return pdf;
}
inline float3 local_direction(const thread Vertex &v, const thread Vertex &endpoint,
                              float3 direction) {
    if (v.pair_valid) {
        if (endpoint.path_id == v.sampled_prev_id)
            return v.sampled_wo_local;
        if (endpoint.path_id == v.sampled_next_id)
            return v.sampled_wi_local;
    }
    return v.frame.local(direction);
}
inline float vertex_pdf(const thread Resources &r, const thread Vertex &v,
                        const thread Vertex &previous, const thread Vertex &next, Mode mode) {
    if (v.type == light_vertex || infinite(v))
        return light_direction_pdf(r, v, next);
    const float3 delta =
        infinite(next) ? -next.point.geometric_normal : next.point.position - v.point.position;
    if (all(delta == 0))
        return 0;
    const float3 wi = normalize(delta);
    if (v.type == camera_vertex)
        return convert_density(camera_pdf(*r.alpha.parameters, v.point.position, wi), v, next);
    const float3 before = infinite(previous) ? -previous.point.geometric_normal
                                             : previous.point.position - v.point.position;
    if (all(before == 0))
        return 0;
    const float3 wo = normalize(before);
    float3 local_wo = v.frame.local(wo), local_wi = v.frame.local(wi);
    if (v.pair_valid) {
        if (previous.path_id == v.sampled_prev_id)
            local_wo = v.sampled_wo_local;
        else if (previous.path_id == v.sampled_next_id)
            local_wo = v.sampled_wi_local;
        if (next.path_id == v.sampled_next_id)
            local_wi = v.sampled_wi_local;
        else if (next.path_id == v.sampled_prev_id)
            local_wi = v.sampled_wo_local;
    }
    const float pdf = rypath_packet::pdf(v.bsdf, local_wo, local_wi, mode);
    return convert_density(pdf, v, next);
}
inline float4 f(const thread Vertex &v, float3 direction, Mode mode) {
    if (v.type != surface_vertex)
        return 0;
    const auto result =
        rypath_packet::evaluate(v.bsdf, v.pair_valid ? v.sampled_wo_local : v.frame.local(v.wo),
                                v.frame.local(direction), v.seed, mode);
    return result.valid ? result.f : float4(NAN);
}

// Each shadow segment excludes just its sampled endpoints. A nearer root of the
// same analytic primitive remains a blocker; no primitive-wide exclusion exists.
inline bool visible(thread Resources &r, const thread Vertex &a, const thread Vertex &b,
                    thread wire::Sample &sample) {
    if (r.alpha.parameters->instance_count == 0)
        return true;
    const float3 direction = b.point.position - a.point.position;
    if (all(direction == 0))
        return false;
    const float3 origin = offset(a, direction);
    const float3 target = offset(b, -direction);
    ray segment(origin, target - origin, 0, 0x1.fffffep-1f);
    MixedIntersector closest;
    closest.set_triangle_cull_mode(triangle_cull_mode::none);
    for (uint crossings = 0; crossings <= r.alpha.parameters->boundary_limit; ++crossings) {
        r.alpha.incoming = segment;
        const auto hit =
            alpha_enabled
                ? trace_masked_surface(segment, r.scene, all_visible, r.alpha)
                : trace_surface(closest, segment, r.scene, all_visible, r.intersection_table);
        if (!hit.valid) {
            sample.status = wire::invalid_hit;
            return false;
        }
        if (hit.type == intersection_type::none)
            return true;
        if (hit.instance_id >= r.alpha.parameters->instance_count) {
            sample.status = wire::invalid_hit;
            return false;
        }
        const device auto &instance = r.alpha.instances[hit.instance_id];
        if (instance.material >= r.alpha.parameters->material_count) {
            sample.status = wire::invalid_hit;
            return false;
        }
        if (r.alpha.materials[instance.material].scattering != 10u && !instance.invisible_emitter)
            return false;
        SurfacePoint point;
        if (!surface(r, hit.coordinates, hit.instance_id, hit.primitive_id, hit.primitive_kind,
                     PointSource::native_hit, point)) {
            sample.status = wire::invalid_arithmetic;
            return false;
        }
        float3 next;
        if (!spawn_boundary(segment, hit.distance, point, normalize(segment.direction), next)) {
            sample.status = wire::invalid_arithmetic;
            return false;
        }
        segment = ray(next, target - next, 0, 0x1.fffffep-1f);
    }
    sample.status = wire::boundary_exhaustion;
    return false;
}

inline bool sample_light_point(thread Resources &r, uint selected, float2 u, thread Vertex &v,
                               bool sample_position = true) {
    const constant auto &p = *r.alpha.parameters;
    v = {};
    v.type = light_vertex;
    v.path_id = 0xffffffffu;
    v.light = selected;
    v.instance = 0xffffffffu;
    v.connectible = true;
    if (selected >= p.emitter_count)
        return true;
    const auto emitter = r.emitters[selected];
    v.instance = emitter.instance;
    v.primitive = emitter.primitive;
    v.kind = emitter.kind;
    if (!sample_position)
        return true;
    v.uv = sample_area_uv(r, emitter, u);
    return surface(r, v.uv, emitter.instance, emitter.primitive, emitter.kind,
                   PointSource::area_sample, v.point);
}
inline bool sample_li(thread Resources &r, const thread Vertex &reference,
                      const thread Wavelengths &lambda, float4 u, thread Vertex &v,
                      thread float3 &direction, thread float &pdf) {
    const constant auto &p = *r.alpha.parameters;
    const auto choice = rypath_light_sampling::sample(
        p, r.alpha.material_data, {reference.point.position, reference.point.shading_normal}, u.x);
    const uint selected = choice.light;
    if (!(choice.pmf > 0)) {
        v = {};
        v.beta = 0;
        pdf = 0;
        return true;
    }
    if (!sample_light_point(r, selected, u.yz, v, false))
        return false;
    float4 radiance;
    if (selected < p.emitter_count) {
        const auto emitter = r.emitters[selected];
        if (!rypath_light_geometry::sample(r.alpha, emitter, reference.point.position,
                                           reference.point.shading_normal, u.yz, v.uv, v.point,
                                           pdf))
            return false;
        pdf *= choice.pmf;
        if (v.point.inverse_area_pdf == 0) {
            v.beta = 0;
            pdf = 0;
            return true;
        }
        const float3 delta = v.point.position - reference.point.position;
        const float d2 = dot(delta, delta);
        if (!(d2 > 0)) {
            v.beta = 0;
            pdf = 0;
            return true;
        }
        direction = delta * rsqrt(d2);
        if (!emitted(r, v, -direction, lambda, radiance))
            return false;
    } else if (selected < p.emitter_count + p.positional_count) {
        const uint id = selected - p.emitter_count;
        const device auto *light = positional_record(r.alpha.material_data, p, id);
        if (!light)
            return false;
        v.delta_light = true;
        v.point.position = unpack(light->position);
        const float3 delta = v.point.position - reference.point.position;
        const float d2 = dot(delta, delta);
        if (!(d2 > 0)) {
            v.beta = 0;
            pdf = 0;
            return true;
        }
        direction = delta * rsqrt(d2);
        radiance = positional_radiance(r, id, direction, lambda) / d2;
        pdf = choice.pmf;
    } else {
        v.type = environment_vertex;
        if (selected < p.emitter_count + p.positional_count + p.distant_count) {
            const uint id = selected - p.emitter_count - p.positional_count;
            const device auto *light = distant_record(r.alpha.material_data, p, id);
            if (!light)
                return false;
            direction = unpack(light->direction);
            radiance = distant_radiance(r, id, lambda);
            pdf = choice.pmf;
            v.delta_light = true;
            v.connectible = false;
        } else {
            if (!sample_environment(r, u.yz, direction, pdf))
                return false;
            radiance = environment_radiance(r, direction, lambda);
            pdf *= choice.pmf;
        }
        v.point.position = reference.point.position + 2 * p.scene_radius * direction;
        v.point.geometric_normal = -direction;
        v.point.shading_normal = -direction;
    }
    v.beta = pdf > 0 ? radiance / pdf : float4(0);
    v.pdf_fwd = light_origin_pdf(r, v, reference);
    return finite_nonnegative(v.beta);
}

#include "packet_walk.h"

inline uint light_path(thread Resources &r, thread wire::Sample &sample, thread Wavelengths &lambda,
                       ulong index, device uchar *path) {
    const constant auto &p = *r.alpha.parameters;
    const auto a = random(r, sample, index, 2), b = random(r, sample, index, 3);
    const auto choice = rypath_light_sampling::sample(
        p, r.alpha.material_data, {float3(0), float3(0)}, sample_uniform(p, a.x));
    if (!(choice.pmf > 0))
        return 0;
    const uint selected = choice.light;
    Vertex v;
    if (!sample_light_point(r, selected, uniform2(p, a.yz), v)) {
        sample.status = wire::invalid_arithmetic;
        return 0;
    }
    float3 direction;
    float pdf_position, pdf_direction, cosine = 1;
    float4 radiance;
    if (selected < p.emitter_count) {
        if (v.point.inverse_area_pdf == 0)
            return 0;
        const device auto &instance = r.alpha.instances[v.instance];
        const bool two_sided = r.emissions[instance.emission].two_sided;
        float2 u = uniform2(p, b.xy);
        float sign = 1;
        if (two_sided) {
            sign = u.x < .5f ? 1 : -1;
            u.x = u.x < .5f ? 2 * u.x : 2 * (u.x - .5f);
        }
        direction = hemisphere(sign * v.point.geometric_normal, u.x, u.y);
        cosine = abs(dot(direction, v.point.geometric_normal));
        pdf_direction = cosine / (two_sided ? 2 * pi : pi);
        pdf_position = area_pdf(r, v);
        if (!emitted(r, v, direction, lambda, radiance)) {
            sample.status = wire::invalid_arithmetic;
            return 0;
        }
    } else if (selected < p.emitter_count + p.positional_count) {
        const uint id = selected - p.emitter_count;
        const device auto *light = positional_record(r.alpha.material_data, p, id);
        if (!light) {
            sample.status = wire::invalid_hit;
            return 0;
        }
        v.point.position = unpack(light->position);
        v.delta_light = true;
        if (!rypath_light_emission::sample(*light, r.alpha.material_data, uniform2(p, b.xy),
                                           direction, pdf_direction)) {
            sample.status = wire::invalid_arithmetic;
            return 0;
        }
        radiance = positional_radiance(r, id, -direction, lambda);
        pdf_position = 1;
    } else {
        v.type = environment_vertex;
        float3 outward;
        if (selected < p.emitter_count + p.positional_count + p.distant_count) {
            const uint id = selected - p.emitter_count - p.positional_count;
            const device auto *light = distant_record(r.alpha.material_data, p, id);
            if (!light) {
                sample.status = wire::invalid_hit;
                return 0;
            }
            outward = unpack(light->direction);
            radiance = distant_radiance(r, id, lambda);
            pdf_direction = 1;
            v.delta_light = true;
            v.connectible = false;
        } else {
            if (!sample_environment(r, uniform2(p, a.yz), outward, pdf_direction)) {
                sample.status = wire::invalid_arithmetic;
                return 0;
            }
            radiance = environment_radiance(r, outward, lambda);
        }
        direction = -outward;
        float3 x, y;
        rypath_surface_detail::coordinate_system(direction, x, y);
        const float radius = p.scene_radius * sqrt(sample_uniform(p, b.x));
        const float angle = 2 * pi * sample_uniform(p, b.y);
        v.point.position = unpack(p.scene_center) + p.scene_radius * outward +
                           radius * (cos(angle) * x + sin(angle) * y);
        v.point.geometric_normal = direction;
        v.point.shading_normal = direction;
        pdf_position = 1 / (pi * p.scene_radius * p.scene_radius);
    }
    if (!finite_nonnegative(radiance) || !isfinite(pdf_position) || !isfinite(pdf_direction)) {
        sample.status = wire::invalid_arithmetic;
        return 0;
    }
    if (!(pdf_position > 0 && pdf_direction > 0) || !nonzero(radiance))
        return 0;
    v.beta = radiance;
    v.path_id = 0x80000000u;
    v.pdf_fwd = pdf_position * choice.pmf;
    path_vertex(path, 0) = v;
    const float4 beta = radiance * (cosine / (v.pdf_fwd * pdf_direction));
    const ray incoming(offset(v, direction), direction);
    const uint n = random_walk(r, sample, lambda, incoming, beta, pdf_direction, path, 1,
                               p.max_depth + 1, Mode::Importance, index);
    if (infinite(v)) {
        if (n > 1) {
            auto first = path_vertex(path, 1);
            first.pdf_fwd = pdf_position;
            if (on_surface(first))
                first.pdf_fwd *= abs(dot(direction, first.point.geometric_normal));
            path_vertex(path, 1) = first;
        }
        path_vertex(path, 0).pdf_fwd =
            v.delta_light ? 0 : environment_pdf(r, -direction) * choice.pmf;
    }
    return n;
}

inline float log_add(float a, float b) {
    if (isinf(a) && a < 0)
        return b;
    const float high = max(a, b), low = min(a, b);
    return high + log(1 + exp(low - high));
}
// The balance heuristic is evaluated in log space so legitimate long-path PDF
// ratios cannot overflow. Zero densities at delta events retain PBRT's remap.
[[clang::noinline]] inline float mis_weight(const thread Resources &r, device uchar *light,
                                            device uchar *camera, const thread Vertex &sampled,
                                            uint s, uint t) {
    if (s + t == 2)
        return 1;
    const Vertex pt = t == 1 ? sampled : path_vertex(camera, t - 1);
    const Vertex qs = s == 1 ? sampled : s > 1 ? path_vertex(light, s - 1) : Vertex{};
    const Vertex pt_minus = t > 1 ? path_vertex(camera, t - 2) : Vertex{};
    const Vertex qs_minus = s > 1 ? path_vertex(light, s - 2) : Vertex{};
    const float p_reverse = s > 0 ? vertex_pdf(r, qs, qs_minus, pt, Mode::Importance)
                                  : light_origin_pdf(r, pt, pt_minus);
    const float p_minus_reverse = t > 1 ? (s > 0 ? vertex_pdf(r, pt, qs, pt_minus, Mode::Importance)
                                                 : light_direction_pdf(r, pt, pt_minus))
                                        : 0;
    const float q_reverse = s > 0 ? vertex_pdf(r, pt, pt_minus, qs, Mode::Radiance) : 0;
    const float q_minus_reverse = s > 1 ? vertex_pdf(r, qs, pt, qs_minus, Mode::Radiance) : 0;
    float sum_log = -INFINITY, ratio_log = 0;
    for (int i = int(t) - 1; i > 0; --i) {
        const auto v = path_vertex(camera, uint(i));
        const float reverse = uint(i) == t - 1   ? p_reverse
                              : uint(i) == t - 2 ? p_minus_reverse
                                                 : v.pdf_rev;
        if (!(reverse >= 0 && v.pdf_fwd >= 0) || !isfinite(reverse) || !isfinite(v.pdf_fwd))
            return NAN;
        ratio_log += log(remap_zero(reverse)) - log(remap_zero(v.pdf_fwd));
        const bool delta = uint(i) == t - 1 ? false : v.delta;
        if (!delta && !path_vertex(camera, uint(i - 1)).delta)
            sum_log = log_add(sum_log, ratio_log);
    }
    ratio_log = 0;
    for (int i = int(s) - 1; i >= 0; --i) {
        const auto v = uint(i) == s - 1 ? qs : path_vertex(light, uint(i));
        const float reverse = uint(i) == s - 1   ? q_reverse
                              : uint(i) == s - 2 ? q_minus_reverse
                                                 : v.pdf_rev;
        if (!(reverse >= 0 && v.pdf_fwd >= 0) || !isfinite(reverse) || !isfinite(v.pdf_fwd))
            return NAN;
        ratio_log += log(remap_zero(reverse)) - log(remap_zero(v.pdf_fwd));
        const bool delta = uint(i) == s - 1 ? false : v.delta;
        const bool delta_before = i > 0 ? path_vertex(light, uint(i - 1)).delta : v.delta_light;
        if (!delta && !delta_before)
            sum_log = log_add(sum_log, ratio_log);
    }
    return exp(-log_add(0, sum_log));
}
inline float4 uniforms4(constant wire::Parameters &p, uint4 words) {
    return float4(sample_uniform(p, words.x), sample_uniform(p, words.y),
                  sample_uniform(p, words.z), sample_uniform(p, words.w));
}
inline bool endpoint_emission(thread Resources &r, const thread Vertex &v,
                              const thread Vertex &previous, const thread Wavelengths &lambda,
                              thread float4 &radiance) {
    if (infinite(v))
        return emitted(r, v, -v.point.geometric_normal, lambda, radiance);
    const float3 delta = previous.point.position - v.point.position;
    if (all(delta == 0)) {
        radiance = 0;
        return true;
    }
    // Escaped camera vertices store -ray.direction as their normal; the emitted
    // environment is queried along the original outward ray.
    return emitted(r, v, normalize(delta), lambda, radiance);
}
inline float direct_pdf(const thread Resources &r, const thread Vertex &from,
                        const thread Vertex &light) {
    const float selection =
        rypath_light_sampling::pmf(*r.alpha.parameters, r.alpha.material_data,
                                   {from.point.position, from.point.shading_normal}, light.light);
    if (infinite(light))
        return environment_pdf(r, -light.point.geometric_normal) * selection;
    const float3 delta = light.point.position - from.point.position;
    const float d2 = dot(delta, delta);
    if (!(d2 > 0))
        return 0;
    if (!on_surface(light))
        return 0;
    auto alpha = r.alpha;
    const wire::Emitter emitter{light.instance, light.primitive, light.kind};
    return selection * rypath_light_geometry::pdf(alpha, emitter, from.point.position,
                                                  from.point.shading_normal, light.uv, light.point);
}

[[clang::noinline]] inline float4 camera_connection(thread Resources &r,
                                                    thread wire::Sample &sample,
                                                    const thread Wavelengths &lambda, ulong index,
                                                    device uchar *light, device uchar *camera,
                                                    uint s, uint t) {
    const constant auto &p = *r.alpha.parameters;
    const Vertex pt = path_vertex(camera, t - 1);
    if (s != 0 && pt.type != surface_vertex)
        return 0;
    Vertex sampled{};
    float4 value(0);
    float alternative_pdf = 0, sampled_pdf = 0;
    if (s == 0) {
        const Vertex previous = path_vertex(camera, t - 2);
        float4 radiance;
        if (!endpoint_emission(r, pt, previous, lambda, radiance)) {
            sample.status = wire::invalid_arithmetic;
            return 0;
        }
        value = pt.beta * radiance;
        if (!p.bidirectional && t > 2 && !previous.delta) {
            const Vertex before = path_vertex(camera, t - 3);
            const float3 wi = infinite(pt) ? -pt.point.geometric_normal
                                           : normalize(pt.point.position - previous.point.position);
            const float3 wo = infinite(before)
                                  ? -before.point.geometric_normal
                                  : normalize(before.point.position - previous.point.position);
            sampled_pdf = rypath_packet::pdf(previous.bsdf, local_direction(previous, before, wo),
                                             local_direction(previous, pt, wi), Mode::Radiance);
            alternative_pdf = direct_pdf(r, previous, pt);
        }
    } else if (s == 1) {
        if (!pt.connectible)
            return 0;
        float3 direction;
        if (!sample_li(r, pt, lambda, uniforms4(p, random(r, sample, index, 133 + t - 2)), sampled,
                       direction, sampled_pdf)) {
            sample.status = wire::invalid_arithmetic;
            return 0;
        }
        if (!(sampled_pdf > 0) || !nonzero(sampled.beta))
            return 0;
        value = pt.beta * f(pt, direction, Mode::Radiance) * sampled.beta *
                abs(dot(pt.point.shading_normal, direction));
        if (nonzero(value) && !visible(r, pt, sampled, sample))
            return 0;
        if (!p.bidirectional && !sampled.delta_light)
            alternative_pdf = rypath_packet::pdf(
                pt.bsdf, pt.pair_valid ? pt.sampled_wo_local : pt.frame.local(pt.wo),
                pt.frame.local(direction), Mode::Radiance);
    } else {
        const Vertex qs = path_vertex(light, s - 1);
        if (!pt.connectible || !qs.connectible)
            return 0;
        const float3 delta = qs.point.position - pt.point.position;
        const float d2 = dot(delta, delta);
        if (!(d2 > 0))
            return 0;
        const float3 wi = delta * rsqrt(d2);
        value =
            pt.beta * f(pt, wi, Mode::Radiance) * qs.beta * f(qs, -wi, Mode::Importance) *
            (abs(dot(pt.point.shading_normal, wi)) * abs(dot(qs.point.shading_normal, wi)) / d2);
        if (nonzero(value) && !visible(r, pt, qs, sample))
            return 0;
    }
    if (!finite_nonnegative(value)) {
        sample.status = wire::invalid_arithmetic;
        return 0;
    }
    if (nonzero(value)) {
        const float weight = p.bidirectional       ? mis_weight(r, light, camera, sampled, s, t)
                             : alternative_pdf > 0 ? power_weight(sampled_pdf, alternative_pdf)
                                                   : 1;
        if (!(weight >= 0 && weight <= 1) || !isfinite(weight)) {
            sample.status = wire::invalid_arithmetic;
            return 0;
        }
        value *= weight;
    }
    return value;
}

[[clang::noinline]] inline bool camera_splat(thread Resources &r, thread wire::Sample &sample,
                                             const thread Wavelengths &lambda, ulong index,
                                             device uchar *light, device uchar *camera, uint s,
                                             thread storage::Splat &splat) {
    const constant auto &p = *r.alpha.parameters;
    const Vertex qs = path_vertex(light, s - 1);
    if (!qs.connectible)
        return false;
    const auto words = random(r, sample, index, 198 + (s - 2) / 2);
    const auto lens_words = (s & 1) == 0 ? words.xy : words.zw;
    Vertex sampled;
    float2 raster;
    float3 direction;
    if (!camera_sample(p, qs, uniform2(p, lens_words), sampled, raster, direction))
        return false;
    float4 value = qs.beta * f(qs, direction, Mode::Importance) * sampled.beta *
                   abs(dot(qs.point.shading_normal, direction));
    if (nonzero(value) && !visible(r, qs, sampled, sample))
        return false;
    if (!finite_nonnegative(value)) {
        sample.status = wire::invalid_arithmetic;
        return false;
    }
    if (!nonzero(value))
        return false;
    value *= mis_weight(r, light, camera, sampled, s, 1);
    if (!finite_nonnegative(value)) {
        sample.status = wire::invalid_arithmetic;
        return false;
    }
    splat.raster[0] = raster.x;
    splat.raster[1] = raster.y;
    for (uint i = 0; i < 4; ++i)
        splat.radiance[i] = value[i];
    splat.strategy = s;
    splat.reserved = 0;
    return true;
}
} // namespace rypath_bdpt
