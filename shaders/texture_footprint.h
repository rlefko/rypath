// SPDX-License-Identifier: Apache-2.0
// Ray differential propagation, patch texture partials, and least-squares solve
// adapted from pbrt-v4. pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob,
// and Greg Humphreys. License text: third_party/pbrt/LICENSE.txt.

#pragma once

#include <metal_stdlib>

inline float rypath_texture_differential_scale(uint total_spp) {
    return metal::max(0.125f, metal::rsqrt(float(total_spp)));
}

inline metal::float3 rypath_texture_direction(metal::float3 d) {
    const float scale = metal::max(metal::abs(d.x), metal::max(metal::abs(d.y), metal::abs(d.z)));
    if (!(scale > 0.0f) || !metal::all(metal::isfinite(d))) {
        return metal::float3(NAN);
    }
    return metal::normalize(metal::precise::divide(d, metal::float3(scale)));
}

inline bool rypath_texture_primary_dp(metal::float3 relative_p, metal::float3 n,
                                      metal::float3 ray_dir, metal::float3 x_ray_dir,
                                      metal::float3 y_ray_dir, float spp_scale,
                                      thread metal::float3 &dpdx, thread metal::float3 &dpdy) {
    const auto rx = ray_dir + spp_scale * (rypath_texture_direction(x_ray_dir) - ray_dir);
    const auto ry = ray_dir + spp_scale * (rypath_texture_direction(y_ray_dir) - ray_dir);
    const float nx = metal::dot(n, rx);
    const float ny = metal::dot(n, ry);
    if (nx == 0.0f || ny == 0.0f || !metal::isfinite(nx) || !metal::isfinite(ny)) {
        return false;
    }
    const float plane = metal::dot(n, relative_p);
    dpdx = metal::precise::divide(plane, nx) * rx - relative_p;
    dpdy = metal::precise::divide(plane, ny) * ry - relative_p;
    return true;
}

inline metal::float3 rypath_texture_reflect(metal::float3 x, metal::float3 normal) {
    return x - metal::precise::divide(2.0f * metal::dot(normal, x), metal::dot(normal, normal)) *
                   normal;
}

inline metal::float3 rypath_texture_approximate_axis(metal::float3 p_down_z, metal::float3 n_down_z,
                                                     metal::float3 delta, metal::float3 u,
                                                     metal::float3 v,
                                                     metal::float3x3 world_from_camera,
                                                     float spp_scale) {
    const auto ray = metal::float3(0, 0, 1) + delta;
    const float denom = metal::dot(n_down_z, ray);
    if (denom == 0.0f) {
        return metal::float3(0.0f);
    }
    const auto offset = metal::precise::divide(n_down_z.z * p_down_z.z, denom) * ray - p_down_z;
    const auto dp = spp_scale * (world_from_camera *
                                 rypath_texture_reflect(rypath_texture_reflect(offset, v), u));
    return metal::all(metal::isfinite(dp)) ? dp : metal::float3(0.0f);
}

inline void rypath_texture_approximate_dp(metal::float3 relative_p, metal::float3 n,
                                          metal::float3x3 world_from_camera,
                                          metal::float3x3 camera_from_world,
                                          metal::float3 minimum_dx, metal::float3 minimum_dy,
                                          float spp_scale, thread metal::float3 &dpdx,
                                          thread metal::float3 &dpdy) {
    const auto p_camera = camera_from_world * relative_p;
    const auto direction = rypath_texture_direction(p_camera);
    if (!metal::all(metal::isfinite(direction))) {
        dpdx = dpdy = metal::float3(0.0f);
        return;
    }
    // PBRT's RotateFromTo maps the camera-relative direction to +Z using
    // two reflections, with the inverse obtained by reversing their order.
    const auto z = metal::float3(0, 0, 1);
    const auto reference =
        metal::abs(direction.x) < 0.72f ? metal::float3(1, 0, 0) : metal::float3(0, 1, 0);
    const auto u = reference - direction;
    const auto v = reference - z;
    const auto p_down_z = rypath_texture_reflect(rypath_texture_reflect(p_camera, u), v);
    const auto n_camera = metal::transpose(world_from_camera) * n;
    const auto n_down_z = rypath_texture_reflect(rypath_texture_reflect(n_camera, u), v);
    dpdx = rypath_texture_approximate_axis(p_down_z, n_down_z, minimum_dx, u, v, world_from_camera,
                                           spp_scale);
    dpdy = rypath_texture_approximate_axis(p_down_z, n_down_z, minimum_dy, u, v, world_from_camera,
                                           spp_scale);
}

inline bool rypath_texture_triangle_uv(metal::float3 barycentric, metal::float2 uv0,
                                       metal::float2 uv1, metal::float2 uv2,
                                       thread metal::float2 &st) {
    st = barycentric.x * uv0 + barycentric.y * uv1 + barycentric.z * uv2;
    return metal::all(metal::isfinite(st));
}

inline float rypath_texture_reciprocal_partial(float partial) {
    return metal::abs(partial) < 1e-8f ? 0.0f : metal::precise::divide(1.0f, partial);
}

inline metal::float2 rypath_texture_lerp_uv(metal::float2 a, metal::float2 b, float t) {
    return (1.0f - t) * a + t * b;
}

inline bool rypath_texture_patch_frame(metal::float2 uv, metal::float3 raw_dpdu,
                                       metal::float3 raw_dpdv, bool authored_uv, metal::float2 uv00,
                                       metal::float2 uv10, metal::float2 uv01, metal::float2 uv11,
                                       thread metal::float2 &st, thread metal::float3 &dpdu,
                                       thread metal::float3 &dpdv) {
    st = uv;
    dpdu = raw_dpdu;
    dpdv = raw_dpdv;
    if (!metal::all(metal::isfinite(raw_dpdu)) || !metal::all(metal::isfinite(raw_dpdv))) {
        return false;
    }
    if (!authored_uv) {
        return metal::all(metal::isfinite(st));
    }
    const auto lower = rypath_texture_lerp_uv(uv00, uv01, uv.y);
    const auto upper = rypath_texture_lerp_uv(uv10, uv11, uv.y);
    st = rypath_texture_lerp_uv(lower, upper, uv.x);
    const auto dstdu = upper - lower;
    const auto dstdv =
        rypath_texture_lerp_uv(uv01, uv11, uv.x) - rypath_texture_lerp_uv(uv00, uv10, uv.x);
    if (!metal::all(metal::isfinite(st)) || !metal::all(metal::isfinite(dstdu)) ||
        !metal::all(metal::isfinite(dstdv))) {
        return false;
    }

    // Preserve PBRT's componentwise reciprocals and orientation for authored
    // bilinear UVs. These are texture tangents only.
    const auto dpds = rypath_texture_reciprocal_partial(dstdu.x) * dpdu +
                      rypath_texture_reciprocal_partial(dstdv.x) * dpdv;
    const auto dpdt = rypath_texture_reciprocal_partial(dstdu.y) * dpdu +
                      rypath_texture_reciprocal_partial(dstdv.y) * dpdv;
    const auto candidate_normal = metal::cross(dpds, dpdt);
    if (!metal::all(metal::isfinite(dpds)) || !metal::all(metal::isfinite(dpdt)) ||
        !metal::all(metal::isfinite(candidate_normal))) {
        return false;
    }
    if (metal::any(candidate_normal != 0.0f)) {
        dpdu = dpds;
        dpdv = metal::dot(metal::cross(raw_dpdu, raw_dpdv), candidate_normal) < 0.0f ? -dpdt : dpdt;
    }
    return true;
}

inline float rypath_texture_difference_of_products(float a, float b, float c, float d) {
    const float cd = c * d;
    return metal::fma(a, b, -cd) + metal::fma(-c, d, cd);
}

inline float rypath_texture_sanitize_derivative(float derivative) {
    return metal::isfinite(derivative) ? metal::clamp(derivative, -1e8f, 1e8f) : 0.0f;
}

inline metal::float2 rypath_texture_solve_axis(metal::float3 dpdu, metal::float3 dpdv,
                                               metal::float3 dp, float aa, float ab, float bb,
                                               float inv_det) {
    const float u = metal::dot(dpdu, dp);
    const float v = metal::dot(dpdv, dp);
    return {rypath_texture_sanitize_derivative(inv_det *
                                               rypath_texture_difference_of_products(bb, u, ab, v)),
            rypath_texture_sanitize_derivative(
                inv_det * rypath_texture_difference_of_products(aa, v, ab, u))};
}

inline bool rypath_texture_uv_derivatives(metal::float3 dpdu, metal::float3 dpdv,
                                          metal::float3 dpdx, metal::float3 dpdy,
                                          thread metal::float2 &dstdx,
                                          thread metal::float2 &dstdy) {
    if (!metal::all(metal::isfinite(dpdu)) || !metal::all(metal::isfinite(dpdv))) {
        return false;
    }
    const float aa = metal::dot(dpdu, dpdu);
    const float ab = metal::dot(dpdu, dpdv);
    const float bb = metal::dot(dpdv, dpdv);
    const float det = rypath_texture_difference_of_products(aa, bb, ab, ab);
    const float inv_det = metal::precise::divide(1.0f, det);
    if (!metal::isfinite(inv_det)) {
        dstdx = dstdy = metal::float2(0.0f);
        return true;
    }
    dstdx = rypath_texture_solve_axis(dpdu, dpdv, dpdx, aa, ab, bb, inv_det);
    dstdy = rypath_texture_solve_axis(dpdu, dpdv, dpdy, aa, ab, bb, inv_det);
    return true;
}
