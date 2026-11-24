// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#define RYPATH_RESERVOIR_THREAD thread
#else
#include <bit>
#include <cmath>
#include <cstdint>
#define RYPATH_RESERVOIR_THREAD
#endif

namespace rypath_reservoir {
#if defined(__METAL_VERSION__)
constant constexpr unsigned no_selection = 0xffffffffu;
constant constexpr unsigned maximum_candidate_count = 32;
constant constexpr unsigned maximum_random_words = 9;
inline unsigned bits(float value) { return as_type<unsigned>(value); }
inline float fraction(float value, thread int &exponent) { return metal::frexp(value, exponent); }
inline float scale(float value, int exponent) { return metal::ldexp(value, exponent); }
#else
inline constexpr unsigned no_selection = 0xffffffffu;
inline constexpr unsigned maximum_candidate_count = 32;
inline constexpr unsigned maximum_random_words = 9;
inline std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
inline float fraction(float value, int &exponent) { return std::frexp(value, &exponent); }
inline float scale(float value, int exponent) { return std::ldexp(value, exponent); }
#endif

inline bool finite(float value) { return (bits(value) & 0x7fffffffu) < 0x7f800000u; }
inline bool positive_normal(float value) {
    const auto word = bits(value);
    return word >= 0x00800000u && word < 0x7f800000u;
}
inline bool normal_or_zero(float value) {
    const auto magnitude = bits(value) & 0x7fffffffu;
    return magnitude == 0 || (magnitude >= 0x00800000u && magnitude < 0x7f800000u);
}
inline float magnitude(float value) {
#if defined(__METAL_VERSION__)
    return as_type<float>(bits(value) & 0x7fffffffu);
#else
    return std::bit_cast<float>(bits(value) & 0x7fffffffu);
#endif
}

enum class Status : unsigned {
    ok,
    invalid_state,
    invalid_candidate,
    invalid_target,
    arithmetic_failure,
    count_overflow,
    incomplete,
    empty
};

struct ScalarResult {
    float value = 0;
    Status status = Status::ok;
};

struct State {
    float score_sum = 0;
    float selected_target = 0;
    unsigned candidate_count = 0;
    unsigned selected_index = no_selection;
    Status status = Status::ok;
};

// Probability = significand * 2^(-bit_count). Keeping the exponent separately
// preserves small events that would underflow a binary32 probability.
struct DyadicProbability {
    unsigned significand = 0;
    unsigned bit_count = 0;
};

struct PreparedUpdate {
    State source;
    float score_sum = 0;
    float target = 0;
    DyadicProbability rare_probability;
    unsigned compared_bits = 0;
    unsigned words_used = 0;
    bool select_new_on_success = true;
    bool complete = false;
    bool selected = false;
    Status status = Status::ok;
};

struct UpdateResult {
    DyadicProbability rare_probability;
    unsigned words_used = 0;
    bool select_new_on_success = true;
    bool selected = false;
    Status status = Status::ok;
};

// A caller-selected support floor, not a radiance clamp. Nonzero subnormal
// inputs are outside this shared CPU/Metal arithmetic contract.
inline ScalarResult target_xyz(float x, float y, float z, float epsilon) {
    if (!normal_or_zero(x) || !normal_or_zero(y) || !normal_or_zero(z) || !positive_normal(epsilon))
        return {0, Status::invalid_target};
    const float xy = magnitude(x) + magnitude(y);
    const float xyz = xy + magnitude(z);
    const float target = epsilon + xyz;
    if (!finite(xy) || !finite(xyz) || !positive_normal(target))
        return {0, Status::arithmetic_failure};
    return {target, Status::ok};
}

// Current full-replay policy: limit importance contrast, never radiance/UCWs.
// Real formula: 1 + 0.1 * L1(XYZ)/(scale + L1(XYZ)). Dividing all terms by
// their maximum keeps the denominator in [1,4] even for finite HDR XYZ whose
// unscaled L1 sum would overflow. Tiny proxy terms may underflow or flush;
// this is not a claim of subnormal transport support. The raw XYZ is untouched.
inline ScalarResult bounded_target_xyz(float x, float y, float z, float target_scale) {
    if (!finite(x) || !finite(y) || !finite(z) || !positive_normal(target_scale))
        return {0, Status::invalid_target};
    const float ax = magnitude(x), ay = magnitude(y), az = magnitude(z);
    float maximum = target_scale;
    if (ax > maximum)
        maximum = ax;
    if (ay > maximum)
        maximum = ay;
    if (az > maximum)
        maximum = az;
    const float normalized_l1 = ((ax / maximum) + (ay / maximum)) + (az / maximum);
    const float normalized_scale = target_scale / maximum;
    const float denominator = normalized_scale + normalized_l1;
    if (!finite(normalized_l1) || normalized_l1 < 0 || normalized_l1 > 3 ||
        !finite(normalized_scale) || normalized_scale < 0 || normalized_scale > 1 ||
        !finite(denominator) || denominator < 1 || denominator > 4)
        return {0, Status::arithmetic_failure};
    const float ratio = normalized_l1 / denominator;
    const float target = 1 + .1f * ratio;
    if (!finite(ratio) || ratio < 0 || ratio > 1 || !positive_normal(target) || target < 1 ||
        target > 1.1f)
        return {0, Status::arithmetic_failure};
    return {target, Status::ok};
}

inline bool valid_state(RYPATH_RESERVOIR_THREAD const State &state) {
    if (state.status != Status::ok || state.candidate_count > maximum_candidate_count)
        return false;
    if (state.candidate_count == 0)
        return state.score_sum == 0 && state.selected_target == 0 &&
               state.selected_index == no_selection;
    return positive_normal(state.score_sum) && positive_normal(state.selected_target) &&
           state.selected_index < state.candidate_count;
}

// One call per scheduled candidate, even when its complete contribution is
// black. A normalized imported UCW is ONE candidate; historical M is not input.
// The local cap bounds accumulated numerical selection/normalization error;
// exact integer-to-float conversion alone is not a sufficient stream limit.
inline PreparedUpdate prepare_update(RYPATH_RESERVOIR_THREAD const State &state,
                                     float normalized_weight, float target) {
    PreparedUpdate pending;
    pending.source = state;
    pending.target = target;
    if (state.status != Status::ok) {
        pending.status = state.status;
        return pending;
    }
    if (!valid_state(state)) {
        pending.status = Status::invalid_state;
        return pending;
    }
    if (state.candidate_count == maximum_candidate_count) {
        pending.status = Status::count_overflow;
        return pending;
    }
    if (!positive_normal(normalized_weight) || !positive_normal(target)) {
        pending.status = Status::invalid_candidate;
        return pending;
    }
    const float score = normalized_weight * target;
    const float sum = state.score_sum + score;
    if (!positive_normal(score) || !positive_normal(sum)) {
        pending.status = Status::arithmetic_failure;
        return pending;
    }
    pending.score_sum = sum;
    if (state.candidate_count == 0) {
        pending.complete = true;
        pending.selected = true;
        return pending;
    }

    // Decide the smaller event directly. The opposite event is represented by
    // control flow, never by rounding 1-p to a binary32 probability of one.
    pending.select_new_on_success = score <= state.score_sum;
    const float small = pending.select_new_on_success ? score : state.score_sum;
    int small_exponent, sum_exponent, ratio_exponent;
    const float small_fraction = fraction(small, small_exponent);
    const float sum_fraction = fraction(sum, sum_exponent);
    const float ratio_fraction = fraction(small_fraction / sum_fraction, ratio_exponent);
    const int exponent = small_exponent - sum_exponent + ratio_exponent;
    const int bit_count = 24 - exponent;
    if (!positive_normal(ratio_fraction) || exponent > 0 || bit_count < 24 || bit_count > 277) {
        pending.status = Status::arithmetic_failure;
        return pending;
    }
    pending.rare_probability = {(bits(ratio_fraction) & 0x007fffffu) | 0x00800000u,
                                unsigned(bit_count)};
    return pending;
}

// Independent uniform words arrive most-significant bit first. Lexicographic
// comparison samples the represented dyadic exactly; no fixed 24-bit selector
// grid can erase or overrepresent a tiny probability. At most nine words suffice.
inline Status feed_random_word(RYPATH_RESERVOIR_THREAD PreparedUpdate &pending, unsigned word) {
    if (pending.status != Status::ok || pending.complete)
        return pending.status;
    const auto probability = pending.rare_probability;
    if (probability.significand < 0x00800000u || probability.significand > 0x00ffffffu ||
        probability.bit_count < 24 || probability.bit_count > 277 ||
        pending.compared_bits >= probability.bit_count ||
        pending.words_used >= maximum_random_words ||
        pending.compared_bits != 32 * pending.words_used) {
        pending.status = Status::invalid_state;
        return pending.status;
    }
    ++pending.words_used;
    for (unsigned bit = 0; bit < 32; ++bit) {
        const unsigned position = pending.compared_bits;
        const unsigned leading_zeros = probability.bit_count - 24;
        const unsigned threshold_bit =
            position < leading_zeros
                ? 0
                : (probability.significand >> (probability.bit_count - 1 - position)) & 1u;
        const unsigned random_bit = (word >> (31 - bit)) & 1u;
        ++pending.compared_bits;
        if (random_bit != threshold_bit) {
            const bool success = random_bit < threshold_bit;
            pending.selected = success == pending.select_new_on_success;
            pending.complete = true;
            return Status::ok;
        }
        if (pending.compared_bits == probability.bit_count) {
            // Equality to the dyadic threshold belongs to the failure interval.
            pending.selected = !pending.select_new_on_success;
            pending.complete = true;
            return Status::ok;
        }
    }
    return Status::ok;
}

// Commit exactly once, against the unchanged source state. Payload copying is
// outside this helper and occurs only after a successful selected=true result.
inline UpdateResult commit_update(RYPATH_RESERVOIR_THREAD State &state,
                                  RYPATH_RESERVOIR_THREAD const PreparedUpdate &pending) {
    UpdateResult result{pending.rare_probability, pending.words_used, pending.select_new_on_success,
                        false, Status::ok};
    if (state.status != Status::ok) {
        result.status = state.status;
        return result;
    }
    if (pending.status != Status::ok) {
        state.status = pending.status;
        result.status = state.status;
        return result;
    }
    const State source = pending.source;
    if (!valid_state(state) || !valid_state(source) ||
        state.candidate_count != source.candidate_count ||
        state.selected_index != source.selected_index ||
        bits(state.score_sum) != bits(source.score_sum) ||
        bits(state.selected_target) != bits(source.selected_target) ||
        state.candidate_count == maximum_candidate_count || !positive_normal(pending.score_sum) ||
        !positive_normal(pending.target)) {
        state.status = Status::invalid_state;
        result.status = state.status;
        return result;
    }
    if (!pending.complete) {
        state.status = Status::incomplete;
        result.status = state.status;
        return result;
    }
    const auto probability = pending.rare_probability;
    const bool valid_first = source.candidate_count == 0 && pending.selected &&
                             pending.words_used == 0 && pending.compared_bits == 0 &&
                             probability.significand == 0 && probability.bit_count == 0;
    const bool valid_later =
        source.candidate_count != 0 && pending.words_used > 0 &&
        pending.words_used <= maximum_random_words && pending.compared_bits > 0 &&
        pending.compared_bits > 32 * (pending.words_used - 1) &&
        pending.compared_bits <= 32 * pending.words_used &&
        pending.compared_bits <= probability.bit_count && probability.significand >= 0x00800000u &&
        probability.significand <= 0x00ffffffu && probability.bit_count >= 24 &&
        probability.bit_count <= 277 && pending.score_sum >= source.score_sum;
    if (!valid_first && !valid_later) {
        state.status = Status::invalid_state;
        result.status = state.status;
        return result;
    }
    if (pending.selected) {
        state.selected_index = state.candidate_count;
        state.selected_target = pending.target;
    }
    state.score_sum = pending.score_sum;
    ++state.candidate_count;
    result.selected = pending.selected;
    return result;
}

// Returns S / (K * t_selected), with the local K<=32 converted exactly.
// Exponent separation avoids an unnecessary overflow of K*t or S/t. A failed
// result invalidates the pool's output; it must never be used as a zero weight.
inline ScalarResult finalize(RYPATH_RESERVOIR_THREAD const State &state) {
    if (state.status != Status::ok)
        return {0, state.status};
    if (!valid_state(state))
        return {0, Status::invalid_state};
    if (state.candidate_count == 0)
        return {0, Status::empty};
    int sum_exponent, target_exponent;
    const float sum_fraction = fraction(state.score_sum, sum_exponent);
    const float target_fraction = fraction(state.selected_target, target_exponent);
    const float fraction_weight = (sum_fraction / target_fraction) / float(state.candidate_count);
    const float weight = scale(fraction_weight, sum_exponent - target_exponent);
    if (!positive_normal(weight))
        return {0, Status::arithmetic_failure};
    return {weight, Status::ok};
}

#undef RYPATH_RESERVOIR_THREAD
} // namespace rypath_reservoir
