// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
namespace rypath {
namespace wavefront_wire {
using Word = render_wire::Word;
enum : Word {
    contract = 10,
    default_capacity = 65536,
    packet_layout_constant = 23,
    packet_events_constant = 24
};
enum Failure : Word {
    invalid_config = 1u,
    queue_overflow = 2u,
    invalid_slot = 4u,
    invalid_sample = 8u,
    duplicate_sample = 16u,
    invalid_counters = 32u
};
enum Argument : Word {
    config_id,
    scalar_state_id,
    packet_state_id,
    queue_in_id,
    queue_out_id,
    free_slots_id,
    counters_id,
    raw_xyz_id,
    cie_id,
    done_id,
    current_dispatch_id,
    refill_dispatch_id,
    packet_hot_id,
    packet_cold_id,
    packet_hit_id,
    replay_input_id,
    hero_state_id,
    hero_cold_id
};
struct Config {
    Word pixel_count, capacity, packet, model;
    Word group_width, replay_enabled, packet_events, packet_layout;
};
#if defined(__METAL_VERSION__)
using Counter = metal::atomic_uint;
#else
using Counter = Word;
#endif
// Separate dispatch boundaries/barriers publish producer writes. Atomic counters
// reserve queue slots only; they are not a substitute for that synchronization.
struct Counters {
    Counter current, out, free, next_pixel, completed, failure, rounds, refill_begin;
};
struct DispatchArguments {
    Word x, y, z;
};
struct Contract {
    Word version, scalar_bytes, scalar_alignment, packet_bytes;
    Word packet_alignment, config_bytes, counters_bytes, dispatch_bytes;
    Word hot_bytes, hot_alignment, cold_bytes, cold_alignment;
    Word hit_bytes, hit_alignment;
    Word hero_bytes, hero_alignment, hero_hot_bytes, hero_hot_alignment;
    Word hero_cold_bytes, hero_cold_alignment, reserved[2];
};
static_assert(sizeof(Config) == 32 && sizeof(Counters) == 32 && sizeof(DispatchArguments) == 12 &&
                  sizeof(Contract) == 88,
              "Wavefront host/shader ABI");
} // namespace wavefront_wire
} // namespace rypath
