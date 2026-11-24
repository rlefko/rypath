// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"
#include "reservoir_shared.h"

#if defined(__METAL_VERSION__)
#define RYPATH_MUTATION_THREAD thread
#else
#define RYPATH_MUTATION_THREAD
#endif

namespace rypath_mutation {
using Word = rypath_replay::Word;
using ReplayState = rypath_replay::ReplayState;
using Words4 = rypath_replay::Words4;
using DyadicProbability = rypath_reservoir::DyadicProbability;

// Accepted submission identity, not the replay sample index. Repeated sample
// indices may reuse canonical samples while this serial must remain unique.
struct MutationIdentity {
    Word pixel, serial_low, serial_high, seed_low, seed_high;
};

enum class Status : Word {
    ok,
    invalid_payload,
    invalid_scale,
    invalid_step,
    invalid_domain,
    invalid_weight,
    invalid_target,
    invalid_decision,
    incomplete,
    arithmetic_failure
};

inline bool valid_step_count(unsigned count) {
    return count == 0 || count == 1 || count == 2 || count == 4;
}

enum class Block : Word { film, lens, wavelength, tail_key };
struct ProposalScale {
    unsigned magnitude_bits = 16;
    unsigned shift = 8;
};
struct ProposalResult {
    ReplayState payload;
    Block block = Block::film;
    Status status = Status::ok;
};

// The caller supplies independent uniform words, independent of payload values.
// A fixed state-independent block mixture and independent signs give an exactly
// symmetric proposal on the unsigned integer torus. Nonzero deltas can still
// leave a rendered contribution unchanged. The key block regenerates the whole
// indirect tape; it is not a local per-bounce perturbation. Fresh samples provide
// coverage between invariant subsets of these block kernels.
inline ProposalResult propose(RYPATH_MUTATION_THREAD const ReplayState &source, Words4 entropy,
                              ProposalScale scale = {}) {
    ProposalResult result{source, Block(entropy.x & 3u), Status::ok};
    if (!rypath_replay::valid(source)) {
        result.status = Status::invalid_payload;
        return result;
    }
    if (scale.magnitude_bits == 0 || scale.magnitude_bits > 31 || scale.shift > 31 ||
        scale.magnitude_bits + scale.shift > 31) {
        result.status = Status::invalid_scale;
        return result;
    }
    const Word mask = (Word(1) << scale.magnitude_bits) - 1;
    const Word magnitude0 = (1 + (entropy.z & mask)) << scale.shift;
    const Word magnitude1 = (1 + (entropy.w & mask)) << scale.shift;
    const Word delta0 = (entropy.y & 1u) ? 0u - magnitude0 : magnitude0;
    const Word delta1 = (entropy.y & 2u) ? 0u - magnitude1 : magnitude1;
    switch (result.block) {
    case Block::film:
        result.payload.film[0] += delta0;
        result.payload.film[1] += delta1;
        break;
    case Block::lens:
        result.payload.lens[0] += delta0;
        result.payload.lens[1] += delta1;
        break;
    case Block::wavelength:
        result.payload.wavelength += delta0;
        break;
    case Block::tail_key:
        result.payload.tail_key[0] += delta0;
        result.payload.tail_key[1] += delta1;
        break;
    }
    return result;
}

enum class Entropy : Word { proposal, acceptance };
struct WordsResult {
    Words4 value{0, 0, 0, 0};
    Status status = Status::ok;
};
struct WordResult {
    Word value = 0;
    Status status = Status::ok;
};

// Mutation reserves complete counter words 0x6d750000 + step*256 + role + block.
// role is 16 (proposal) or 32 (acceptance), step<4, block=0 or block<3 respectively.
// These do not alias current fresh/tail domains. Pixel and the full absolute
// submission serial occupy separate words; the seed is the Philox key. An active
// history must never reuse an identity/step decision stream. Neither the mutated
// payload nor destination camera parameters enter the decision generator.
inline WordsResult decision_counter(RYPATH_MUTATION_THREAD const MutationIdentity &identity,
                                    unsigned step, Entropy entropy, unsigned block) {
    if (step >= 4)
        return {{0, 0, 0, 0}, Status::invalid_step};
    if ((entropy != Entropy::proposal && entropy != Entropy::acceptance) ||
        (entropy == Entropy::proposal && block != 0) ||
        (entropy == Entropy::acceptance && block >= 3))
        return {{0, 0, 0, 0}, Status::invalid_domain};
    const Word role = entropy == Entropy::proposal ? 16 : 32;
    return {{identity.pixel, identity.serial_low, identity.serial_high,
             0x6d750000u + Word(step) * 256 + role + Word(block)},
            Status::ok};
}

inline WordsResult decision_words(RYPATH_MUTATION_THREAD const MutationIdentity &identity,
                                  unsigned step, Entropy entropy, unsigned block = 0) {
    auto result = decision_counter(identity, step, entropy, block);
    if (result.status == Status::ok)
        result.value = rypath_replay::philox(result.value, {identity.seed_low, identity.seed_high});
    return result;
}

inline WordResult acceptance_word(RYPATH_MUTATION_THREAD const MutationIdentity &identity,
                                  unsigned step, unsigned ordinal) {
    if (ordinal >= rypath_reservoir::maximum_random_words)
        return {0, Status::invalid_domain};
    const auto result = decision_words(identity, step, Entropy::acceptance, ordinal / 4);
    if (result.status != Status::ok)
        return {0, result.status};
    switch (ordinal & 3u) {
    case 0:
        return {result.value.x, Status::ok};
    case 1:
        return {result.value.y, Status::ok};
    case 2:
        return {result.value.z, Status::ok};
    default:
        return {result.value.w, Status::ok};
    }
}

struct PreparedAcceptance {
    // Only the general dyadic bit-comparison primitive is reused. This is not a
    // streaming-reservoir update: do not call reservoir::commit_update on it.
    rypath_reservoir::PreparedUpdate decision;
    DyadicProbability smaller_ratio;
    float source_weight = 0;
    float source_target = 0;
    float proposal_target = 0;
    float accepted_weight = 0;
    Status status = Status::ok;
};
struct AcceptanceResult {
    float weight = 0;
    unsigned words_used = 0;
    bool accepted = false;
    Status status = Status::ok;
};

// Round min(t_old,t_new)/max(t_old,t_new) in its normal mantissa, retaining its
// exponent. Equal targets use an exact unit probability and no entropy.
inline PreparedAcceptance prepare_acceptance(float weight, float source_target,
                                             float proposal_target) {
    namespace math = rypath_reservoir;
    PreparedAcceptance pending;
    pending.source_weight = weight;
    pending.source_target = source_target;
    pending.proposal_target = proposal_target;
    if (!math::positive_normal(weight)) {
        pending.status = Status::invalid_weight;
        return pending;
    }
    if (!math::positive_normal(source_target) || !math::positive_normal(proposal_target)) {
        pending.status = Status::invalid_target;
        return pending;
    }
    pending.decision.select_new_on_success = true;
    if (source_target == proposal_target) {
        pending.smaller_ratio = {0x00800000u, 23};
        pending.accepted_weight = weight;
        pending.decision.complete = true;
        pending.decision.selected = true;
        return pending;
    }
    const bool uphill = proposal_target > source_target;
    const float small = uphill ? source_target : proposal_target;
    const float large = uphill ? proposal_target : source_target;
    int small_exponent, large_exponent, ratio_exponent;
    const float small_fraction = math::fraction(small, small_exponent);
    const float large_fraction = math::fraction(large, large_exponent);
    const float ratio_fraction = math::fraction(small_fraction / large_fraction, ratio_exponent);
    const int exponent = small_exponent - large_exponent + ratio_exponent;
    const int bit_count = 24 - exponent;
    if (!math::positive_normal(ratio_fraction) || exponent > 0 || bit_count < 24 ||
        bit_count > 277) {
        pending.status = Status::arithmetic_failure;
        return pending;
    }
    pending.smaller_ratio = {(math::bits(ratio_fraction) & 0x007fffffu) | 0x00800000u,
                             unsigned(bit_count)};
    pending.decision.rare_probability = pending.smaller_ratio;
    pending.decision.complete = uphill;
    pending.decision.selected = uphill;

    // Actual forward/reverse dyadic acceptance, not an independently rounded
    // ideal target ratio: uphill correction=q_reverse; downhill correction=1/q.
    // Splitting exponents preserves extreme ratios when the final UCW is normal.
    int weight_exponent;
    const float weight_fraction = math::fraction(weight, weight_exponent);
    const float corrected_fraction =
        uphill ? weight_fraction * ratio_fraction : weight_fraction / ratio_fraction;
    int corrected_exponent;
    const float corrected_mantissa = math::fraction(corrected_fraction, corrected_exponent);
    const int final_exponent =
        corrected_exponent + (uphill ? weight_exponent + exponent : weight_exponent - exponent);
    // Check the exact scaling of the already rounded 24-bit fraction, BEFORE
    // ldexp. A subnormal scaled value can round back up to FLT_MIN, adding a
    // second rounding despite a normal returned value. Likewise toward-zero
    // overflow may return finite FLT_MAX. Neither lies in the one-round domain.
    if (!math::positive_normal(corrected_fraction) || final_exponent < -125 ||
        final_exponent > 128) {
        pending.status = Status::arithmetic_failure;
        return pending;
    }
    pending.accepted_weight = math::scale(corrected_mantissa, final_exponent);
    // Validate the accepted outcome BEFORE observing acceptance entropy. The
    // caller propagates failure; treating it as a rejection would change the
    // forward kernel without its corresponding reverse correction.
    if (!math::positive_normal(pending.accepted_weight))
        pending.status = Status::arithmetic_failure;
    return pending;
}

inline Status feed_random_word(RYPATH_MUTATION_THREAD PreparedAcceptance &pending, Word word) {
    if (pending.status != Status::ok)
        return pending.status;
    if (rypath_reservoir::feed_random_word(pending.decision, word) != rypath_reservoir::Status::ok)
        pending.status = Status::invalid_decision;
    return pending.status;
}

// Symmetric Q and actual acceptance A imply accepted correction A(y,x)/A(x,y).
// Incoming accepted weighted flow is then Q(x,y)*A(y,x), completing rejected
// flow 1-sum_x Q(y,x)*A(y,x). This preserves the uniform-domain UCW identity in
// real arithmetic even when A is a rounded MH probability. It reduces to
// t_old/t_new in exact arithmetic (Sawhney et al., Section 3, Eq.16):
// https://arxiv.org/html/2211.00166
// There is ONE normal multiply/divide rounding in each accepted UCW; the checked
// exponent domain then makes power-of-two scaling exact. Values whose scaled
// rounded fraction is subnormal/overflow fail before entropy, even if ldexp
// could round/saturate them back into the normal range. No rounding occurs on
// rejection/equal targets. With relative operation error u, n fixed steps add at
// most (1+u)^n-1 weighted-measure error, provided every possible accepted outcome
// passes this normal-scaling preflight. CPU RNE uses u=2^-24; conservative correctly rounded
// non-fast Metal uses u=2^-23 only after backend qualification. This is not exact
// float unbiasedness. State-dependent stopping and asymmetric block choices are
// outside this contract; the caller must run its configured 0/1/2/4 steps.
inline AcceptanceResult
finalize_acceptance(RYPATH_MUTATION_THREAD const PreparedAcceptance &pending) {
    const unsigned used = pending.decision.words_used;
    if (pending.status != Status::ok)
        return {0, used, false, pending.status};
    const auto expected =
        prepare_acceptance(pending.source_weight, pending.source_target, pending.proposal_target);
    if (expected.status != Status::ok)
        return {0, used, false, expected.status};
    const auto ratio = pending.smaller_ratio;
    if (pending.decision.status != rypath_reservoir::Status::ok ||
        !pending.decision.select_new_on_success ||
        ratio.significand != expected.smaller_ratio.significand ||
        ratio.bit_count != expected.smaller_ratio.bit_count ||
        rypath_reservoir::bits(pending.accepted_weight) !=
            rypath_reservoir::bits(expected.accepted_weight) ||
        pending.decision.rare_probability.significand !=
            expected.decision.rare_probability.significand ||
        pending.decision.rare_probability.bit_count != expected.decision.rare_probability.bit_count)
        return {0, used, false, Status::invalid_decision};
    if (!pending.decision.complete)
        return {0, used, false, Status::incomplete};
    if (pending.proposal_target >= pending.source_target) {
        if (!pending.decision.selected || used != 0 || pending.decision.compared_bits != 0)
            return {0, used, false, Status::invalid_decision};
    } else if (used == 0 || used > rypath_reservoir::maximum_random_words ||
               pending.decision.compared_bits <= 32 * (used - 1) ||
               pending.decision.compared_bits > 32 * used ||
               pending.decision.compared_bits > ratio.bit_count) {
        return {0, used, false, Status::invalid_decision};
    }
    return {pending.decision.selected ? pending.accepted_weight : pending.source_weight, used,
            pending.decision.selected, Status::ok};
}

#undef RYPATH_MUTATION_THREAD
} // namespace rypath_mutation
