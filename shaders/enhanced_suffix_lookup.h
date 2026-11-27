// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included inside rypath_bdpt, after the original hero helpers and capture observer.
namespace enhanced_suffix_runtime {
// Keep the live boundary-PDF query and deferred result outside the large
// event shader closure; exact full-frame controls qualify this call boundary.
template <typename State, typename Sink>
[[clang::noinline]] inline bool lookup(thread State &walk, const thread HeroCurrentVertex &pivot,
                                       thread Sink &sink) {
    thread auto &cache = sink.cache_sink();
    if (walk.complete || !sink.has_interior() || !cache.selected_prefix())
        return false;
    const auto receipt = cache.evaluations[cache.pixel];
    // Mutation changes the tuple (including possibly lambda/tail); its scratch
    // entry must be captured anew. Only immutable, same-tuple imports can hit.
    if (receipt.source.bank != receipt.result.bank ||
        receipt.source.pixel != receipt.result.pixel ||
        !(receipt.suffix_context_low || receipt.suffix_context_high))
        return false;
    if (cache.suffixes == nullptr || cache.counters == nullptr ||
        !ew::valid(receipt.source, cache.config.pixel_count)) {
        walk.sample.status = wire::invalid_arithmetic;
        walk.complete = true;
        return true;
    }
    const ulong index =
        ulong(receipt.source.bank) * cache.config.pixel_count + receipt.source.pixel;
    const tape::Entry entry = cache.suffixes[index];
    tape::Input input{};
    input.beta = walk.beta;
    input.density = walk.hero.density;
    input.pre_pivot_density = walk.hero.previous;
    input.selected_slot = sink.state.requested_slot;
    input.pivot_event = sink.state.hybrid.active_record.event_ordinal;
    input.context_low = receipt.suffix_context_low;
    input.context_high = receipt.suffix_context_high;
    auto result = tape::readout(entry, input);
    if (result.status == tape::Status::needs_immediate_pdf) {
        const uint needed = tape::density_lanes(result.mis_density);
        if (needed & ~pivot.bsdf.active_mask) {
            result.status = tape::Status::cache_miss;
        } else {
            const float3 after(entry.immediate_direction[0], entry.immediate_direction[1],
                               entry.immediate_direction[2]);
            if (!enhanced_hybrid_geometry::unit(after)) {
                result.status = tape::Status::invalid_arithmetic;
            } else {
                const float3 before = walk.latest.point.position - pivot.point.position;
                float4 live_pdf = 0;
                bool valid = true;
                if (!all(before == 0)) {
                    const auto evaluated =
                        hero_history_evaluate(pivot.bsdf, pivot.frame.local(normalize(before)),
                                              pivot.frame.local(after), pivot.seed);
                    valid = evaluated.valid;
                    live_pdf = evaluated.pdf;
                }
                if (!valid) {
                    result.status = tape::Status::invalid_arithmetic;
                } else {
                    // Finish the validated deferred result; do not repeat the
                    // terminal arithmetic after the live boundary PDF query.
                    result = tape::complete_immediate(entry, result, live_pdf, 1,
                                                      pivot.bsdf.active_mask);
                }
            }
        }
    }
    if (result.status == tape::Status::cache_miss) {
        atomic_fetch_add_explicit(&cache.counters->suffix_misses, 1u, memory_order_relaxed);
        // Result can contain partially advanced values. Never apply them: the
        // untouched post-pivot walk is the sole full-evaluation fallback base.
        return false;
    }
    if (result.status != tape::Status::ok) {
        walk.sample.status = wire::invalid_arithmetic;
        walk.complete = true;
        return true;
    }
    sink.add(sink.state.requested_slot, result.value, walk.lambda);
    if (sink.state.failed)
        walk.sample.status = wire::invalid_arithmetic;
    else
        atomic_fetch_add_explicit(
            &cache.counters->suffix_hits[entry.flags & tape::operation_count_mask], 1u,
            memory_order_relaxed);
    walk.complete = true;
    return true;
}
} // namespace enhanced_suffix_runtime
