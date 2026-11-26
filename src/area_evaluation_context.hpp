// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rypath {
// Immutable contribution measure/target configuration. Camera changes permit
// replay, but changing this domain requires invalidating the old history.
struct AreaPathDomainContext {
    std::uint32_t mode = 0, depth = 0;
    float target_scale = 1;
    std::uint32_t replay = 0;
    std::uint32_t canonical_transport_version = 0, fresh_discovery_version = 0;
};
static_assert(sizeof(AreaPathDomainContext) == 24, "Area path-domain context has no padding");
inline bool same_area_path_domain_context(const AreaPathDomainContext &a,
                                          const AreaPathDomainContext &b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}
// These records contain only scalar words, Point and arrays. Summing every
// member size makes whole-record binary comparison independent of padding.
static_assert(
    sizeof(render_wire::Parameters) ==
        sizeof(render_wire::Parameters::origin) + sizeof(render_wire::Parameters::forward) +
            sizeof(render_wire::Parameters::right) + sizeof(render_wire::Parameters::up) +
            sizeof(render_wire::Parameters::film_scale) + sizeof(render_wire::Parameters::knots) +
            sizeof(render_wire::Parameters::width) + sizeof(render_wire::Parameters::height) +
            sizeof(render_wire::Parameters::pixel_begin) +
            sizeof(render_wire::Parameters::pixel_count) +
            sizeof(render_wire::Parameters::sample_low) +
            sizeof(render_wire::Parameters::sample_high) +
            sizeof(render_wire::Parameters::seed_low) + sizeof(render_wire::Parameters::seed_high) +
            sizeof(render_wire::Parameters::emergency_limit) +
            sizeof(render_wire::Parameters::lane_count) +
            sizeof(render_wire::Parameters::instance_count) +
            sizeof(render_wire::Parameters::model) + sizeof(render_wire::Parameters::max_depth) +
            sizeof(render_wire::Parameters::material_count) +
            sizeof(render_wire::Parameters::knot_count) +
            sizeof(render_wire::Parameters::emission_count) +
            sizeof(render_wire::Parameters::emitter_count) +
            sizeof(render_wire::Parameters::emitter_slots) +
            sizeof(render_wire::Parameters::filter_kind) +
            sizeof(render_wire::Parameters::filter_x_radius) +
            sizeof(render_wire::Parameters::filter_y_radius) +
            sizeof(render_wire::Parameters::normal_count) +
            sizeof(render_wire::Parameters::sampler_kind) +
            sizeof(render_wire::Parameters::randomization) +
            sizeof(render_wire::Parameters::sobol_scale_exponent) +
            sizeof(render_wire::Parameters::material_data_words) +
            sizeof(render_wire::Parameters::medium_begin) +
            sizeof(render_wire::Parameters::medium_count) +
            sizeof(render_wire::Parameters::camera_medium) +
            sizeof(render_wire::Parameters::boundary_limit) +
            sizeof(render_wire::Parameters::positional_begin) +
            sizeof(render_wire::Parameters::positional_count) +
            sizeof(render_wire::Parameters::distant_begin) +
            sizeof(render_wire::Parameters::distant_count) +
            sizeof(render_wire::Parameters::distant_scene_radius) +
            sizeof(render_wire::Parameters::lens_radius) +
            sizeof(render_wire::Parameters::focal_distance) +
            sizeof(render_wire::Parameters::lens_x) + sizeof(render_wire::Parameters::lens_y) +
            sizeof(render_wire::Parameters::zsobol_log_spp) +
            sizeof(render_wire::Parameters::gaussian_width) +
            sizeof(render_wire::Parameters::gaussian_height) +
            sizeof(render_wire::Parameters::filter_sigma) +
            sizeof(render_wire::Parameters::packet_transport) +
            sizeof(render_wire::Parameters::bidirectional) +
            sizeof(render_wire::Parameters::packet_lane_bytes) +
            sizeof(render_wire::Parameters::packet_filter_weight) +
            sizeof(render_wire::Parameters::scene_center) +
            sizeof(render_wire::Parameters::scene_radius) +
            sizeof(render_wire::Parameters::camera_normal) +
            sizeof(render_wire::Parameters::camera_image_area) +
            sizeof(render_wire::Parameters::camera_plane_depth) +
            sizeof(render_wire::Parameters::material_seed) +
            sizeof(render_wire::Parameters::light_sampler) +
            sizeof(render_wire::Parameters::light_count) +
            sizeof(render_wire::Parameters::light_alias_begin) +
            sizeof(render_wire::Parameters::light_records_begin) +
            sizeof(render_wire::Parameters::light_nodes_begin) +
            sizeof(render_wire::Parameters::light_node_count) +
            sizeof(render_wire::Parameters::infinite_lights_begin) +
            sizeof(render_wire::Parameters::infinite_light_count) +
            sizeof(render_wire::Parameters::environment_light) +
            sizeof(render_wire::Parameters::hero_wavelengths),
    "Evaluation-context wire record must have no padding");
static_assert(sizeof(render_wire::TextureParameters) ==
                  sizeof(render_wire::TextureParameters::texture_count) +
                      sizeof(render_wire::TextureParameters::level_count) +
                      sizeof(render_wire::TextureParameters::texel_word_count) +
                      sizeof(render_wire::TextureParameters::uv_count) +
                      sizeof(render_wire::TextureParameters::frame_count) +
                      sizeof(render_wire::TextureParameters::spp_scale) +
                      sizeof(render_wire::TextureParameters::world_from_camera) +
                      sizeof(render_wire::TextureParameters::camera_from_world) +
                      sizeof(render_wire::TextureParameters::minimum_dx) +
                      sizeof(render_wire::TextureParameters::minimum_dy) +
                      sizeof(render_wire::TextureParameters::transfer_count),
              "Evaluation-context wire record must have no padding");

inline bool same_area_evaluation_parameters(render_wire::Parameters a, render_wire::Parameters b,
                                            const render_wire::TextureParameters &ta,
                                            const render_wire::TextureParameters &tb) {
    // Replay replaces every random stream with payload coordinates. The two
    // sample words only label/validate the output; all remaining words,
    // including destination pixel mapping and both seeds, must match exactly.
    a.sample_low = b.sample_low = 0;
    a.sample_high = b.sample_high = 0;
    return std::memcmp(&a, &b, sizeof(a)) == 0 && std::memcmp(&ta, &tb, sizeof(ta)) == 0;
}
inline bool same_area_evaluation_bytes(const void *a, std::size_t a_size, const void *b,
                                       std::size_t b_size) {
    return a_size == b_size && (a_size == 0 || (a && b && std::memcmp(a, b, a_size) == 0));
}
inline std::uint64_t logical_area_evaluations(std::uint64_t pixels, bool history,
                                              std::uint32_t mutations) {
    return pixels * ((history ? 7u : 5u) + mutations);
}
inline bool valid_area_evaluation_accounting(std::uint64_t pixels, bool history,
                                             std::uint32_t mutations, bool cache_used,
                                             std::uint64_t completed, std::uint64_t cached,
                                             std::uint64_t logical) {
    const auto expected = logical_area_evaluations(pixels, history, mutations);
    const auto expected_cached = cache_used && history ? 2 * pixels : 0;
    return (!cache_used || history) && logical == expected && cached == expected_cached &&
           completed == expected - expected_cached;
}
// Physical transport, cached responses and analytic null completions are
// disjoint categories. A cached null response remains a cache hit, not a
// fabricated wavefront completion. Every fresh owner executes physical work.
inline bool valid_enhanced_area_evaluation_accounting(std::uint64_t pixels, bool history,
                                                      std::uint32_t mutations, bool cache_used,
                                                      std::uint64_t physical, std::uint64_t cached,
                                                      std::uint64_t analytic_null,
                                                      std::uint64_t logical) {
    const auto expected = logical_area_evaluations(pixels, history, mutations);
    const auto expected_cached = cache_used && history ? 2 * pixels : 0;
    return (!cache_used || history) && logical == expected && cached == expected_cached &&
           physical >= pixels && physical <= expected - expected_cached &&
           analytic_null == expected - expected_cached - physical;
}
} // namespace rypath
