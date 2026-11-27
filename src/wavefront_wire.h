// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
namespace rypath {
namespace wavefront_wire {
using Word = render_wire::Word;
enum : Word {
    contract = 21,
    default_capacity = 65536,
    packet_layout_constant = 23,
    packet_events_constant = 24,
    contribution_mode_constant = 27,
    enhanced_mode_constant = 28
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
    hero_cold_id,
    contribution_evaluation_id,
    contribution_state_id,
    enhanced_records_id,
    enhanced_evaluation_id,
    enhanced_suffix_id,
    work_list_id
};
struct Config {
    Word pixel_count, capacity, packet, model;
    Word group_width, replay_enabled, packet_events, packet_layout;
    Word contribution_mode, contribution_depth;
    float contribution_target_scale;
    Word contribution_replay; // full 0 / prefix 1; summed mode requires 0.
    Word contribution_serial_low, contribution_serial_high, compact_enhanced_pairs, reserved2;
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
    Counter null_completed;
    Counter suffix_hits[3], suffix_misses;
    Counter work_count;
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
    Word hero_cold_bytes, hero_cold_alignment;
    Word contribution_state_bytes, contribution_state_alignment;
    Word enhanced_state_bytes, enhanced_state_alignment;
    Word enhanced_record_bytes, enhanced_evaluation_bytes;
    Word suffix_entry_bytes;
};
static_assert(sizeof(Config) == 64 && sizeof(Counters) == 56 && sizeof(DispatchArguments) == 12 &&
                  sizeof(Contract) == 108,
              "Wavefront host/shader ABI");
static_assert(contribution_evaluation_id == 18 && contribution_state_id == 19,
              "Wavefront v17 contribution argument ABI");
} // namespace wavefront_wire
} // namespace rypath

static_assert(__builtin_offsetof(rypath::wavefront_wire::Config, contribution_replay) == 44,
              "Contribution replay consumes the original offset 44 reserved word");

static_assert(rypath::wavefront_wire::work_list_id == 23 &&
                  __builtin_offsetof(rypath::wavefront_wire::Config, compact_enhanced_pairs) ==
                      56 &&
                  __builtin_offsetof(rypath::wavefront_wire::Counters, work_count) == 52,
              "Wavefront v21 compact request ABI");
