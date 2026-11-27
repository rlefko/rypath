// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "area_pair_atlas_shared.h"
#include "contribution_wire.h"
#include "enhanced_motion_wire.h"
#include "enhanced_wire.h"
#include "render_wire.h"
#include "replay_shared.h"
#include "reservoir_shared.h"
namespace rypath {
namespace area_wire {
using Word = render_wire::Word;
enum : Word {
    contract = 17,
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
enum Stencil : Word {
    adjacent_stencil = 0,
    reciprocal16_stencil = 1,
    paired_gaussian16_stencil = 2
};
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
    // Immutable across this session/history domain. Summed mode requires depth0.
    Word contribution_mode, contribution_depth;
    Word history_source_bank;
    // Actual evaluator context; temporal_reverse uses the frozen source context.
    // Zero disables all suffix-arena reads/writes, including sentinel indexing.
    Word suffix_context_low, suffix_context_high;
    Word temporal_reprojection;
    Word stochastic_spatial_pairs, forward_atlas, canonical_atlas;
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
    Word valid, selected_slot;
    // Exact selected pre-weight F(payload[,j]); never the conditional mean.
    render_wire::Point xyz;
    // Structural Enhanced confidence; legacy records require the zero sentinel.
    Word confidence;
    enhanced_wire::Reference record;
    float target_h_fraction;
    int target_h_exponent;
    // Enhanced stores normalized fractions in normalizer/target and these
    // exponents separately. Legacy records keep actual FLOAT values and zero exponents.
    int normalizer_exponent, target_exponent;
};
struct Temporal {
    rypath_replay::ReplayState payload;
    float normalizer;
    render_wire::Point xyz;
    float target;
    Word selected_slot, reserved[2];
    Word confidence;
    enhanced_wire::Reference record;
    float target_h_fraction;
    int target_h_exponent;
    int normalizer_exponent, target_exponent;
};
struct Accumulator {
    rypath_reservoir::State state;
    rypath_replay::ReplayState payload;
    render_wire::Point xyz;
    float target;
    Word selected_slot;
    // Enhanced zero-score inputs still count as structurally present proposals.
    // The positive-score categorical selector's count is a separate quantity.
    Word logical_count;
    Word confidence;
    enhanced_wire::Reference record;
    float target_h_fraction;
    int target_h_exponent;
    // Enhanced state.selected_target mirrors the physical target above.
    // Legacy's unit-target categorical selector and zero exponents are unchanged.
    int score_exponent, target_exponent;
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
    cross_targets_id,
    contribution_evaluation_id,
    enhanced_records_id,
    enhanced_evaluation_id,
    enhanced_suffix_id,
    donors_id,
    paired_atlas_id,
    paired_config_id
};
struct Contract {
    Word version, config_bytes, history_bytes, temporal_bytes;
    Word accumulator_bytes, replay_bytes, counters_bytes, contribution_evaluation_bytes;
    Word suffix_entry_bytes, donor_bytes;
    Word paired_config_bytes, paired_entry_bytes;
};
static_assert(__builtin_offsetof(History, confidence) == 60 &&
                  __builtin_offsetof(Temporal, confidence) == 64 &&
                  __builtin_offsetof(Accumulator, confidence) == 76,
              "Area v10 structural confidence word offsets");
static_assert(sizeof(FirstFailure) == 96 && sizeof(FirstFailure) <= 256,
              "Area first-failure record ABI");
static_assert(sizeof(Config) == 116 && sizeof(History) == 88 && sizeof(Temporal) == 92 &&
                  sizeof(Accumulator) == 104 && sizeof(Contract) == 48 && sizeof(Counters) == 112,
              "Area replay host/shader ABI");
} // namespace area_wire
} // namespace rypath

static_assert(rypath::area_wire::donors_id == 14 &&
                  __builtin_offsetof(rypath::area_wire::Config, temporal_reprojection) == 100,
              "Area v15 frozen temporal donor ABI");

static_assert(rypath::area_wire::paired_atlas_id == 15 && rypath::area_wire::paired_config_id == 16,
              "Area v16 immutable paired atlas arguments");
