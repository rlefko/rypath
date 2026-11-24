// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included by render.metal; the leaf evaluator remains the complete PT estimator.
#include "../src/area_wire.h"
#include "../src/mutation_shared.h"
#include "../src/reuse_mis_shared.h"
namespace aw = rypath::area_wire;
namespace reservoir = rypath_reservoir;
namespace mutation = rypath_mutation;
namespace reuse_mis = rypath_reuse_mis;
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
    device float *cross_targets [[id(aw::cross_targets_id)]];
};
// The existing sticky flag atomic also selects the sole record writer. Other
// lanes never read this record; command completion precedes every host read.
inline void area_fail_record(constant AreaArguments &a, uint failure, uint pixel, uint line,
                             uint status, uint4 w0, uint4 w1 = uint4(0), uint4 w2 = uint4(0),
                             uint4 w3 = uint4(0)) {
    const uint old = atomic_fetch_or_explicit(&a.counters->failure, failure, memory_order_relaxed);
    if (old != 0)
        return;
    device auto &record = a.counters->first_failure;
    record.source_line = line;
    record.pixel = pixel;
    record.phase = a.config->phase;
    record.candidate = a.config->candidate;
    record.serial_low = a.config->serial_low;
    record.serial_high = a.config->serial_high;
    record.flags = failure;
    record.substatus = status;
    const uint4 words[4] = {w0, w1, w2, w3};
    for (uint i = 0; i < 16; ++i)
        record.operands[i] = words[i / 4][i % 4];
}
inline void area_fail_at(constant AreaArguments &a, uint failure, uint pixel, uint line) {
    area_fail_record(
        a, failure, pixel, line, 0,
        uint4(a.config->pixel_count, a.config->width, a.config->height, a.config->history_used),
        uint4(a.config->mutation_steps, a.config->output_mode,
              as_type<uint>(a.config->target_scale), a.config->seed_low),
        uint4(a.config->seed_high, a.config->reserved[0], a.config->reserved[1],
              a.config->reserved[2]));
}
#define AREA_FAIL(a, flag) area_fail_at(a, flag, pixel, __LINE__)
#define AREA_DETAIL(a, flag, status, ...)                                                          \
    area_fail_record(a, flag, pixel, __LINE__, uint(status), __VA_ARGS__)
inline bool area_failed(constant AreaArguments &a) {
    return atomic_load_explicit(&a.counters->failure, memory_order_relaxed) != 0;
}
inline bool area_config(constant AreaArguments &a, uint pixel) {
    const auto c = *a.config;
    const bool schedule =
        (c.phase == aw::fresh && c.candidate == 0) ||
        (c.phase == aw::temporal && c.candidate == 1 && c.history_used == 1) ||
        (c.phase == aw::temporal_reverse && c.candidate == 0 && c.history_used == 1) ||
        (c.phase == aw::spatial_final && c.candidate == 0) ||
        (c.phase == aw::spatial && c.candidate <= 4) ||
        (c.phase == aw::mutation && c.candidate < c.mutation_steps);
    if (c.pixel_count == 0 || c.width == 0 || c.height == 0 ||
        ulong(c.width) * c.height != c.pixel_count || c.history_used > 1 ||
        !mutation::valid_step_count(c.mutation_steps) ||
        ulong(c.pixel_count) * c.mutation_steps > 0xfffffffful ||
        !reservoir::positive_normal(c.target_scale) ||
        c.output_mode > aw::conditional_mean_output || c.reserved[0] || c.reserved[1] ||
        c.reserved[2] || !schedule) {
        AREA_FAIL(a, aw::invalid_config);
        return false;
    }
    return !area_failed(a);
}
inline uint area_neighbor_direction(constant AreaArguments &a, uint pixel, uint direction) {
    const uint x = pixel % a.config->width, y = pixel / a.config->width;
    switch (direction) {
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
inline uint area_neighbor(constant AreaArguments &a, uint pixel) {
    return area_neighbor_direction(a, pixel, a.config->candidate);
}
inline bool area_same_payload(const thread rypath_replay::ReplayState &a,
                              const thread rypath_replay::ReplayState &b) {
    return a.tail_key[0] == b.tail_key[0] && a.tail_key[1] == b.tail_key[1] &&
           a.film[0] == b.film[0] && a.film[1] == b.film[1] && a.lens[0] == b.lens[0] &&
           a.lens[1] == b.lens[1] && a.wavelength == b.wavelength && a.version == b.version;
}
inline uint4 area_payload_head(const thread rypath_replay::ReplayState &p) {
    return uint4(p.tail_key[0], p.tail_key[1], p.film[0], p.film[1]);
}
inline uint4 area_payload_tail(const thread rypath_replay::ReplayState &p) {
    return uint4(p.lens[0], p.lens[1], p.wavelength, p.version);
}
inline uint4 area_state_values(const thread reservoir::State &state) {
    return uint4(as_type<uint>(state.score_sum), as_type<uint>(state.selected_target),
                 state.candidate_count, state.selected_index);
}
inline bool area_history(constant AreaArguments &a, uint pixel, const thread aw::History &history) {
    if (history.valid != 1 || history.reserved != 0 || !rypath_replay::valid(history.payload) ||
        !reservoir::positive_normal(history.normalizer) ||
        !reservoir::positive_normal(history.target)) {
        AREA_DETAIL(a, aw::invalid_payload, 0,
                    uint4(as_type<uint>(history.normalizer), as_type<uint>(history.target),
                          history.valid, history.reserved),
                    area_payload_head(history.payload), area_payload_tail(history.payload));
        return false;
    }
    return true;
}
inline bool area_target(constant AreaArguments &a, uint pixel, wire::Point xyz,
                        thread float &target) {
    const auto checked = reuse_mis::importance_target(xyz.x, xyz.y, xyz.z, a.config->target_scale);
    if (checked.status != reservoir::Status::ok) {
        AREA_DETAIL(a, aw::invalid_value, checked.status,
                    as_type<uint4>(float4(xyz.x, xyz.y, xyz.z, a.config->target_scale)));
        return false;
    }
    target = checked.value;
    return true;
}
// Separate decision stream. With phase<=5, candidate<=4 and ordinal<9 this
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
                        float score, float target) {
    // The shared categorical selector receives an already MIS-weighted score.
    // Its target is one; the physical selected target lives separately.
    auto pending = reservoir::prepare_update(accumulator.state, score, 1.f);
    if (pending.status != reservoir::Status::ok) {
        AREA_DETAIL(a, aw::invalid_reservoir, pending.status, area_state_values(pending.source),
                    as_type<uint4>(float4(score, target, pending.score_sum, pending.target)),
                    uint4(uint(pending.source.status), uint(pending.status), pending.words_used,
                          pending.compared_bits),
                    uint4(pending.rare_probability.significand, pending.rare_probability.bit_count,
                          pending.complete, pending.selected));
        return false;
    }
    for (uint ordinal = 0; !pending.complete && ordinal < reservoir::maximum_random_words;
         ++ordinal) {
        if (reservoir::feed_random_word(pending, area_selection_word(a, pixel, ordinal)) !=
            reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_reservoir, pending.status, area_state_values(pending.source),
                        as_type<uint4>(float4(score, target, pending.score_sum, pending.target)),
                        uint4(uint(pending.source.status), uint(pending.status), pending.words_used,
                              pending.compared_bits),
                        uint4(pending.rare_probability.significand,
                              pending.rare_probability.bit_count, pending.complete,
                              pending.selected));
            return false;
        }
    }
    const auto result = reservoir::commit_update(accumulator.state, pending);
    if (result.status != reservoir::Status::ok) {
        AREA_DETAIL(a, aw::invalid_reservoir, result.status, area_state_values(pending.source),
                    as_type<uint4>(float4(score, target, pending.score_sum, pending.target)),
                    uint4(uint(pending.source.status), uint(pending.status), pending.words_used,
                          pending.compared_bits),
                    uint4(pending.rare_probability.significand, pending.rare_probability.bit_count,
                          pending.complete, pending.selected));
        return false;
    }
    if (result.selected) {
        accumulator.payload = payload;
        accumulator.xyz = xyz;
        accumulator.target = target;
    }
    return true;
}
inline bool area_finish(constant AreaArguments &a, uint pixel,
                        const thread aw::Accumulator &accumulator, uint count,
                        thread float &normalizer) {
    if (accumulator.state.candidate_count != count || !rypath_replay::valid(accumulator.payload)) {
        AREA_DETAIL(a, aw::invalid_count, accumulator.state.status,
                    area_state_values(accumulator.state), uint4(count, 0, 0, 0),
                    area_payload_head(accumulator.payload), area_payload_tail(accumulator.payload));
        return false;
    }
    if (!reservoir::valid_state(accumulator.state) || accumulator.state.selected_target != 1.f) {
        AREA_DETAIL(a, aw::invalid_reservoir, accumulator.state.status,
                    area_state_values(accumulator.state));
        return false;
    }
    float target;
    if (!area_target(a, pixel, accumulator.xyz, target))
        return false;
    if (target != accumulator.target) {
        AREA_DETAIL(a, aw::invalid_reservoir, 0,
                    as_type<uint4>(
                        float4(target, accumulator.target, accumulator.xyz.x, accumulator.xyz.y)),
                    uint4(as_type<uint>(accumulator.xyz.z), 0, 0, 0));
        return false;
    }
    // The MIS functions form a partition of unity. Another /K would bias it.
    normalizer = accumulator.state.score_sum;
    return true;
}
inline bool area_temporal_valid(constant AreaArguments &a, uint pixel,
                                const thread aw::Temporal &source) {
    float target = 0;
    if (!rypath_replay::valid(source.payload) || !reservoir::positive_normal(source.normalizer) ||
        source.reserved[0] || source.reserved[1] || source.reserved[2] ||
        !area_target(a, pixel, source.xyz, target) || source.target != target) {
        AREA_DETAIL(a, aw::invalid_value, 0,
                    as_type<uint4>(float4(source.normalizer, source.target, target, source.xyz.x)),
                    uint4(as_type<uint>(source.xyz.y), as_type<uint>(source.xyz.z),
                          source.reserved[0], source.reserved[1]),
                    area_payload_head(source.payload), area_payload_tail(source.payload));
        return false;
    }
    return true;
}
inline bool area_weighted_xyz(constant AreaArguments &a, uint pixel, float normalizer,
                              wire::Point xyz, float target, thread wire::Point &weighted) {
    const auto x = reuse_mis::signed_contribution(normalizer, xyz.x, target);
    const auto y = reuse_mis::signed_contribution(normalizer, xyz.y, target);
    const auto z = reuse_mis::signed_contribution(normalizer, xyz.z, target);
    if (x.status != reservoir::Status::ok || y.status != reservoir::Status::ok ||
        z.status != reservoir::Status::ok) {
        AREA_DETAIL(a, aw::invalid_value, x.status,
                    as_type<uint4>(float4(normalizer, xyz.x, xyz.y, xyz.z)),
                    uint4(as_type<uint>(target), uint(x.status), uint(y.status), uint(z.status)),
                    as_type<uint4>(float4(x.value, y.value, z.value, 0)));
        return false;
    }
    weighted = {x.value, y.value, z.value};
    return true;
}
inline bool area_add_output(constant AreaArguments &a, uint pixel, float score, wire::Point xyz,
                            float target) {
    if (a.config->output_mode != aw::conditional_mean_output)
        return true;
    wire::Point term;
    if (!area_weighted_xyz(a, pixel, score, xyz, target, term))
        return false;
    const auto old = a.raw_xyz[pixel];
    const auto x = reuse_mis::signed_sum(old.x, term.x);
    const auto y = reuse_mis::signed_sum(old.y, term.y);
    const auto z = reuse_mis::signed_sum(old.z, term.z);
    const wire::Point sum{x.value, y.value, z.value};
    if (x.status != reservoir::Status::ok || y.status != reservoir::Status::ok ||
        z.status != reservoir::Status::ok) {
        AREA_DETAIL(a, aw::invalid_value, 0, as_type<uint4>(float4(old.x, old.y, old.z, score)),
                    as_type<uint4>(float4(term.x, term.y, term.z, target)),
                    uint4(uint(x.status), uint(y.status), uint(z.status), 0));
        return false;
    }
    a.raw_xyz[pixel] = sum;
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
        a.counters->first_failure = aw::FirstFailure{};
        (void)area_config(a, 0xffffffffu);
    }
}
kernel void rypath_area_prepare(constant wire::Parameters &p [[buffer(0)]],
                                constant AreaArguments &a [[buffer(1)]],
                                uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    if (a.config->phase == aw::mutation) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    if (p.width != a.config->width || p.height != a.config->height || p.pixel_begin != 0 ||
        p.pixel_count != a.config->pixel_count || p.sampler_kind != wire::independent_sampler) {
        AREA_DETAIL(
            a, aw::invalid_config, 0, uint4(p.width, p.height, p.pixel_begin, p.pixel_count),
            uint4(a.config->width, a.config->height, a.config->pixel_count, p.sampler_kind));
        return;
    }
    if (a.config->phase == aw::fresh) {
        a.replay[pixel] =
            rypath_replay::fresh({pixel, p.sample_low, p.sample_high, p.seed_low, p.seed_high});
    } else if (a.config->phase == aw::temporal_reverse) {
        const aw::Temporal source = a.temporal[pixel];
        if (!area_temporal_valid(a, pixel, source))
            return;
        a.replay[pixel] = source.payload;
    } else if (a.config->phase == aw::temporal) {
        const aw::History source = a.history_source[pixel];
        if (!area_history(a, pixel, source))
            return;
        a.replay[pixel] = source.payload;
    } else {
        if (a.config->candidate == 0) {
            AREA_FAIL(a, aw::invalid_config);
            return;
        }
        const aw::Temporal source = a.temporal[area_neighbor(a, pixel)];
        if (!area_temporal_valid(a, pixel, source)) {
            return;
        }
        a.replay[pixel] = source.payload;
    }
}
kernel void rypath_area_merge(constant AreaArguments &a [[buffer(1)]],
                              uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    if (a.config->phase == aw::mutation) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    const auto payload = a.replay[pixel];
    if (!rypath_replay::valid(payload)) {
        AREA_DETAIL(a, aw::invalid_payload, 0, area_payload_head(payload),
                    area_payload_tail(payload));
        return;
    }
    aw::Accumulator accumulator{};
    auto xyz = a.leaf_xyz[pixel];
    float target;
    if (!area_target(a, pixel, xyz, target))
        return;
    float score = target;
    uint expected = 0;
    if (a.config->phase == aw::fresh) {
        a.temporal[pixel] = {payload, target, xyz, target, {0, 0, 0}};
        if (a.config->history_used)
            return; // Fresh score also needs this payload's previous-context target.
    } else if (a.config->phase == aw::temporal_reverse) {
        const aw::Temporal source = a.temporal[pixel];
        if (!area_temporal_valid(a, pixel, source) || !area_same_payload(source.payload, payload)) {
            AREA_DETAIL(a, aw::invalid_payload, 0, area_payload_head(source.payload),
                        area_payload_tail(source.payload), area_payload_head(payload),
                        area_payload_tail(payload));
            return;
        }
        const auto checked = reuse_mis::temporal_fresh_score(source.target, target);
        if (checked.status != reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_value, checked.status,
                        as_type<uint4>(float4(source.target, target, checked.value, 0)));
            return;
        }
        score = checked.value;
        xyz = source.xyz;
        target = source.target;
    } else if (a.config->phase == aw::temporal) {
        const aw::History source = a.history_source[pixel];
        if (!area_history(a, pixel, source) || !area_same_payload(source.payload, payload)) {
            AREA_DETAIL(a, aw::invalid_payload, 0, area_payload_head(source.payload),
                        area_payload_tail(source.payload), area_payload_head(payload),
                        area_payload_tail(payload));
            return;
        }
        accumulator = a.accumulators[pixel];
        expected = 1;
        const auto checked =
            reuse_mis::temporal_history_score(source.normalizer, target, source.target);
        if (checked.status != reservoir::Status::ok) {
            AREA_DETAIL(
                a, aw::invalid_value, checked.status,
                as_type<uint4>(float4(source.normalizer, target, source.target, checked.value)));
            return;
        }
        score = checked.value;
    } else if (a.config->phase == aw::spatial) {
        if (a.config->candidate == 0) {
            AREA_FAIL(a, aw::invalid_config);
            return;
        }
        const aw::Temporal source = a.temporal[area_neighbor(a, pixel)];
        if (!area_temporal_valid(a, pixel, source) || !area_same_payload(source.payload, payload)) {
            AREA_DETAIL(a, aw::invalid_payload, 0, area_payload_head(source.payload),
                        area_payload_tail(source.payload), area_payload_head(payload),
                        area_payload_tail(payload));
            return;
        }
        accumulator = a.accumulators[pixel];
        expected = a.config->candidate - 1;
        const auto checked =
            reuse_mis::spatial_pair_score(source.normalizer, target, source.target);
        if (checked.status != reservoir::Status::ok) {
            AREA_DETAIL(
                a, aw::invalid_value, checked.status,
                as_type<uint4>(float4(source.normalizer, target, source.target, checked.value)));
            return;
        }
        score = checked.value;
        // The inverse direction at the neighboring pixel provides the center
        // payload's reverse target after all four immutable-cache passes finish.
        a.cross_targets[ulong(a.config->candidate - 1) * a.config->pixel_count + pixel] = target;
        if (!area_add_output(a, pixel, score, xyz, target))
            return;
    } else {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    if (accumulator.state.candidate_count != expected) {
        AREA_DETAIL(a, aw::invalid_count, accumulator.state.status,
                    area_state_values(accumulator.state), uint4(expected, 0, 0, 0));
        return;
    }
    if (!area_update(a, pixel, accumulator, payload, xyz, score, target))
        return;
    a.accumulators[pixel] = accumulator;
}
kernel void rypath_area_temporal_finish(constant AreaArguments &a [[buffer(1)]],
                                        uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    const uint count = a.config->history_used ? 2u : 1u;
    if ((a.config->history_used && a.config->phase != aw::temporal) ||
        (!a.config->history_used && a.config->phase != aw::fresh)) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    const auto accumulator = a.accumulators[pixel];
    float normalizer;
    if (!area_finish(a, pixel, accumulator, count, normalizer))
        return;
    a.temporal[pixel] = {
        accumulator.payload, normalizer, accumulator.xyz, accumulator.target, {0, 0, 0}};
}
kernel void rypath_area_spatial_begin(constant AreaArguments &a [[buffer(1)]],
                                      uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    if (a.config->phase != aw::spatial || a.config->candidate != 0) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    const auto center = a.temporal[pixel];
    if (!area_temporal_valid(a, pixel, center)) {
        return;
    }
    a.accumulators[pixel] = aw::Accumulator{};
    a.raw_xyz[pixel] = {0, 0, 0};
}
kernel void rypath_area_finalize(constant AreaArguments &a [[buffer(1)]],
                                 uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    if (a.config->phase != aw::spatial_final || a.config->candidate != 0) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    auto accumulator = a.accumulators[pixel];
    if (accumulator.state.candidate_count != 4) {
        AREA_DETAIL(a, aw::invalid_count, accumulator.state.status,
                    area_state_values(accumulator.state));
        return;
    }
    const aw::Temporal center = a.temporal[pixel];
    if (!area_temporal_valid(a, pixel, center))
        return;
    float center_score = 0;
    for (uint direction = 1; direction <= 4; ++direction) {
        const uint neighbor = area_neighbor_direction(a, pixel, direction);
        const uint opposite = direction == 1 ? 2 : direction == 2 ? 1 : direction == 3 ? 4 : 3;
        // A clamped self-neighbor does not have the ordinary opposite inverse.
        const float reverse_target =
            neighbor == pixel
                ? center.target
                : a.cross_targets[ulong(opposite - 1) * a.config->pixel_count + neighbor];
        const auto part =
            reuse_mis::spatial_pair_score(center.normalizer, center.target, reverse_target);
        if (part.status != reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_value, part.status,
                        as_type<uint4>(
                            float4(center.normalizer, center.target, reverse_target, part.value)),
                        uint4(direction, neighbor, opposite, 0));
            return;
        }
        const auto sum = direction == 1 ? part : reuse_mis::add_scores(center_score, part.value);
        if (sum.status != reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_value, sum.status,
                        as_type<uint4>(float4(center_score, part.value, sum.value, 0)),
                        uint4(direction, neighbor, opposite, 0));
            return;
        }
        center_score = sum.value;
    }
    if (!area_update(a, pixel, accumulator, center.payload, center.xyz, center_score,
                     center.target) ||
        !area_add_output(a, pixel, center_score, center.xyz, center.target))
        return;
    float normalizer;
    if (!area_finish(a, pixel, accumulator, aw::spatial_candidates, normalizer))
        return;
    if (a.config->output_mode == aw::selected_output) {
        wire::Point weighted;
        if (!area_weighted_xyz(a, pixel, normalizer, accumulator.xyz, accumulator.target, weighted))
            return;
        a.raw_xyz[pixel] = weighted;
    }
    // Output marginalization never changes the selected sample reused later.
    a.history_destination[pixel] = {accumulator.payload, normalizer, accumulator.target, 1, 0};
}

// The source cache always belongs to the CURRENT destination evaluation. Each
// configured step proposes independently, fully replays that proposal, then
// finalizes its actual represented acceptance probability before the next step.
inline mutation::MutationIdentity area_mutation_identity(constant AreaArguments &a, uint pixel) {
    return {pixel, a.config->serial_low, a.config->serial_high, a.config->seed_low,
            a.config->seed_high};
}
inline bool area_mutation_source(constant AreaArguments &a, uint pixel,
                                 const thread aw::Temporal &source) {
    const auto target = reuse_mis::importance_target(source.xyz.x, source.xyz.y, source.xyz.z,
                                                     a.config->target_scale);
    if (!rypath_replay::valid(source.payload) || !reservoir::positive_normal(source.normalizer) ||
        !reservoir::positive_normal(source.target) || source.reserved[0] != 0 ||
        source.reserved[1] != 0 || source.reserved[2] != 0 ||
        target.status != reservoir::Status::ok || target.value != source.target) {
        AREA_DETAIL(a, aw::invalid_mutation, target.status,
                    as_type<uint4>(float4(source.normalizer, source.target, target.value,
                                          a.config->target_scale)),
                    as_type<uint4>(float4(source.xyz.x, source.xyz.y, source.xyz.z, 0)),
                    area_payload_head(source.payload), area_payload_tail(source.payload));
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
        AREA_DETAIL(a, aw::invalid_mutation, entropy.status, area_payload_head(source),
                    area_payload_tail(source));
        return {source, mutation::Block::film, entropy.status};
    }
    const auto proposal = mutation::propose(source, entropy.value);
    if (proposal.status != mutation::Status::ok)
        AREA_DETAIL(a, aw::invalid_mutation, proposal.status, area_payload_head(source),
                    area_payload_tail(source),
                    uint4(entropy.value.x, entropy.value.y, entropy.value.z, entropy.value.w));
    return proposal;
}
kernel void rypath_area_mutation_prepare(constant wire::Parameters &p [[buffer(0)]],
                                         constant AreaArguments &a [[buffer(1)]],
                                         uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    if (a.config->phase != aw::mutation || p.width != a.config->width ||
        p.height != a.config->height || p.pixel_begin != 0 ||
        p.pixel_count != a.config->pixel_count || p.sampler_kind != wire::independent_sampler) {
        AREA_DETAIL(
            a, aw::invalid_config, 0, uint4(p.width, p.height, p.pixel_begin, p.pixel_count),
            uint4(a.config->width, a.config->height, a.config->pixel_count, p.sampler_kind));
        return;
    }
    const auto source = a.temporal[pixel];
    if (!area_mutation_source(a, pixel, source))
        return;
    const auto proposal = area_mutation_proposal(a, pixel, source.payload);
    if (proposal.status != mutation::Status::ok)
        return;
    a.replay[pixel] = proposal.payload;
}
kernel void rypath_area_mutation_finish(constant AreaArguments &a [[buffer(1)]],
                                        uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    if (a.config->phase != aw::mutation) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    const auto source = a.temporal[pixel];
    if (!area_mutation_source(a, pixel, source))
        return;
    const auto proposal = area_mutation_proposal(a, pixel, source.payload);
    const auto evaluated = a.replay[pixel];
    if (proposal.status != mutation::Status::ok ||
        !area_same_payload(proposal.payload, evaluated)) {
        AREA_DETAIL(a, aw::invalid_mutation, proposal.status, area_payload_head(proposal.payload),
                    area_payload_tail(proposal.payload), area_payload_head(evaluated),
                    area_payload_tail(evaluated));
        return;
    }
    const auto xyz = a.leaf_xyz[pixel];
    const auto target = reuse_mis::importance_target(xyz.x, xyz.y, xyz.z, a.config->target_scale);
    if (target.status != reservoir::Status::ok) {
        AREA_DETAIL(a, aw::invalid_mutation, target.status,
                    as_type<uint4>(float4(xyz.x, xyz.y, xyz.z, a.config->target_scale)));
        return;
    }
    auto pending =
        reuse_mis::prepare_normalizer_acceptance(source.normalizer, source.target, target.value);
    if (pending.status != mutation::Status::ok) {
        AREA_DETAIL(a, aw::invalid_mutation, pending.status,
                    as_type<uint4>(float4(source.normalizer, source.target, target.value,
                                          pending.accepted_normalizer)),
                    uint4(pending.smaller_ratio.significand, pending.smaller_ratio.bit_count,
                          uint(pending.decision.status), 0));
        return;
    }
    const auto identity = area_mutation_identity(a, pixel);
    for (uint ordinal = 0; !pending.decision.complete && ordinal < reservoir::maximum_random_words;
         ++ordinal) {
        const auto entropy = mutation::acceptance_word(identity, a.config->candidate, ordinal);
        if (entropy.status != mutation::Status::ok ||
            reuse_mis::feed_random_word(pending, entropy.value) != mutation::Status::ok) {
            AREA_DETAIL(
                a, aw::invalid_mutation, pending.status,
                as_type<uint4>(float4(source.normalizer, source.target, target.value,
                                      pending.accepted_normalizer)),
                uint4(ordinal, entropy.value, uint(entropy.status), uint(pending.decision.status)),
                uint4(pending.smaller_ratio.significand, pending.smaller_ratio.bit_count,
                      pending.decision.words_used, pending.decision.compared_bits));
            return;
        }
    }
    const auto result = reuse_mis::finalize_normalizer_acceptance(pending);
    if (result.status != mutation::Status::ok) {
        AREA_DETAIL(a, aw::invalid_mutation, result.status,
                    as_type<uint4>(float4(source.normalizer, source.target, target.value,
                                          pending.accepted_normalizer)),
                    uint4(pending.smaller_ratio.significand, pending.smaller_ratio.bit_count,
                          pending.decision.words_used, pending.decision.compared_bits),
                    uint4(pending.decision.complete, pending.decision.selected,
                          uint(pending.decision.status), 0));
        return;
    }
    // Counts are diagnostics, never an acceptance/stopping criterion. Valid
    // dispatches attempt exactly pixel_count*mutation_steps; configuration
    // validation proves this product fits the shared 32-bit counters.
    const uint maximum = a.config->pixel_count * a.config->mutation_steps;
    if (atomic_fetch_add_explicit(&a.counters->mutations, 1u, memory_order_relaxed) >= maximum) {
        AREA_DETAIL(a, aw::invalid_mutation, result.status,
                    uint4(maximum, result.words_used, result.accepted, 0),
                    as_type<uint4>(
                        float4(source.normalizer, source.target, target.value, result.normalizer)));
        return;
    }
    if (result.accepted) {
        if (atomic_fetch_add_explicit(&a.counters->accepted, 1u, memory_order_relaxed) >= maximum) {
            AREA_DETAIL(a, aw::invalid_mutation, result.status,
                        uint4(maximum, result.words_used, result.accepted, 0),
                        as_type<uint4>(float4(source.normalizer, source.target, target.value,
                                              result.normalizer)));
            return;
        }
        a.temporal[pixel] = {proposal.payload, result.normalizer, xyz, target.value, {0, 0, 0}};
    }
    // Rejection retains the exact old payload/Z/XYZ/target. The host always
    // launches every configured step, including after rejections/equal targets.
}

#undef AREA_DETAIL
#undef AREA_FAIL
