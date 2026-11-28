// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Include only AFTER reconstruction_guides.metal. Host
// integration must pair immutable geometry and Area-history generations and
// retain both bundles. Geometry itself has no generation authentication tag.
#include "../src/enhanced_motion_wire.h"
namespace emw = rypath::enhanced_motion_wire;
namespace motion = rypath_enhanced_motion;
struct EnhancedMotionArguments {
    device const emw::Config *config [[id(emw::config_id)]];
    device emw::Geometry *current_geometry [[id(emw::current_geometry_id)]];
    device const emw::Geometry *previous_geometry [[id(emw::previous_geometry_id)]];
    device emw::Donor *donors [[id(emw::donors_id)]];
    device emw::Counters *counters [[id(emw::counters_id)]];
};
inline void enhanced_motion_fail(constant EnhancedMotionArguments &a, uint flag) {
    atomic_fetch_or_explicit(&a.counters->failure, flag, memory_order_relaxed);
}
inline bool enhanced_motion_camera(const thread motion::Camera &camera, uint width, uint height) {
    if (camera.width != width || camera.height != height ||
        !motion::finite(camera.common_to_matrix_origin))
        return false;
    for (uint k = 0; k < 16; ++k)
        if (!isfinite(camera.world_to_view[k]) || !isfinite(camera.view_to_clip[k]))
            return false;
    return true;
}
inline bool enhanced_motion_config(const thread emw::Config &c) {
    return c.version == emw::contract && motion::dimensions(c.width, c.height) &&
           c.history_valid <= 1 && c.reserved == 0 &&
           enhanced_motion_camera(c.current_camera, c.width, c.height) &&
           enhanced_motion_camera(c.previous_camera, c.width, c.height) &&
           motion::finite(c.previous_origin_to_common) && motion::valid_policy(c.policy) &&
           c.policy.minimum_normal_cosine == .99f && c.policy.maximum_plane_footprints == .5f &&
           c.policy.relative_depth_separation == 32.f * 0x1p-23f && c.policy.require_group == 0;
}
inline bool enhanced_motion_valid(constant EnhancedMotionArguments &a,
                                  const thread emw::Config &c) {
    if (!enhanced_motion_config(c)) {
        enhanced_motion_fail(a, emw::invalid_config);
        return false;
    }
    return atomic_load_explicit(&a.counters->failure, memory_order_relaxed) == 0;
}
inline void enhanced_motion_count(constant EnhancedMotionArguments &a, device atomic_uint *counter,
                                  uint pixels) {
    if (atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed) >= pixels)
        enhanced_motion_fail(a, emw::invalid_count);
}
inline void enhanced_motion_publish_geometry(constant EnhancedMotionArguments &a, uint pixel,
                                             const thread emw::Geometry &geometry, uint pixels) {
    a.current_geometry[pixel] = geometry;
    if (geometry.status == emw::finite)
        enhanced_motion_count(a, &a.counters->geometry_hits, pixels);
    else if (geometry.status == emw::sky)
        enhanced_motion_count(a, &a.counters->geometry_sky, pixels);
    else
        enhanced_motion_count(a, &a.counters->geometry_unsupported, pixels);
    enhanced_motion_count(a, &a.counters->geometry_completed, pixels);
}
// Follow primary_ray's actual parameterization. The ordinary demo camera
// uses film scale/aspect and a flipped vertical basis; centered filters already
// use the same affine raster basis as PBRT through centered_primary_ray.
inline void enhanced_motion_camera_rays(constant wire::Parameters &p,
                                        const thread wire::Sample &sample, thread float3 &rx,
                                        thread float3 &ry) {
    if (p.model != wire::demo_model || centered_filter(p)) {
        texture_camera_rays(p, sample, rx, ry);
        return;
    }
    const float2 uv = camera_uniforms(p, sample);
    const float x = (float(sample.pixel % p.width) + uv.x) / float(p.width);
    const float y = (float(sample.pixel / p.width) + uv.y) / float(p.height);
    const float aspect = float(p.width) / float(p.height);
    const float3 d = unpack(p.forward) + (2.f * x - 1.f) * aspect * p.film_scale * unpack(p.right) +
                     (1.f - 2.f * y) * p.film_scale * unpack(p.up);
    // Aspect/width == 1/height. Divide before doubling to avoid a needless
    // intermediate overflow; these are one-pixel rays, not sample-budget LOD.
    const float step = 2.f * precise::divide(p.film_scale, float(p.height));
    rx = d + step * unpack(p.right);
    ry = d - step * unpack(p.up);
}
inline motion::Vec3 enhanced_motion_vec(wire::Point point) { return {point.x, point.y, point.z}; }
inline bool enhanced_motion_surface(const thread emw::Geometry &g, motion::Vec3 origin_delta,
                                    thread motion::Surface &surface) {
    const auto position = enhanced_motion_vec(g.camera_relative_position);
    const auto normal = enhanced_motion_vec(g.geometric_normal);
    if (!motion::finite(position) || !motion::finite(normal) || !isfinite(g.distance) ||
        g.distance < 0 || !isfinite(g.uv_x) || !isfinite(g.uv_y) || !isfinite(g.pixel_footprint) ||
        g.pixel_footprint < 0 || g.group != emw::absent)
        return false;
    surface = {};
    if (g.status == emw::sky) {
        motion::Vec3 direction;
        if (g.instance != emw::absent || g.primitive != emw::absent ||
            g.primitive_kind != emw::absent || g.material != emw::absent || g.distance != 0 ||
            g.uv_x != 0 || g.uv_y != 0 || g.pixel_footprint != 0 || normal.x != 0 ||
            normal.y != 0 || normal.z != 0 || !motion::unit(position, direction))
            return false;
        surface.geometry = motion::Geometry::sky;
        return true; // A sky direction is never translated as a finite position.
    }
    if (g.status == emw::unsupported) {
        surface.geometry = motion::Geometry::unsupported;
        return true;
    }
    if (g.status != emw::finite || g.instance == emw::absent || g.primitive == emw::absent ||
        g.material == emw::absent ||
        (g.primitive_kind != wire::triangle_primitive &&
         g.primitive_kind != wire::patch_primitive && g.primitive_kind != wire::analytic_primitive))
        return false;
    surface.common_position = motion::add(position, origin_delta);
    surface.geometric_normal = normal;
    surface.pixel_footprint = g.pixel_footprint;
    surface.instance = g.instance;
    surface.material = g.material;
    surface.group = g.group;
    surface.geometry = motion::Geometry::finite_surface;
    return motion::valid_surface(surface);
}
kernel void rypath_enhanced_motion_contract_v1(device emw::Contract *out [[buffer(0)]]) {
    *out = {emw::contract,      sizeof(emw::Config),   sizeof(emw::Geometry),
            sizeof(emw::Donor), sizeof(emw::Counters), sizeof(motion::Camera)};
}
kernel void rypath_enhanced_motion_reset(constant EnhancedMotionArguments &a [[buffer(1)]],
                                         uint lane [[thread_position_in_grid]]) {
    if (lane != 0)
        return;
    const auto c = *a.config;
    atomic_store_explicit(&a.counters->failure,
                          enhanced_motion_config(c) ? 0u : emw::invalid_config,
                          memory_order_relaxed);
    atomic_store_explicit(&a.counters->geometry_completed, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->geometry_hits, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->geometry_sky, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->geometry_unsupported, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->donor_completed, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->cold, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->ordinary, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->dual, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->fallback, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->reserved0, 0u, memory_order_relaxed);
    atomic_store_explicit(&a.counters->reserved1, 0u, memory_order_relaxed);
}

#define RYPATH_ENHANCED_MOTION_GEOMETRY_PARAMETERS                                                 \
    constant wire::Parameters &p [[buffer(0)]], constant EnhancedMotionArguments &a [[buffer(1)]], \
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

kernel void rypath_enhanced_motion_geometry(RYPATH_ENHANCED_MOTION_GEOMETRY_PARAMETERS) {
    const auto c = *a.config;
    if (!enhanced_motion_valid(a, c))
        return;
    const uint pixels = c.width * c.height;
    if (lane >= pixels)
        return;
    if (p.width != c.width || p.height != c.height ||
        (p.model != wire::demo_model && p.model != wire::pbrt_model) || p.pixel_begin != 0 ||
        p.pixel_count != pixels || p.lane_count != pixels || p.lens_radius != 0 ||
        p.sampler_kind != wire::independent_sampler) {
        enhanced_motion_fail(a, emw::invalid_config);
        return;
    }
    // Fixed representative query: no physical sample label, random seed, U or j
    // enters the sample. This matches the guide's center-film/lens 550nm law.
    wire::Sample sample{wire::success,
                        lane,
                        0,
                        0,
                        550.f,
                        0,
                        0,
                        {0x80000000u, 0x80000000u, 0, 0},
                        {0x80000000u, 0x80000000u},
                        {0, 0}};
    const ray incoming = primary_ray(p, sample);
    if (!all(isfinite(incoming.origin)) || !all(isfinite(incoming.direction)) ||
        !any(incoming.direction != float3(0))) {
        enhanced_motion_fail(a, emw::invalid_geometry);
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
        enhanced_motion_fail(a, emw::invalid_geometry);
        return;
    }
    emw::Geometry geometry{};
    geometry.instance = geometry.primitive = geometry.primitive_kind = geometry.material =
        geometry.group = emw::absent;
    if (hit.type == intersection_type::none) {
        geometry.status = emw::sky;
        geometry.camera_relative_position = pack_point(incoming.direction);
    } else {
        if (hit.instance_id >= p.instance_count || hit.geometry_id != 0 ||
            !all(isfinite(hit.coordinates)) || !isfinite(hit.distance) || hit.distance < 0) {
            enhanced_motion_fail(a, emw::invalid_geometry);
            return;
        }
        const device auto &instance = instances[hit.instance_id];
        if (hit.primitive_id >= primitive_count(instance, hit.primitive_kind) ||
            instance.material >= p.material_count) {
            enhanced_motion_fail(a, emw::invalid_geometry);
            return;
        }
        SurfacePoint point;
        if (!surface_point(hit.coordinates, hit.primitive_kind, instance, hit.primitive_id,
                           positions, triangles, patches, normals, p.normal_count,
                           PointSource::native_hit, point, material_data)) {
            enhanced_motion_fail(a, emw::invalid_geometry);
            return;
        }
        const float3 normal = point.geometric_normal;
        motion::Vec3 unit_normal;
        const float3 relative = incoming.direction * hit.distance;
        if (!all(isfinite(relative)) ||
            !motion::unit({normal.x, normal.y, normal.z}, unit_normal)) {
            enhanced_motion_fail(a, emw::invalid_geometry);
            return;
        }
        geometry.status = emw::finite;
        geometry.instance = hit.instance_id;
        geometry.primitive = hit.primitive_id;
        geometry.primitive_kind = hit.primitive_kind;
        geometry.material = instance.material; // Explicit ROOT identity, including mixes.
        geometry.distance = hit.distance;
        geometry.uv_x = hit.coordinates.x;
        geometry.uv_y = hit.coordinates.y;
        geometry.camera_relative_position = pack_point(relative);
        geometry.geometric_normal = pack_point(normal);
        float3 rx, ry, dpdx, dpdy;
        enhanced_motion_camera_rays(p, sample, rx, ry);
        if (!all(isfinite(rx)) || !all(isfinite(ry))) {
            enhanced_motion_fail(a, emw::invalid_geometry);
            return;
        }
        if (!rypath_texture_primary_dp(relative, normal, incoming.direction, rx, ry, 1.f, dpdx,
                                       dpdy) ||
            !all(isfinite(dpdx)) || !all(isfinite(dpdy))) {
            geometry.status = emw::unsupported;
        } else {
            const float footprint = max(length(dpdx), length(dpdy));
            if (!isfinite(footprint) || !(footprint > 0))
                geometry.status = emw::unsupported;
            else
                geometry.pixel_footprint = footprint;
        }
    }
    // No medium correspondence model in this first camera-only geometry pilot.
    if (p.medium_count != 0 || p.camera_medium != emw::absent)
        geometry.status = emw::unsupported;
    enhanced_motion_publish_geometry(a, lane, geometry, pixels);
}
#undef RYPATH_ENHANCED_MOTION_GEOMETRY_PARAMETERS

kernel void rypath_enhanced_motion_donors(constant EnhancedMotionArguments &a [[buffer(1)]],
                                          uint destination [[thread_position_in_grid]]) {
    const auto c = *a.config;
    if (!enhanced_motion_valid(a, c))
        return;
    const uint pixels = c.width * c.height;
    if (destination >= pixels)
        return;
    const uint completed =
        atomic_load_explicit(&a.counters->geometry_completed, memory_order_relaxed);
    const ulong classified =
        ulong(atomic_load_explicit(&a.counters->geometry_hits, memory_order_relaxed)) +
        atomic_load_explicit(&a.counters->geometry_sky, memory_order_relaxed) +
        atomic_load_explicit(&a.counters->geometry_unsupported, memory_order_relaxed);
    if (completed != pixels || classified != pixels ||
        atomic_load_explicit(&a.counters->reserved0, memory_order_relaxed) != 0 ||
        atomic_load_explicit(&a.counters->reserved1, memory_order_relaxed) != 0) {
        enhanced_motion_fail(a, emw::invalid_count);
        return;
    }
    motion::Surface current, old_at_k;
    const auto geometry = a.current_geometry[destination];
    if (!enhanced_motion_surface(geometry, {}, current)) {
        enhanced_motion_fail(a, emw::invalid_geometry);
        return;
    }
    // The host must authenticate snapshot generations before binding. Cold
    // history never reads previous_geometry, even if its bytes are poisoned.
    if (c.history_valid && current.geometry == motion::Geometry::finite_surface) {
        const auto x = motion::project(c.current_camera, current.common_position);
        const auto y0 = motion::project(c.previous_camera, current.common_position);
        if (x.status == motion::ProjectionStatus::invalid_input ||
            y0.status == motion::ProjectionStatus::invalid_input) {
            enhanced_motion_fail(a, emw::invalid_projection);
            return;
        }
        uint current_pixel, k;
        if (x.status == motion::ProjectionStatus::valid &&
            y0.status == motion::ProjectionStatus::valid &&
            motion::point_index(x.pixel, c.width, c.height, current_pixel) &&
            current_pixel == destination && motion::point_index(y0.pixel, c.width, c.height, k)) {
            const auto old_geometry = a.previous_geometry[k];
            if (!enhanced_motion_surface(old_geometry, c.previous_origin_to_common, old_at_k)) {
                enhanced_motion_fail(a, emw::invalid_geometry);
                return;
            }
        }
    }
    auto decision = motion::begin(destination, c.history_valid, c.current_camera, c.previous_camera,
                                  current, old_at_k, c.policy);
    if (decision.status == motion::Status::needs_dual_candidate) {
        if (decision.source_pixel >= pixels) {
            enhanced_motion_fail(a, emw::invalid_count);
            return;
        }
        motion::Surface old_at_q;
        const auto old_geometry = a.previous_geometry[decision.source_pixel];
        if (!enhanced_motion_surface(old_geometry, c.previous_origin_to_common, old_at_q)) {
            enhanced_motion_fail(a, emw::invalid_geometry);
            return;
        }
        decision =
            motion::finish(decision, destination, c.previous_camera, current, old_at_q, c.policy);
    }
    if (decision.status != motion::Status::complete || decision.source_pixel >= pixels) {
        enhanced_motion_fail(a, emw::invalid_projection);
        return;
    }
    a.donors[destination] = {decision.source_pixel, uint(decision.classification), c.serial_low,
                             c.serial_high};
    switch (decision.classification) {
    case motion::Classification::cold:
        enhanced_motion_count(a, &a.counters->cold, pixels);
        break;
    case motion::Classification::ordinary:
        enhanced_motion_count(a, &a.counters->ordinary, pixels);
        break;
    case motion::Classification::dual:
        enhanced_motion_count(a, &a.counters->dual, pixels);
        break;
    case motion::Classification::same_pixel_fallback:
        enhanced_motion_count(a, &a.counters->fallback, pixels);
        break;
    }
    enhanced_motion_count(a, &a.counters->donor_completed, pixels);
}
