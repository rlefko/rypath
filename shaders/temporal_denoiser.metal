// SPDX-License-Identifier: GPL-3.0-only
// Signed RGB adaptation inspired by Schied et al. (HPG 2017) and Dammertz et al.
// (HPG 2010). No luminance clipping, history clamping, radiance demodulation or
// confidence claim. Temporal history is saved BEFORE every spatial pass.
#include <metal_stdlib>
using namespace metal;
#include "../src/temporal_denoiser_wire.h"
namespace tdw = rypath::temporal_denoiser_wire;

static void temporal_denoiser_fail(device tdw::Counters &status, uint flag) {
    atomic_fetch_or_explicit(&status.failure, flag, memory_order_relaxed);
}
static bool temporal_denoiser_sky(uint4 identity) { return all(identity == uint4(0xffffffffu)); }
static bool temporal_denoiser_normal(float3 normal) {
    const float n = dot(normal, normal);
    return all(isfinite(normal)) && isfinite(n) && n > 0.f;
}
static bool temporal_denoiser_guides(float depth, float3 normal, uint4 identity) {
    return isfinite(depth) && all(isfinite(normal)) &&
           (temporal_denoiser_sky(identity) || (depth > 0.f && temporal_denoiser_normal(normal)));
}
// Relative reversed-depth tolerance plus float roundoff, without an absolute
// floor that would merge very distant surfaces. Minimum-normal only covers underflow.
static float temporal_denoiser_depth_tolerance(float a, float b) {
    const float scale = max(abs(a), abs(b));
    return .02f * scale + max(0x1p-126f, 8.f * 0x1p-23f * scale);
}
static bool temporal_denoiser_same_surface(uint4 a, uint4 b, float za, float zb, float3 na,
                                           float3 nb) {
    if (!all(a == b))
        return false;
    if (temporal_denoiser_sky(a))
        return true;
    if (!temporal_denoiser_guides(za, na, a) || !temporal_denoiser_guides(zb, nb, b))
        return false;
    const float tolerance = temporal_denoiser_depth_tolerance(za, zb);
    return abs(za - zb) <= tolerance && dot(normalize(na), normalize(nb)) >= .9f;
}
// Conservative geometry support: exact instance/primitive/kind/material IDs.
// This sacrifices smoothing across primitive seams instead of inventing correspondence.
static float temporal_denoiser_geometry(uint4 a, uint4 b, float za, float zb, float3 na, float3 nb,
                                        float3 da, float3 db, float3 sa, float3 sb) {
    if (!all(a == b))
        return 0.f;
    float weight = 1.f;
    if (!temporal_denoiser_sky(a)) {
        if (!temporal_denoiser_guides(za, na, a) || !temporal_denoiser_guides(zb, nb, b))
            return 0.f;
        const float tolerance = temporal_denoiser_depth_tolerance(za, zb);
        weight =
            exp(-abs(za - zb) / tolerance) * pow(max(0.f, dot(normalize(na), normalize(nb))), 32.f);
    }
    return weight * exp(-4.f * (dot(abs(da - db), float3(1.f)) + dot(abs(sa - sb), float3(1.f))));
}

kernel void rypath_temporal_denoiser_contract(device tdw::Contract &contract [[buffer(0)]]) {
    contract = {tdw::version,
                sizeof(tdw::Config),
                sizeof(tdw::Counters),
                tdw::output_id + 1u,
                {0, 0, 0, 0}};
}

kernel void rypath_temporal_denoiser_reset(device tdw::Counters &status [[buffer(1)]]) {
    atomic_store_explicit(&status.failure, 0u, memory_order_relaxed);
    atomic_store_explicit(&status.completed, 0u, memory_order_relaxed);
    atomic_store_explicit(&status.history_accepted, 0u, memory_order_relaxed);
    atomic_store_explicit(&status.history_rejected, 0u, memory_order_relaxed);
}

kernel void rypath_temporal_denoiser_accumulate(
    constant tdw::Config &config [[buffer(0)]], device tdw::Counters &status [[buffer(1)]],
    texture2d<float, access::read> color [[texture(tdw::color_id)]],
    texture2d<float, access::read> depth [[texture(tdw::depth_id)]],
    texture2d<float, access::read> motion [[texture(tdw::motion_id)]],
    texture2d<float, access::read> diffuse [[texture(tdw::diffuse_id)]],
    texture2d<float, access::read> specular [[texture(tdw::specular_id)]],
    texture2d<float, access::read> normal [[texture(tdw::normal_id)]],
    texture2d<float, access::read> expected_depth [[texture(tdw::previous_depth_id)]],
    texture2d<uint, access::read> identity [[texture(tdw::identity_id)]],
    texture2d<float, access::read> previous_mean [[texture(tdw::history_mean_id)]],
    texture2d<float, access::read> previous_variance [[texture(tdw::history_variance_id)]],
    texture2d<uint, access::read> previous_count [[texture(tdw::history_count_id)]],
    texture2d<float, access::read> previous_z [[texture(tdw::history_depth_id)]],
    texture2d<float, access::read> previous_normal [[texture(tdw::history_normal_id)]],
    texture2d<uint, access::read> previous_identity [[texture(tdw::history_identity_id)]],
    texture2d<float, access::write> next_mean [[texture(tdw::next_mean_id)]],
    texture2d<float, access::write> next_variance [[texture(tdw::next_variance_id)]],
    texture2d<uint, access::write> next_count [[texture(tdw::next_count_id)]],
    texture2d<float, access::write> next_z [[texture(tdw::next_depth_id)]],
    texture2d<float, access::write> next_normal [[texture(tdw::next_normal_id)]],
    texture2d<uint, access::write> next_identity [[texture(tdw::next_identity_id)]],
    texture2d<float, access::write> filtered_mean [[texture(tdw::destination_mean_id)]],
    texture2d<float, access::write> filtered_variance [[texture(tdw::destination_variance_id)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (pixel.x >= config.width || pixel.y >= config.height)
        return;
    if (config.version != tdw::version || config.history_limit == 0u ||
        config.history_limit > tdw::maximum_history || config.iteration > tdw::maximum_iterations) {
        temporal_denoiser_fail(status, tdw::invalid_input);
        return;
    }
    const float4 input = color.read(pixel);
    const float2 displacement = motion.read(pixel).xy;
    const float z = depth.read(pixel).x;
    const float expected = expected_depth.read(pixel).x;
    const float3 n = normal.read(pixel).xyz;
    const float3 d = diffuse.read(pixel).xyz, s = specular.read(pixel).xyz;
    const uint4 id = identity.read(pixel);
    if (!all(isfinite(input)) || input.w != 1.f || !all(isfinite(displacement)) ||
        !all(isfinite(d)) || !all(isfinite(s)) || !isfinite(expected) ||
        !temporal_denoiser_guides(z, n, id)) {
        temporal_denoiser_fail(status, tdw::invalid_input);
        return;
    }
    float3 mean = input.xyz, variance = 0.f;
    uint count = 1u;
    bool accepted = false;
    // Motion maps current pixel CENTER to previous pixel CENTER. Floor selects
    // the nearest center, and out-of-raster positions are rejected, never clamped.
    const float2 previous = float2(pixel) + .5f + displacement;
    // A one-frame limit is exact replacement: do not subtract an old HDR mean
    // only to cancel it again, or form unused overflowing moments at alpha=1.
    if (config.history_limit > 1 && !config.reset_history && all(isfinite(previous)) &&
        all(previous >= 0.f) && previous.x < float(config.width) &&
        previous.y < float(config.height)) {
        const uint2 q = uint2(floor(previous));
        if (temporal_denoiser_same_surface(id, previous_identity.read(q), expected,
                                           previous_z.read(q).x, n, previous_normal.read(q).xyz)) {
            const float3 old_mean = previous_mean.read(q).xyz;
            const float3 old_variance = previous_variance.read(q).xyz;
            const uint old_count = previous_count.read(q).x;
            if (!all(isfinite(old_mean)) || !all(isfinite(old_variance)) ||
                any(old_variance < 0.f) || old_count == 0u || old_count > config.history_limit) {
                temporal_denoiser_fail(status, tdw::invalid_history);
                return;
            }
            count = min(old_count + 1u, config.history_limit);
            const float alpha = 1.f / float(count);
            const float3 delta = input.xyz - old_mean;
            mean = old_mean + alpha * delta;
            variance = (1.f - alpha) * (old_variance + alpha * delta * delta);
            accepted = true;
        }
    }
    if (!all(isfinite(mean)) || !all(isfinite(variance)) || any(variance < 0.f)) {
        temporal_denoiser_fail(status, tdw::arithmetic_overflow);
        return;
    }
    // Geometry-aware cold variance is only a spatial noise proxy. Persisting it
    // as temporal moments would change the centered recurrence's n=1 initial state.
    // v/n is a noise-scale heuristic, not a confidence interval for correlated
    // Area/MCMC samples. Early histories also use a geometry-aware local proxy.
    float3 spatial_variance = variance / float(count);
    if (count < 4u && config.iteration != 0u) {
        float weight_sum = 1.f;
        float3 local_mean = input.xyz, local_variance = 0.f;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0)
                    continue;
                const int2 candidate = int2(pixel) + int2(x, y);
                if (any(candidate < 0) || candidate.x >= int(config.width) ||
                    candidate.y >= int(config.height))
                    continue;
                const uint2 q = uint2(candidate);
                const float3 value = color.read(q).xyz;
                const float weight = temporal_denoiser_geometry(
                    id, identity.read(q), z, depth.read(q).x, n, normal.read(q).xyz, d,
                    diffuse.read(q).xyz, s, specular.read(q).xyz);
                if (!isfinite(weight)) {
                    temporal_denoiser_fail(status, tdw::invalid_input);
                    return;
                }
                if (weight <= 0.f)
                    continue;
                if (!all(isfinite(value))) {
                    temporal_denoiser_fail(status, tdw::invalid_input);
                    return;
                }
                const float alpha = weight / (weight_sum + weight);
                const float3 delta = value - local_mean;
                local_mean += alpha * delta;
                local_variance = (1.f - alpha) * (local_variance + alpha * delta * delta);
                weight_sum += weight;
            }
        spatial_variance = max(spatial_variance, local_variance / float(count));
        if (!all(isfinite(local_mean)) || !all(isfinite(spatial_variance)) ||
            any(spatial_variance < 0.f)) {
            temporal_denoiser_fail(status, tdw::arithmetic_overflow);
            return;
        }
    }
    next_mean.write(float4(mean, 1), pixel);
    next_variance.write(float4(variance, 0), pixel);
    next_count.write(uint4(count, 0, 0, 0), pixel);
    next_z.write(float4(z, 0, 0, 0), pixel);
    next_normal.write(float4(n, 0), pixel);
    next_identity.write(id, pixel);
    filtered_mean.write(float4(mean, 1), pixel);
    filtered_variance.write(float4(spatial_variance, 0), pixel);
    if (accepted)
        atomic_fetch_add_explicit(&status.history_accepted, 1u, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&status.history_rejected, 1u, memory_order_relaxed);
}

kernel void rypath_temporal_denoiser_spatial(
    constant tdw::Config &config [[buffer(0)]], device tdw::Counters &status [[buffer(1)]],
    texture2d<float, access::read> depth [[texture(tdw::depth_id)]],
    texture2d<float, access::read> diffuse [[texture(tdw::diffuse_id)]],
    texture2d<float, access::read> specular [[texture(tdw::specular_id)]],
    texture2d<float, access::read> normal [[texture(tdw::normal_id)]],
    texture2d<uint, access::read> identity [[texture(tdw::identity_id)]],
    texture2d<float, access::read> source_mean [[texture(tdw::source_mean_id)]],
    texture2d<float, access::read> source_variance [[texture(tdw::source_variance_id)]],
    texture2d<float, access::write> destination_mean [[texture(tdw::destination_mean_id)]],
    texture2d<float, access::write> destination_variance [[texture(tdw::destination_variance_id)]],
    uint2 pixel [[thread_position_in_grid]]) {
    if (pixel.x >= config.width || pixel.y >= config.height)
        return;
    if (atomic_load_explicit(&status.failure, memory_order_relaxed))
        return;
    const float3 center = source_mean.read(pixel).xyz, v = source_variance.read(pixel).xyz;
    const float z = depth.read(pixel).x;
    const float3 n = normal.read(pixel).xyz, d = diffuse.read(pixel).xyz,
                 s = specular.read(pixel).xyz;
    const uint4 id = identity.read(pixel);
    constexpr float kernel_weight[5] = {1.f, 4.f, 6.f, 4.f, 1.f};
    float weight_sum = 0.f;
    float3 mean = center, variance = 0.f;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            const int2 candidate = int2(pixel) + int2(x, y) * int(config.step);
            if (any(candidate < 0) || candidate.x >= int(config.width) ||
                candidate.y >= int(config.height))
                continue;
            const uint2 q = uint2(candidate);
            const float geometry = temporal_denoiser_geometry(
                id, identity.read(q), z, depth.read(q).x, n, normal.read(q).xyz, d,
                diffuse.read(q).xyz, s, specular.read(q).xyz);
            if (!isfinite(geometry) || geometry < 0.f) {
                temporal_denoiser_fail(status, tdw::invalid_input);
                return;
            }
            if (geometry == 0.f)
                continue;
            const float3 value = source_mean.read(q).xyz,
                         neighbor_variance = source_variance.read(q).xyz;
            const float3 delta = value - center;
            const float3 noise = 4.f * (v + neighbor_variance) + 1e-4f;
            const float3 scaled = delta * delta / noise;
            const float chromatic_distance = dot(scaled, float3(1.f));
            if (!all(isfinite(value)) || !all(isfinite(v)) || !all(isfinite(neighbor_variance)) ||
                any(v < 0.f) || any(neighbor_variance < 0.f) || !all(isfinite(noise)) ||
                !all(isfinite(scaled)) || !isfinite(chromatic_distance)) {
                temporal_denoiser_fail(status, tdw::arithmetic_overflow);
                return;
            }
            const float weight = kernel_weight[x + 2] * kernel_weight[y + 2] / 256.f * geometry *
                                 exp(-chromatic_distance);
            if (!isfinite(weight) || weight < 0.f) {
                temporal_denoiser_fail(status, tdw::invalid_input);
                return;
            }
            if (weight == 0.f)
                continue;
            const float alpha = weight / (weight_sum + weight);
            mean += alpha * (value - mean);
            variance = (1.f - alpha) * (1.f - alpha) * variance + alpha * alpha * neighbor_variance;
            weight_sum += weight;
        }
    if (!(weight_sum > 0.f) || !all(isfinite(mean)) || !all(isfinite(variance)) ||
        any(variance < 0.f)) {
        temporal_denoiser_fail(status, tdw::arithmetic_overflow);
        return;
    }
    destination_mean.write(float4(mean, 1), pixel);
    destination_variance.write(float4(variance, 0), pixel);
}

kernel void rypath_temporal_denoiser_finish(constant tdw::Config &config [[buffer(0)]],
                                            device tdw::Counters &status [[buffer(1)]],
                                            texture2d<float, access::read> source_mean
                                            [[texture(tdw::source_mean_id)]],
                                            texture2d<float, access::write> output
                                            [[texture(tdw::output_id)]],
                                            uint2 pixel [[thread_position_in_grid]]) {
    if (pixel.x >= config.width || pixel.y >= config.height)
        return;
    if (atomic_load_explicit(&status.failure, memory_order_relaxed))
        return;
    const float4 value = source_mean.read(pixel);
    if (!all(isfinite(value))) {
        temporal_denoiser_fail(status, tdw::arithmetic_overflow);
        return;
    }
    output.write(value, pixel);
    atomic_fetch_add_explicit(&status.completed, 1u, memory_order_relaxed);
}
