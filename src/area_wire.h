// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
#include "replay_shared.h"
#include "reservoir_shared.h"
namespace rypath {
namespace area_wire {
using Word = render_wire::Word;
enum : Word { contract = 3, spatial_candidates = 5, config_count = 11, mutation_config_begin = 7 };
enum Phase : Word { fresh = 0, temporal = 1, spatial = 2, mutation = 3 };
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
};
#if defined(__METAL_VERSION__)
using Counter = metal::atomic_uint;
#else
using Counter = Word;
#endif
struct Counters {
    Counter failure, mutations, accepted, reserved;
};
struct History {
    rypath_replay::ReplayState payload;
    float weight;
    Word valid, reserved[2];
};
struct Temporal {
    rypath_replay::ReplayState payload;
    float weight;
    render_wire::Point xyz;
    float target;
    Word reserved[3];
};
struct Accumulator {
    rypath_reservoir::State state;
    rypath_replay::ReplayState payload;
    render_wire::Point xyz;
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
    failure_id
};
struct Contract {
    Word version, config_bytes, history_bytes, temporal_bytes;
    Word accumulator_bytes, replay_bytes, counters_bytes, reserved;
};
static_assert(sizeof(Config) == 48 && sizeof(History) == 48 && sizeof(Temporal) == 64 &&
                  sizeof(Accumulator) == 64 && sizeof(Contract) == 32 && sizeof(Counters) == 16,
              "Area replay host/shader ABI");
} // namespace area_wire
} // namespace rypath
