// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace rypath_scalar_light {
enum : uint { area = 0, positional = 1, distant = 2, environment = 3 };
struct Incident {
    float3 direction = float3(0), endpoint = float3(0);
    SurfacePoint target{};
    float radiance = 0, pdf = 0;
    uint kind = environment;
    bool invisible = false, valid = true;
};

inline float environment_pdf(constant wire::Parameters &p, const device uint *data,
                             rypath_light_sampling::Context context,
                             constant wire::TextureGraphParameters &graph, const device float *cdf,
                             float3 direction) {
    // Constant environments use BSDF/phase continuation alone under MIS compensation.
    if (!environment_image_enabled || p.environment_light == 0xffffffffu)
        return 0;
    return rypath_light_sampling::pmf(p, data, context, p.environment_light) *
           rypath_image_light::environment_pdf(graph, cdf, direction, true);
}

inline Incident sample(thread SurfaceAlpha &alpha, const device wire::AreaEmission *emissions,
                       const device wire::Emitter *emitters, rypath_image_light::Images images,
                       const device float *cdf, rypath_light_sampling::Context context,
                       float selection, float2 u) {
    Incident result;
    const constant auto &p = *alpha.parameters;
    const auto chosen = rypath_light_sampling::sample(p, alpha.material_data, context, selection);
    if (!(chosen.pmf > 0))
        return result;
    const uint id = chosen.light;
    if (id < p.emitter_count) {
        result.kind = area;
        const auto emitter = emitters[id];
        if (emitter.instance >= p.instance_count) {
            result.valid = false;
            return result;
        }
        const device auto &instance = alpha.instances[emitter.instance];
        if (instance.emission >= p.emission_count) {
            result.valid = false;
            return result;
        }
        float2 uv;
        float conditional = 0;
        if (!rypath_light_geometry::sample(alpha, emitter, context.position, context.normal, u, uv,
                                           result.target, conditional)) {
            result.valid = false;
            return result;
        }
        if (!(conditional > 0))
            return result;
        const float3 d = result.target.position - context.position;
        const float distance_squared = dot(d, d);
        if (!(distance_squared > 0))
            return result;
        result.direction = d / sqrt(distance_squared);
        result.endpoint = result.target.position;
        result.pdf = chosen.pmf * conditional;
        result.invisible = instance.invisible_emitter != 0;
        const device auto &emission = emissions[instance.emission];
        const float cosine = dot(result.target.geometric_normal, -result.direction);
        if (cosine == 0 || (!emission.two_sided && cosine < 0))
            return result;
        result.valid = area_radiance(emission, instance, emitter.primitive, emitter.kind, uv,
                                     result.target, alpha, images, result.radiance);
    } else if (id < p.emitter_count + p.positional_count) {
        result.kind = positional;
        const auto light = positional_record(alpha.material_data, p, id - p.emitter_count);
        if (!light) {
            result.valid = false;
            return result;
        }
        float intensity = light->intensity[dense_index(alpha.sample->wavelength)];
        if (intensity == 0)
            return result;
        result.endpoint = unpack(light->position);
        float distance_squared;
        result.valid = point_direction(context.position, result.endpoint, result.direction,
                                       distance_squared, light->kind != wire::point_light);
        if (!result.valid)
            return result;
        result.valid = positional_intensity(*light, result.direction, images,
                                            alpha.sample->wavelength, intensity);
        result.radiance = intensity / distance_squared;
        result.pdf = chosen.pmf;
    } else if (id < p.emitter_count + p.positional_count + p.distant_count) {
        result.kind = distant;
        const auto light =
            distant_record(alpha.material_data, p, id - p.emitter_count - p.positional_count);
        if (!light) {
            result.valid = false;
            return result;
        }
        result.direction = unpack(light->direction);
        result.endpoint = context.position + result.direction * (2 * p.distant_scene_radius);
        result.radiance = light->radiance[dense_index(alpha.sample->wavelength)];
        result.pdf = chosen.pmf;
    } else if (id == p.environment_light) {
        if (!environment_image_enabled)
            return result;
        float conditional = 0;
        result.valid = rypath_image_light::sample_environment(*alpha.graph.parameters, cdf, u,
                                                              result.direction, conditional, true);
        if (!result.valid)
            return result;
        result.pdf = chosen.pmf * conditional;
        result.radiance = rypath_image_light::environment_radiance(
            images, *alpha.graph.parameters, result.direction, alpha.sample->wavelength);
    } else {
        result.valid = false;
    }
    result.valid = result.valid && isfinite(result.radiance) && result.radiance >= 0 &&
                   isfinite(result.pdf) && result.pdf >= 0;
    return result;
}

inline ray connection(const thread Incident &light, float3 origin) {
    if (light.kind == environment)
        return ray(origin, light.direction);
    if (light.kind == area) {
        const float3 delta = light.target.position - origin;
        return ray(origin,
                   shadow_segment_direction(origin, delta, light.target,
                                            dot(light.target.geometric_normal, -delta)),
                   0, 0x1.fffffep-1f);
    }
    return ray(origin, light.endpoint - origin, 0,
               light.kind == distant ? 1 - 0.0001f : 0x1.fffffep-1f);
}
inline bool delta(const thread Incident &light) {
    return light.kind == positional || light.kind == distant;
}
} // namespace rypath_scalar_light
