// SPDX-License-Identifier: GPL-3.0-only
#include "reuse_mis_math_probe_wire.h"
#include <metal_stdlib>
namespace p = reuse_mis_probe;
namespace r = rypath_reuse_mis;
kernel void rypath_reuse_mis_math_probe(device const p::Query *queries [[buffer(0)]],
                                        device p::Result *results [[buffer(1)]],
                                        constant uint &count [[buffer(2)]],
                                        uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    const p::Query q = queries[index];
    p::Result out{};
    out.target = r::importance_target(q.x, q.y, q.z, q.target_scale);
    out.fresh = r::temporal_fresh_score(q.current_target, q.previous_target);
    out.history = r::temporal_history_score(q.Z, q.current_target, q.previous_target);
    out.pair = r::spatial_pair_score(q.Z, q.current_target, q.previous_target);
    out.contribution = r::signed_contribution(q.Z, q.component, q.current_target);
    auto pending =
        r::prepare_normalizer_acceptance(q.normalizer, q.current_target, q.previous_target);
    out.ratio = pending.smaller_ratio;
    out.prepared_status = uint(pending.status);
    out.prepared_normalizer = pending.accepted_normalizer;
    for (uint i = 0; i < metal::min(q.word_count, 9u) && !pending.decision.complete &&
                     pending.status == r::MutationStatus::ok;
         ++i)
        r::feed_random_word(pending, q.words[i]);
    const auto result = r::finalize_normalizer_acceptance(pending);
    out.final_normalizer = result.normalizer;
    out.final_status = uint(result.status);
    out.complete = pending.decision.complete;
    out.selected = pending.decision.selected;
    out.compared_bits = pending.decision.compared_bits;
    out.accepted = result.accepted;
    out.words_used = result.words_used;
    results[index] = out;
}
