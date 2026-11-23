// SPDX-License-Identifier: GPL-3.0-only
#include "area_math_probe_wire.h"
#include <metal_stdlib>
using namespace metal;
namespace probe = area_math_probe;
namespace replay = rypath_replay;
namespace reservoir = rypath_reservoir;
kernel void rypath_area_math_probe(device const probe::Query *queries [[buffer(0)]],
                                   device probe::Result *results [[buffer(1)]],
                                   constant uint &count [[buffer(2)]],
                                   uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    device const probe::Query &query = queries[index];
    probe::Result result{};
    const replay::FreshIdentity identity = query.identity;
    result.philox = replay::philox(query.counter, query.key);
    result.fresh = replay::fresh(identity);
    result.tail = replay::tail_words(result.fresh, query.address);
    result.uniform_value = replay::uniform(query.uniform_word);
    result.target = reservoir::target_xyz(query.xyz[0], query.xyz[1], query.xyz[2], query.epsilon);
    const auto bounded = reservoir::bounded_target_xyz(query.xyz[0], query.xyz[1], query.xyz[2],
                                                       query.bounded_scale);
    result.bounded_value = bounded.value;
    result.bounded_status = uint(bounded.status);
    reservoir::State state = query.initial;
    for (uint i = 0; i < min(query.candidate_count, probe::maximum_candidates); ++i) {
        auto pending = reservoir::prepare_update(state, query.candidates[i].weight,
                                                 query.candidates[i].target);
        result.steps[i].rare = pending.rare_probability;
        result.steps[i].score_sum = pending.score_sum;
        for (uint word = 0; word < min(query.candidates[i].word_count, 9u) && !pending.complete;
             ++word)
            reservoir::feed_random_word(pending, query.candidates[i].words[word]);
        const auto committed = reservoir::commit_update(state, pending);
        result.steps[i].words_used = pending.words_used;
        result.steps[i].compared_bits = pending.compared_bits;
        result.steps[i].complete = pending.complete;
        result.steps[i].selected = committed.selected;
        result.steps[i].status = uint(committed.status);
    }
    result.state = state;
    result.normalized = reservoir::finalize(state);
    results[index] = result;
}
