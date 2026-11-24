// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included after render.metal helpers. This classifies a moving point, not a
// visibility or light-transport connection: ordinary opaque/glass surfaces do
// not block movement, and there is no attenuation or stochastic scattering.
#include "../src/camera_medium_wire.h"
namespace cm = rypath::camera_medium_wire;
struct CameraMediumArguments {
    device const cm::Config *config [[id(cm::config_id)]];
    device cm::Result *result [[id(cm::result_id)]];
};
inline bool camera_medium_id(uint medium, constant wire::Parameters &p) {
    return medium == 0xffffffffu || medium < p.medium_count;
}
inline bool camera_medium_representable(float3 value) {
    for (uint axis = 0; axis < 3; ++axis) {
        const uint magnitude = as_type<uint>(value[axis]) & 0x7fffffffu;
        if (magnitude >= 0x7f800000u || (magnitude != 0 && magnitude < 0x00800000u))
            return false;
    }
    return true;
}
struct CameraInterfaceAlpha {
    constant wire::Parameters *p;
    const device wire::Instance *instances;
    const device wire::Material *materials;
    const device uint *data;
    thread cm::Status *failure;
    bool has_previous;
    SurfaceHit previous;
    MaskedAlphaResult operator()(uint ordinal, uint geometry, uint primitive,
                                 intersection_type type, float2 uv, float distance) const {
        if (ordinal >= p->instance_count || geometry != 0 || !isfinite(distance) ||
            !all(isfinite(uv))) {
            *failure = cm::Status::invalid_hit;
            return {false, false};
        }
        const device auto &instance = instances[ordinal];
        const uint kind = type == intersection_type::triangle ? wire::triangle_primitive
                          : instance.analytic_count           ? wire::analytic_primitive
                                                              : wire::patch_primitive;
        if (primitive >= primitive_count(instance, kind)) {
            *failure = cm::Status::invalid_hit;
            return {false, false};
        }
        if (!camera_medium_id(instance.inside_medium, *p) ||
            !camera_medium_id(instance.outside_medium, *p)) {
            *failure = cm::Status::invalid_medium;
            return {false, false};
        }
        if (instance.inside_medium == instance.outside_medium)
            return {false, true};
        if (instance.alpha_root != 0xffffffffu || instance.material >= p->material_count) {
            *failure = instance.material >= p->material_count ? cm::Status::invalid_geometry
                                                              : cm::Status::unsupported_interface;
            return {false, false};
        }
        const device auto &material = materials[instance.material];
        if (material.scattering == 11) {
            const auto aux = material_auxiliary(material, data, p->material_data_words);
            if (!aux) {
                *failure = cm::Status::invalid_geometry;
                return {false, false};
            }
            if (aux->gltf.alpha_mode != 0) {
                *failure = cm::Status::unsupported_interface;
                return {false, false};
            }
        }
        if (has_previous) {
            if (distance < previous.distance)
                return {false, true};
            if (distance == previous.distance) {
                // Only the exact hit already consumed may be removed. Another
                // primitive/root at this represented depth has unresolved order;
                // never step over it or merge it by a scene-scale tolerance.
                if (ordinal == previous.instance_id && geometry == previous.geometry_id &&
                    primitive == previous.primitive_id && kind == previous.primitive_kind &&
                    all(uv == previous.coordinates))
                    return {false, true};
                *failure = cm::Status::unresolved_boundary;
                return {false, false};
            }
        }
        // Both geometric sides participate. Material lobes, culling, mix choices,
        // null flags and invisible-emitter flags do not change interface labels.
        return {true, true};
    }
};
kernel void rypath_camera_medium_contract(device cm::Contract *result [[buffer(0)]]) {
    *result = {
        cm::contract, sizeof(cm::Config), sizeof(cm::Result), cm::maximum_crossings, {0, 0, 0, 0}};
}

#define RYPATH_CAMERA_MEDIUM_PARAMETERS                                                            \
    constant wire::Parameters &p [[buffer(0)]], constant CameraMediumArguments &a [[buffer(1)]],   \
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

kernel void rypath_camera_medium_classify(RYPATH_CAMERA_MEDIUM_PARAMETERS) {
    if (lane != 0)
        return;
    cm::Result result{cm::Status::invalid_config,
                      p.camera_medium,
                      0,
                      0xffffffffu,
                      0xffffffffu,
                      0xffffffffu,
                      {0, 0}};
    const auto c = *a.config;
    const float3 endpoint(c.destination[0], c.destination[1], c.destination[2]);
    const float3 initial_origin = unpack(p.origin);
    if (c.version != cm::contract || c.crossing_limit == 0 ||
        c.crossing_limit > cm::maximum_crossings || c.crossing_limit > p.boundary_limit ||
        c.reserved[0] != 0 || c.reserved[1] != 0 || c.padding != 0) {
        *a.result = result;
        return;
    }
    if (!camera_medium_id(result.medium, p)) {
        result.status = cm::Status::invalid_medium;
        *a.result = result;
        return;
    }
    const float3 direction = endpoint - initial_origin;
    if (!camera_medium_representable(endpoint) || !camera_medium_representable(initial_origin) ||
        !camera_medium_representable(direction)) {
        result.status = cm::Status::unrepresentable_segment;
        *a.result = result;
        return;
    }
    if (all(endpoint == initial_origin)) {
        result.status = cm::Status::success;
        *a.result = result;
        return;
    }
    // Every query uses exactly this original origin/direction and [0,+infinity).
    // Native traversal omitted a real destination hit at the finite t_max=1
    // endpoint in the GPU regression. The destination is now internal to the
    // queried halfline; oriented error bounds below decide whether its closest
    // unprocessed interface lies before, at, or beyond the destination. Offset
    // jumps and t_min advancement could discard other thin/near-equal roots.
    const ray incoming(initial_origin, direction, 0.f, INFINITY);
    cm::Status query_failure = cm::Status::success;
    CameraInterfaceAlpha alpha{&p, instances, materials, material_data, &query_failure, false, {}};
    while (true) {
        const auto hit = p.instance_count == 0
                             ? SurfaceHit{intersection_type::none, 0, 0, 0, 0, float2(0), true}
                             : trace_masked_surface(incoming, scene, all_visible, alpha);
        if (!hit.valid || query_failure != cm::Status::success) {
            result.status =
                query_failure == cm::Status::success ? cm::Status::invalid_hit : query_failure;
            break;
        }
        if (hit.type == intersection_type::none) {
            result.status = cm::Status::success;
            break;
        }
        result.last_instance = hit.instance_id;
        result.last_primitive = hit.primitive_id;
        result.last_kind = hit.primitive_kind;
        if (hit.instance_id >= p.instance_count || hit.geometry_id != 0 ||
            !isfinite(hit.distance) || hit.distance < 0 || !all(isfinite(hit.coordinates))) {
            result.status = cm::Status::invalid_hit;
            break;
        }
        const device auto &instance = instances[hit.instance_id];
        SurfacePoint point;
        if (hit.primitive_id >= primitive_count(instance, hit.primitive_kind) ||
            !surface_point(hit.coordinates, hit.primitive_kind, instance, hit.primitive_id,
                           positions, triangles, patches, normals, p.normal_count,
                           PointSource::native_hit, point, material_data)) {
            result.status = cm::Status::invalid_geometry;
            break;
        }
        const float side = dot(point.geometric_normal, incoming.direction);
        const float side_error =
            gamma_bound(5) * dot(abs(point.geometric_normal), abs(incoming.direction));
        if (!isfinite(side) || !isfinite(side_error) || !(abs(side) > side_error)) {
            result.status = hit.distance >= 1.f ? cm::Status::unresolved_endpoint
                                                : cm::Status::unresolved_boundary;
            break;
        }
        const float3 outgoing_normal = side > 0 ? point.geometric_normal : -point.geometric_normal;
        const float3 departure = endpoint - point.position;
        const float separation = dot(outgoing_normal, departure);
        const float rounding = gamma_bound(5) * dot(abs(outgoing_normal), abs(departure));
        const float uncertainty = dot(abs(outgoing_normal), point.error) * (1 + gamma_bound(4));
        if (!all(isfinite(point.error)) || any(point.error < float3(0)) ||
            !all(isfinite(departure)) || !isfinite(separation) || !isfinite(rounding) ||
            !isfinite(uncertainty)) {
            result.status = cm::Status::unresolved_endpoint;
            break;
        }
        if (hit.distance >= 1.f) {
            // A future interface must not become a spurious crossing or exhaust
            // the budget. Certify the endpoint is strictly on its incoming
            // side; t/side disagreement or an uncertainty slab is unresolved.
            result.status = separation + rounding < -uncertainty ? cm::Status::success
                                                                 : cm::Status::unresolved_endpoint;
            break;
        }
        if (!(separation - rounding > uncertainty)) {
            result.status = cm::Status::unresolved_endpoint;
            break;
        }
        if (result.crossings >= c.crossing_limit) {
            result.status = cm::Status::boundary_exhaustion;
            break;
        }
        const uint expected = side > 0 ? instance.inside_medium : instance.outside_medium;
        if (result.medium != expected) {
            result.status = cm::Status::inconsistent_side;
            break;
        }
        result.medium = outgoing_medium(instance, result.medium, point.geometric_normal, direction);
        ++result.crossings;
        alpha.previous = hit;
        alpha.has_previous = true;
    }
    *a.result = result;
}
#undef RYPATH_CAMERA_MEDIUM_PARAMETERS
