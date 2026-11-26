// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
#include "replay_shared.h"
#include "reservoir_shared.h"
namespace rypath {
namespace area_wire {
using Word = render_wire::Word;
enum : Word {
    contract = 7,
    spatial_candidates = 5,
    config_count = 13,
    mutation_config_begin = 7,
    temporal_reverse_config = 11,
    spatial_finalize_config = 12
};
enum Phase : Word {
    fresh = 0,
    temporal = 1,
    spatial = 2,
    mutation = 3,
    temporal_reverse = 4,
    spatial_final = 5
};
enum Output : Word { selected_output = 0, conditional_mean_output = 1 };
enum MutationPolicy : Word { legacy_mutation = 0, spectral_refresh_mutation = 1 };
enum Stencil : Word { adjacent_stencil = 0, reciprocal16_stencil = 1 };
enum Failure : Word {
    invalid_config = 1,
    invalid_payload = 2,
    invalid_value = 4,
    invalid_reservoir = 8,
    invalid_count = 16,
    invalid_mutation = 32
};
struct Config {
    Word pixel_count, width, height, phase;
    Word candidate, history_used, serial_low, serial_high;
    Word seed_low, seed_high;
    float target_scale;
    Word mutation_steps;
    Word output_mode, mutation_policy, stencil, temporal_cache;
    int offset_x[2], offset_y[2];
};
#if defined(__METAL_VERSION__)
using Counter = metal::atomic_uint;
#else
using Counter = Word;
#endif
// First atomic failure reporter only; host reads after command completion.
// source_line refers to shaders/area_reuse.metal. Operand layouts follow that
// site's diagnostic call and preserve raw IEEE float bits without conversion.
struct FirstFailure {
    Word source_line, pixel, phase, candidate;
    Word serial_low, serial_high, flags, substatus;
    Word operands[16];
};
struct Counters {
    Counter failure, mutations, accepted, reserved;
    FirstFailure first_failure;
};
struct History {
    rypath_replay::ReplayState payload;
    float normalizer, target;
    Word valid, reserved;
    // Exact selected pre-weight F(payload); never the published conditional mean.
    render_wire::Point xyz;
};
struct Temporal {
    rypath_replay::ReplayState payload;
    float normalizer;
    render_wire::Point xyz;
    float target;
    Word reserved[3];
};
struct Accumulator {
    rypath_reservoir::State state;
    rypath_replay::ReplayState payload;
    render_wire::Point xyz;
    float target;
};
enum Argument : Word {
    config_id,
    replay_id,
    leaf_xyz_id,
    accumulators_id,
    temporal_id,
    history_source_id,
    history_destination_id,
    raw_xyz_id,
    failure_id,
    cross_targets_id
};
struct Contract {
    Word version, config_bytes, history_bytes, temporal_bytes;
    Word accumulator_bytes, replay_bytes, counters_bytes, reserved;
};
static_assert(sizeof(FirstFailure) == 96 && sizeof(FirstFailure) <= 256,
              "Area first-failure record ABI");
static_assert(sizeof(Config) == 80 && sizeof(History) == 60 && sizeof(Temporal) == 64 &&
                  sizeof(Accumulator) == 68 && sizeof(Contract) == 32 && sizeof(Counters) == 112,
              "Area replay host/shader ABI");
} // namespace area_wire
} // namespace rypath
