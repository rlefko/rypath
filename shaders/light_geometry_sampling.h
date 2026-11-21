// SPDX-License-Identifier: GPL-3.0-only AND Apache-2.0
// PBRT incident geometry proposals; see light_geometry_sampling_shared.h.
#pragma once
#include "../src/light_geometry_sampling_shared.h"

// Included after SurfaceAlpha, surface_point and sample_emitter_uv.
namespace rypath_light_geometry {
namespace math = rypath_light_geometry_math;

static float3 world_point(const device wire::Instance &instance, float3 p) {
    return float3(dot(float3(instance.matrix[0], instance.matrix[1], instance.matrix[2]), p),
                  dot(float3(instance.matrix[4], instance.matrix[5], instance.matrix[6]), p),
                  dot(float3(instance.matrix[8], instance.matrix[9], instance.matrix[10]), p)) +
           float3(instance.matrix[3], instance.matrix[7], instance.matrix[11]);
}
static void corners(thread SurfaceAlpha &alpha, const thread wire::Emitter &emitter,
                    thread float3 *p) {
    const device auto &instance = alpha.instances[emitter.instance];
    if (emitter.kind == wire::triangle_primitive) {
        const auto face = alpha.triangles[instance.triangle_begin + emitter.primitive];
        for (uint i = 0; i < 3; ++i)
            p[i] = world_point(instance,
                               unpack(alpha.positions[instance.vertex_begin + face.vertices[i]]));
    } else {
        const auto face = alpha.patches[instance.patch_begin + emitter.primitive];
        for (uint i = 0; i < 4; ++i)
            p[i] = world_point(instance,
                               unpack(alpha.positions[instance.vertex_begin + face.vertices[i]]));
    }
}
static float2 area_uv_sample(thread SurfaceAlpha &alpha, const thread wire::Emitter &emitter,
                             float2 u) {
    if (emitter.kind == wire::triangle_primitive)
        return math::triangle_uniform_uv(u);
    if (emitter.kind == wire::patch_primitive) {
        float3 p[4];
        corners(alpha, emitter, p);
        return math::rectangle(p[0], p[1], p[2], p[3])
                   ? u
                   : math::bilinear_sample(u, math::patch_jacobians(p[0], p[1], p[2], p[3]));
    }
    const device auto &instance = alpha.instances[emitter.instance];
    const auto record = analytic_record(instance, emitter.primitive, alpha.material_data);
    return record ? sample_analytic_area(*record, u).uv : float2(NAN);
}
static float area_pdf(thread SurfaceAlpha &alpha, const thread wire::Emitter &emitter, float2 uv,
                      const thread SurfacePoint &point) {
    if (!(point.inverse_area_pdf > 0))
        return 0;
    float parametric = 1;
    if (emitter.kind == wire::patch_primitive) {
        float3 p[4];
        corners(alpha, emitter, p);
        if (!math::rectangle(p[0], p[1], p[2], p[3]))
            parametric = math::bilinear_pdf(uv, math::patch_jacobians(p[0], p[1], p[2], p[3]));
    }
    return parametric / point.inverse_area_pdf;
}
static float area_solid_angle_pdf(float area_density, float3 reference,
                                  const thread SurfacePoint &point) {
    const float3 delta = point.position - reference;
    const float distance2 = dot(delta, delta);
    if (!(distance2 > 0) || !(area_density > 0))
        return 0;
    const float cosine = abs(dot(point.geometric_normal, delta * rsqrt(distance2)));
    return cosine > 0 ? area_density * distance2 / cosine : 0;
}

// Similarities (including reflections) preserve a sphere's cone measure.
// Clipped spheres, nonuniform scale/shear and a conservative boundary slab use
// the existing clipped object-area proposal and its actual affine Jacobian.
static bool sphere_cone(thread SurfaceAlpha &alpha, const thread wire::Emitter &emitter,
                        float3 reference, thread float3 &center, thread float &radius) {
    if (emitter.kind != wire::analytic_primitive)
        return false;
    const device auto &instance = alpha.instances[emitter.instance];
    const auto record = analytic_record(instance, emitter.primitive, alpha.material_data);
    if (!record || record->subtype != 1u || record->z_min != -record->radius ||
        record->z_max != record->radius || record->phi_max != 2 * math::pi)
        return false;
    const float3 a(instance.matrix[0], instance.matrix[1], instance.matrix[2]);
    const float3 b(instance.matrix[4], instance.matrix[5], instance.matrix[6]);
    const float3 c(instance.matrix[8], instance.matrix[9], instance.matrix[10]);
    const float aa = dot(a, a), bb = dot(b, b), cc = dot(c, c);
    const float tolerance = 0x1p-19f * aa;
    if (!(aa > 0) || !isfinite(aa) || abs(aa - bb) > tolerance || abs(aa - cc) > tolerance ||
        abs(dot(a, b)) > tolerance || abs(dot(a, c)) > tolerance || abs(dot(b, c)) > tolerance)
        return false;
    center = float3(instance.matrix[3], instance.matrix[7], instance.matrix[11]);
    radius = record->radius * sqrt(aa);
    const float distance = length(center - reference);
    const float coordinate_scale =
        max(max(abs(center.x), max(abs(center.y), abs(center.z))),
            max(abs(reference.x), max(abs(reference.y), abs(reference.z))));
    const float error = 0x1p-17f * (coordinate_scale + radius);
    return distance > radius + error &&
           isfinite(math::sphere_cone_pdf(radius, distance * distance));
}
static float2 sphere_uv(thread SurfaceAlpha &alpha, const thread wire::Emitter &emitter,
                        float3 position, float3 center) {
    const device auto &instance = alpha.instances[emitter.instance];
    const float3 delta = position - center;
    const float3 n = normalize(float3(
        instance.matrix[0] * delta.x + instance.matrix[4] * delta.y + instance.matrix[8] * delta.z,
        instance.matrix[1] * delta.x + instance.matrix[5] * delta.y + instance.matrix[9] * delta.z,
        instance.matrix[2] * delta.x + instance.matrix[6] * delta.y +
            instance.matrix[10] * delta.z));
    float phi = atan2(n.y, n.x);
    if (phi < 0)
        phi += 2 * math::pi;
    return float2(phi / (2 * math::pi), 1 - acos(clamp(n.z, -1.f, 1.f)) / math::pi);
}

static float pdf(thread SurfaceAlpha &alpha, const thread wire::Emitter &emitter, float3 reference,
                 float3 normal, float2 uv, const thread SurfacePoint &point) {
    const float3 delta = point.position - reference;
    if (!(dot(delta, delta) > 0) || !(point.inverse_area_pdf > 0))
        return 0;
    if (emitter.kind == wire::triangle_primitive) {
        float3 p[3];
        corners(alpha, emitter, p);
        const float area = math::triangle_solid_angle(p[0], p[1], p[2], reference);
        if (math::spherical_triangle(area)) {
            const float2 square =
                math::triangle_inverse(p[0], p[1], p[2], reference, normalize(delta));
            return (dot(normal, normal) > 0
                        ? math::bilinear_pdf(
                              square, math::triangle_cosines(p[0], p[1], p[2], reference, normal))
                        : 1.f) /
                   area;
        }
    } else if (emitter.kind == wire::patch_primitive) {
        float3 p[4];
        corners(alpha, emitter, p);
        const float area = math::rectangle_solid_angle(p[0], p[1], p[2], p[3], reference);
        if (math::rectangle(p[0], p[1], p[2], p[3]) && area > 1e-4f) {
            const auto frame = math::rectangle_frame(reference, p[0], p[1] - p[0], p[2] - p[0]);
            const float2 square = math::rectangle_inverse(frame, uv);
            return (dot(normal, normal) > 0
                        ? math::bilinear_pdf(square, math::rectangle_cosines(p[0], p[1], p[2], p[3],
                                                                             reference, normal))
                        : 1.f) /
                   area;
        }
    } else {
        float3 center;
        float radius;
        if (sphere_cone(alpha, emitter, reference, center, radius)) {
            // A far root may be visible through an alpha hole. The cone
            // proposal selects the near root only; continuation owns that path.
            if (dot(point.position - center, reference - point.position) < 0)
                return 0;
            return math::sphere_cone_pdf(radius, dot(reference - center, reference - center));
        }
    }
    return area_solid_angle_pdf(area_pdf(alpha, emitter, uv, point), reference, point);
}

static bool sample(thread SurfaceAlpha &alpha, const thread wire::Emitter &emitter,
                   float3 reference, float3 normal, float2 u, thread float2 &uv,
                   thread SurfacePoint &point, thread float &pdf_omega) {
    if (!all(isfinite(u)) || any(u < 0) || any(u > 1))
        return false;
    u = min(u, float2(math::one_minus_epsilon));
    const device auto &instance = alpha.instances[emitter.instance];
    bool selected = false;
    if (emitter.kind == wire::triangle_primitive) {
        float3 p[3];
        corners(alpha, emitter, p);
        const float area = math::triangle_solid_angle(p[0], p[1], p[2], reference);
        if (math::spherical_triangle(area)) {
            if (dot(normal, normal) > 0)
                u = math::bilinear_sample(
                    u, math::triangle_cosines(p[0], p[1], p[2], reference, normal));
            if (!math::triangle_sample(p[0], p[1], p[2], reference, u, uv))
                return false;
            selected = true;
        }
    } else if (emitter.kind == wire::patch_primitive) {
        float3 p[4];
        corners(alpha, emitter, p);
        const float area = math::rectangle_solid_angle(p[0], p[1], p[2], p[3], reference);
        if (math::rectangle(p[0], p[1], p[2], p[3]) && area > 1e-4f) {
            if (dot(normal, normal) > 0)
                u = math::bilinear_sample(
                    u, math::rectangle_cosines(p[0], p[1], p[2], p[3], reference, normal));
            uv = math::rectangle_sample(
                math::rectangle_frame(reference, p[0], p[1] - p[0], p[2] - p[0]), u);
            selected = true;
        }
    } else {
        float3 center;
        float radius;
        if (sphere_cone(alpha, emitter, reference, center, radius)) {
            uv = sphere_uv(alpha, emitter, math::sphere_cone_point(center, radius, reference, u),
                           center);
            selected = true;
        }
    }
    if (!selected)
        uv = area_uv_sample(alpha, emitter, u);
    if (!all(isfinite(uv)) || !surface_point(uv, emitter.kind, instance, emitter.primitive,
                                             alpha.positions, alpha.triangles, alpha.patches,
                                             alpha.normals, alpha.parameters->normal_count,
                                             PointSource::area_sample, point, alpha.material_data))
        return false;
    pdf_omega = pdf(alpha, emitter, reference, normal, uv, point);
    return isfinite(pdf_omega) && pdf_omega >= 0;
}
} // namespace rypath_light_geometry
