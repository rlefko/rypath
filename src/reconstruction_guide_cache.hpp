// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "reconstruction_guide_wire.h"
#include <bit>
#include <vector>
namespace rypath::reconstruction {
// A stream owns one immutable scene/profile. bind_guides authorizes that owner
// on every submission, including hits, before this key is used for dispatch.
inline std::vector<std::uint32_t> surface_guide_key(render_wire::Parameters p,
                                                    const render_wire::TextureParameters &t,
                                                    const reconstruction_guide_wire::Config &c) {
    p.sample_low = p.sample_high = 0; // Center-pinhole guides never sample transport.
    std::vector<std::uint32_t> key;
    key.reserve((sizeof(p) + sizeof(t) + sizeof(c)) / sizeof(std::uint32_t));
    const auto add = [&](auto value) { key.push_back(std::bit_cast<std::uint32_t>(value)); };
    const auto point = [&](render_wire::Point value) {
        add(value.x);
        add(value.y);
        add(value.z);
    };
    point(p.origin);
    point(p.forward);
    point(p.right);
    point(p.up);
    add(p.film_scale);
    for (auto value : p.knots)
        add(value);
    add(p.width);
    add(p.height);
    add(p.pixel_begin);
    add(p.pixel_count);
    add(p.sample_low);
    add(p.sample_high);
    add(p.seed_low);
    add(p.seed_high);
    add(p.emergency_limit);
    add(p.lane_count);
    add(p.instance_count);
    add(p.model);
    add(p.max_depth);
    add(p.material_count);
    add(p.knot_count);
    add(p.emission_count);
    add(p.emitter_count);
    add(p.emitter_slots);
    add(p.filter_kind);
    add(p.filter_x_radius);
    add(p.filter_y_radius);
    add(p.normal_count);
    add(p.sampler_kind);
    add(p.randomization);
    add(p.sobol_scale_exponent);
    add(p.material_data_words);
    add(p.medium_begin);
    add(p.medium_count);
    add(p.camera_medium);
    add(p.boundary_limit);
    add(p.positional_begin);
    add(p.positional_count);
    add(p.distant_begin);
    add(p.distant_count);
    add(p.distant_scene_radius);
    add(p.lens_radius);
    add(p.focal_distance);
    point(p.lens_x);
    point(p.lens_y);
    add(p.zsobol_log_spp);
    add(p.gaussian_width);
    add(p.gaussian_height);
    add(p.filter_sigma);
    add(p.packet_transport);
    add(p.bidirectional);
    add(p.packet_lane_bytes);
    add(p.packet_filter_weight);
    point(p.scene_center);
    add(p.scene_radius);
    point(p.camera_normal);
    add(p.camera_image_area);
    add(p.camera_plane_depth);
    add(p.material_seed);
    add(p.light_sampler);
    add(p.light_count);
    add(p.light_alias_begin);
    add(p.light_records_begin);
    add(p.light_nodes_begin);
    add(p.light_node_count);
    add(p.infinite_lights_begin);
    add(p.infinite_light_count);
    add(p.environment_light);
    add(t.texture_count);
    add(t.level_count);
    add(t.texel_word_count);
    add(t.uv_count);
    add(t.frame_count);
    add(t.spp_scale);
    for (auto value : t.world_from_camera)
        add(value);
    for (auto value : t.camera_from_world)
        add(value);
    point(t.minimum_dx);
    point(t.minimum_dy);
    add(t.transfer_count);
    add(c.width);
    add(c.height);
    add(c.model);
    add(c.previous_valid);
    for (auto value : c.current_world_to_view)
        add(value);
    for (auto value : c.current_view_to_clip)
        add(value);
    for (auto value : c.previous_world_to_view)
        add(value);
    for (auto value : c.previous_view_to_clip)
        add(value);
    for (auto value : c.current_camera_to_matrix_origin)
        add(value);
    for (auto value : c.previous_camera_to_matrix_origin)
        add(value);
    for (auto value : c.xyz_to_rgb)
        add(value);
    add(c.spectral_nodes);
    add(c.flags);
    for (auto value : c.reserved)
        add(value);
    return key;
}
} // namespace rypath::reconstruction
