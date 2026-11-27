// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "reuse_mis_shared.h"
#if defined(__METAL_VERSION__)
#define RYPATH_ER_THREAD thread
#else
#define RYPATH_ER_THREAD
#endif

// Enhanced-only arithmetic. A Number never materializes a subnormal mantissa.
// All represented sums/ratios retain the existing binary32 operation ordering;
// this is not a claim of exact real arithmetic or exact FLOAT unbiasedness.
namespace rypath_enhanced_reservoir {
namespace math = rypath_reservoir;
namespace reuse = rypath_reuse_mis;
using Number = reuse::Scaled;
using Word = rypath_replay::Word;
using Status = math::Status;
using MutationStatus = rypath_mutation::Status;
using DyadicProbability = math::DyadicProbability;
enum : unsigned {
    maximum_exponent = 4096,
    // At most 2*D+1 positive observations for D<=65; confidence M is separate.
    maximum_positive_candidates = 131,
    maximum_candidate_count = maximum_positive_candidates,
    // Smallest represented mantissa ratio has normalized exponent >=0;
    // the two serialized exponents differ by at most 8192.
    maximum_probability_bits = 24 + 2 * maximum_exponent,
    maximum_random_words = (maximum_probability_bits + 31) / 32,
    no_selection = 0xffffffffu
};
static_assert(maximum_probability_bits == 8216 && maximum_random_words == 257);
inline bool zero(Number x) { return math::bits(x.fraction) == 0 && x.exponent == 0; }
inline bool positive(Number x) {
    return math::positive_normal(x.fraction) && x.fraction >= .5f && x.fraction < 1 &&
           x.exponent >= -int(maximum_exponent) && x.exponent <= int(maximum_exponent);
}
inline bool valid_number(Number x) { return zero(x) || positive(x); }
inline bool same(Number a, Number b) {
    return math::bits(a.fraction) == math::bits(b.fraction) && a.exponent == b.exponent;
}
inline bool less(Number a, Number b) {
    if (zero(a))
        return !zero(b);
    if (zero(b))
        return false;
    return reuse::less(a, b);
}
struct NumberResult {
    Number value{0, 0};
    Status status = Status::ok;
};
inline NumberResult checked_result(Number x) {
    return positive(x) ? NumberResult{x, Status::ok}
                       : NumberResult{{0, 0}, Status::arithmetic_failure};
}
inline NumberResult from_float(float x) {
    if (!math::finite(x) || ((math::bits(x) >> 31) && (math::bits(x) & 0x7fffffffu)))
        return {{0, 0}, Status::invalid_candidate};
    return (math::bits(x) & 0x7fffffffu) == 0 ? NumberResult{}
                                              : checked_result(reuse::decompose_positive(x));
}
inline NumberResult checked_add(Number a, Number b) {
    if (!valid_number(a) || !valid_number(b))
        return {{0, 0}, Status::invalid_candidate};
    if (zero(a))
        return {b, Status::ok};
    if (zero(b))
        return {a, Status::ok};
    return checked_result(reuse::sum(a, b));
}
inline NumberResult checked_multiply(Number a, Number b) {
    if (!valid_number(a) || !valid_number(b))
        return {{0, 0}, Status::invalid_candidate};
    if (zero(a) || zero(b))
        return {};
    return checked_result(reuse::product(a, b));
}
inline NumberResult checked_divide(Number a, Number b) {
    if (!valid_number(a) || !positive(b))
        return {{0, 0}, Status::invalid_candidate};
    if (zero(a))
        return {};
    return checked_result(reuse::quotient(a, b));
}
struct Decision {
    DyadicProbability rare_probability;
    Word compared_bits = 0, words_used = 0;
    bool select_new_on_success = true, complete = false, selected = false;
    Status status = Status::ok;
};
inline bool valid_probability(DyadicProbability p) {
    return p.significand >= 0x00800000u && p.significand <= 0x00ffffffu && p.bit_count >= 24 &&
           p.bit_count <= maximum_probability_bits;
}
inline bool same_probability(DyadicProbability a, DyadicProbability b) {
    return a.significand == b.significand && a.bit_count == b.bit_count;
}
inline bool valid_progress(RYPATH_ER_THREAD const Decision &d) {
    if (d.status != Status::ok || !valid_probability(d.rare_probability) ||
        d.words_used > maximum_random_words || d.compared_bits > d.rare_probability.bit_count)
        return false;
    if (!d.complete)
        return !d.selected && d.compared_bits == 32 * d.words_used &&
               d.compared_bits < d.rare_probability.bit_count;
    return d.words_used > 0 && d.compared_bits > 32 * (d.words_used - 1) &&
           d.compared_bits <= 32 * d.words_used;
}
inline DyadicProbability ratio(Number small, Number large) {
    // Inputs are validated before subtraction; intermediate exponent is bounded
    // by +/-8192 and need not fit the serialized Number exponent domain.
    if (!positive(small) || !positive(large) || rypath_enhanced_reservoir::less(large, small))
        return {};
    const auto r =
        reuse::normalized(small.fraction / large.fraction, small.exponent - large.exponent);
    const int count = 24 - r.exponent;
    if (!math::positive_normal(r.fraction) || r.exponent > 0 || count < 24 ||
        count > int(maximum_probability_bits))
        return {};
    return {(math::bits(r.fraction) & 0x007fffffu) | 0x00800000u, unsigned(count)};
}
inline Status feed_decision(RYPATH_ER_THREAD Decision &d, Word word) {
    if (d.status != Status::ok)
        return d.status;
    if (!valid_progress(d)) {
        d.status = Status::invalid_state;
        return d.status;
    }
    if (d.complete)
        return Status::ok;
    ++d.words_used;
    for (Word bit = 0; bit < 32; ++bit) {
        const Word position = d.compared_bits;
        const Word leading_zeros = d.rare_probability.bit_count - 24;
        const Word threshold = position < leading_zeros
                                   ? 0
                                   : (d.rare_probability.significand >>
                                      (d.rare_probability.bit_count - 1 - position)) &
                                         1u;
        const Word random = (word >> (31 - bit)) & 1u;
        ++d.compared_bits;
        if (random != threshold) {
            d.selected = (random < threshold) == d.select_new_on_success;
            d.complete = true;
            return Status::ok;
        }
        if (d.compared_bits == d.rare_probability.bit_count) {
            d.selected = !d.select_new_on_success; // Strict threshold: equality fails.
            d.complete = true;
            return Status::ok;
        }
    }
    return Status::ok;
}
struct State {
    Number score_sum{0, 0}, selected_target{0, 0};
    Word candidate_count = 0, selected_index = no_selection;
    Status status = Status::ok;
};
static_assert(sizeof(Number) == 8 && sizeof(State) == 28);
inline bool valid_state(RYPATH_ER_THREAD const State &s) {
    if (s.status != Status::ok || s.candidate_count > maximum_candidate_count)
        return false;
    return s.candidate_count == 0
               ? zero(s.score_sum) && zero(s.selected_target) && s.selected_index == no_selection
               : positive(s.score_sum) && positive(s.selected_target) &&
                     s.selected_index < s.candidate_count;
}
inline bool same_state(RYPATH_ER_THREAD const State &a, RYPATH_ER_THREAD const State &b) {
    return same(a.score_sum, b.score_sum) && same(a.selected_target, b.selected_target) &&
           a.candidate_count == b.candidate_count && a.selected_index == b.selected_index &&
           a.status == b.status;
}
struct PreparedUpdate {
    State source;
    Number score{0, 0}, target{0, 0}, score_sum{0, 0};
    Decision decision;
    Status status = Status::ok;
};
inline PreparedUpdate prepare_score(RYPATH_ER_THREAD const State &s, Number score, Number target) {
    PreparedUpdate p;
    p.source = s;
    p.score = score;
    p.target = target;
    if (s.status != Status::ok)
        p.status = s.status;
    else if (!valid_state(s))
        p.status = Status::invalid_state;
    else if (s.candidate_count == maximum_candidate_count)
        p.status = Status::count_overflow;
    else if (!positive(score) || !positive(target))
        p.status = Status::invalid_candidate;
    else {
        const auto sum = checked_add(s.score_sum, score);
        p.status = sum.status;
        p.score_sum = sum.value;
        if (p.status != Status::ok)
            return p;
        if (s.candidate_count == 0) {
            p.decision.complete = p.decision.selected = true;
        } else {
            p.decision.select_new_on_success = !rypath_enhanced_reservoir::less(s.score_sum, score);
            p.decision.rare_probability =
                ratio(p.decision.select_new_on_success ? score : s.score_sum, sum.value);
            if (!valid_probability(p.decision.rare_probability))
                p.status = Status::arithmetic_failure;
        }
    }
    return p;
}
inline bool valid_pending(RYPATH_ER_THREAD const PreparedUpdate &p) {
    const auto expected = prepare_score(p.source, p.score, p.target);
    if (p.status != Status::ok || expected.status != Status::ok ||
        !same(p.score_sum, expected.score_sum) || p.decision.status != Status::ok ||
        !same_probability(p.decision.rare_probability, expected.decision.rare_probability) ||
        p.decision.select_new_on_success != expected.decision.select_new_on_success)
        return false;
    if (p.source.candidate_count == 0)
        return p.decision.complete && p.decision.selected && p.decision.words_used == 0 &&
               p.decision.compared_bits == 0;
    return valid_progress(p.decision);
}
inline Status feed_random_word(RYPATH_ER_THREAD PreparedUpdate &p, Word word) {
    if (p.status != Status::ok)
        return p.status;
    if (!valid_pending(p))
        p.status = Status::invalid_state;
    else if (p.source.candidate_count != 0)
        p.status = feed_decision(p.decision, word);
    return p.status;
}
struct UpdateResult {
    DyadicProbability rare_probability;
    Word words_used = 0;
    bool select_new_on_success = true, selected = false;
    Status status = Status::ok;
};
inline UpdateResult commit_update(RYPATH_ER_THREAD State &s,
                                  RYPATH_ER_THREAD const PreparedUpdate &p) {
    UpdateResult result{p.decision.rare_probability, p.decision.words_used,
                        p.decision.select_new_on_success, false, Status::ok};
    if (s.status != Status::ok) {
        result.status = s.status;
        return result;
    }
    if (p.status != Status::ok)
        s.status = p.status;
    else if (!same_state(s, p.source) || !valid_pending(p))
        s.status = Status::invalid_state;
    else if (!p.decision.complete)
        s.status = Status::incomplete;
    else {
        if (p.decision.selected) {
            s.selected_index = s.candidate_count;
            s.selected_target = p.target;
        }
        s.score_sum = p.score_sum;
        ++s.candidate_count;
        result.selected = p.decision.selected;
    }
    result.status = s.status;
    return result;
}
struct PreparedNormalizerAcceptance {
    Number source_normalizer{0, 0}, source_target{0, 0}, proposal_target{0, 0};
    Number accepted_normalizer{0, 0};
    DyadicProbability smaller_ratio;
    Decision decision;
    MutationStatus status = MutationStatus::ok;
};
struct NormalizerAcceptanceResult {
    Number normalizer{0, 0};
    Word words_used = 0;
    bool accepted = false;
    MutationStatus status = MutationStatus::ok;
};
inline PreparedNormalizerAcceptance prepare_normalizer_acceptance(Number Z, Number source,
                                                                  Number proposal) {
    PreparedNormalizerAcceptance p;
    p.source_normalizer = Z;
    p.source_target = source;
    p.proposal_target = proposal;
    if (!positive(Z)) {
        p.status = MutationStatus::invalid_weight;
        return p;
    }
    if (!positive(source) || !positive(proposal)) {
        p.status = MutationStatus::invalid_target;
        return p;
    }
    if (same(source, proposal)) {
        p.smaller_ratio = {0x00800000u, 23};
        p.accepted_normalizer = Z;
        p.decision.complete = p.decision.selected = true;
        return p;
    }
    const bool uphill = rypath_enhanced_reservoir::less(source, proposal);
    const Number small = uphill ? source : proposal, large = uphill ? proposal : source;
    p.smaller_ratio = ratio(small, large);
    p.decision.rare_probability = p.smaller_ratio;
    p.decision.complete = p.decision.selected = uphill;
    const float r = small.fraction / large.fraction;
    const float product = r * large.fraction;
    const float correction = uphill ? product / small.fraction : small.fraction / product;
    const auto corrected = checked_multiply(Z, reuse::decompose_positive(correction));
    if (!valid_probability(p.smaller_ratio) || !math::positive_normal(correction) ||
        corrected.status != Status::ok)
        p.status = MutationStatus::arithmetic_failure;
    else
        p.accepted_normalizer = corrected.value;
    return p;
}
inline bool valid_pending(RYPATH_ER_THREAD const PreparedNormalizerAcceptance &p) {
    const auto expected =
        prepare_normalizer_acceptance(p.source_normalizer, p.source_target, p.proposal_target);
    if (p.status != MutationStatus::ok || expected.status != MutationStatus::ok ||
        p.decision.status != Status::ok ||
        !same_probability(p.smaller_ratio, expected.smaller_ratio) ||
        !same_probability(p.decision.rare_probability, expected.decision.rare_probability) ||
        !same(p.accepted_normalizer, expected.accepted_normalizer) ||
        !p.decision.select_new_on_success)
        return false;
    if (!rypath_enhanced_reservoir::less(p.proposal_target, p.source_target))
        return p.decision.complete && p.decision.selected && p.decision.words_used == 0 &&
               p.decision.compared_bits == 0;
    return valid_progress(p.decision);
}
inline MutationStatus feed_random_word(RYPATH_ER_THREAD PreparedNormalizerAcceptance &p,
                                       Word word) {
    if (p.status != MutationStatus::ok)
        return p.status;
    if (!valid_pending(p) || (rypath_enhanced_reservoir::less(p.proposal_target, p.source_target) &&
                              feed_decision(p.decision, word) != Status::ok))
        p.status = MutationStatus::invalid_decision;
    return p.status;
}
inline NormalizerAcceptanceResult
finalize_normalizer_acceptance(RYPATH_ER_THREAD const PreparedNormalizerAcceptance &p) {
    if (p.status != MutationStatus::ok)
        return {{0, 0}, p.decision.words_used, false, p.status};
    if (!valid_pending(p))
        return {{0, 0}, p.decision.words_used, false, MutationStatus::invalid_decision};
    if (!p.decision.complete)
        return {{0, 0}, p.decision.words_used, false, MutationStatus::incomplete};
    return {p.decision.selected ? p.accepted_normalizer : p.source_normalizer,
            p.decision.words_used, p.decision.selected, MutationStatus::ok};
}
} // namespace rypath_enhanced_reservoir
#undef RYPATH_ER_THREAD
