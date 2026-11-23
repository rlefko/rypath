// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
namespace rypath {
namespace reconstruction_guide_wire {
using Word = render_wire::Word;
enum : Word { contract = 1, node_count = 16 };
enum Flag : Word { media_scene = 1u };
enum Failure : Word {
    invalid_config = 1u,
    invalid_hit = 2u,
    invalid_material = 4u,
    invalid_projection = 8u,
    invalid_value = 16u,
    counter_overflow = 32u
};
struct Config {
    Word width, height, model, previous_valid;
    // Column-major matrices, matching reconstruction::Camera.
    float current_world_to_view[16], current_view_to_clip[16];
    float previous_world_to_view[16], previous_view_to_clip[16];
    // CPU double differences, then checked float packing. Both subtract the
    // respective MATRIX origin from the CURRENT camera origin; w is zero.
    float current_camera_to_matrix_origin[4], previous_camera_to_matrix_origin[4];
    float xyz_to_rgb[12]; // Three padded rows; w is zero, no exposure.
    Word spectral_nodes, flags, reserved[2];
};
struct SpectralNode {
    float wavelength, r, g, b; // Host-prepared CIE/D65 quadrature RGB weights.
};
#if defined(__METAL_VERSION__)
using Counter = metal::atomic_uint;
#else
using Counter = Word;
#endif
// completed counts successful color conversions (also in color-only mode).
// Other counts are per pixel except passthrough, which counts accepted null/invisible
// candidate callbacks (native traversal may revisit a candidate).
struct Counters {
    Counter failure, completed, color_negative, color_high;
    Counter albedo_clamped, roughness_clamped, mix_fallback, measured_fallback;
    Counter fourier_fallback, layered_approximation, transmission_approximation,
        normal_approximation;
    Counter passthrough, media_approximation, invalid_previous, background;
};
enum Argument : Word {
    config_id,
    raw_xyz_id,
    nodes_id,
    counters_id,
    color_id,
    depth_id,
    motion_id,
    diffuse_id,
    specular_id,
    normal_id,
    roughness_id
};
struct Contract {
    Word version, config_bytes, node_bytes, counters_bytes;
    Word nodes, argument_count, reserved[2];
};
static_assert(sizeof(Config) == 368 && sizeof(SpectralNode) == 16 && sizeof(Counters) == 64 &&
                  sizeof(Contract) == 32,
              "Reconstruction guide ABI");
} // namespace reconstruction_guide_wire
} // namespace rypath
