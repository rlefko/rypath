// SPDX-License-Identifier: Apache-2.0
// Surface partials and map evaluation adapted from pbrt-v4. pbrt is copyright
// (c) 1998–2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// License text: third_party/pbrt/LICENSE.txt.

#pragma once

#include "../src/render_wire.h"
#include "../src/texture_graph_wire.h"
#include "texture_footprint.h"

namespace rypath_surface_detail {

using namespace metal;
namespace wire = rypath::render_wire;
namespace graph = rypath::texture_graph_wire;

struct Frame {
    float3 geometric_normal;
    float3 n;
    float3 dpdu;
    float3 dpdv;
    float3 dndu;
    float3 dndv;
};

inline float3 unpack(wire::Point point) { return {point.x, point.y, point.z}; }

inline float3 transform(const device float *matrix, float3 vector) {
    return {dot(float3(matrix[0], matrix[1], matrix[2]), vector),
            dot(float3(matrix[3], matrix[4], matrix[5]), vector),
            dot(float3(matrix[6], matrix[7], matrix[8]), vector)};
}

inline bool unit_vector(float3 vector, thread float3 &result) {
    const float3 magnitude = abs(vector);
    const float scale = max(max(magnitude.x, magnitude.y), magnitude.z);
    if (!(scale > 0.0f) || !all(isfinite(vector))) {
        return false;
    }
    result = normalize(precise::divide(vector, float3(scale)));
    return all(isfinite(result));
}

inline float3 same_hemisphere(float3 n, float3 reference) {
    return dot(n, reference) < 0.0f ? -n : n;
}

inline void coordinate_system(float3 n, thread float3 &s, thread float3 &t) {
    const float sign = copysign(1.0f, n.z);
    const float a = precise::divide(-1.0f, sign + n.z);
    const float b = n.x * n.y * a;
    s = float3(1 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    t = float3(b, sign + n.y * n.y * a, -n.y);
}

inline float3 diff_products(float a, float3 b, float c, float3 d) {
    return {rypath_texture_difference_of_products(a, b.x, c, d.x),
            rypath_texture_difference_of_products(a, b.y, c, d.y),
            rypath_texture_difference_of_products(a, b.z, c, d.z)};
}

inline bool limit_tangents(thread Frame &frame) {
    if (!all(isfinite(frame.dpdu)) || !all(isfinite(frame.dpdv))) {
        return false;
    }
    // PBRT reduces the shading partials together when authored data creates
    // an excessively long tangent, before applying the instance transform.
    while (dot(frame.dpdu, frame.dpdu) > 1.0e16f || dot(frame.dpdv, frame.dpdv) > 1.0e16f) {
        frame.dpdu = precise::divide(frame.dpdu, float3(1.0e8f));
        frame.dpdv = precise::divide(frame.dpdv, float3(1.0e8f));
    }
    return true;
}

inline bool place_frame(const device wire::SurfaceTransform &xf, thread Frame &frame) {
    // Shape shading geometry is authoritative before ObjectInstance placement.
    frame.geometric_normal = same_hemisphere(frame.geometric_normal, frame.n);
    if (!unit_vector(transform(xf.placement_normal, frame.geometric_normal),
                     frame.geometric_normal) ||
        !unit_vector(transform(xf.placement_normal, frame.n), frame.n)) {
        return false;
    }
    frame.n = same_hemisphere(frame.n, frame.geometric_normal);
    frame.dpdu = transform(xf.placement, frame.dpdu);
    frame.dpdv = transform(xf.placement, frame.dpdv);
    frame.dndu = transform(xf.placement_normal, frame.dndu);
    frame.dndv = transform(xf.placement_normal, frame.dndv);
    return all(isfinite(frame.dpdu)) && all(isfinite(frame.dpdv)) && all(isfinite(frame.dndu)) &&
           all(isfinite(frame.dndv));
}

inline float3 vertex_normal(const device wire::SurfaceTransform &xf, wire::Point normal) {
    const float3 n = transform(xf.shape_normal, unpack(normal));
    return xf.authored_reverse ? -n : n;
}

// Form the shading basis in the mesh declaration's rendering frame. Transforming
// an already-orthogonalized basis through an instance differs from rebuilding it
// from world-space edges when the placement has nonuniform scale.
inline bool triangle(const device wire::SurfaceVertex *vertices,
                     const device wire::SurfaceTransform &xf, const thread uint *indices,
                     float2 barycentric12, const thread float2 *uvs, thread Frame &frame) {
    for (uint i = 0; i < 3; ++i) {
        if (indices[i] >= xf.vertex_count) {
            return false;
        }
    }
    const auto v0 = vertices[xf.vertex_begin + indices[0]];
    const auto v1 = vertices[xf.vertex_begin + indices[1]];
    const auto v2 = vertices[xf.vertex_begin + indices[2]];
    const float3 p02 = transform(xf.shape, unpack(v0.position) - unpack(v2.position));
    const float3 p12 = transform(xf.shape, unpack(v1.position) - unpack(v2.position));
    float3 geometric;
    if (!unit_vector(cross(p02, p12), geometric)) {
        return false;
    }
    frame.geometric_normal = xf.shape_reverse_winding ? -geometric : geometric;
    frame.n = frame.geometric_normal;
    frame.dndu = frame.dndv = float3(0);
    const float2 st02 = uvs[0] - uvs[2], st12 = uvs[1] - uvs[2];
    const float determinant = rypath_texture_difference_of_products(st02.x, st12.y, st02.y, st12.x);
    const bool regular_uv = abs(determinant) >= 1.0e-9f;
    if (regular_uv) {
        const float inv_det = precise::divide(1.0f, determinant);
        frame.dpdu = diff_products(st12.y, p02, st02.y, p12) * inv_det;
        frame.dpdv = diff_products(st02.x, p12, st12.x, p02) * inv_det;
    }
    if (!regular_uv || all(cross(frame.dpdu, frame.dpdv) == 0.0f)) {
        coordinate_system(-geometric, frame.dpdu, frame.dpdv);
    }

    const float b0 = 1.0f - (barycentric12.x + barycentric12.y);
    const float b1 = barycentric12.x, b2 = barycentric12.y;
    if (xf.has_normals) {
        const float3 n0 = vertex_normal(xf, v0.normal), n1 = vertex_normal(xf, v1.normal),
                     n2 = vertex_normal(xf, v2.normal);
        const float3 n = b0 * n0 + b1 * n1 + b2 * n2;
        if (any(n != 0.0f) && !unit_vector(n, frame.n)) {
            return false;
        }
        const float3 n02 = n0 - n2, n12 = n1 - n2;
        if (regular_uv) {
            const float inv_det = precise::divide(1.0f, determinant);
            frame.dndu = diff_products(st12.y, n02, st02.y, n12) * inv_det;
            frame.dndv = diff_products(st02.x, n12, st12.x, n02) * inv_det;
        } else {
            const float3 dn = cross(n2 - n0, n1 - n0);
            if (any(dn != 0.0f)) {
                coordinate_system(dn, frame.dndu, frame.dndv);
            }
        }
    }
    if (xf.has_normals || xf.has_tangents) {
        float3 tangent = frame.dpdu;
        if (xf.has_tangents) {
            const float3 s = transform(xf.shape, b0 * unpack(v0.tangent) + b1 * unpack(v1.tangent) +
                                                     b2 * unpack(v2.tangent));
            if (any(s != 0.0f)) {
                tangent = s;
            }
        }
        const float3 bitangent = cross(frame.n, tangent);
        if (any(bitangent != 0.0f)) {
            frame.dpdu = cross(bitangent, frame.n);
            frame.dpdv = bitangent;
        } else {
            coordinate_system(frame.n, frame.dpdu, frame.dpdv);
        }
        if (!limit_tangents(frame)) {
            return false;
        }
    }
    return place_frame(xf, frame);
}

// glTF frames are formed from authored vertex attributes, then placed and
// orthonormalized. Handedness and backface reversal are independent of PBRT's
// declaration-frame shading-normal convention above.
inline float determinant(const device float *m) {
    return dot(float3(m[0], m[1], m[2]), cross(float3(m[3], m[4], m[5]), float3(m[6], m[7], m[8])));
}
inline bool gltf_triangle(const device wire::SurfaceVertex *vertices,
                          const device wire::SurfaceTransform &xf, const thread uint *indices,
                          float2 bary, float3 incoming, float3 map, bool mapped, float scale,
                          thread Frame &frame) {
    for (uint i = 0; i < 3; ++i)
        if (indices[i] >= xf.vertex_count)
            return false;
    const auto a = vertices[xf.vertex_begin + indices[0]];
    const auto b = vertices[xf.vertex_begin + indices[1]];
    const auto c = vertices[xf.vertex_begin + indices[2]];
    const float3 weights(1 - bary.x - bary.y, bary.x, bary.y);
    float3 geometric;
    if (!unit_vector(
            cross(unpack(b.position) - unpack(a.position), unpack(c.position) - unpack(a.position)),
            geometric))
        return false;
    float3 n = xf.has_normals ? weights.x * unpack(a.normal) + weights.y * unpack(b.normal) +
                                    weights.z * unpack(c.normal)
                              : geometric;
    if (xf.authored_reverse) {
        n = -n;
        geometric = -geometric;
    }
    if (!unit_vector(transform(xf.placement_normal, transform(xf.shape_normal, geometric)),
                     frame.geometric_normal) ||
        !unit_vector(transform(xf.placement_normal, transform(xf.shape_normal, n)), n))
        return false;
    float3 t, bitangent;
    if (xf.has_tangents) {
        t = weights.x * unpack(a.tangent) + weights.y * unpack(b.tangent) +
            weights.z * unpack(c.tangent);
        t = transform(xf.placement, transform(xf.shape, t));
        if (!unit_vector(t - dot(t, n) * n, t))
            return false;
        const float sign = a.tangent_sign * copysign(1.0f, determinant(xf.shape)) *
                           copysign(1.0f, determinant(xf.placement));
        bitangent = sign * cross(n, t);
    } else {
        if (mapped)
            return false;
        coordinate_system(n, t, bitangent);
    }
    if (dot(frame.geometric_normal, incoming) > 0) {
        n = -n;
        t = -t;
        bitangent = -bitangent;
    }
    if (mapped) {
        float3 normal(2 * map - 1);
        normal.xy *= scale;
        if (!unit_vector(normal, normal) ||
            !unit_vector(normal.x * t + normal.y * bitangent + normal.z * n, n))
            return false;
        if (!unit_vector(t - dot(t, n) * n, t))
            coordinate_system(n, t, bitangent);
    }
    frame.n = n;
    frame.dpdu = t;
    frame.dpdv = cross(n, t);
    frame.dndu = frame.dndv = float3(0);
    return all(isfinite(n)) && all(isfinite(t));
}

inline float3 reflect_plane(float3 value, float3 normal) {
    return value - precise::divide(2.0f * dot(value, normal), dot(normal, normal)) * normal;
}

// Two plane reflections provide a stable rotation even for antiparallel normals.
inline float3 rotate_normal_frame(float3 value, float3 from, float3 to) {
    const float3 reference = abs(from.x) < 0.72f && abs(to.x) < 0.72f   ? float3(1, 0, 0)
                             : abs(from.y) < 0.72f && abs(to.y) < 0.72f ? float3(0, 1, 0)
                                                                        : float3(0, 0, 1);
    return reflect_plane(reflect_plane(value, reference - from), reference - to);
}

inline bool patch(const device wire::SurfaceVertex *vertices,
                  const device wire::SurfaceTransform &xf, const thread uint *indices, float2 hit,
                  const thread float2 *uvs, thread Frame &frame) {
    for (uint i = 0; i < 4; ++i) {
        if (indices[i] >= xf.vertex_count) {
            return false;
        }
    }
    const auto v0 = vertices[xf.vertex_begin + indices[0]];
    const auto v1 = vertices[xf.vertex_begin + indices[1]];
    const auto v2 = vertices[xf.vertex_begin + indices[2]];
    const auto v3 = vertices[xf.vertex_begin + indices[3]];
    const float3 p0 = unpack(v0.position), p1 = unpack(v1.position);
    const float3 p2 = unpack(v2.position), p3 = unpack(v3.position);
    const float3 pu = transform(xf.shape, mix(p1 - p0, p3 - p2, hit.y));
    const float3 pv = transform(xf.shape, mix(p2 - p0, p3 - p1, hit.x));
    float2 st;
    if (!rypath_texture_patch_frame(hit, pu, pv, true, uvs[0], uvs[1], uvs[2], uvs[3], st,
                                    frame.dpdu, frame.dpdv)) {
        return false;
    }
    const float2 dstdu = mix(uvs[1] - uvs[0], uvs[3] - uvs[2], hit.y);
    const float2 dstdv = mix(uvs[2] - uvs[0], uvs[3] - uvs[1], hit.x);
    const float duds = rypath_texture_reciprocal_partial(dstdu.x);
    const float dudt = rypath_texture_reciprocal_partial(dstdu.y);
    const float dvds = rypath_texture_reciprocal_partial(dstdv.x);
    const float dvdt = rypath_texture_reciprocal_partial(dstdv.y);
    float3 raw_normal;
    if (!unit_vector(cross(frame.dpdu, frame.dpdv), raw_normal)) {
        return false;
    }
    frame.geometric_normal = xf.shape_reverse_winding ? -raw_normal : raw_normal;
    frame.n = frame.geometric_normal;

    const float e = dot(frame.dpdu, frame.dpdu), f = dot(frame.dpdu, frame.dpdv),
                g = dot(frame.dpdv, frame.dpdv);
    const float egff = rypath_texture_difference_of_products(e, g, f, f);
    const float inv = egff == 0 ? 0.0f : precise::divide(1.0f, egff);
    const float mixed = dot(raw_normal, transform(xf.shape, (p3 - p2) - (p1 - p0))) * inv;
    float3 dndu = mixed * (f * frame.dpdu - e * frame.dpdv);
    float3 dndv = mixed * (f * frame.dpdv - g * frame.dpdu);
    if (xf.has_normals) {
        const float3 n0 = vertex_normal(xf, v0.normal), n1 = vertex_normal(xf, v1.normal);
        const float3 n2 = vertex_normal(xf, v2.normal), n3 = vertex_normal(xf, v3.normal);
        const float3 interpolated = mix(mix(n0, n1, hit.x), mix(n2, n3, hit.x), hit.y);
        if (any(interpolated != 0.0f)) {
            float3 authored;
            if (!unit_vector(interpolated, authored)) {
                return false;
            }
            frame.dpdu = rotate_normal_frame(frame.dpdu, frame.n, authored);
            frame.dpdv = rotate_normal_frame(frame.dpdv, frame.n, authored);
            if (!limit_tangents(frame)) {
                return false;
            }
            frame.n = authored;
            dndu = mix(n1 - n0, n3 - n2, hit.y);
            dndv = mix(n2 - n0, n3 - n1, hit.x);
        }
    }
    frame.dndu = duds * dndu + dvds * dndv;
    frame.dndv = dudt * dndu + dvdt * dndv;
    return place_frame(xf, frame);
}

inline bool normal_map(Frame frame, float3 rgb, float3 world_geometric_normal,
                       thread float3 &result, thread Frame *mapped = nullptr) {
    float3 local, tangent;
    if (!unit_vector(2.0f * rgb - 1.0f, local) || !unit_vector(frame.dpdu, tangent)) {
        return false;
    }
    const float3 bitangent = cross(frame.n, tangent);
    if (!unit_vector(local.x * tangent + local.y * bitangent + local.z * frame.n, result)) {
        return false;
    }
    result = same_hemisphere(result, world_geometric_normal);
    if (mapped != nullptr) {
        *mapped = frame;
        mapped->n = result;
        if (!unit_vector(tangent - dot(tangent, result) * result, mapped->dpdu))
            return false;
        mapped->dpdv = cross(result, mapped->dpdu);
    }
    return true;
}

template <typename Evaluator>
inline bool bump_map(Frame frame, graph::Context context, float3 world_geometric_normal,
                     Evaluator evaluate, thread float3 &result, thread Frame *mapped = nullptr) {
    const float u_width = 0.5f * (abs(context.dudx) + abs(context.dudy));
    const float v_width = 0.5f * (abs(context.dvdx) + abs(context.dvdy));
    const float du = u_width == 0.0f ? 0.0005f : u_width;
    const float dv = v_width == 0.0f ? 0.0005f : v_width;
    const float3 p = unpack(context.p);
    const float3 pu = p + du * frame.dpdu, pv = p + dv * frame.dpdv;
    graph::Context u = context, v = context;
    u.p = {pu.x, pu.y, pu.z};
    u.u += du;
    v.p = {pv.x, pv.y, pv.z};
    v.v += dv;
    // The finite differences keep the original geometric normal and footprint.
    // The shading normal enters only when converting the displacement to a vector.
    float value, u_value, v_value;
    if (!evaluate(u, u_value) || !evaluate(v, v_value) || !evaluate(context, value)) {
        return false;
    }
    const float3 dpdu =
        frame.dpdu + precise::divide(u_value - value, du) * frame.n + value * frame.dndu;
    const float3 dpdv =
        frame.dpdv + precise::divide(v_value - value, dv) * frame.n + value * frame.dndv;
    if (!unit_vector(cross(dpdu, dpdv), result)) {
        return false;
    }
    result = same_hemisphere(result, world_geometric_normal);
    if (mapped != nullptr) {
        *mapped = frame;
        mapped->n = result;
        mapped->dpdu = dpdu;
        mapped->dpdv = dpdv;
    }
    return true;
}

} // namespace rypath_surface_detail
