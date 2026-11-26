// SPDX-License-Identifier: GPL-3.0-only
#include "mutation_math_probe_wire.h"
#include <metal_stdlib>
namespace mp = mutation_probe;
namespace mh = rypath_mutation;
kernel void rypath_mutation_math_probe(device const mp::Query *queries [[buffer(0)]],
                                       device mp::Result *results [[buffer(1)]],
                                       constant uint &count [[buffer(2)]],
                                       uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    device const mp::Query &q = queries[index];
    const rypath_replay::ReplayState source = q.source;
    const mh::MutationIdentity identity = q.identity;
    mp::Result result{};
    result.proposal = mh::propose(source, q.entropy, mh::Policy(q.policy), q.scale);
    const auto counter = mh::decision_counter(identity, q.step, mh::Entropy::proposal, 0);
    result.counter = counter.value;
    result.counter_status = uint(counter.status);
    const auto entropy = mh::decision_words(identity, q.step, mh::Entropy::proposal);
    result.proposal_words = entropy.value;
    result.proposal_words_status = uint(entropy.status);
    for (uint i = 0; i < 9; ++i) {
        const auto word = mh::acceptance_word(identity, q.step, i);
        result.acceptance_words[i] = word.value;
        result.acceptance_status[i] = uint(word.status);
    }
    const auto ordinal = mh::acceptance_word(identity, q.step, q.acceptance_ordinal);
    result.ordinal_value = ordinal.value;
    result.ordinal_status = uint(ordinal.status);
    auto pending = mh::prepare_acceptance(q.weight, q.source_target, q.proposal_target);
    result.ratio = pending.smaller_ratio;
    result.prepared_status = uint(pending.status);
    result.prepared_weight = pending.accepted_weight;
    for (uint word = 0; word < metal::min(q.word_count, 9u) && !pending.decision.complete &&
                        pending.status == mh::Status::ok;
         ++word)
        mh::feed_random_word(pending, q.words[word]);
    const auto final = mh::finalize_acceptance(pending);
    result.complete = pending.decision.complete;
    result.selected = pending.decision.selected;
    result.compared_bits = pending.decision.compared_bits;
    result.final_weight = final.weight;
    result.final_status = uint(final.status);
    result.accepted = final.accepted;
    result.words_used = final.words_used;
    result.valid_step_count = mh::valid_step_count(q.requested_steps);
    results[index] = result;
}
