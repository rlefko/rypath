// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Include after render.metal. All outputs are reconstruction/presentation data;
// raw XYZ and transport materials are read-only. No new transport estimator.
#include "../src/reconstruction_guide_wire.h"
namespace gw = rypath::reconstruction_guide_wire;
struct ReconstructionGuideArguments {
    device const gw::Config *config [[id(gw::config_id)]];
    device const wire::Point *raw_xyz [[id(gw::raw_xyz_id)]];
    device const gw::SpectralNode *nodes [[id(gw::nodes_id)]];
    device gw::Counters *counters [[id(gw::counters_id)]];
    texture2d<float, access::write> color [[id(gw::color_id)]];
    texture2d<float, access::write> depth [[id(gw::depth_id)]];
    texture2d<float, access::write> motion [[id(gw::motion_id)]];
    texture2d<float, access::write> diffuse [[id(gw::diffuse_id)]];
    texture2d<float, access::write> specular [[id(gw::specular_id)]];
    texture2d<float, access::write> normal [[id(gw::normal_id)]];
    texture2d<float, access::write> roughness [[id(gw::roughness_id)]];
};
inline void reconstruction_guide_fail(constant ReconstructionGuideArguments &a, uint flag) {
    atomic_fetch_or_explicit(&a.counters->failure, flag, memory_order_relaxed);
}
inline bool reconstruction_guide_valid(constant ReconstructionGuideArguments &a) {
    const auto c = *a.config;
    if (c.width == 0 || c.height == 0 || ulong(c.width) * c.height > 0xfffffffful ||
        c.previous_valid > 1 || (c.model != wire::demo_model && c.model != wire::pbrt_model) ||
        c.spectral_nodes != gw::node_count || (c.flags & ~uint(gw::media_scene)) != 0 ||
        c.reserved[0] != 0 || c.reserved[1] != 0) {
        reconstruction_guide_fail(a, gw::invalid_config);
        return false;
    }
    return atomic_load_explicit(&a.counters->failure, memory_order_relaxed) == 0;
}
inline void reconstruction_guide_count(constant ReconstructionGuideArguments &a,
                                       device atomic_uint *counter, uint count = 1) {
    const uint old = atomic_fetch_add_explicit(counter, count, memory_order_relaxed);
    if (old > 0xffffffffu - count)
        reconstruction_guide_fail(a, gw::counter_overflow);
}
inline float4 reconstruction_guide_multiply(device const float *m, float4 v) {
    return float4(m[0], m[1], m[2], m[3]) * v.x + float4(m[4], m[5], m[6], m[7]) * v.y +
           float4(m[8], m[9], m[10], m[11]) * v.z + float4(m[12], m[13], m[14], m[15]) * v.w;
}
inline bool reconstruction_guide_project(device const float *world_to_view,
                                         device const float *view_to_clip, float3 point,
                                         bool background, uint2 dimensions, thread float2 &pixel,
                                         thread float &depth) {
    const auto view =
        reconstruction_guide_multiply(world_to_view, float4(point, background ? 0.f : 1.f));
    const auto clip = reconstruction_guide_multiply(view_to_clip, view);
    if (!all(isfinite(view)) || !all(isfinite(clip)) || !(view.z > 0) || !(clip.w > 0))
        return false;
    const float2 ndc = clip.xy / clip.w;
    pixel = float2((ndc.x + 1) * .5f * dimensions.x, (1 - ndc.y) * .5f * dimensions.y);
    depth = background ? 0 : clip.z / clip.w;
    return all(isfinite(pixel)) && isfinite(depth) && (background || (depth > 0 && depth <= 1));
}
inline bool reconstruction_guide_motion(constant ReconstructionGuideArguments &a, float3 relative,
                                        bool background, thread float &depth,
                                        thread float2 &motion) {
    const auto c = a.config;
    const uint2 dimensions(c->width, c->height);
    const float3 current_offset(c->current_camera_to_matrix_origin[0],
                                c->current_camera_to_matrix_origin[1],
                                c->current_camera_to_matrix_origin[2]);
    const float3 previous_offset(c->previous_camera_to_matrix_origin[0],
                                 c->previous_camera_to_matrix_origin[1],
                                 c->previous_camera_to_matrix_origin[2]);
    float2 current;
    if (!reconstruction_guide_project(c->current_world_to_view, c->current_view_to_clip,
                                      background ? relative : relative + current_offset, background,
                                      dimensions, current, depth))
        return false;
    motion = 0;
    if (c->previous_valid) {
        float2 previous;
        float ignored;
        if (reconstruction_guide_project(c->previous_world_to_view, c->previous_view_to_clip,
                                         background ? relative : relative + previous_offset,
                                         background, dimensions, previous, ignored)) {
            motion = previous - current;
        } else {
            // Explicitly force an off-raster previous lookup. Zero motion here
            // would incorrectly claim correspondence to a visible old pixel.
            motion = -float2(dimensions) - current;
            reconstruction_guide_count(a, &a.counters->invalid_previous);
        }
    }
    return all(isfinite(motion)) && all(abs(motion) <= float2(65504));
}
inline bool reconstruction_guide_context(const thread SurfaceAlpha &resources,
                                         const device wire::Instance &instance, uint primitive,
                                         uint kind, float2 uv, const thread SurfacePoint &point,
                                         thread graph_wire::Context &context) {
    float3 dpdu, dpdv;
    return surface_texture_context(instance, primitive, kind, uv, point, resources.positions,
                                   resources.triangles, resources.patches, resources.uvs,
                                   resources.frames, *resources.graph.images.parameters,
                                   *resources.parameters, *resources.sample,
                                   resources.incoming.direction, true, context, dpdu, dpdv);
}
// Visibility follows the existing deterministic mix selector at the fixed guide
// wavelength. Material GUIDES for any mix use the separately counted neutral
// fallback; a selected null child remains a genuine passthrough surface.
inline bool reconstruction_guide_material(const thread SurfaceAlpha &resources,
                                          const device wire::Instance &instance,
                                          const thread SurfacePoint &point,
                                          graph_wire::Context context, thread uint &material) {
    const constant auto &p = *resources.parameters;
    material = instance.material;
    for (uint depth = 0; depth <= 64; ++depth) {
        if (material >= p.material_count)
            return false;
        const device auto &m = resources.materials[material];
        if (m.scattering != 9)
            return true;
        const auto aux = material_auxiliary(m, resources.material_data, p.material_data_words);
        float amount;
        if (!aux || depth == 64 ||
            !parameter_value(aux->mix_amount_root, aux->mix_amount, resources.graph, context, 550.f,
                             amount))
            return false;
        const float selector =
            alpha_uniform(point.position, -resources.incoming.direction, as_type<float>(material));
        material = amount <= 0         ? aux->first_material
                   : amount >= 1       ? aux->second_material
                   : selector < amount ? aux->second_material
                                       : aux->first_material;
    }
    return false;
}
struct ReconstructionGuideAlpha {
    SurfaceAlpha base;
    thread uint *skipped;
    MaskedAlphaResult operator()(uint ordinal, uint geometry, uint primitive,
                                 intersection_type type, float2 uv, float distance) const {
        const auto result = base(ordinal, geometry, primitive, type, uv, distance);
        if (!result.valid || !result.accepted)
            return result;
        const device auto &instance = base.instances[ordinal];
        if (instance.invisible_emitter) {
            if (*skipped == 0xffffffffu)
                return {false, false};
            ++*skipped;
            return {false, true};
        }
        uint material = instance.material;
        if (base.materials[material].scattering == 9) {
            const uint kind = type == intersection_type::triangle ? wire::triangle_primitive
                              : instance.analytic_count           ? wire::analytic_primitive
                                                                  : wire::patch_primitive;
            SurfacePoint point;
            graph_wire::Context context{};
            if (!surface_point(uv, kind, instance, primitive, base.positions, base.triangles,
                               base.patches, base.normals, base.parameters->normal_count,
                               PointSource::native_hit, point, base.material_data) ||
                !reconstruction_guide_context(base, instance, primitive, kind, uv, point,
                                              context) ||
                !reconstruction_guide_material(base, instance, point, context, material))
                return {false, false};
        }
        if (base.materials[material].scattering == 10) {
            if (*skipped == 0xffffffffu)
                return {false, false};
            ++*skipped;
            return {false, true};
        }
        return result;
    }
};
inline bool reconstruction_guide_reflectance(const thread SurfaceAlpha &resources,
                                             const device wire::Material &m,
                                             const device wire::SpectralKnot *knots,
                                             graph_wire::Context context, float wavelength,
                                             thread float &rho) {
    const constant auto &p = *resources.parameters;
    const thread auto &images = resources.graph.images;
    if (m.scattering != 0 && m.scattering != 4 && m.scattering != 5) {
        rho = 1;
    } else if (p.model == wire::demo_model && m.kind == wire::sampled_material) {
        rho = demo_reflectance(m, p, wavelength);
    } else if (m.kind == wire::sampled_material && m.knot_count >= 2 &&
               m.knot_begin <= p.knot_count && m.knot_count <= p.knot_count - m.knot_begin) {
        rho = pbrt_reflectance(knots + m.knot_begin, m.knot_count, wavelength);
    } else if (m.kind == wire::constant_material) {
        rho = m.scalar_value;
    } else if (m.kind == wire::blackbody_material) {
        rho = blackbody_reflectance(m.scalar_value, wavelength);
    } else if (m.kind == wire::rgb_sigmoid_material) {
        rho = sigmoid_reflectance(m, wavelength);
    } else if (m.kind == wire::image_material &&
               m.image_texture < images.parameters->texture_count) {
        const device auto &texture = images.textures[m.image_texture];
        float3 rgb = rypath_image::lookup(texture, images.levels, images.texels, images.transfer,
                                          *images.parameters, float2(context.u, context.v),
                                          float2(context.dudx, context.dvdx),
                                          float2(context.dudy, context.dvdy));
        rgb *= texture.scale;
        if (texture.invert)
            rgb = 1 - rgb;
        if (!all(isfinite(rgb)))
            return false;
        rho = rypath_image::spectrum(rgb, wavelength, images.spectra);
    } else if (m.kind == wire::graph_material) {
        if (!resources.graph.evaluate(m.texture_root, context, wavelength, rho))
            return false;
        if (m.scattering != 4)
            rho = clamp(rho, 0.f, 1.f);
    } else {
        return false;
    }
    return isfinite(rho) && (m.scattering == 4 || (rho >= 0 && rho <= 1));
}
// These are deterministic optical guide proxies, not integrated BSDF albedos.
// GGX uses max(alpha_x,alpha_y), discarding anisotropy; wavelength-varying
// roughness uses the maximum across nodes. Coatings ignore internal scattering.
inline bool reconstruction_guide_optical(const thread SurfaceAlpha &resources,
                                         const device wire::Material &m,
                                         const device wire::SpectralKnot *knots,
                                         graph_wire::Context context, float wavelength,
                                         thread float &diffuse, thread float &specular,
                                         thread float &roughness) {
    float rho;
    if (!reconstruction_guide_reflectance(resources, m, knots, context, wavelength, rho))
        return false;
    diffuse = rho;
    specular = 0;
    roughness = 1;
    if (m.scattering == 0)
        return true;
    const constant auto &p = *resources.parameters;
    const auto aux = material_auxiliary(m, resources.material_data, p.material_data_words);
    rypath_scattering::Prepared prepared{};
    if (!prepare_scattering(m, aux, rho, knots, p, resources.graph, context, wavelength,
                            resources.material_data, resources.graph.images.spectra, prepared))
        return false;
    diffuse = 0;
    switch (m.scattering) {
    case 1:
        roughness = prepared.conductor.smooth
                        ? 0
                        : max(prepared.conductor.alpha_x, prepared.conductor.alpha_y);
        return rypath_conductor::fresnel(1, prepared.conductor.eta, prepared.conductor.k, specular);
    case 2:
    case 3:
        roughness = m.scattering == 3 || prepared.dielectric.smooth
                        ? 0
                        : max(prepared.dielectric.alpha_x, prepared.dielectric.alpha_y);
        if (!rypath_dielectric::fresnel(1, prepared.dielectric.eta, specular))
            return false;
        if (m.scattering == 3)
            specular = 2 * specular / (1 + specular);
        return true;
    case 4:
        diffuse = prepared.reflectance + prepared.transmittance;
        return isfinite(diffuse);
    case 5:
    case 6: {
        float top;
        if (!rypath_dielectric::fresnel(1, prepared.layered.interface.eta, top))
            return false;
        specular = top;
        const float attenuation = (1 - top) * (1 - top);
        roughness = prepared.layered.interface.smooth ? 0
                                                      : max(prepared.layered.interface.alpha_x,
                                                            prepared.layered.interface.alpha_y);
        if (m.scattering == 5) {
            diffuse = attenuation * prepared.layered.reflectance;
        } else {
            float base;
            if (!rypath_conductor::fresnel(1, prepared.layered.conductor.eta,
                                           prepared.layered.conductor.k, base))
                return false;
            specular += attenuation * base;
            roughness = max(roughness, prepared.layered.conductor.smooth
                                           ? 0
                                           : max(prepared.layered.conductor.alpha_x,
                                                 prepared.layered.conductor.alpha_y));
        }
        return isfinite(diffuse) && isfinite(specular);
    }
    case 11:
        diffuse = .96f * (1 - prepared.gltf.metallic) * prepared.gltf.color;
        specular =
            .04f * (1 - prepared.gltf.metallic) + prepared.gltf.metallic * prepared.gltf.color;
        roughness = prepared.gltf.alpha;
        return isfinite(diffuse) && isfinite(specular);
    default:
        return false;
    }
}
kernel void rypath_reconstruction_guide_contract(device gw::Contract *out [[buffer(0)]]) {
    *out = {gw::contract,
            sizeof(gw::Config),
            sizeof(gw::SpectralNode),
            sizeof(gw::Counters),
            gw::node_count,
            gw::roughness_id + 1,
            {0, 0}};
}
kernel void rypath_reconstruction_guide_reset(constant ReconstructionGuideArguments &a
                                              [[buffer(1)]],
                                              uint lane [[thread_position_in_grid]]) {
    if (lane != 0)
        return;
    auto counters = reinterpret_cast<device atomic_uint *>(a.counters);
    for (uint i = 0; i < sizeof(gw::Counters) / sizeof(uint); ++i)
        atomic_store_explicit(counters + i, 0u, memory_order_relaxed);
    (void)reconstruction_guide_valid(a);
}
kernel void rypath_reconstruction_guide_color(constant ReconstructionGuideArguments &a
                                              [[buffer(1)]],
                                              uint lane [[thread_position_in_grid]]) {
    if (!reconstruction_guide_valid(a) || lane >= ulong(a.config->width) * a.config->height)
        return;
    const auto xyz = a.raw_xyz[lane];
    const auto m = a.config->xyz_to_rgb;
    const float3 color(m[0] * xyz.x + m[1] * xyz.y + m[2] * xyz.z,
                       m[4] * xyz.x + m[5] * xyz.y + m[6] * xyz.z,
                       m[8] * xyz.x + m[9] * xyz.y + m[10] * xyz.z);
    if (!all(isfinite(color))) {
        reconstruction_guide_fail(a, gw::invalid_value);
        return;
    }
    if (any(color < float3(0)))
        reconstruction_guide_count(a, &a.counters->color_negative);
    if (any(color > float3(65504)))
        reconstruction_guide_count(a, &a.counters->color_high);
    a.color.write(float4(clamp(color, float3(0), float3(65504)), 1),
                  uint2(lane % a.config->width, lane / a.config->width));
    reconstruction_guide_count(a, &a.counters->completed);
}

#define RYPATH_RECONSTRUCTION_GUIDE_PARAMETERS                                                     \
    constant wire::Parameters &p [[buffer(0)]],                                                    \
        constant ReconstructionGuideArguments &a [[buffer(1)]],                                    \
        instance_acceleration_structure scene [[buffer(2)]],                                       \
        const device wire::Instance *instances [[buffer(3)]],                                      \
        const device wire::Point *positions [[buffer(4)]],                                         \
        const device wire::Triangle *triangles [[buffer(5)]],                                      \
        const device wire::Material *materials [[buffer(6)]],                                      \
        const device float *environment [[buffer(7)]],                                             \
        const device wire::SpectralKnot *knots [[buffer(8)]],                                      \
        const device wire::AreaEmission *emissions [[buffer(9)]],                                  \
        const device wire::Emitter *emitters [[buffer(10)]],                                       \
        const device wire::Point *normals [[buffer(11)]],                                          \
        const device sobol::Tables &tables [[buffer(12)]],                                         \
        const device wire::Patch *patches [[buffer(13)]],                                          \
        MixedIntersectionTable intersection_table [[buffer(14)]],                                  \
        const device wire::Point2 *texture_uvs [[buffer(15)]],                                     \
        const device wire::TextureFrame *texture_frames [[buffer(16)]],                            \
        const device wire::ImageTexture *image_textures [[buffer(17)]],                            \
        const device wire::TextureLevel *image_levels [[buffer(18)]],                              \
        const device wire::Word *image_texels [[buffer(19)]],                                      \
        const device float *image_transfer [[buffer(20)]],                                         \
        const device float *image_spectra [[buffer(21)]],                                          \
        constant wire::TextureParameters &texture_p [[buffer(22)]],                                \
        const device graph_wire::Node *graph_nodes [[buffer(23)]],                                 \
        const device wire::SpectralKnot *graph_knots [[buffer(24)]],                               \
        constant wire::TextureGraphParameters &graph_p [[buffer(25)]],                             \
        const device wire::SurfaceVertex *surface_vertices [[buffer(26)]],                         \
        const device wire::SurfaceTransform *surface_transforms [[buffer(27)]],                    \
        const device float *environment_cdf [[buffer(28)]],                                        \
        const device float *illuminant [[buffer(29)]],                                             \
        const device uint *material_data [[buffer(30)]], uint lane [[thread_position_in_grid]]

kernel void rypath_reconstruction_guide_surface(RYPATH_RECONSTRUCTION_GUIDE_PARAMETERS) {
    const uint total = a.config->width * a.config->height;
    if (!reconstruction_guide_valid(a) || lane >= total)
        return;
    if (p.width != a.config->width || p.height != a.config->height || p.model != a.config->model ||
        p.pixel_begin != 0 || p.pixel_count != total || p.lane_count != total ||
        p.lens_radius != 0 || p.sampler_kind != wire::independent_sampler) {
        reconstruction_guide_fail(a, gw::invalid_config);
        return;
    }
    wire::Sample sample{wire::success,
                        lane,
                        p.sample_low,
                        p.sample_high,
                        550.f,
                        0,
                        0,
                        {0x80000000u, 0x80000000u, 0, 0},
                        {0x80000000u, 0x80000000u},
                        {0, 0}};
    const ray incoming = primary_ray(p, sample);
    if (!all(isfinite(incoming.origin)) || !all(isfinite(incoming.direction)) ||
        !any(incoming.direction != float3(0))) {
        reconstruction_guide_fail(a, gw::invalid_value);
        return;
    }
    const GraphResources graph{
        graph_nodes,
        graph_knots,
        &graph_p,
        {image_textures, image_levels, image_texels, image_transfer, image_spectra, &texture_p}};
    SurfaceAlpha resources{
        materials, material_data, instances,      positions,        triangles,          patches,
        normals,   texture_uvs,   texture_frames, surface_vertices, surface_transforms, &p,
        &sample,   graph,         incoming};
    uint skipped = 0;
    ReconstructionGuideAlpha alpha{resources, &skipped};
    const auto hit = p.instance_count == 0
                         ? SurfaceHit{intersection_type::none, 0, 0, 0, 0, float2(0), true}
                         : trace_masked_surface(incoming, scene, all_visible, alpha);
    if (!hit.valid) {
        reconstruction_guide_fail(a, gw::invalid_hit);
        return;
    }
    if (skipped) {
        reconstruction_guide_count(a, &a.counters->passthrough, skipped);
        if (atomic_load_explicit(&a.counters->failure, memory_order_relaxed) != 0)
            return;
    }
    if ((a.config->flags & gw::media_scene) || p.camera_medium != 0xffffffffu)
        reconstruction_guide_count(a, &a.counters->media_approximation);
    const bool background = hit.type == intersection_type::none;
    const float3 relative = background ? incoming.direction : incoming.direction * hit.distance;
    float depth;
    float2 motion;
    if ((!background && (!isfinite(hit.distance) || hit.distance < 0)) ||
        !reconstruction_guide_motion(a, relative, background, depth, motion)) {
        reconstruction_guide_fail(a, gw::invalid_projection);
        return;
    }
    float3 diffuse(0), specular(0), normal(0);
    float roughness = 1;
    if (background) {
        reconstruction_guide_count(a, &a.counters->background);
    } else {
        if (hit.instance_id >= p.instance_count || hit.geometry_id != 0 ||
            !all(isfinite(hit.coordinates))) {
            reconstruction_guide_fail(a, gw::invalid_hit);
            return;
        }
        const device auto &instance = instances[hit.instance_id];
        const uint kind = hit.primitive_kind;
        if (hit.primitive_id >= primitive_count(instance, kind) ||
            instance.material >= p.material_count) {
            reconstruction_guide_fail(a, gw::invalid_hit);
            return;
        }
        SurfacePoint point;
        if (!surface_point(hit.coordinates, kind, instance, hit.primitive_id, positions, triangles,
                           patches, normals, p.normal_count, PointSource::native_hit, point,
                           material_data)) {
            reconstruction_guide_fail(a, gw::invalid_hit);
            return;
        }
        const device auto &root = materials[instance.material];
        const bool mixed = root.scattering == 9;
        graph_wire::Context context{};
        const bool context_needed = root.scattering != 0 || root.kind == wire::image_material ||
                                    root.kind == wire::graph_material ||
                                    root.bump_root != 0xffffffffu || root.normal_map != 0xffffffffu;
        if (context_needed &&
            !reconstruction_guide_context(resources, instance, hit.primitive_id, kind,
                                          hit.coordinates, point, context)) {
            reconstruction_guide_fail(a, gw::invalid_material);
            return;
        }
        uint material = instance.material;
        if (mixed &&
            !reconstruction_guide_material(resources, instance, point, context, material)) {
            reconstruction_guide_fail(a, gw::invalid_material);
            return;
        }
        const device auto &m = materials[material];
        if (m.scattering == 10 || instance.invisible_emitter) {
            reconstruction_guide_fail(a, gw::invalid_hit);
            return;
        }
        normal = mixed ? point.geometric_normal : point.shading_normal;
        const bool detail =
            m.bump_root != 0xffffffffu || m.normal_map != 0xffffffffu || m.scattering == 11;
        if (!mixed && detail) {
            rypath_surface_detail::Frame frame;
            if (!apply_surface_detail(hit.instance_id, instance, hit.primitive_id, kind,
                                      hit.coordinates, triangles, patches, texture_uvs,
                                      surface_vertices, surface_transforms, m, graph, context,
                                      550.f, point, frame, true,
                                      material_auxiliary(m, material_data, p.material_data_words),
                                      incoming.direction)) {
                reconstruction_guide_fail(a, gw::invalid_material);
                return;
            }
            normal = point.shading_normal;
        }
        if (!rypath_surface_detail::unit_vector(normal, normal)) {
            reconstruction_guide_fail(a, gw::invalid_value);
            return;
        }
        if (mixed || m.bump_root != 0xffffffffu)
            reconstruction_guide_count(a, &a.counters->normal_approximation);
        const bool fallback = mixed || m.scattering == 7 || m.scattering == 8;
        if (fallback) {
            // A neutral demodulation guide makes no claim to reproduce these
            // BSDFs. Invalid IDs/contexts above remain errors, never fallbacks.
            if ((m.scattering == 7 || m.scattering == 8) && !mixed) {
                const auto aux = material_auxiliary(m, material_data, p.material_data_words);
                if (!aux || aux->table_offset >= p.material_data_words) {
                    reconstruction_guide_fail(a, gw::invalid_material);
                    return;
                }
            }
            diffuse = 1;
            roughness = 1;
            if (mixed)
                reconstruction_guide_count(a, &a.counters->mix_fallback);
            else if (m.scattering == 7)
                reconstruction_guide_count(a, &a.counters->measured_fallback);
            else
                reconstruction_guide_count(a, &a.counters->fourier_fallback);
        } else {
            roughness = 0;
            for (uint i = 0; i < gw::node_count; ++i) {
                const auto node = a.nodes[i];
                const float3 weight(node.r, node.g, node.b);
                float d, s, r;
                if (!(node.wavelength >= 360 && node.wavelength <= 830) || !all(isfinite(weight)) ||
                    !reconstruction_guide_optical(resources, m, knots, context, node.wavelength, d,
                                                  s, r) ||
                    !isfinite(d) || !isfinite(s) || !isfinite(r) || d < 0 || s < 0 || r < 0) {
                    reconstruction_guide_fail(a, gw::invalid_material);
                    return;
                }
                diffuse += weight * d;
                specular += weight * s;
                roughness = max(roughness, r);
            }
            if (m.scattering == 5 || m.scattering == 6)
                reconstruction_guide_count(a, &a.counters->layered_approximation);
            if (m.scattering == 2 || m.scattering == 3 || m.scattering == 4)
                reconstruction_guide_count(a, &a.counters->transmission_approximation);
        }
        if (!all(isfinite(diffuse)) || !all(isfinite(specular)) || !isfinite(roughness)) {
            reconstruction_guide_fail(a, gw::invalid_value);
            return;
        }
        if (any(diffuse < float3(0)) || any(diffuse > float3(1)) || any(specular < float3(0)) ||
            any(specular > float3(1)))
            reconstruction_guide_count(a, &a.counters->albedo_clamped);
        if (roughness < 0 || roughness > 1)
            reconstruction_guide_count(a, &a.counters->roughness_clamped);
        diffuse = clamp(diffuse, float3(0), float3(1));
        specular = clamp(specular, float3(0), float3(1));
        roughness = clamp(roughness, 0.f, 1.f);
    }
    const uint2 pixel(lane % a.config->width, lane / a.config->width);
    a.depth.write(float4(depth, 0, 0, 0), pixel);
    a.motion.write(float4(motion, 0, 0), pixel);
    a.diffuse.write(float4(diffuse, 1), pixel);
    a.specular.write(float4(specular, 1), pixel);
    a.normal.write(float4(normal, background ? 0.f : 1.f), pixel);
    a.roughness.write(float4(roughness, 0, 0, 0), pixel);
}
#undef RYPATH_RECONSTRUCTION_GUIDE_PARAMETERS
