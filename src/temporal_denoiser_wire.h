// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
namespace rypath {
namespace temporal_denoiser_wire {
using Word = render_wire::Word;
enum : Word { version = 3, maximum_history = 64, maximum_iterations = 5 };
enum Failure : Word { invalid_input = 1u, arithmetic_overflow = 2u, invalid_history = 4u };
struct Config {
    Word width, height, history_limit, reset_history;
    Word step, iteration, version, spatial_options;
};
#if defined(__METAL_VERSION__)
using Counter = metal::atomic_uint;
#else
using Counter = Word;
#endif
struct Counters {
    Counter failure, completed, history_accepted, history_rejected;
};
enum Texture : Word {
    color_id,
    depth_id,
    motion_id,
    diffuse_id,
    specular_id,
    normal_id,
    previous_depth_id,
    identity_id,
    history_mean_id,
    history_variance_id,
    history_count_id,
    history_depth_id,
    history_normal_id,
    history_identity_id,
    next_mean_id,
    next_variance_id,
    next_count_id,
    next_depth_id,
    next_normal_id,
    next_identity_id,
    source_mean_id,
    source_variance_id,
    destination_mean_id,
    destination_variance_id,
    output_id,
    spatial_position_id,
    spatial_normal_id,
    spatial_group_id,
    spatial_protection_id
};
struct Contract {
    Word version, config_bytes, counters_bytes, texture_count;
    Word reserved[4];
};
static_assert(sizeof(Contract) == 32, "Temporal contract ABI");
static_assert(sizeof(Config) == 32 && sizeof(Counters) == 16, "Temporal denoiser ABI");
} // namespace temporal_denoiser_wire
} // namespace rypath
