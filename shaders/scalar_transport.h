// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace rypath_scalar {
struct ScalarResources {
    constant wire::Parameters *p;
    instance_acceleration_structure scene;
    const device wire::Instance *instances;
    const device wire::Point *positions;
    const device wire::Triangle *triangles;
    const device wire::Material *materials;
    const device float *environment;
    const device wire::SpectralKnot *knots;
    const device wire::AreaEmission *emissions;
    const device wire::Emitter *emitters;
    const device wire::Point *normals;
    const device sobol::Tables *tables;
    const device wire::Patch *patches;
    MixedIntersectionTable intersection_table;
    const device wire::Point2 *texture_uvs;
    const device wire::TextureFrame *texture_frames;
    const device wire::ImageTexture *image_textures;
    const device wire::TextureLevel *image_levels;
    const device wire::Word *image_texels;
    const device float *image_transfer;
    const device float *image_spectra;
    constant wire::TextureParameters *texture_p;
    const device graph_wire::Node *graph_nodes;
    const device wire::SpectralKnot *graph_knots;
    constant wire::TextureGraphParameters *graph_p;
    const device wire::SurfaceVertex *surface_vertices;
    const device wire::SurfaceTransform *surface_transforms;
    const device float *environment_cdf;
    const device float *illuminant;
    const device uint *material_data;
    thread const rypath_replay::ReplayState *replay = nullptr;
};

// Only value state crosses dispatch boundaries; resources and thread pointers are rebuilt.
struct ScalarState {
    wire::Sample sample;
    ulong index;
    float3 ray_origin, ray_direction;
    float ray_min_distance, ray_max_distance;
    float3 primary_direction;
    float throughput, escape_weight, previous_bsdf_pdf;
    bool previous_delta;
    rypath_light_sampling::Context previous_light_context;
    float E;
    uint current_medium, medium_draw, boundary_crossings;
    SurfaceHit hit;
    bool finished;
};

inline ray scalar_ray(const thread ScalarState &state) {
    return ray(state.ray_origin, state.ray_direction, state.ray_min_distance,
               state.ray_max_distance);
}
inline void scalar_store_ray(thread ScalarState &state, ray incoming) {
    state.ray_origin = incoming.origin;
    state.ray_direction = incoming.direction;
    state.ray_min_distance = incoming.min_distance;
    state.ray_max_distance = incoming.max_distance;
}

inline ScalarState scalar_init(const thread ScalarResources &r, uint lane) {
    ScalarState state{};
    state.sample = r.replay ? begin_replay_sample(*r.p, *r.tables, lane, *r.replay)
                            : begin_sample(*r.p, *r.tables, lane, state.index);
    const ray incoming = primary_ray(*r.p, state.sample);
    scalar_store_ray(state, incoming);
    state.primary_direction = incoming.direction;
    state.throughput = 1.0f;
    state.escape_weight = 1.0f;
    state.previous_light_context = {incoming.origin, float3(0)};
    state.E = sky_radiance(r.environment, *r.p, state.sample.wavelength);
    state.current_medium = r.p->camera_medium;
    return state;
}

inline void scalar_finish(thread ScalarState &state) {
    if (state.sample.status == wire::success &&
        (!isfinite(state.sample.radiance) || state.sample.radiance < 0.0f))
        state.sample.status = wire::invalid_arithmetic;
    state.finished = true;
}

// Geometry-free rendering has its own established estimator: the light
// selector shares flight_words.y and phase uses the NEXT medium block. General
// surface transport consumes a separate light block and evaluates general MIS.
// Preserve the sky estimator and random stream in both offline and queued modes.
inline void scalar_sky_step(const thread ScalarResources &r, thread ScalarState &state) {
    if (state.finished)
        return;
    const constant auto &p = *r.p;
    thread auto &sample = state.sample;
    thread auto &throughput = state.throughput;
    thread auto &draw = state.medium_draw;
    ray incoming = scalar_ray(state);
    const auto material_data = r.material_data;
    const rypath_image_light::Images light_images{r.image_textures, r.image_levels,  r.image_texels,
                                                  r.image_transfer, r.image_spectra, r.illuminant,
                                                  r.texture_p};
    if (p.model == wire::pbrt_model &&
        (!all(isfinite(incoming.origin)) || !all(isfinite(incoming.direction)))) {
        sample.status = wire::invalid_arithmetic;
        scalar_finish(state);
        return;
    }
    // Positive extinction in an all-space medium never reaches the environment.
    if (media_enabled && p.camera_medium != 0xffffffffu) {
        const auto m = medium_record(material_data, p, p.camera_medium);
        if (m == nullptr) {
            sample.status = wire::invalid_hit;
            scalar_finish(state);
            return;
        }
        const uint wavelength = dense_index(sample.wavelength);
        const float a = m->sigma_a[wavelength], s = m->sigma_s[wavelength], total = a + s;
        if (total > 0) {
            const auto segment = rypath_medium::segment(a, s, m->emission[wavelength], INFINITY);
            if (!segment.valid)
                sample.status = wire::invalid_arithmetic;
            do {
                if (sample.status != wire::success)
                    break;

                if (sample.scatterings < p.max_depth)
                    sample.radiance += throughput * segment.emission;
                if (sample.scatterings >= p.max_depth || s == 0)
                    break;
                if (sample.scatterings >= p.emergency_limit) {
                    sample.status = wire::emergency_depth;
                    break;
                }
                const auto flight_words = medium_words(p, sample, draw, r.replay);
                const float distance = rypath_medium::flight(total, u01(flight_words.x));
                const float3 point = fma(float3(distance), incoming.direction, incoming.origin);
                if (!isfinite(distance) || !all(isfinite(point))) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                throughput *= s / total;
                const auto chosen = rypath_light_sampling::sample(
                    p, material_data, {point, float3(0)}, u01(flight_words.y));
                if (chosen.pmf > 0) {
                    float3 direction(0);
                    float radiance = 0, distance_to_light = 0;
                    if (chosen.light >= p.emitter_count &&
                        chosen.light < p.emitter_count + p.positional_count) {
                        const auto light =
                            positional_record(material_data, p, chosen.light - p.emitter_count);
                        if (!light) {
                            sample.status = wire::invalid_hit;
                            break;
                        }
                        float distance_squared;
                        float intensity = light->intensity[wavelength];
                        if (intensity != 0) {
                            if (!point_direction(point, unpack(light->position), direction,
                                                 distance_squared,
                                                 light->kind != wire::point_light) ||
                                !positional_intensity(*light, direction, light_images,
                                                      sample.wavelength, intensity)) {
                                sample.status = wire::invalid_arithmetic;
                                break;
                            }
                            radiance = intensity / distance_squared;
                            distance_to_light = sqrt(distance_squared);
                        }
                    } else if (chosen.light >= p.emitter_count + p.positional_count &&
                               chosen.light <
                                   p.emitter_count + p.positional_count + p.distant_count) {
                        const auto light = distant_record(
                            material_data, p, chosen.light - p.emitter_count - p.positional_count);
                        if (!light) {
                            sample.status = wire::invalid_hit;
                            break;
                        }
                        direction = unpack(light->direction);
                        radiance = light->radiance[wavelength];
                        distance_to_light = 2 * p.distant_scene_radius;
                    }
                    // An infinite environment cannot cross a medium with
                    // positive extinction; its selected sample contributes zero.
                    if (radiance > 0) {
                        const float tr = exp(-total * distance_to_light);
                        sample.radiance +=
                            throughput *
                            rypath_medium::phase(-incoming.direction, direction, m->g) *
                            (radiance / chosen.pmf) * tr;
                    }
                    if (!isfinite(sample.radiance) || sample.radiance < 0) {
                        sample.status = wire::invalid_arithmetic;
                        break;
                    }
                }
                const auto phase_words = medium_words(p, sample, draw, r.replay);
                const float3 wi = rypath_medium::phase_sample(
                    -incoming.direction, m->g, float2(u01(phase_words.x), u01(phase_words.y)));
                ++sample.scatterings;
                if (sample.scatterings >= 3) {
                    if (phase_words.z >= 0xc0000000u)
                        break;
                    throughput /= .75f;
                }
                if (!all(isfinite(wi)) || !isfinite(throughput)) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                incoming = ray(point, wi);
                scalar_store_ray(state, incoming);
                return;
            } while (false);
            scalar_finish(state);
            return;
        }
    }
    if (environment_image_enabled) {
        sample.radiance = rypath_image_light::environment_radiance(
            light_images, *r.graph_p, incoming.direction, sample.wavelength);
    } else {
        sample.radiance = sky_radiance(r.environment, p, sample.wavelength);
    }
    scalar_finish(state);
}

// Call trace before shade for every active event, including depth-neutral boundaries.
inline void scalar_trace(const thread ScalarResources &r, thread ScalarState &state) {
    if (state.finished)
        return;
    const constant auto &p = *r.p;
    const auto scene = r.scene;
    const auto instances = r.instances;
    const auto positions = r.positions;
    const auto triangles = r.triangles;
    const auto materials = r.materials;
    const auto normals = r.normals;
    const auto patches = r.patches;
    const auto intersection_table = r.intersection_table;
    const auto texture_uvs = r.texture_uvs;
    const auto texture_frames = r.texture_frames;
    const auto image_textures = r.image_textures;
    const auto image_levels = r.image_levels;
    const auto image_texels = r.image_texels;
    const auto image_transfer = r.image_transfer;
    const auto image_spectra = r.image_spectra;
    const constant auto &texture_p = *r.texture_p;
    const auto graph_nodes = r.graph_nodes;
    const auto graph_knots = r.graph_knots;
    const constant auto &graph_p = *r.graph_p;
    const auto surface_vertices = r.surface_vertices;
    const auto surface_transforms = r.surface_transforms;
    const auto material_data = r.material_data;
    thread auto &sample = state.sample;
    ray incoming = scalar_ray(state);
    thread auto &throughput = state.throughput;
    const GraphResources graph{
        graph_nodes,
        graph_knots,
        &graph_p,
        {image_textures, image_levels, image_texels, image_transfer, image_spectra, &texture_p}};
    SurfaceAlpha alpha{
        materials, material_data, instances,      positions,        triangles,          patches,
        normals,   texture_uvs,   texture_frames, surface_vertices, surface_transforms, &p,
        &sample,   graph,         incoming};
    MixedIntersector closest;
    closest.set_triangle_cull_mode(triangle_cull_mode::none);
    if (!all(isfinite(incoming.origin)) || !all(isfinite(incoming.direction)) ||
        !isfinite(throughput) || !isfinite(sample.radiance)) {
        sample.status = wire::invalid_arithmetic;
        scalar_finish(state);
        return;
    }
    alpha.incoming = incoming;
    state.hit =
        p.instance_count == 0 ? SurfaceHit{intersection_type::none, 0, 0, 0, 0, float2(0), true}
        : alpha_enabled       ? trace_masked_surface(incoming, scene, all_visible, alpha)
                        : trace_surface(closest, incoming, scene, all_visible, intersection_table);
    if (!state.hit.valid) {
        sample.status = wire::invalid_hit;
        scalar_finish(state);
        return;
    }
}

inline void scalar_shade(const thread ScalarResources &r, thread ScalarState &state) {
    if (state.finished)
        return;
    if (r.p->instance_count == 0) {
        scalar_sky_step(r, state);
        return;
    }
    const constant auto &p = *r.p;
    const auto scene = r.scene;
    const auto instances = r.instances;
    const auto positions = r.positions;
    const auto triangles = r.triangles;
    const auto materials = r.materials;
    const auto knots = r.knots;
    const auto emissions = r.emissions;
    const auto emitters = r.emitters;
    const auto normals = r.normals;
    const device auto &tables = *r.tables;
    const auto patches = r.patches;
    const auto intersection_table = r.intersection_table;
    const auto texture_uvs = r.texture_uvs;
    const auto texture_frames = r.texture_frames;
    const auto image_textures = r.image_textures;
    const auto image_levels = r.image_levels;
    const auto image_texels = r.image_texels;
    const auto image_transfer = r.image_transfer;
    const auto image_spectra = r.image_spectra;
    const constant auto &texture_p = *r.texture_p;
    const auto graph_nodes = r.graph_nodes;
    const auto graph_knots = r.graph_knots;
    const constant auto &graph_p = *r.graph_p;
    const auto surface_vertices = r.surface_vertices;
    const auto surface_transforms = r.surface_transforms;
    const auto environment_cdf = r.environment_cdf;
    const auto illuminant = r.illuminant;
    const auto material_data = r.material_data;
    thread auto &sample = state.sample;
    thread auto &index = state.index;
    ray incoming = scalar_ray(state);
    thread auto &primary_direction = state.primary_direction;
    thread auto &throughput = state.throughput;
    thread auto &escape_weight = state.escape_weight;
    thread auto &previous_bsdf_pdf = state.previous_bsdf_pdf;
    thread auto &previous_delta = state.previous_delta;
    thread auto &previous_light_context = state.previous_light_context;
    thread auto &E = state.E;
    thread auto &current_medium = state.current_medium;
    thread auto &medium_draw = state.medium_draw;
    thread auto &boundary_crossings = state.boundary_crossings;
    const GraphResources graph{
        graph_nodes,
        graph_knots,
        &graph_p,
        {image_textures, image_levels, image_texels, image_transfer, image_spectra, &texture_p}};
    SurfaceAlpha alpha{
        materials, material_data, instances,      positions,        triangles,          patches,
        normals,   texture_uvs,   texture_frames, surface_vertices, surface_transforms, &p,
        &sample,   graph,         incoming};
    const rypath_image_light::Images light_images{image_textures, image_levels,  image_texels,
                                                  image_transfer, image_spectra, illuminant,
                                                  &texture_p};
    MixedIntersector shadow;
    shadow.set_triangle_cull_mode(triangle_cull_mode::none);
    shadow.accept_any_intersection(true);
    const auto hit = state.hit;
    // One outer event per call. Nested material-loop breaks retain their original scope.
    while (true) {
        if (media_enabled && current_medium != 0xffffffffu) {
            const auto m = medium_record(material_data, p, current_medium);
            if (m == nullptr) {
                sample.status = wire::invalid_hit;
                break;
            }
            if (medium_draw >= rypath::sampling_contract::medium_block_limit - 3) {
                sample.status = wire::boundary_exhaustion;
                break;
            }
            const uint wavelength = dense_index(sample.wavelength);
            const float a = m->sigma_a[wavelength], s = m->sigma_s[wavelength], total = a + s;
            const float ray_length = length(incoming.direction);
            const float distance =
                hit.type == intersection_type::none ? INFINITY : hit.distance * ray_length;
            const auto segment = rypath_medium::segment(
                a, s, sample.scatterings < p.max_depth ? m->emission[wavelength] : 0, distance);
            if (!segment.valid || !(ray_length > 0) || !isfinite(ray_length)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            sample.radiance += throughput * segment.emission;
            if (total > 0) {
                const auto flight_words = medium_words(p, sample, medium_draw, r.replay);
                const float flight = rypath_medium::flight(total, u01(flight_words.x));
                if (flight < distance || isinf(distance)) {
                    if (!isfinite(flight)) {
                        sample.status = wire::invalid_arithmetic;
                        break;
                    }
                    if (sample.scatterings >= p.max_depth || s == 0)
                        break;
                    if (sample.scatterings >= p.emergency_limit) {
                        sample.status = wire::emergency_depth;
                        break;
                    }
                    const float3 direction = incoming.direction / ray_length;
                    const float3 position = fma(float3(flight), direction, incoming.origin);
                    if (!all(isfinite(position))) {
                        sample.status = wire::invalid_arithmetic;
                        break;
                    }
                    throughput *= s / total;
                    const auto light_words = medium_words(p, sample, medium_draw, r.replay);
                    const auto phase_words = medium_words(p, sample, medium_draw, r.replay);
                    const auto direct =
                        medium_direct(position, -direction, m->g, current_medium, light_words,
                                      scene, intersection_table, alpha, materials, material_data,
                                      emissions, emitters, light_images, environment_cdf);
                    if (direct.status != wire::success) {
                        sample.status = direct.status;
                        break;
                    }
                    sample.radiance += throughput * direct.value;
                    const float3 wi = rypath_medium::phase_sample(
                        -direction, m->g, float2(u01(phase_words.x), u01(phase_words.y)));
                    previous_bsdf_pdf = rypath_medium::phase(-direction, wi, m->g);
                    if (!all(isfinite(wi)) || !(previous_bsdf_pdf > 0) ||
                        !isfinite(previous_bsdf_pdf)) {
                        sample.status = wire::invalid_arithmetic;
                        break;
                    }
                    ++sample.scatterings;
                    if (sample.scatterings >= 3) {
                        if (phase_words.z >= 0xc0000000u)
                            break;
                        throughput /= .75f;
                    }
                    previous_delta = false;
                    previous_light_context = {position, float3(0)};
                    escape_weight = 1;
                    incoming = ray(position, wi);
                    boundary_crossings = 0;
                    scalar_store_ray(state, incoming);
                    return;
                }
            }
        }
        if (hit.type == intersection_type::none) {
            const float Le = environment_image_enabled
                                 ? rypath_image_light::environment_radiance(
                                       light_images, graph_p, incoming.direction, sample.wavelength)
                                 : E;
            if (p.model == wire::pbrt_model && sample.scatterings != 0u && !previous_delta) {
                const float pdf = rypath_scalar_light::environment_pdf(
                    p, material_data, previous_light_context, graph_p, environment_cdf,
                    incoming.direction);
                escape_weight = power_weight(previous_bsdf_pdf, pdf);
            }
            sample.radiance += throughput * escape_weight * Le;
            break;
        }
        const uint bounce = sample.scatterings;
        const bool procedural_hit = hit.type == intersection_type::bounding_box;
        if (!hit.valid || (!procedural_hit && hit.type != intersection_type::triangle) ||
            hit.instance_id >= p.instance_count || !isfinite(hit.distance)) {
            sample.status = wire::invalid_hit;
            break;
        }

        const device wire::Instance &instance = instances[hit.instance_id];
        const uint kind = hit.primitive_kind;
        const uint hit_count = primitive_count(instance, kind);
        if (hit.geometry_id != 0 || hit.primitive_id >= hit_count ||
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
        if (media_enabled && materials[instance.material].scattering == 10u) {
            if (boundary_crossings++ >= p.boundary_limit) {
                sample.status = wire::boundary_exhaustion;
                break;
            }
            if (!surface_point(hit_uv, kind, instance, hit.primitive_id, positions, triangles,
                               patches, normals, p.normal_count, PointSource::native_hit, point,
                               material_data)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            current_medium = outgoing_medium(instance, current_medium, point.geometric_normal,
                                             incoming.direction);
            float3 origin;
            if (!spawn_boundary(incoming, hit.distance, point, incoming.direction, origin)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            incoming = ray(origin, incoming.direction);
            scalar_store_ray(state, incoming);
            return;
        }
        const bool emissive = instance.emission != wire::no_emission;
        if (emissive) {
            if (instance.emission >= p.emission_count || p.emitter_count == 0u) {
                sample.status = wire::invalid_hit;
                break;
            }
            if (!surface_point(hit_uv, kind, instance, hit.primitive_id, positions, triangles,
                               patches, normals, p.normal_count, PointSource::native_hit, point,
                               material_data)) {
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
                    const uint light_id =
                        instance.emitter_begin +
                        (kind == wire::triangle_primitive ? hit.primitive_id
                         : kind == wire::patch_primitive
                             ? instance.triangle_count + hit.primitive_id
                             : instance.triangle_count + instance.patch_count + hit.primitive_id);
                    if (light_id >= p.emitter_count) {
                        sample.status = wire::invalid_hit;
                        break;
                    }
                    const auto emitter = emitters[light_id];
                    const float pdf =
                        rypath_light_sampling::pmf(p, material_data, previous_light_context,
                                                   light_id) *
                        rypath_light_geometry::pdf(alpha, emitter, previous_light_context.position,
                                                   previous_light_context.normal, hit_uv, point);
                    weight = power_weight(previous_bsdf_pdf, pdf);
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
        const device wire::Material &root_material = materials[instance.material];
        const bool root_framed = root_material.scattering != 0;
        const bool context_material = root_framed || root_material.kind == wire::image_material ||
                                      root_material.kind == wire::graph_material ||
                                      root_material.bump_root != 0xffffffffu ||
                                      root_material.normal_map != 0xffffffffu;
        if (context_material && !emissive &&
            !surface_point(hit_uv, kind, instance, hit.primitive_id, positions, triangles, patches,
                           normals, p.normal_count, PointSource::native_hit, point,
                           material_data)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        graph_wire::Context texture_context{};
        float3 geometric_dpdu(0), geometric_dpdv(0);
        if ((root_framed || root_material.kind == wire::graph_material ||
             root_material.bump_root != 0xffffffffu || root_material.normal_map != 0xffffffffu) &&
            !surface_texture_context(instance, hit.primitive_id, kind, hit_uv, point, positions,
                                     triangles, patches, texture_uvs, texture_frames, texture_p, p,
                                     sample, primary_direction, true, texture_context,
                                     geometric_dpdu, geometric_dpdv)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        uint material_index = instance.material;
        for (uint depth = 0; depth <= 64; ++depth) {
            const device auto &node = materials[material_index];
            if (!surface_materials_enabled || node.scattering != 9)
                break;
            auto aux = material_auxiliary(node, material_data, p.material_data_words);
            float amount;
            if (aux == nullptr || depth == 64 ||
                !parameter_value(aux->mix_amount_root, aux->mix_amount, graph, texture_context,
                                 sample.wavelength, amount)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            const float selector =
                alpha_uniform(point.position, -incoming.direction, as_type<float>(material_index));
            material_index = amount <= 0         ? aux->first_material
                             : amount >= 1       ? aux->second_material
                             : selector < amount ? aux->second_material
                                                 : aux->first_material;
            if (material_index >= p.material_count) {
                sample.status = wire::invalid_hit;
                break;
            }
        }
        if (sample.status != wire::success)
            break;
        const device wire::Material &material = materials[material_index];
        const bool dielectric = dielectric_enabled && material.scattering == 2u;
        const bool microfacet = (conductor_enabled || dielectric_enabled ||
                                 surface_materials_enabled || gltf_enabled) &&
                                material.scattering != 0;
        const uint4 material_random =
            r.replay ? rypath_replay_sampler::vector(
                           rypath_replay_sampler::material(*r.replay, bounce, material_index, 0))
                     : philox(uint4(sample.pixel, sample.sample_low, sample.sample_high,
                                    rypath::sampling_contract::material_local_block(bounce)),
                              uint2(p.seed_low ^ material_index,
                                    p.seed_high ^ rypath::sampling_contract::material_stream_v1));
        float rho = NAN;
        if (microfacet && material.scattering != 4 && material.scattering != 5) {
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
            if (material.scattering != 4)
                rho = clamp(rho, 0.0f, 1.0f);
        } else {
            sample.status = wire::invalid_hit;
            break;
        }
        if (!isfinite(rho) || (material.scattering != 4 && !(rho >= 0.0f && rho <= 1.0f))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        if (rho == 0.0f && material.scattering == 0) {
            break;
        }
        const bool detail = surface_detail_enabled && (material.bump_root != 0xffffffffu ||
                                                       material.normal_map != 0xffffffffu);
        rypath_surface_detail::Frame shading_frame;
        if ((detail || microfacet) &&
            !apply_surface_detail(
                hit.instance_id, instance, hit.primitive_id, kind, hit_uv, triangles, patches,
                texture_uvs, surface_vertices, surface_transforms, material, graph, texture_context,
                sample.wavelength, point, shading_frame, microfacet,
                material_auxiliary(material, material_data, p.material_data_words),
                incoming.direction)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const bool authored_normals =
            instance.normal_begin != wire::no_normals || detail || microfacet;
        if (!authored_normals && bounce >= p.emergency_limit) {
            sample.status = wire::emergency_depth;
            break;
        }
        if (!emissive && !context_material &&
            !surface_point(hit_uv, kind, instance, hit.primitive_id, positions, triangles, patches,
                           normals, p.normal_count, PointSource::native_hit, point,
                           material_data)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const float incident_cosine = dot(point.shading_normal, incoming.direction);
        if (authored_normals && !microfacet && incident_cosine == 0.0f) {
            break;
        }
        if (bounce >= p.emergency_limit) {
            sample.status = wire::emergency_depth;
            break;
        }
        const float3 n = incident_cosine > 0.0f ? -point.shading_normal : point.shading_normal;
        rypath_scattering::Prepared scattering{};
        ScatteringFrame frame{};
        float3 wo(0);
        if (microfacet) {
            if (!prepare_scattering(
                    material, material_auxiliary(material, material_data, p.material_data_words),
                    rho, knots, p, graph, texture_context, sample.wavelength, material_data,
                    image_spectra, scattering) ||
                !scattering_frame(shading_frame, frame)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            wo = frame.local(-incoming.direction);
            if (wo.z == 0)
                break;
        }
        bool delta = microfacet && rypath_scattering::delta_only(scattering);
        const bool both_hemispheres = microfacet && rypath_scattering::both_hemispheres(scattering);
        const bool directional_offset = authored_normals || procedural_hit;
        const float3 area_reference =
            directional_offset ? point.position : spawn_origin(incoming, hit.distance, point, n);
        if (!all(isfinite(area_reference))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const uint4 light_words = draw_words(p, tables, sample, index, 2 + 2 * bounce, r.replay);
        const uint4 bsdf_words = draw_words(p, tables, sample, index, 1 + 2 * bounce, r.replay);
        const rypath_light_sampling::Context light_context{
            point.position, microfacet ? frame.z : point.shading_normal};
        if (p.model == wire::demo_model) {
            float3 light_direction;
            float light_pdf = 1.0f / (4.0f * pi);
            float light_radiance = E;
            const float2 light_u(sample_uniform(p, light_words.x),
                                 sample_uniform(p, light_words.y));
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
            const float3 local_light = microfacet ? frame.local(light_direction) : float3(0);
            const float light_cosine = both_hemispheres ? abs(local_light.z)
                                       : microfacet ? (wo.z > 0 ? local_light.z : -local_light.z)
                                                    : dot(n, light_direction);
            if (!delta && light_radiance > 0.0f && light_cosine > 0.0f) {
                const float3 origin = directional_offset ? spawn_toward(incoming, hit.distance,
                                                                        point, light_direction)
                                                         : area_reference;
                if (!all(isfinite(origin))) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                const auto visibility = connection_transmission(
                    shadow, ray(origin, light_direction), scene, intersection_table, alpha,
                    materials, material_data,
                    outgoing_medium(instance, current_medium, point.geometric_normal,
                                    light_direction));
                if (visibility.status != wire::success) {
                    sample.status = visibility.status;
                    break;
                }
                if (visibility.value > 0) {
                    float factor;
                    if (microfacet) {
                        const auto evaluated =
                            evaluate_scattering(scattering, wo, local_light, material_random.y);
                        factor = evaluated.valid
                                     ? scattering_direct(evaluated.f * light_cosine, evaluated.pdf,
                                                         light_pdf, false)
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
                    sample.radiance += throughput * (microfacet ? 1.0f : rho) * factor *
                                       light_radiance * visibility.value;
                }
            }
        } else if (!delta) {
            const auto light = rypath_scalar_light::sample(
                alpha, emissions, emitters, light_images, environment_cdf, light_context,
                sample_uniform(p, light_words.x),
                float2(sample_uniform(p, light_words.y), sample_uniform(p, light_words.z)));
            if (!light.valid) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            if (light.pdf > 0 && light.radiance > 0) {
                const float3 local = microfacet ? frame.local(light.direction) : float3(0);
                const float cosine = both_hemispheres ? abs(local.z)
                                     : microfacet     ? (wo.z > 0 ? local.z : -local.z)
                                                      : dot(n, light.direction);
                if (cosine > 0) {
                    float f_cos = rho * cosine / pi;
                    float bsdf_pdf = cosine / pi;
                    if (microfacet) {
                        const auto evaluated =
                            evaluate_scattering(scattering, wo, local, material_random.z);
                        if (!evaluated.valid) {
                            sample.status = wire::invalid_arithmetic;
                            break;
                        }
                        f_cos = evaluated.f * cosine;
                        bsdf_pdf = evaluated.pdf;
                    }
                    const float factor =
                        scattering_direct(f_cos, bsdf_pdf, light.pdf,
                                          rypath_scalar_light::delta(light) || light.invisible);
                    const float3 origin = directional_offset ? spawn_toward(incoming, hit.distance,
                                                                            point, light.direction)
                                                             : area_reference;
                    const auto connection = rypath_scalar_light::connection(light, origin);
                    if (!isfinite(factor) || !all(isfinite(connection.direction)) ||
                        !(dot(connection.direction, light.direction) > 0)) {
                        sample.status = wire::invalid_arithmetic;
                        break;
                    }
                    const auto visibility = connection_transmission(
                        shadow, connection, scene, intersection_table, alpha, materials,
                        material_data,
                        outgoing_medium(instance, current_medium, point.geometric_normal,
                                        light.direction),
                        light.kind == rypath_scalar_light::distant, light.endpoint);
                    if (visibility.status != wire::success) {
                        sample.status = visibility.status;
                        break;
                    }
                    sample.radiance += throughput * factor * light.radiance * visibility.value;
                }
            }
        }
        float3 wi;
        float continuation_pdf = 0;
        float continuation_cosine = 0;
        if (microfacet) {
            const float lobe =
                (dielectric || material.scattering == 11)
                    ? sample_uniform(
                          p, dielectric_lobe_word(p, tables, sample, index, bounce, r.replay))
                    : u01(material_random.x);
            const auto local_continuation = sample_scattering(
                scattering, wo, lobe,
                float2(sample_uniform(p, bsdf_words.x), sample_uniform(p, bsdf_words.y)),
                material_random.w);
            const auto resolved = resolve_scattering_continuation(local_continuation, frame);
            const auto continuation = resolved.sample;
            if (!continuation.valid) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            if (!continuation.accepted)
                break;
            wi = continuation.wi;
            throughput *= continuation.weight;
            continuation_pdf = continuation.pdf;
            continuation_cosine = resolved.cosine;
            delta = continuation.delta;
        } else {
            wi = hemisphere(n, sample_uniform(p, bsdf_words.x), sample_uniform(p, bsdf_words.y));
            throughput *= rho;
        }
        // Transmission can amplify radiance throughput. Detect overflow before
        // roulette can terminate the path and hide the invalid multiplication.
        if (!isfinite(throughput) || throughput < 0.0f) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        ++sample.scatterings;
        if (sample.scatterings >= 3) {
            if (decision_word(p, bsdf_words.z) >= 0xc0000000u) {
                break;
            }
            throughput /= 0.75f;
            if (!isfinite(throughput)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
        }
        const float cosine = microfacet ? continuation_cosine : dot(n, wi);
        if (!(cosine > 0.0f)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        previous_bsdf_pdf = microfacet ? continuation_pdf : cosine / pi;
        previous_delta = delta;
        previous_light_context = light_context;
        escape_weight = delta        ? 1.0f
                        : microfacet ? power_weight(previous_bsdf_pdf, 1.0f / (4.0f * pi))
                                     : bsdf_weight(cosine);
        const float3 origin =
            directional_offset ? spawn_toward(incoming, hit.distance, point, wi) : area_reference;
        if (!all(isfinite(origin))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        incoming = ray(origin, wi);
        if (media_enabled)
            current_medium = outgoing_medium(instance, current_medium, point.geometric_normal, wi);
        boundary_crossings = 0;
        scalar_store_ray(state, incoming);
        return;
    }
    scalar_finish(state);
}

} // namespace rypath_scalar
