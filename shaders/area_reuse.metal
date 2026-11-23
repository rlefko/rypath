// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included by render.metal; the leaf evaluator remains the complete PT estimator.
#include "../src/area_wire.h"
#include "../src/mutation_shared.h"
namespace aw = rypath::area_wire;
namespace reservoir = rypath_reservoir;
namespace mutation = rypath_mutation;
struct AreaArguments {
    device const aw::Config *config [[id(aw::config_id)]];
    device rypath_replay::ReplayState *replay [[id(aw::replay_id)]];
    device const wire::Point *leaf_xyz [[id(aw::leaf_xyz_id)]];
    device aw::Accumulator *accumulators [[id(aw::accumulators_id)]];
    device aw::Temporal *temporal [[id(aw::temporal_id)]];
    device const aw::History *history_source [[id(aw::history_source_id)]];
    device aw::History *history_destination [[id(aw::history_destination_id)]];
    device wire::Point *raw_xyz [[id(aw::raw_xyz_id)]];
    device aw::Counters *counters [[id(aw::failure_id)]];
};
inline void area_fail(constant AreaArguments &a, uint failure) {
    atomic_fetch_or_explicit(&a.counters->failure, failure, memory_order_relaxed);
}
inline bool area_failed(constant AreaArguments &a) {
    return atomic_load_explicit(&a.counters->failure, memory_order_relaxed) != 0;
}
inline bool area_config(constant AreaArguments &a) {
    const auto c = *a.config;
    const bool schedule = (c.phase == aw::fresh && c.candidate == 0) ||
                          (c.phase == aw::temporal && c.candidate == 1 && c.history_used == 1) ||
                          (c.phase == aw::spatial && c.candidate <= 4) ||
                          (c.phase == aw::mutation && c.candidate < c.mutation_steps);
    if (c.pixel_count == 0 || c.width == 0 || c.height == 0 ||
        ulong(c.width) * c.height != c.pixel_count || c.history_used > 1 ||
        !mutation::valid_step_count(c.mutation_steps) ||
        ulong(c.pixel_count) * c.mutation_steps > 0xfffffffful ||
        !reservoir::positive_normal(c.target_scale) || !schedule) {
        area_fail(a, aw::invalid_config);
        return false;
    }
    return !area_failed(a);
}
inline uint area_neighbor(constant AreaArguments &a, uint pixel) {
    const uint x = pixel % a.config->width, y = pixel / a.config->width;
    switch (a.config->candidate) {
    case 1:
        return x > 0 ? pixel - 1 : pixel;
    case 2:
        return x < a.config->width - 1 ? pixel + 1 : pixel;
    case 3:
        return y > 0 ? pixel - a.config->width : pixel;
    case 4:
        return y < a.config->height - 1 ? pixel + a.config->width : pixel;
    default:
        return pixel;
    }
}
inline bool area_same_payload(const thread rypath_replay::ReplayState &a,
                              const thread rypath_replay::ReplayState &b) {
    return a.tail_key[0] == b.tail_key[0] && a.tail_key[1] == b.tail_key[1] &&
           a.film[0] == b.film[0] && a.film[1] == b.film[1] && a.lens[0] == b.lens[0] &&
           a.lens[1] == b.lens[1] && a.wavelength == b.wavelength && a.version == b.version;
}
inline bool area_history(constant AreaArguments &a, const thread aw::History &history) {
    if (history.valid != 1 || history.reserved[0] != 0 || history.reserved[1] != 0 ||
        !rypath_replay::valid(history.payload) || !reservoir::positive_normal(history.weight)) {
        area_fail(a, aw::invalid_payload);
        return false;
    }
    return true;
}
inline bool area_target(constant AreaArguments &a, wire::Point xyz, thread float &target) {
    const auto checked = reservoir::bounded_target_xyz(xyz.x, xyz.y, xyz.z, a.config->target_scale);
    if (checked.status != reservoir::Status::ok) {
        area_fail(a, aw::invalid_value);
        return false;
    }
    target = checked.value;
    return true;
}
// Separate decision stream. With phase<=2, candidate<=4 and ordinal<9 this
// mixed-radix tag is injective. Pixel and the complete submission serial retain
// their own counter words; no payload/key/coordinate becomes selection entropy.
inline uint area_selection_word(constant AreaArguments &a, uint pixel, uint ordinal) {
    const uint tag = a.config->phase * 128u + a.config->candidate * 16u + ordinal;
    const auto words =
        rypath_replay::philox({pixel, a.config->serial_low, a.config->serial_high, tag},
                              {a.config->seed_low ^ uint(rypath_replay::DecisionDomain::selection),
                               a.config->seed_high ^ 0x61726531u});
    return words.x;
}
inline bool area_update(constant AreaArguments &a, uint pixel, thread aw::Accumulator &accumulator,
                        const thread rypath_replay::ReplayState &payload, wire::Point xyz,
                        float weight, float target) {
    auto pending = reservoir::prepare_update(accumulator.state, weight, target);
    if (pending.status != reservoir::Status::ok) {
        area_fail(a, aw::invalid_reservoir);
        return false;
    }
    for (uint ordinal = 0; !pending.complete && ordinal < reservoir::maximum_random_words;
         ++ordinal) {
        if (reservoir::feed_random_word(pending, area_selection_word(a, pixel, ordinal)) !=
            reservoir::Status::ok) {
            area_fail(a, aw::invalid_reservoir);
            return false;
        }
    }
    const auto result = reservoir::commit_update(accumulator.state, pending);
    if (result.status != reservoir::Status::ok) {
        area_fail(a, aw::invalid_reservoir);
        return false;
    }
    if (result.selected) {
        accumulator.payload = payload;
        accumulator.xyz = xyz;
    }
    return true;
}
inline bool area_finish(constant AreaArguments &a, const thread aw::Accumulator &accumulator,
                        uint count, thread float &weight) {
    if (accumulator.state.candidate_count != count || !rypath_replay::valid(accumulator.payload)) {
        area_fail(a, aw::invalid_count);
        return false;
    }
    const auto result = reservoir::finalize(accumulator.state);
    if (result.status != reservoir::Status::ok) {
        area_fail(a, aw::invalid_reservoir);
        return false;
    }
    float target;
    if (!area_target(a, accumulator.xyz, target))
        return false;
    if (target != accumulator.state.selected_target) {
        area_fail(a, aw::invalid_reservoir);
        return false;
    }
    weight = result.value;
    return true;
}
kernel void rypath_area_contract(device aw::Contract *out [[buffer(0)]]) {
    *out = {aw::contract,
            sizeof(aw::Config),
            sizeof(aw::History),
            sizeof(aw::Temporal),
            sizeof(aw::Accumulator),
            sizeof(rypath_replay::ReplayState),
            sizeof(aw::Counters),
            0};
}
kernel void rypath_area_reset(constant AreaArguments &a [[buffer(1)]],
                              uint lane [[thread_position_in_grid]]) {
    if (lane == 0) {
        atomic_store_explicit(&a.counters->failure, 0u, memory_order_relaxed);
        atomic_store_explicit(&a.counters->mutations, 0u, memory_order_relaxed);
        atomic_store_explicit(&a.counters->accepted, 0u, memory_order_relaxed);
        atomic_store_explicit(&a.counters->reserved, 0u, memory_order_relaxed);
        (void)area_config(a);
    }
}
kernel void rypath_area_prepare(constant wire::Parameters &p [[buffer(0)]],
                                constant AreaArguments &a [[buffer(1)]],
                                uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a))
        return;
    if (a.config->phase == aw::mutation) {
        area_fail(a, aw::invalid_config);
        return;
    }
    if (p.width != a.config->width || p.height != a.config->height || p.pixel_begin != 0 ||
        p.pixel_count != a.config->pixel_count || p.sampler_kind != wire::independent_sampler) {
        area_fail(a, aw::invalid_config);
        return;
    }
    if (a.config->phase == aw::fresh) {
        a.replay[pixel] =
            rypath_replay::fresh({pixel, p.sample_low, p.sample_high, p.seed_low, p.seed_high});
    } else if (a.config->phase == aw::temporal) {
        const aw::History source = a.history_source[pixel];
        if (!area_history(a, source))
            return;
        a.replay[pixel] = source.payload;
    } else {
        if (a.config->candidate == 0) {
            area_fail(a, aw::invalid_config);
            return;
        }
        const aw::Temporal source = a.temporal[area_neighbor(a, pixel)];
        if (!rypath_replay::valid(source.payload) || !reservoir::positive_normal(source.weight)) {
            area_fail(a, aw::invalid_payload);
            return;
        }
        a.replay[pixel] = source.payload;
    }
}
kernel void rypath_area_merge(constant AreaArguments &a [[buffer(1)]],
                              uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a))
        return;
    if (a.config->phase == aw::mutation) {
        area_fail(a, aw::invalid_config);
        return;
    }
    const auto payload = a.replay[pixel];
    if (!rypath_replay::valid(payload)) {
        area_fail(a, aw::invalid_payload);
        return;
    }
    aw::Accumulator accumulator{};
    float weight = 1;
    uint expected = 0;
    if (a.config->phase == aw::temporal) {
        const aw::History source = a.history_source[pixel];
        if (!area_history(a, source) || !area_same_payload(source.payload, payload)) {
            area_fail(a, aw::invalid_payload);
            return;
        }
        accumulator = a.accumulators[pixel];
        expected = 1;
        weight = source.weight;
    } else if (a.config->phase == aw::spatial) {
        if (a.config->candidate == 0) {
            area_fail(a, aw::invalid_config);
            return;
        }
        const aw::Temporal source = a.temporal[area_neighbor(a, pixel)];
        if (!rypath_replay::valid(source.payload) || !reservoir::positive_normal(source.weight) ||
            !area_same_payload(source.payload, payload)) {
            area_fail(a, aw::invalid_payload);
            return;
        }
        accumulator = a.accumulators[pixel];
        expected = a.config->candidate;
        weight = source.weight;
    }
    if (accumulator.state.candidate_count != expected) {
        area_fail(a, aw::invalid_count);
        return;
    }
    const auto xyz = a.leaf_xyz[pixel];
    float target;
    if (!area_target(a, xyz, target) ||
        !area_update(a, pixel, accumulator, payload, xyz, weight, target))
        return;
    a.accumulators[pixel] = accumulator;
}
kernel void rypath_area_temporal_finish(constant AreaArguments &a [[buffer(1)]],
                                        uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a))
        return;
    const uint count = a.config->history_used ? 2u : 1u;
    if ((a.config->history_used && a.config->phase != aw::temporal) ||
        (!a.config->history_used && a.config->phase != aw::fresh)) {
        area_fail(a, aw::invalid_config);
        return;
    }
    const auto accumulator = a.accumulators[pixel];
    float weight;
    if (!area_finish(a, accumulator, count, weight))
        return;
    a.temporal[pixel] = {
        accumulator.payload, weight, accumulator.xyz, accumulator.state.selected_target, {0, 0, 0}};
}
kernel void rypath_area_spatial_begin(constant AreaArguments &a [[buffer(1)]],
                                      uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a))
        return;
    if (a.config->phase != aw::spatial || a.config->candidate != 0) {
        area_fail(a, aw::invalid_config);
        return;
    }
    const auto center = a.temporal[pixel];
    float target;
    if (!rypath_replay::valid(center.payload) || !reservoir::positive_normal(center.weight)) {
        area_fail(a, aw::invalid_payload);
        return;
    }
    if (!area_target(a, center.xyz, target))
        return;
    if (target != center.target) {
        area_fail(a, aw::invalid_value);
        return;
    }
    aw::Accumulator accumulator{};
    if (!area_update(a, pixel, accumulator, center.payload, center.xyz, center.weight, target))
        return;
    a.accumulators[pixel] = accumulator;
}
kernel void rypath_area_finalize(constant AreaArguments &a [[buffer(1)]],
                                 uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a))
        return;
    if (a.config->phase != aw::spatial || a.config->candidate != 4) {
        area_fail(a, aw::invalid_config);
        return;
    }
    const auto accumulator = a.accumulators[pixel];
    float weight;
    if (!area_finish(a, accumulator, aw::spatial_candidates, weight))
        return;
    const float3 xyz(accumulator.xyz.x, accumulator.xyz.y, accumulator.xyz.z);
    const float3 weighted = xyz * weight;
    if (!all(isfinite(weighted)) || !reservoir::normal_or_zero(weighted.x) ||
        !reservoir::normal_or_zero(weighted.y) || !reservoir::normal_or_zero(weighted.z) ||
        any((xyz != float3(0)) & (weighted == float3(0)))) {
        area_fail(a, aw::invalid_value);
        return;
    }
    a.history_destination[pixel] = {accumulator.payload, weight, 1, {0, 0}};
    a.raw_xyz[pixel] = {weighted.x, weighted.y, weighted.z};
}

// The source cache always belongs to the CURRENT destination evaluation. Each
// configured step proposes independently, fully replays that proposal, then
// finalizes its actual represented acceptance probability before the next step.
inline mutation::MutationIdentity area_mutation_identity(constant AreaArguments &a, uint pixel) {
    return {pixel, a.config->serial_low, a.config->serial_high, a.config->seed_low,
            a.config->seed_high};
}
inline bool area_mutation_source(constant AreaArguments &a, const thread aw::Temporal &source) {
    const auto target = reservoir::bounded_target_xyz(source.xyz.x, source.xyz.y, source.xyz.z,
                                                      a.config->target_scale);
    if (!rypath_replay::valid(source.payload) || !reservoir::positive_normal(source.weight) ||
        !reservoir::positive_normal(source.target) || source.reserved[0] != 0 ||
        source.reserved[1] != 0 || source.reserved[2] != 0 ||
        target.status != reservoir::Status::ok || target.value != source.target) {
        area_fail(a, aw::invalid_mutation);
        return false;
    }
    return true;
}
inline mutation::ProposalResult
area_mutation_proposal(constant AreaArguments &a, uint pixel,
                       const thread rypath_replay::ReplayState &source) {
    const auto identity = area_mutation_identity(a, pixel);
    const auto entropy =
        mutation::decision_words(identity, a.config->candidate, mutation::Entropy::proposal);
    if (entropy.status != mutation::Status::ok) {
        area_fail(a, aw::invalid_mutation);
        return {source, mutation::Block::film, entropy.status};
    }
    const auto proposal = mutation::propose(source, entropy.value);
    if (proposal.status != mutation::Status::ok)
        area_fail(a, aw::invalid_mutation);
    return proposal;
}
kernel void rypath_area_mutation_prepare(constant wire::Parameters &p [[buffer(0)]],
                                         constant AreaArguments &a [[buffer(1)]],
                                         uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a))
        return;
    if (a.config->phase != aw::mutation || p.width != a.config->width ||
        p.height != a.config->height || p.pixel_begin != 0 ||
        p.pixel_count != a.config->pixel_count || p.sampler_kind != wire::independent_sampler) {
        area_fail(a, aw::invalid_config);
        return;
    }
    const auto source = a.temporal[pixel];
    if (!area_mutation_source(a, source))
        return;
    const auto proposal = area_mutation_proposal(a, pixel, source.payload);
    if (proposal.status != mutation::Status::ok)
        return;
    a.replay[pixel] = proposal.payload;
}
kernel void rypath_area_mutation_finish(constant AreaArguments &a [[buffer(1)]],
                                        uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a))
        return;
    if (a.config->phase != aw::mutation) {
        area_fail(a, aw::invalid_config);
        return;
    }
    const auto source = a.temporal[pixel];
    if (!area_mutation_source(a, source))
        return;
    const auto proposal = area_mutation_proposal(a, pixel, source.payload);
    const auto evaluated = a.replay[pixel];
    if (proposal.status != mutation::Status::ok ||
        !area_same_payload(proposal.payload, evaluated)) {
        area_fail(a, aw::invalid_mutation);
        return;
    }
    const auto xyz = a.leaf_xyz[pixel];
    const auto target = reservoir::bounded_target_xyz(xyz.x, xyz.y, xyz.z, a.config->target_scale);
    if (target.status != reservoir::Status::ok) {
        area_fail(a, aw::invalid_mutation);
        return;
    }
    auto pending = mutation::prepare_acceptance(source.weight, source.target, target.value);
    if (pending.status != mutation::Status::ok) {
        area_fail(a, aw::invalid_mutation);
        return;
    }
    const auto identity = area_mutation_identity(a, pixel);
    for (uint ordinal = 0; !pending.decision.complete && ordinal < reservoir::maximum_random_words;
         ++ordinal) {
        const auto entropy = mutation::acceptance_word(identity, a.config->candidate, ordinal);
        if (entropy.status != mutation::Status::ok ||
            mutation::feed_random_word(pending, entropy.value) != mutation::Status::ok) {
            area_fail(a, aw::invalid_mutation);
            return;
        }
    }
    const auto result = mutation::finalize_acceptance(pending);
    if (result.status != mutation::Status::ok) {
        area_fail(a, aw::invalid_mutation);
        return;
    }
    // Counts are diagnostics, never an acceptance/stopping criterion. Valid
    // dispatches attempt exactly pixel_count*mutation_steps; configuration
    // validation proves this product fits the shared 32-bit counters.
    const uint maximum = a.config->pixel_count * a.config->mutation_steps;
    if (atomic_fetch_add_explicit(&a.counters->mutations, 1u, memory_order_relaxed) >= maximum) {
        area_fail(a, aw::invalid_mutation);
        return;
    }
    if (result.accepted) {
        if (atomic_fetch_add_explicit(&a.counters->accepted, 1u, memory_order_relaxed) >= maximum) {
            area_fail(a, aw::invalid_mutation);
            return;
        }
        a.temporal[pixel] = {proposal.payload, result.weight, xyz, target.value, {0, 0, 0}};
    }
    // Rejection retains the exact old payload/UCW/XYZ/target. The host always
    // launches every configured step, including after rejections/equal targets.
}
