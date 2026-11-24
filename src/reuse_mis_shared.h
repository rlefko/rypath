// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "mutation_shared.h"

#if defined(__METAL_VERSION__)
#define RYPATH_REUSE_THREAD thread
#else
#define RYPATH_REUSE_THREAD
#endif

// Strong-target, fixed-source-coefficient MIS over full replay evaluations.
// This stores the reservoir normalizer Z, not W=Z/t. Feed positive scores into
// reservoir::prepare_update(state, score, 1); final Z is state.score_sum, never
// reservoir::finalize() (which applies the legacy /K). Raw XYZ is never clipped.
// This fixed-coefficient source-MIS / finite-replay adaptation is our policy,
// not the paper's full reconnection implementation. Baseline and primary
// references: docs/restir-importance.md and docs/mcmc-mutations.md (Sawhney et al.,
// Section3/Eq16, https://arxiv.org/html/2211.00166). Normalizer correction below
// accounts separately for the actual rounded dyadic acceptance probability.
namespace rypath_reuse_mis {
namespace math = rypath_reservoir;
using ScalarResult = math::ScalarResult;
using Status = math::Status;
using MutationStatus = rypath_mutation::Status;
using Word = rypath_replay::Word;

// Positive scaled values keep every multiplication/division normal. Exponents
// stay separate until the final result passes the exact normal-scaling domain.
// Only signed_contribution accepts a subnormal factor; integer normalization
// prevents input/output FTZ from silently erasing a recoverable normal result.
struct Scaled {
    float fraction;
    int exponent;
};
inline Scaled normalized(float f, int exponent) {
    int shift;
    const float value = math::fraction(f, shift);
    return {value, exponent + shift};
}
inline Scaled decompose_positive(float value) {
    const unsigned word = math::bits(value);
    if ((word & 0x7f800000u) == 0) {
        int exponent;
        const float fraction = math::fraction(float(word & 0x007fffffu), exponent);
        return {fraction, exponent - 149};
    }
    int exponent;
    const float fraction = math::fraction(value, exponent);
    return {fraction, exponent};
}
inline Scaled product(Scaled a, Scaled b) {
    return normalized(a.fraction * b.fraction, a.exponent + b.exponent);
}
inline Scaled quotient(Scaled a, Scaled b) {
    return normalized(a.fraction / b.fraction, a.exponent - b.exponent);
}
inline Scaled sum(Scaled a, Scaled b) {
    if (a.exponent < b.exponent) {
        const auto swap = a;
        a = b;
        b = swap;
    }
    const int gap = a.exponent - b.exponent;
    // At >24 exponent bits the small term is below half an RNE mantissa ulp.
    // Avoid producing an irrelevant subnormal intermediate on Metal.
    if (gap > 24)
        return a;
    return normalized(a.fraction + math::scale(b.fraction, -gap), a.exponent);
}
inline ScalarResult materialize(Scaled value) {
    if (!math::positive_normal(value.fraction) || value.fraction < .5f || value.fraction >= 1 ||
        value.exponent < -125 || value.exponent > 128)
        return {0, Status::arithmetic_failure};
    const float result = math::scale(value.fraction, value.exponent);
    if (!math::positive_normal(result))
        return {0, Status::arithmetic_failure};
    return {result, Status::ok};
}
inline bool less(Scaled a, Scaled b) {
    return a.exponent < b.exponent || (a.exponent == b.exponent && a.fraction < b.fraction);
}

// max(FLT_MIN, 1e-6*scale) + ((abs(X)/4+abs(Y)/4)+abs(Z)/4).
// Grouping is retained in scaled arithmetic to admit all finite XYZ, including
// subnormals, without intermediate FTZ/overflow; this is not radiance clamping.
inline ScalarResult importance_target(float x, float y, float z, float target_scale) {
    if (!math::finite(x) || !math::finite(y) || !math::finite(z) ||
        !math::positive_normal(target_scale))
        return {0, Status::invalid_target};
    const Scaled minimum{.5f, -125};
    Scaled floor = product(decompose_positive(target_scale), decompose_positive(1e-6f));
    if (less(floor, minimum))
        floor = minimum;
    bool have = false;
    Scaled energy{.5f, -125};
    const float values[3] = {x, y, z};
    for (unsigned i = 0; i < 3; ++i) {
        const float value = math::magnitude(values[i]);
        if (math::bits(value) == 0)
            continue;
        auto part = decompose_positive(value);
        part.exponent -= 2;
        energy = have ? sum(energy, part) : part;
        have = true;
    }
    return materialize(have ? sum(floor, energy) : floor);
}
inline ScalarResult temporal_fresh_score(float current_target, float previous_target_at_fresh) {
    if (!math::positive_normal(current_target) || !math::positive_normal(previous_target_at_fresh))
        return {0, Status::invalid_target};
    const auto current = decompose_positive(current_target);
    const auto previous =
        product(decompose_positive(previous_target_at_fresh), decompose_positive(20.f));
    return materialize(quotient(product(current, current), sum(current, previous)));
}
inline ScalarResult temporal_history_score(float history_Z, float current_target,
                                           float previous_target) {
    if (!math::positive_normal(history_Z))
        return {0, Status::invalid_candidate};
    if (!math::positive_normal(current_target) || !math::positive_normal(previous_target))
        return {0, Status::invalid_target};
    const auto current = decompose_positive(current_target);
    const auto twenty = decompose_positive(20.f);
    return materialize(
        quotient(product(product(decompose_positive(history_Z), twenty), current),
                 sum(current, product(twenty, decompose_positive(previous_target)))));
}
// Neighbor pair, or one of the center's four pair terms. Sum the four center
// terms with add_scores; source coefficient 1/4 is fixed, never sample-dependent.
inline ScalarResult spatial_pair_score(float source_Z, float destination_target,
                                       float source_target) {
    if (!math::positive_normal(source_Z))
        return {0, Status::invalid_candidate};
    if (!math::positive_normal(destination_target) || !math::positive_normal(source_target))
        return {0, Status::invalid_target};
    const auto destination = decompose_positive(destination_target);
    return materialize(quotient(
        product(decompose_positive(source_Z), destination),
        product(decompose_positive(4.f), sum(destination, decompose_positive(source_target)))));
}
inline ScalarResult add_scores(float a, float b) {
    if (!math::positive_normal(a) || !math::positive_normal(b))
        return {0, Status::invalid_candidate};
    return materialize(sum(decompose_positive(a), decompose_positive(b)));
}
inline ScalarResult signed_contribution(float Z, float component, float target) {
    if (!math::positive_normal(Z))
        return {0, Status::invalid_candidate};
    if (!math::positive_normal(target) || !math::finite(component))
        return {0, Status::invalid_target};
    const auto magnitude = math::magnitude(component);
    if (math::bits(magnitude) == 0)
        return {component, Status::ok}; // Preserve exact signed zero.
    auto result = materialize(quotient(
        product(decompose_positive(Z), decompose_positive(magnitude)), decompose_positive(target)));
    if (result.status == Status::ok && (math::bits(component) & 0x80000000u))
        result.value = -result.value;
    return result;
}

struct PreparedNormalizerAcceptance {
    math::PreparedUpdate decision;
    math::DyadicProbability smaller_ratio;
    float source_normalizer = 0;
    float source_target = 0;
    float proposal_target = 0;
    float accepted_normalizer = 0;
    MutationStatus status = MutationStatus::ok;
};
struct NormalizerAcceptanceResult {
    float normalizer = 0;
    unsigned words_used = 0;
    bool accepted = false;
    MutationStatus status = MutationStatus::ok;
};

// A rounded dyadic MH probability is represented without materializing tiny q.
// The Z correction uses that actual probability: r=fl(sf/lf), p=fl(r*lf),
// C=fl(p/sf) uphill or fl(sf/p) downhill. Exponents cancel from C. Thus this is
// NOT mutation::prepare_acceptance(Z,...), which interprets its input as UCW.
// Basic operation errors are about 3u per accepted update, not exact float
// unbiasedness or a global bound. Equal/rejected normalizers retain their bits.
inline PreparedNormalizerAcceptance prepare_normalizer_acceptance(float Z, float source_target,
                                                                  float proposal_target) {
    PreparedNormalizerAcceptance pending;
    pending.source_normalizer = Z;
    pending.source_target = source_target;
    pending.proposal_target = proposal_target;
    if (!math::positive_normal(Z)) {
        pending.status = MutationStatus::invalid_weight;
        return pending;
    }
    if (!math::positive_normal(source_target) || !math::positive_normal(proposal_target)) {
        pending.status = MutationStatus::invalid_target;
        return pending;
    }
    pending.decision.select_new_on_success = true;
    if (source_target == proposal_target) {
        pending.smaller_ratio = {0x00800000u, 23};
        pending.accepted_normalizer = Z;
        pending.decision.complete = pending.decision.selected = true;
        return pending;
    }
    const bool uphill = proposal_target > source_target;
    const auto small = decompose_positive(uphill ? source_target : proposal_target);
    const auto large = decompose_positive(uphill ? proposal_target : source_target);
    const float r = small.fraction / large.fraction;
    const auto ratio = normalized(r, small.exponent - large.exponent);
    const int bit_count = 24 - ratio.exponent;
    if (!math::positive_normal(ratio.fraction) || ratio.exponent > 0 || bit_count < 24 ||
        bit_count > 277) {
        pending.status = MutationStatus::arithmetic_failure;
        return pending;
    }
    pending.smaller_ratio = {(math::bits(ratio.fraction) & 0x007fffffu) | 0x00800000u,
                             unsigned(bit_count)};
    pending.decision.rare_probability = pending.smaller_ratio;
    pending.decision.complete = pending.decision.selected = uphill;
    const float p = r * large.fraction;
    const float correction = uphill ? p / small.fraction : small.fraction / p;
    const auto corrected =
        materialize(product(decompose_positive(Z), decompose_positive(correction)));
    if (corrected.status != Status::ok)
        pending.status = MutationStatus::arithmetic_failure;
    else
        pending.accepted_normalizer = corrected.value;
    return pending;
}
inline bool valid_pending(RYPATH_REUSE_THREAD const PreparedNormalizerAcceptance &pending) {
    const auto expected = prepare_normalizer_acceptance(
        pending.source_normalizer, pending.source_target, pending.proposal_target);
    return expected.status == MutationStatus::ok && pending.decision.status == Status::ok &&
           pending.decision.select_new_on_success &&
           pending.smaller_ratio.significand == expected.smaller_ratio.significand &&
           pending.smaller_ratio.bit_count == expected.smaller_ratio.bit_count &&
           pending.decision.rare_probability.significand ==
               expected.decision.rare_probability.significand &&
           pending.decision.rare_probability.bit_count ==
               expected.decision.rare_probability.bit_count &&
           math::bits(pending.accepted_normalizer) == math::bits(expected.accepted_normalizer);
}
inline MutationStatus feed_random_word(RYPATH_REUSE_THREAD PreparedNormalizerAcceptance &pending,
                                       Word word) {
    if (pending.status != MutationStatus::ok)
        return pending.status;
    if (!valid_pending(pending) || math::feed_random_word(pending.decision, word) != Status::ok)
        pending.status = MutationStatus::invalid_decision;
    return pending.status;
}
inline NormalizerAcceptanceResult
finalize_normalizer_acceptance(RYPATH_REUSE_THREAD const PreparedNormalizerAcceptance &pending) {
    const unsigned used = pending.decision.words_used;
    if (pending.status != MutationStatus::ok)
        return {0, used, false, pending.status};
    if (!valid_pending(pending))
        return {0, used, false, MutationStatus::invalid_decision};
    if (!pending.decision.complete)
        return {0, used, false, MutationStatus::incomplete};
    if (pending.proposal_target >= pending.source_target) {
        if (!pending.decision.selected || used != 0 || pending.decision.compared_bits != 0)
            return {0, used, false, MutationStatus::invalid_decision};
    } else if (used == 0 || used > math::maximum_random_words ||
               pending.decision.compared_bits <= 32 * (used - 1) ||
               pending.decision.compared_bits > 32 * used ||
               pending.decision.compared_bits > pending.smaller_ratio.bit_count) {
        return {0, used, false, MutationStatus::invalid_decision};
    }
    return {pending.decision.selected ? pending.accepted_normalizer : pending.source_normalizer,
            used, pending.decision.selected, MutationStatus::ok};
}
} // namespace rypath_reuse_mis
#undef RYPATH_REUSE_THREAD
