// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included by render.metal. Contribution mode supports full and selected-prefix
// replay; Enhanced also owns charts and their bounded sequential suffix entries.
#include "../src/area_stencil_shared.h"
#include "../src/area_wire.h"
#include "../src/enhanced_confidence_shared.h"
#include "../src/enhanced_path_shared.h"
#include "../src/enhanced_resampling_decision_shared.h"
#include "../src/mutation_shared.h"
#include "../src/reuse_mis_shared.h"
#include "../src/stochastic_spatial_shared.h"
namespace aw = rypath::area_wire;
namespace pair_atlas = rypath_area_pair_atlas;
namespace cw = rypath::contribution_wire;
namespace ew = rypath::enhanced_wire;
namespace reservoir = rypath_reservoir;
namespace mutation = rypath_mutation;
namespace reuse_mis = rypath_reuse_mis;
namespace enhanced_path = rypath_enhanced_path;
namespace confidence = rypath_enhanced_confidence;
namespace enhanced_reservoir = rypath_enhanced_reservoir;
namespace enhanced_entropy = rypath_enhanced_resampling_decision;
using AreaNumber = enhanced_reservoir::Number;
using AreaNumberResult = enhanced_reservoir::NumberResult;
struct AreaArguments {
    device const aw::Config *config [[id(aw::config_id)]];
    device rypath_replay::ReplayState *replay [[id(aw::replay_id)]];
    device wire::Point *leaf_xyz [[id(aw::leaf_xyz_id)]];
    device aw::Accumulator *accumulators [[id(aw::accumulators_id)]];
    device aw::Temporal *temporal [[id(aw::temporal_id)]];
    device const aw::History *history_source [[id(aw::history_source_id)]];
    device aw::History *history_destination [[id(aw::history_destination_id)]];
    device wire::Point *raw_xyz [[id(aw::raw_xyz_id)]];
    device aw::Counters *counters [[id(aw::failure_id)]];
    device uchar *cross_targets [[id(aw::cross_targets_id)]];
    device cw::Evaluation *contribution_evaluation [[id(aw::contribution_evaluation_id)]];
    device ew::Record *enhanced_records [[id(aw::enhanced_records_id)]];
    device ew::Evaluation *enhanced_evaluation [[id(aw::enhanced_evaluation_id)]];
    device rypath_enhanced_suffix_tape::Entry *enhanced_suffix [[id(aw::enhanced_suffix_id)]];
    device const rypath::enhanced_motion_wire::Donor *donors [[id(aw::donors_id)]];
    device const pair_atlas::Entry *paired_atlas [[id(aw::paired_atlas_id)]];
    device const pair_atlas::Config *paired_config [[id(aw::paired_config_id)]];
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
        uint4(a.config->seed_high, a.config->mutation_policy, a.config->stencil,
              a.config->temporal_cache));
}
#define AREA_FAIL(a, flag) area_fail_at(a, flag, pixel, __LINE__)
#define AREA_DETAIL(a, flag, status, ...)                                                          \
    area_fail_record(a, flag, pixel, __LINE__, uint(status), __VA_ARGS__)
inline bool area_failed(constant AreaArguments &a) {
    return atomic_load_explicit(&a.counters->failure, memory_order_relaxed) != 0;
}
// Resolve the SAME immutable old domain for every temporal payload, record,
// confidence and reverse score. New reservoir/evaluation storage remains pixel d.
inline bool area_temporal_source(constant AreaArguments &a, uint pixel, thread uint &source) {
    source = pixel;
    if (pixel >= a.config->pixel_count) {
        AREA_FAIL(a, aw::invalid_payload);
        return false;
    }
    if (!a.config->temporal_reprojection)
        return true;
    const auto donor = a.donors[pixel];
    if (!rypath::enhanced_motion_wire::valid_donor(donor, a.config->pixel_count,
                                                   a.config->serial_low, a.config->serial_high)) {
        AREA_DETAIL(
            a, aw::invalid_payload, donor.classification,
            uint4(donor.source_pixel, donor.classification, donor.serial_low, donor.serial_high));
        return false;
    }
    source = donor.source_pixel;
    return true;
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
        c.output_mode > aw::conditional_mean_output ||
        c.mutation_policy > aw::spectral_refresh_mutation ||
        c.stencil > aw::paired_gaussian16_stencil || c.temporal_cache > 1 ||
        (c.temporal_cache && !c.history_used) || c.temporal_reprojection > 1 ||
        (c.temporal_reprojection &&
         (c.contribution_mode != cw::enhanced || !c.history_used || c.temporal_cache)) ||
        c.spatial_mode > 2 ||
        (c.spatial_mode &&
         (c.contribution_mode != cw::enhanced || c.stencil != aw::paired_gaussian16_stencil ||
          !rypath_stochastic_spatial::valid({c.forward_atlas, c.canonical_atlas}))) ||
        (c.spatial_mode == 2 && c.forward_atlas != c.canonical_atlas) ||
        (!c.spatial_mode && (c.forward_atlas || c.canonical_atlas)) ||
        c.enhanced_history_cap == 0 || c.enhanced_history_cap > confidence::default_temporal_cap ||
        (c.contribution_mode != cw::enhanced &&
         c.enhanced_history_cap != confidence::default_temporal_cap) ||
        c.contribution_mode > cw::enhanced ||
        (c.contribution_mode == cw::summed && c.contribution_depth != 0) ||
        (c.contribution_mode != cw::summed && c.contribution_depth > 65) ||
        (c.contribution_mode == cw::enhanced && c.history_source_bank > 1) ||
        (c.contribution_mode != cw::enhanced &&
         (c.history_source_bank != 0 || c.suffix_context_low || c.suffix_context_high)) ||
        (c.stencil == aw::reciprocal16_stencil &&
         (!rypath_area_stencil::valid({c.offset_x[0], c.offset_y[0]}) ||
          !rypath_area_stencil::valid({c.offset_x[1], c.offset_y[1]}))) ||
        !schedule) {
        AREA_FAIL(a, aw::invalid_config);
        return false;
    }
    if (c.stencil == aw::paired_gaussian16_stencil) {
        // Once per pixel/kernel, not once for each of finalize's four neighbors.
        // Legacy modes never read their reflected-minimum typed sentinels.
        const auto paired = *a.paired_config;
        if (c.offset_x[0] || c.offset_x[1] || c.offset_y[0] || c.offset_y[1] ||
            !pair_atlas::valid_config(paired)) {
            AREA_FAIL(a, aw::invalid_config);
            return false;
        }
    }
    uint source;
    if (pixel < c.pixel_count && c.temporal_reprojection && !area_temporal_source(a, pixel, source))
        return false;
    return !area_failed(a);
}
inline bool area_contribution(constant AreaArguments &a) {
    return a.config->contribution_mode != cw::summed;
}
inline bool area_enhanced(constant AreaArguments &a) {
    return a.config->contribution_mode == cw::enhanced;
}
// Legacy carriers hold the ORIGINAL FLOAT in fraction and exponent0. Only
// Enhanced carriers are normalized Numbers; legacy never enters scaled math.
inline AreaNumberResult area_legacy(reservoir::ScalarResult x) { return {{x.value, 0}, x.status}; }
template <typename Source> inline AreaNumber area_Z(const thread Source &s) {
    return {s.normalizer, s.normalizer_exponent};
}
template <typename Source> inline AreaNumber area_T(const thread Source &s) {
    return {s.target, s.target_exponent};
}
inline AreaNumber area_S(const thread aw::Accumulator &s) {
    return {s.state.score_sum, s.score_exponent};
}
inline bool area_number_valid(constant AreaArguments &a, AreaNumber x) {
    return area_enhanced(a) ? enhanced_reservoir::valid_number(x)
                            : x.exponent == 0 && reservoir::normal_or_zero(x.fraction) &&
                                  (as_type<uint>(x.fraction) & 0x80000000u) == 0;
}
inline uint4 area_numbers(AreaNumber x, AreaNumber y) {
    return {as_type<uint>(x.fraction), as_type<uint>(x.exponent), as_type<uint>(y.fraction),
            as_type<uint>(y.exponent)};
}
inline enhanced_reservoir::State area_selector(const thread aw::Accumulator &s) {
    return {area_S(s),
            {s.state.selected_target, s.target_exponent},
            s.state.candidate_count,
            s.state.selected_index,
            s.state.status};
}
// In stochastic mode only the canonical atlas is ever read after the maps finish.
inline ulong area_cross_index(constant AreaArguments &a, uint atlas, uint pixel) {
    return a.config->spatial_mode ? ulong(pixel) : ulong(atlas - 1) * a.config->pixel_count + pixel;
}
inline void area_cross_store(constant AreaArguments &a, ulong index, AreaNumber x) {
    if (area_enhanced(a))
        reinterpret_cast<device AreaNumber *>(a.cross_targets)[index] = x;
    else
        reinterpret_cast<device float *>(a.cross_targets)[index] = x.fraction;
}
inline AreaNumber area_cross_load(constant AreaArguments &a, ulong index) {
    return area_enhanced(a)
               ? reinterpret_cast<device AreaNumber *>(a.cross_targets)[index]
               : AreaNumber{reinterpret_cast<device float *>(a.cross_targets)[index], 0};
}
inline bool area_confidence_valid(constant AreaArguments &a, uint value) {
    return area_enhanced(a) ? confidence::valid_source({value, 1}) : value == 0;
}
inline bool area_zero(wire::Point xyz) {
    return ((as_type<uint>(xyz.x) | as_type<uint>(xyz.y) | as_type<uint>(xyz.z)) & 0x7fffffffu) ==
           0;
}
inline bool area_slot_valid(constant AreaArguments &a, uint slot) {
    return area_contribution(a) ? slot < 2u * a.config->contribution_depth + 1u ||
                                      (area_enhanced(a) && slot == cw::enhanced_null_slot)
                                : slot == 0;
}
inline bool area_null(constant AreaArguments &a, uint slot) {
    return area_enhanced(a) && slot == cw::enhanced_null_slot;
}
inline bool area_owned_value_valid(constant AreaArguments &a, uint slot, ew::Reference record,
                                   reuse_mis::Scaled h) {
    if (!area_enhanced(a))
        return record.bank == 0 && record.pixel == 0 && as_type<uint>(h.fraction) == 0 &&
               h.exponent == 0;
    // A zero mapped candidate may be rejected without ever materializing its
    // record. Validate record bytes only when publishing/copying or reading a
    // cached evaluator result; scalar validation must not add a false lifetime.
    return ew::valid(record, a.config->pixel_count) && enhanced_path::valid_density(h) &&
           (!area_null(a, slot) || enhanced_path::unit_density(h));
}
inline bool area_same_reference(ew::Reference x, ew::Reference y) {
    return x.bank == y.bank && x.pixel == y.pixel;
}
// Copy only on accepted owner publication, never as per-evaluation local state.
// Null records are initialized without reading their potentially poisoned source.
inline bool area_copy_record(constant AreaArguments &a, uint pixel, uint slot, ew::Reference source,
                             ew::Reference destination) {
    if (!area_enhanced(a))
        return true;
    if (!ew::valid(source, a.config->pixel_count) ||
        !ew::valid(destination, a.config->pixel_count) || !area_slot_valid(a, slot)) {
        AREA_FAIL(a, aw::invalid_payload);
        return false;
    }
    ew::Record record = ew::empty_record();
    if (!area_null(a, slot)) {
        record = a.enhanced_records[ulong(source.bank) * a.config->pixel_count + source.pixel];
        if (record.tag == rypath_enhanced_chart::terminal_surface ||
            !rypath_enhanced_chart::valid_chart(record, slot, a.config->contribution_depth)) {
            AREA_FAIL(a, aw::invalid_payload);
            return false;
        }
    }
    a.enhanced_records[ulong(destination.bank) * a.config->pixel_count + destination.pixel] =
        record;
    if (a.config->suffix_context_low || a.config->suffix_context_high) {
        const ulong target = ulong(destination.bank) * a.config->pixel_count + destination.pixel;
        const ulong origin = ulong(source.bank) * a.config->pixel_count + source.pixel;
        if (area_null(a, slot))
            a.enhanced_suffix[target] = rypath_enhanced_suffix_tape::Entry{};
        else if (target != origin)
            a.enhanced_suffix[target] = a.enhanced_suffix[origin];
        // Preserve the captured context, even when this frame evaluated a
        // different context. A later miss must replay, never restamp stale data.
    }
    return true;
}
inline uint area_count(constant AreaArguments &a, const thread aw::Accumulator &accumulator) {
    return area_enhanced(a) ? accumulator.logical_count : accumulator.state.candidate_count;
}
inline bool area_selected_values(constant AreaArguments &a, uint slot, AreaNumber normalizer,
                                 AreaNumber target, wire::Point xyz) {
    if (!area_number_valid(a, normalizer) || !area_number_valid(a, target))
        return false;
    if (area_null(a, slot))
        return enhanced_reservoir::zero(normalizer) && enhanced_reservoir::zero(target) &&
               area_zero(xyz);
    return area_enhanced(a)
               ? enhanced_reservoir::positive(normalizer) && enhanced_reservoir::positive(target)
               : reservoir::positive_normal(normalizer.fraction) &&
                     reservoir::positive_normal(target.fraction);
}
inline uint area_neighbor_direction(constant AreaArguments &a, uint pixel, uint direction) {
    if (a.config->stencil == aw::paired_gaussian16_stencil) {
        if (direction == 0)
            return pixel;
        if (direction > pair_atlas::tile_count) {
            AREA_FAIL(a, aw::invalid_config);
            return pixel;
        }
        const auto tile = a.paired_config->tiles[direction - 1];
        const auto pose = a.paired_config->poses[direction - 1];
        const auto result =
            pair_atlas::neighbor_tile(tile, pose, pair_atlas::total_entries, a.paired_atlas, pixel,
                                      a.config->width, a.config->height);
        if (result.status != pair_atlas::Status::ok) {
            AREA_DETAIL(a, aw::invalid_payload, result.status,
                        uint4(direction, tile.first_entry, tile.width, pose.transform),
                        uint4(pose.offset_x, pose.offset_y, pose.reserved, result.pixel));
            return pixel; // Sticky failure forbids successful publication.
        }
        return result.pixel;
    }
    if (a.config->stencil == aw::reciprocal16_stencil)
        return rypath_area_stencil::neighbor(pixel, a.config->width, a.config->height,
                                             {{a.config->offset_x[0], a.config->offset_y[0]},
                                              {a.config->offset_x[1], a.config->offset_y[1]}},
                                             direction);
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
inline uint area_reverse_direction(constant AreaArguments &a, uint direction) {
    // Each atlas is its OWN involution. Other atlas candidates are independent;
    // only legacy +/- vector stencils invert through the opposite slot.
    return a.config->stencil == aw::paired_gaussian16_stencil
               ? direction
               : rypath_area_stencil::inverse_slot(direction);
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
    if (history.valid != 1 || !area_confidence_valid(a, history.confidence) ||
        !area_owned_value_valid(a, history.selected_slot, history.record,
                                {history.target_h_fraction, history.target_h_exponent}) ||
        (area_enhanced(a) &&
         (history.record.bank != a.config->history_source_bank || history.record.pixel != pixel)) ||
        !area_slot_valid(a, history.selected_slot) || !rypath_replay::valid(history.payload) ||
        !area_selected_values(a, history.selected_slot, area_Z(history), area_T(history),
                              history.xyz)) {
        AREA_DETAIL(a, aw::invalid_payload, 0,
                    uint4(as_type<uint>(history.normalizer), as_type<uint>(history.target),
                          history.valid, history.selected_slot),
                    area_payload_head(history.payload), area_payload_tail(history.payload));
        return false;
    }
    if (area_enhanced(a)) {
        const auto target = enhanced_path::target(
            {history.xyz.x, history.xyz.y, history.xyz.z}, a.config->target_scale,
            {history.target_h_fraction, history.target_h_exponent});
        if (target.status != reservoir::Status::ok ||
            !enhanced_reservoir::same(target.value, area_T(history))) {
            AREA_FAIL(a, aw::invalid_payload);
            return false;
        }
    }
    return true;
}
// Cap the temporal INPUT coefficient before either side's MIS evaluation.
// A null selected reservoir is still a structurally present history domain.
inline bool area_temporal_confidence(constant AreaArguments &a, uint pixel,
                                     thread confidence::Temporal &result) {
    confidence::Source previous{};
    if (a.config->history_used) {
        uint source_pixel;
        if (!area_temporal_source(a, pixel, source_pixel))
            return false;
        const auto source = a.history_source[source_pixel];
        if (!area_history(a, source_pixel, source))
            return false;
        previous = {source.confidence, 1};
    }
    result = confidence::temporal(previous, a.config->enhanced_history_cap);
    if (result.status != confidence::Status::ok) {
        AREA_DETAIL(a, aw::invalid_value, result.status,
                    uint4(previous.confidence, previous.present, 0, 0));
        return false;
    }
    return true;
}
inline bool area_target(constant AreaArguments &a, uint pixel, wire::Point xyz,
                        thread AreaNumber &target, reuse_mis::Scaled h = {.5f, 1}) {
    const auto checked =
        area_enhanced(a) ? enhanced_path::target({xyz.x, xyz.y, xyz.z}, a.config->target_scale, h)
                         : area_legacy(reuse_mis::importance_target(xyz.x, xyz.y, xyz.z,
                                                                    a.config->target_scale));
    if (checked.status != reservoir::Status::ok) {
        AREA_DETAIL(a, aw::invalid_value, checked.status,
                    as_type<uint4>(float4(xyz.x, xyz.y, xyz.z, a.config->target_scale)));
        return false;
    }
    target = checked.value;
    return true;
}
inline void area_prepare_evaluation(constant AreaArguments &a, uint pixel, uint requested_slot,
                                    ew::Reference source = ew::absent()) {
    if (area_contribution(a))
        a.contribution_evaluation[pixel] = {requested_slot, reservoir::no_selection, 0.f, 0};
    if (area_enhanced(a)) {
        const bool fresh = requested_slot == reservoir::no_selection;
        const auto result = fresh || a.config->phase == aw::mutation
                                ? ew::Reference{ew::fresh_bank, pixel}
                                : source;
        a.enhanced_evaluation[pixel] = {source,
                                        result,
                                        0.f,
                                        0,
                                        0,
                                        0,
                                        a.config->suffix_context_low,
                                        a.config->suffix_context_high};
    }
}
inline bool area_owned_result(constant AreaArguments &a, uint pixel, thread ew::Reference &record,
                              thread reuse_mis::Scaled &h, uint spatial_neighbor = 0xffffffffu) {
    record = {0, 0};
    h = {0, 0};
    if (!area_enhanced(a))
        return true; // Never read legacy's one-record sentinels.
    const auto receipt = a.enhanced_evaluation[pixel];
    const auto request = a.contribution_evaluation[pixel];
    auto pending = receipt;
    pending.target_h_fraction = 0;
    pending.target_h_exponent = 0;
    pending.ready = 0;
    pending.normalizer_exponent = 0;
    const bool fresh = request.requested_slot == reservoir::no_selection;
    record = receipt.result;
    h = {receipt.target_h_fraction, receipt.target_h_exponent};
    if (receipt.ready != 1 || (!fresh && receipt.normalizer_exponent != 0) ||
        receipt.suffix_context_low != a.config->suffix_context_low ||
        receipt.suffix_context_high != a.config->suffix_context_high ||
        !area_slot_valid(a, request.selected_slot) ||
        !rypath_enhanced_runtime::pending_owned(pending, a.config->pixel_count, pixel, fresh) ||
        !area_owned_value_valid(a, request.selected_slot, record, h)) {
        AREA_FAIL(a, aw::invalid_payload);
        return false;
    }
    ew::Reference expected = ew::absent();
    if (a.config->phase == aw::temporal) {
        uint source_pixel;
        if (!area_temporal_source(a, pixel, source_pixel))
            return false;
        expected = a.history_source[source_pixel].record;
    } else if (a.config->phase == aw::spatial) {
        const uint neighbor =
            spatial_neighbor == 0xffffffffu ? area_neighbor(a, pixel) : spatial_neighbor;
        if (area_failed(a))
            return false;
        if (neighbor >= a.config->pixel_count) {
            AREA_FAIL(a, aw::invalid_payload);
            return false;
        }
        expected = a.temporal[neighbor].record;
    } else if (!fresh)
        expected = a.temporal[pixel].record;
    const auto result =
        fresh || a.config->phase == aw::mutation ? ew::Reference{ew::fresh_bank, pixel} : expected;
    if (!area_same_reference(receipt.source, expected) || !area_same_reference(record, result)) {
        AREA_FAIL(a, aw::invalid_payload);
        return false;
    }
    return true;
}
inline bool area_pending_evaluation(constant AreaArguments &a, uint pixel, uint requested_slot) {
    if (!area_contribution(a))
        return true;
    const auto e = a.contribution_evaluation[pixel];
    if (!area_slot_valid(a, requested_slot) || e.requested_slot != requested_slot ||
        e.selected_slot != reservoir::no_selection || as_type<uint>(e.normalizer) != 0 ||
        e.ready != 0 ||
        (area_enhanced(a) && a.enhanced_evaluation[pixel].normalizer_exponent != 0)) {
        AREA_DETAIL(
            a, aw::invalid_payload, 0,
            uint4(e.requested_slot, e.selected_slot, as_type<uint>(e.normalizer), e.ready),
            uint4(requested_slot, a.config->contribution_mode, a.config->contribution_depth, 0));
        return false;
    }
    return true;
}
inline bool area_evaluated_slot(constant AreaArguments &a, uint pixel, bool fresh,
                                AreaNumber target, thread uint &selected_slot,
                                thread AreaNumber &normalizer) {
    selected_slot = 0;
    normalizer = target;
    if (!area_contribution(a))
        return true;
    const auto e = a.contribution_evaluation[pixel];
    const int exponent = area_enhanced(a) ? a.enhanced_evaluation[pixel].normalizer_exponent : 0;
    const AreaNumber received{e.normalizer, exponent};
    const bool request_valid =
        fresh ? e.requested_slot == reservoir::no_selection
              : area_slot_valid(a, e.requested_slot) && e.requested_slot == e.selected_slot;
    const bool fresh_null = fresh && area_null(a, e.selected_slot);
    const bool normalizer_valid =
        fresh_null ? enhanced_reservoir::zero(received) && enhanced_reservoir::zero(target)
        : fresh    ? area_enhanced(a)
                         ? enhanced_reservoir::positive(received)
                         : reservoir::positive_normal(e.normalizer) && e.normalizer >= target.fraction
                   : enhanced_reservoir::zero(received);
    if (e.ready != 1 || !area_slot_valid(a, e.selected_slot) || !request_valid ||
        (area_null(a, e.selected_slot) && !enhanced_reservoir::zero(target)) || !normalizer_valid) {
        AREA_DETAIL(a, aw::invalid_payload, 0, area_numbers(received, target),
                    uint4(e.requested_slot, e.selected_slot, e.ready, fresh));
        return false;
    }
    selected_slot = e.selected_slot;
    normalizer = received;
    return true;
}
inline void area_consume_evaluation(constant AreaArguments &a, uint pixel) {
    if (area_contribution(a))
        a.contribution_evaluation[pixel].ready = 0;
    if (area_enhanced(a))
        a.enhanced_evaluation[pixel].ready = 0;
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
                        AreaNumber score, AreaNumber target, uint selected_slot,
                        uint source_confidence = 0, ew::Reference record = {0, 0},
                        reuse_mis::Scaled h = {0, 0}) {
    if (!area_slot_valid(a, selected_slot) || !area_slot_valid(a, accumulator.selected_slot) ||
        !area_owned_value_valid(a, selected_slot, record, h)) {
        AREA_DETAIL(a, aw::invalid_payload, 0,
                    uint4(selected_slot, accumulator.selected_slot,
                          accumulator.state.candidate_count, a.config->contribution_mode));
        return false;
    }
    if (!area_enhanced(a) && (accumulator.confidence != 0 || source_confidence != 0)) {
        AREA_FAIL(a, aw::invalid_value);
        return false;
    }
    if (area_enhanced(a)) {
        // Score0/null does not delete its input domain. Only an absent boundary
        // neighbor has confidence0; logical scheduling still observes that slot.
        if (accumulator.confidence > confidence::maximum_confidence ||
            !confidence::valid_source({source_confidence, uint(source_confidence != 0)}) ||
            source_confidence > confidence::maximum_confidence - accumulator.confidence ||
            (source_confidence == 0 && !enhanced_reservoir::zero(score))) {
            AREA_FAIL(a, aw::invalid_value);
            return false;
        }
        accumulator.confidence += source_confidence;
        if (accumulator.logical_count >= aw::spatial_candidates ||
            !enhanced_reservoir::valid_number(score) || !enhanced_reservoir::valid_number(target) ||
            (!enhanced_reservoir::zero(score) &&
             (!enhanced_reservoir::positive(target) || area_null(a, selected_slot)))) {
            AREA_FAIL(a, aw::invalid_reservoir);
            return false;
        }
        if (accumulator.logical_count == 0) {
            accumulator.payload = payload; // An opaque receipt for a possible null result.
            accumulator.xyz = {0, 0, 0};
            accumulator.target = 0;
            accumulator.target_exponent = 0;
            accumulator.selected_slot = cw::enhanced_null_slot;
            accumulator.record = record;
            accumulator.target_h_fraction = .5f;
            accumulator.target_h_exponent = 1;
        }
        if (enhanced_reservoir::zero(score)) {
            ++accumulator.logical_count;
            return true; // Retain the domain, but draw no categorical entropy.
        }
    }
    if (area_enhanced(a)) {
        auto state = area_selector(accumulator);
        // Score is ALREADY proposal/MIS weighted. The physical selected target
        // is passed separately and mirrored in the accumulator's wire fields.
        auto pending = enhanced_reservoir::prepare_score(state, score, target);
        if (pending.status != reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_reservoir, pending.status, area_numbers(score, target),
                        area_numbers(pending.score_sum, pending.target));
            return false;
        }
        const enhanced_entropy::Identity identity{pixel, a.config->serial_low,
                                                  a.config->serial_high, a.config->seed_low,
                                                  a.config->seed_high};
        for (uint ordinal = 0;
             !pending.decision.complete && ordinal < enhanced_reservoir::maximum_random_words;
             ++ordinal) {
            const auto entropy = enhanced_entropy::word(
                identity, enhanced_entropy::area_site(a.config->phase, a.config->candidate),
                ordinal);
            if (entropy.status != enhanced_entropy::Status::ok ||
                enhanced_reservoir::feed_random_word(pending, entropy.value) !=
                    reservoir::Status::ok) {
                AREA_DETAIL(a, aw::invalid_reservoir, pending.status, area_numbers(score, target),
                            uint4(ordinal, entropy.value, uint(entropy.status), 0));
                return false;
            }
        }
        const auto result = enhanced_reservoir::commit_update(state, pending);
        if (result.status != reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_reservoir, result.status, area_numbers(score, target));
            return false;
        }
        accumulator.state = {state.score_sum.fraction, state.selected_target.fraction,
                             state.candidate_count, state.selected_index, state.status};
        accumulator.score_exponent = state.score_sum.exponent;
        if (result.selected) {
            accumulator.payload = payload;
            accumulator.xyz = xyz;
            accumulator.target = target.fraction;
            accumulator.target_exponent = target.exponent;
            accumulator.selected_slot = selected_slot;
            accumulator.record = record;
            accumulator.target_h_fraction = h.fraction;
            accumulator.target_h_exponent = h.exponent;
        }
        ++accumulator.logical_count;
        return true;
    }
    if (score.exponent != 0 || target.exponent != 0 || accumulator.score_exponent != 0 ||
        accumulator.target_exponent != 0) {
        AREA_FAIL(a, aw::invalid_reservoir);
        return false;
    }
    // The shared categorical selector receives an already MIS-weighted score.
    // Its target is one; the physical selected target lives separately.
    const float legacy_score = score.fraction, legacy_target = target.fraction;
    auto pending = reservoir::prepare_update(accumulator.state, legacy_score, 1.f);
    if (pending.status != reservoir::Status::ok) {
        AREA_DETAIL(
            a, aw::invalid_reservoir, pending.status, area_state_values(pending.source),
            as_type<uint4>(float4(legacy_score, legacy_target, pending.score_sum, pending.target)),
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
                        as_type<uint4>(
                            float4(legacy_score, legacy_target, pending.score_sum, pending.target)),
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
        AREA_DETAIL(
            a, aw::invalid_reservoir, result.status, area_state_values(pending.source),
            as_type<uint4>(float4(legacy_score, legacy_target, pending.score_sum, pending.target)),
            uint4(uint(pending.source.status), uint(pending.status), pending.words_used,
                  pending.compared_bits),
            uint4(pending.rare_probability.significand, pending.rare_probability.bit_count,
                  pending.complete, pending.selected));
        return false;
    }
    if (result.selected) {
        accumulator.payload = payload;
        accumulator.xyz = xyz;
        accumulator.target = legacy_target;
        accumulator.selected_slot = selected_slot;
        accumulator.record = record;
        accumulator.target_h_fraction = h.fraction;
        accumulator.target_h_exponent = h.exponent;
    }
    if (area_enhanced(a))
        ++accumulator.logical_count;
    return true;
}
inline bool area_finish(constant AreaArguments &a, uint pixel,
                        const thread aw::Accumulator &accumulator, uint count,
                        thread AreaNumber &normalizer) {
    if (area_count(a, accumulator) != count || !area_confidence_valid(a, accumulator.confidence) ||
        !area_owned_value_valid(a, accumulator.selected_slot, accumulator.record,
                                {accumulator.target_h_fraction, accumulator.target_h_exponent}) ||
        !rypath_replay::valid(accumulator.payload) ||
        !area_slot_valid(a, accumulator.selected_slot)) {
        AREA_FAIL(a, aw::invalid_count);
        return false;
    }
    if (area_enhanced(a)) {
        const auto state = area_selector(accumulator);
        if (!enhanced_reservoir::valid_state(state) ||
            !enhanced_reservoir::same(state.selected_target, area_T(accumulator)) ||
            !area_selected_values(a, accumulator.selected_slot, area_S(accumulator),
                                  area_T(accumulator), accumulator.xyz) ||
            (area_null(a, accumulator.selected_slot) && state.candidate_count != 0)) {
            AREA_FAIL(a, aw::invalid_reservoir);
            return false;
        }
    } else if (accumulator.score_exponent != 0 || accumulator.target_exponent != 0 ||
               !reservoir::valid_state(accumulator.state) ||
               accumulator.state.selected_target != 1.f) {
        AREA_FAIL(a, aw::invalid_reservoir);
        return false;
    }
    AreaNumber target;
    if (!area_target(a, pixel, accumulator.xyz, target,
                     {accumulator.target_h_fraction, accumulator.target_h_exponent}))
        return false;
    if (!enhanced_reservoir::same(target, area_T(accumulator))) {
        AREA_DETAIL(a, aw::invalid_reservoir, 0, area_numbers(target, area_T(accumulator)));
        return false;
    }
    normalizer = area_S(accumulator); // No extra /K or confidence normalization.
    return true;
}
inline bool area_temporal_valid(constant AreaArguments &a, uint pixel,
                                const thread aw::Temporal &source) {
    AreaNumber target{0, 0};
    if (!rypath_replay::valid(source.payload) || !area_confidence_valid(a, source.confidence) ||
        !area_owned_value_valid(a, source.selected_slot, source.record,
                                {source.target_h_fraction, source.target_h_exponent}) ||
        (area_enhanced(a) && source.record.bank != ew::fresh_bank &&
         source.record.bank != ew::temporal_bank) ||
        !area_selected_values(a, source.selected_slot, area_Z(source), area_T(source),
                              source.xyz) ||
        !area_slot_valid(a, source.selected_slot) || source.reserved[0] || source.reserved[1] ||
        !area_target(a, pixel, source.xyz, target,
                     {source.target_h_fraction, source.target_h_exponent}) ||
        !enhanced_reservoir::same(area_T(source), target)) {
        AREA_DETAIL(
            a, aw::invalid_value, 0,
            as_type<uint4>(float4(source.normalizer, source.target, target.fraction, source.xyz.x)),
            uint4(as_type<uint>(source.xyz.y), as_type<uint>(source.xyz.z), source.reserved[0],
                  source.reserved[1]),
            area_payload_head(source.payload), area_payload_tail(source.payload));
        return false;
    }
    return true;
}
inline bool area_weighted_xyz(constant AreaArguments &a, uint pixel, AreaNumber normalizer,
                              wire::Point xyz, AreaNumber target, thread wire::Point &weighted) {
    if (area_enhanced(a)) {
        if (!enhanced_reservoir::valid_number(normalizer) ||
            !enhanced_reservoir::valid_number(target)) {
            AREA_FAIL(a, aw::invalid_value);
            return false;
        }
        // A zero MIS weight contributes zero even for a non-null evaluated atom.
        if (enhanced_reservoir::zero(normalizer)) {
            if (!all(isfinite(float3(xyz.x, xyz.y, xyz.z)))) {
                AREA_FAIL(a, aw::invalid_value);
                return false;
            }
            weighted = {0, 0, 0};
            return true;
        }
        const auto result = enhanced_path::readout(normalizer, {xyz.x, xyz.y, xyz.z}, target);
        if (result.status != enhanced_path::Status::ok) {
            AREA_DETAIL(a, aw::invalid_value, result.status, area_numbers(normalizer, target));
            return false;
        }
        weighted = {result.value.x, result.value.y, result.value.z};
        return true;
    }
    if (normalizer.exponent != 0 || target.exponent != 0) {
        AREA_FAIL(a, aw::invalid_value);
        return false;
    }
    const auto x = reuse_mis::signed_contribution(normalizer.fraction, xyz.x, target.fraction);
    const auto y = reuse_mis::signed_contribution(normalizer.fraction, xyz.y, target.fraction);
    const auto z = reuse_mis::signed_contribution(normalizer.fraction, xyz.z, target.fraction);
    if (x.status != reservoir::Status::ok || y.status != reservoir::Status::ok ||
        z.status != reservoir::Status::ok) {
        AREA_DETAIL(
            a, aw::invalid_value, x.status,
            as_type<uint4>(float4(normalizer.fraction, xyz.x, xyz.y, xyz.z)),
            uint4(as_type<uint>(target.fraction), uint(x.status), uint(y.status), uint(z.status)),
            as_type<uint4>(float4(x.value, y.value, z.value, 0)));
        return false;
    }
    weighted = {x.value, y.value, z.value};
    return true;
}
inline bool area_add_output(constant AreaArguments &a, uint pixel, AreaNumber score,
                            wire::Point xyz, AreaNumber target) {
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
        AREA_DETAIL(a, aw::invalid_value, 0,
                    as_type<uint4>(float4(old.x, old.y, old.z, score.fraction)),
                    as_type<uint4>(float4(term.x, term.y, term.z, target.fraction)),
                    uint4(uint(x.status), uint(y.status), uint(z.status), 0));
        return false;
    }
    a.raw_xyz[pixel] = sum;
    return true;
}
kernel void rypath_area_contract_v20(device aw::Contract *out [[buffer(0)]]) {
    *out = {aw::contract,
            sizeof(aw::Config),
            sizeof(aw::History),
            sizeof(aw::Temporal),
            sizeof(aw::Accumulator),
            sizeof(rypath_replay::ReplayState),
            sizeof(aw::Counters),
            sizeof(cw::Evaluation),
            sizeof(rypath_enhanced_suffix_tape::Entry),
            sizeof(rypath::enhanced_motion_wire::Donor),
            sizeof(pair_atlas::Config),
            sizeof(pair_atlas::Entry)};
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
        p.pixel_count != a.config->pixel_count || p.sampler_kind != wire::independent_sampler ||
        (area_contribution(a) && (p.packet_transport != 2 || p.bidirectional != 0 ||
                                  p.max_depth != a.config->contribution_depth))) {
        AREA_DETAIL(
            a, aw::invalid_config, 0, uint4(p.width, p.height, p.pixel_begin, p.pixel_count),
            uint4(a.config->width, a.config->height, a.config->pixel_count, p.sampler_kind));
        return;
    }
    if (a.config->phase == aw::fresh) {
        a.replay[pixel] =
            rypath_replay::fresh({pixel, p.sample_low, p.sample_high, p.seed_low, p.seed_high});
        area_prepare_evaluation(a, pixel, reservoir::no_selection);
    } else if (a.config->phase == aw::temporal_reverse) {
        const aw::Temporal source = a.temporal[pixel];
        if (!area_temporal_valid(a, pixel, source))
            return;
        a.replay[pixel] = source.payload;
        area_prepare_evaluation(a, pixel, source.selected_slot, source.record);
    } else if (a.config->phase == aw::temporal) {
        uint source_pixel;
        if (!area_temporal_source(a, pixel, source_pixel))
            return;
        const aw::History source = a.history_source[source_pixel];
        if (!area_history(a, source_pixel, source))
            return;
        a.replay[pixel] = source.payload;
        area_prepare_evaluation(a, pixel, source.selected_slot, source.record);
    } else {
        if (a.config->candidate == 0) {
            AREA_FAIL(a, aw::invalid_config);
            return;
        }
        const uint neighbor = area_neighbor(a, pixel);
        if (area_failed(a))
            return;
        const aw::Temporal source = a.temporal[neighbor];
        if (!area_temporal_valid(a, pixel, source)) {
            return;
        }
        a.replay[pixel] = source.payload;
        area_prepare_evaluation(a, pixel, source.selected_slot, source.record);
    }
}
// Host permits this kernel only when complete source/destination evaluator
// contexts are bit-identical. Pixel ownership is unchanged (no reprojection).
// prepare and merge still perform their original validation and score updates.
template <typename Source>
inline bool area_publish_cached(constant AreaArguments &a, uint pixel,
                                const thread Source &source) {
    if (!area_pending_evaluation(a, pixel, source.selected_slot))
        return false;
    if (area_enhanced(a)) {
        auto receipt = a.enhanced_evaluation[pixel];
        if (!rypath_enhanced_runtime::pending_owned(receipt, a.config->pixel_count, pixel, false) ||
            !area_same_reference(receipt.source, source.record) ||
            !area_same_reference(receipt.result, source.record)) {
            AREA_FAIL(a, aw::invalid_payload);
            return false;
        }
        // The cache is an evaluator substitute and must retain transport's
        // record validation. Analytic nulls never read unused record bytes.
        if (!area_null(a, source.selected_slot)) {
            const auto record =
                a.enhanced_records[ulong(source.record.bank) * a.config->pixel_count +
                                   source.record.pixel];
            if (record.tag == rypath_enhanced_chart::terminal_surface ||
                !rypath_enhanced_chart::valid_chart(record, source.selected_slot,
                                                    a.config->contribution_depth)) {
                AREA_FAIL(a, aw::invalid_payload);
                return false;
            }
        }
        receipt.target_h_fraction = source.target_h_fraction;
        receipt.target_h_exponent = source.target_h_exponent;
        receipt.normalizer_exponent = 0;
        receipt.ready = 1;
        a.enhanced_evaluation[pixel] = receipt;
    }
    a.leaf_xyz[pixel] = source.xyz;
    if (area_contribution(a))
        a.contribution_evaluation[pixel] = {source.selected_slot, source.selected_slot, 0.f, 1};
    return true;
}
kernel void rypath_area_cache_evaluation(constant AreaArguments &a [[buffer(1)]],
                                         uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    if (!a.config->temporal_cache) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    if (a.config->phase == aw::temporal_reverse) {
        const auto fresh = a.temporal[pixel];
        if (!area_temporal_valid(a, pixel, fresh))
            return;
        area_publish_cached(a, pixel, fresh);
    } else if (a.config->phase == aw::temporal) {
        uint source_pixel;
        if (!area_temporal_source(a, pixel, source_pixel))
            return;
        const auto source = a.history_source[source_pixel];
        AreaNumber target;
        if (!area_history(a, source_pixel, source) ||
            !area_target(a, pixel, source.xyz, target,
                         area_enhanced(a)
                             ? reuse_mis::Scaled{source.target_h_fraction, source.target_h_exponent}
                             : reuse_mis::Scaled{.5f, 1}))
            return;
        if (!enhanced_reservoir::same(target, area_T(source))) {
            AREA_DETAIL(
                a, aw::invalid_value, 0,
                as_type<uint4>(float4(source.xyz.x, source.xyz.y, source.xyz.z, target.fraction)),
                uint4(as_type<uint>(source.target), 0, 0, 0));
            return;
        }
        area_publish_cached(a, pixel, source);
    } else {
        AREA_FAIL(a, aw::invalid_config);
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
    ew::Reference record{};
    reuse_mis::Scaled h{};
    const uint spatial_neighbor = a.config->phase == aw::spatial ? area_neighbor(a, pixel) : pixel;
    if (area_failed(a) || !area_owned_result(a, pixel, record, h, spatial_neighbor))
        return;
    AreaNumber target;
    if (!area_target(a, pixel, xyz, target, h))
        return;
    uint selected_slot;
    AreaNumber fresh_normalizer;
    if (!area_evaluated_slot(a, pixel, a.config->phase == aw::fresh, target, selected_slot,
                             fresh_normalizer))
        return;
    AreaNumber score = target;
    uint expected = 0;
    uint source_confidence = 0;
    if (a.config->phase == aw::fresh) {
        source_confidence = area_enhanced(a) ? 1u : 0u;
        if (area_contribution(a))
            score = fresh_normalizer;
        a.temporal[pixel] = {payload,           score.fraction, xyz,
                             target.fraction,   selected_slot,  {0, 0},
                             source_confidence, record,         h.fraction,
                             h.exponent,        score.exponent, target.exponent};
        if (a.config->history_used) {
            area_consume_evaluation(a, pixel);
            return; // Fresh score also needs this payload's previous-context target.
        }
    } else if (a.config->phase == aw::temporal_reverse) {
        const aw::Temporal source = a.temporal[pixel];
        if (!area_temporal_valid(a, pixel, source) || !area_same_payload(source.payload, payload) ||
            selected_slot != source.selected_slot) {
            AREA_DETAIL(a, aw::invalid_payload, 0, area_payload_head(source.payload),
                        area_payload_tail(source.payload), area_payload_head(payload),
                        area_payload_tail(payload));
            return;
        }
        confidence::Temporal temporal_confidence{};
        if (area_enhanced(a)) {
            if (source.confidence != 1 ||
                !area_temporal_confidence(a, pixel, temporal_confidence)) {
                AREA_FAIL(a, aw::invalid_value);
                return;
            }
            source_confidence = temporal_confidence.fresh.confidence;
        }
        const auto checked =
            area_enhanced(a)
                ? enhanced_path::canonical_score(area_Z(source), area_T(source), target,
                                                 float(temporal_confidence.fresh.confidence),
                                                 float(temporal_confidence.history.confidence))
            : area_contribution(a)
                ? area_legacy(reuse_mis::temporal_fresh_score(source.normalizer, source.target,
                                                              target.fraction))
                : area_legacy(reuse_mis::temporal_fresh_score(source.target, target.fraction));
        if (checked.status != reservoir::Status::ok) {
            AREA_DETAIL(
                a, aw::invalid_value, checked.status,
                as_type<uint4>(float4(source.target, target.fraction, checked.value.fraction, 0)));
            return;
        }
        score = checked.value;
        xyz = source.xyz;
        target = area_T(source);
        record = source.record;
        h = {source.target_h_fraction, source.target_h_exponent};
    } else if (a.config->phase == aw::temporal) {
        uint source_pixel;
        if (!area_temporal_source(a, pixel, source_pixel))
            return;
        const aw::History source = a.history_source[source_pixel];
        if (!area_history(a, source_pixel, source) || !area_same_payload(source.payload, payload) ||
            selected_slot != source.selected_slot) {
            AREA_DETAIL(a, aw::invalid_payload, 0, area_payload_head(source.payload),
                        area_payload_tail(source.payload), area_payload_head(payload),
                        area_payload_tail(payload));
            return;
        }
        accumulator = a.accumulators[pixel];
        expected = 1;
        confidence::Temporal temporal_confidence{};
        if (area_enhanced(a)) {
            if (!area_temporal_confidence(a, pixel, temporal_confidence))
                return;
            source_confidence = temporal_confidence.history.confidence;
        }
        const auto checked =
            area_enhanced(a)
                ? enhanced_path::pair_score({area_Z(source), area_T(source), target,
                                             float(temporal_confidence.history.confidence),
                                             float(temporal_confidence.fresh.confidence)})
                : area_legacy(reuse_mis::temporal_history_score(source.normalizer, target.fraction,
                                                                source.target));
        if (checked.status != reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_value, checked.status,
                        as_type<uint4>(float4(source.normalizer, target.fraction, source.target,
                                              checked.value.fraction)));
            return;
        }
        score = checked.value;
    } else if (a.config->phase == aw::spatial) {
        if (a.config->candidate == 0) {
            AREA_FAIL(a, aw::invalid_config);
            return;
        }
        const uint neighbor = spatial_neighbor;
        const aw::Temporal source = a.temporal[neighbor];
        if (!area_temporal_valid(a, pixel, source) || !area_same_payload(source.payload, payload) ||
            selected_slot != source.selected_slot) {
            AREA_DETAIL(a, aw::invalid_payload, 0, area_payload_head(source.payload),
                        area_payload_tail(source.payload), area_payload_head(payload),
                        area_payload_tail(payload));
            return;
        }
        // A canonical-only leaf builds the inverse-target map without adding
        // a donor proposal or touching the conditional-mean output/reservoir.
        if (a.config->spatial_mode && a.config->candidate != a.config->forward_atlas) {
            if (a.config->candidate != a.config->canonical_atlas) {
                AREA_FAIL(a, aw::invalid_config);
                return;
            }
            area_cross_store(a, area_cross_index(a, a.config->candidate, pixel), target);
            area_consume_evaluation(a, pixel);
            return;
        }
        accumulator = a.accumulators[pixel];
        expected = a.config->spatial_mode ? 0u : a.config->candidate - 1;
        aw::Temporal center{};
        const bool boundary = neighbor == pixel;
        if (area_enhanced(a)) {
            center = a.temporal[pixel];
            if (!area_temporal_valid(a, pixel, center))
                return;
            source_confidence = boundary ? 0u : source.confidence;
        }
        const auto checked =
            area_enhanced(a)
                ? boundary ? AreaNumberResult{{0, 0}, reservoir::Status::ok}
                           : enhanced_path::pair_score(
                                 {area_Z(source), area_T(source), target, float(source.confidence),
                                  float(center.confidence), a.config->spatial_mode ? 1.f : .25f})
                : area_legacy(reuse_mis::spatial_pair_score(source.normalizer, target.fraction,
                                                            source.target));
        if (checked.status != reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_value, checked.status,
                        as_type<uint4>(float4(source.normalizer, target.fraction, source.target,
                                              checked.value.fraction)));
            return;
        }
        score = checked.value;
        // The inverse direction at the neighboring pixel provides the center
        // payload's reverse target after all four immutable-cache passes finish.
        if (!a.config->spatial_mode || a.config->candidate == a.config->canonical_atlas)
            area_cross_store(a, area_cross_index(a, a.config->candidate, pixel), target);
        if (!area_add_output(a, pixel, score, xyz, target))
            return;
    } else {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    if (area_count(a, accumulator) != expected) {
        AREA_DETAIL(a, aw::invalid_count, accumulator.state.status,
                    area_state_values(accumulator.state), uint4(expected, 0, 0, 0));
        return;
    }
    if (!area_update(a, pixel, accumulator, payload, xyz, score, target, selected_slot,
                     source_confidence, record, h))
        return;
    a.accumulators[pixel] = accumulator;
    area_consume_evaluation(a, pixel);
}
// Empty specialization leaves original entry points with no diagnostic resources
// or runtime mode branch. Enabled variants only observe publication provenance.
struct AreaNoDuplication {
    inline bool temporal(constant AreaArguments &, uint, const thread aw::Accumulator &) const {
        return true;
    }
    inline bool final(constant AreaArguments &, uint, const thread aw::Accumulator &) const {
        return true;
    }
};
struct AreaDuplication {
    constant DuplicationArguments *diagnostic;
    constant wire::Parameters *parameters;
    inline bool temporal(constant AreaArguments &a, uint pixel,
                         const thread aw::Accumulator &winner) const {
        uint source = pixel;
        if (!area_temporal_source(a, pixel, source))
            return false;
        const auto config = *a.config;
        return duplication_temporal(*diagnostic, config, *parameters, pixel, source, winner);
    }
    inline bool final(constant AreaArguments &a, uint pixel,
                      const thread aw::Accumulator &winner) const {
        const auto config = *a.config;
        return duplication_final(*diagnostic, config, *parameters, pixel, winner);
    }
};
template <typename Publication>
inline void area_temporal_finish_body(constant AreaArguments &a, uint pixel,
                                      const thread Publication &publication) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    const uint count = a.config->history_used ? 2u : 1u;
    if ((a.config->history_used && a.config->phase != aw::temporal) ||
        (!a.config->history_used && a.config->phase != aw::fresh)) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    const auto accumulator = a.accumulators[pixel];
    AreaNumber normalizer;
    if (!area_finish(a, pixel, accumulator, count, normalizer))
        return;
    if (area_enhanced(a)) {
        confidence::Temporal temporal_confidence{};
        if (!area_temporal_confidence(a, pixel, temporal_confidence) ||
            accumulator.confidence != temporal_confidence.output.confidence) {
            AREA_FAIL(a, aw::invalid_count);
            return;
        }
    }
    if (!publication.temporal(a, pixel, accumulator))
        return;
    ew::Reference record{};
    if (area_enhanced(a)) {
        record = {ew::temporal_bank, pixel};
        if (!area_copy_record(a, pixel, accumulator.selected_slot, accumulator.record, record))
            return;
    }
    a.temporal[pixel] = {accumulator.payload,
                         normalizer.fraction,
                         accumulator.xyz,
                         accumulator.target,
                         accumulator.selected_slot,
                         {0, 0},
                         accumulator.confidence,
                         record,
                         accumulator.target_h_fraction,
                         accumulator.target_h_exponent,
                         normalizer.exponent,
                         accumulator.target_exponent};
}
kernel void rypath_area_temporal_finish(constant AreaArguments &a [[buffer(1)]],
                                        uint pixel [[thread_position_in_grid]]) {
    area_temporal_finish_body(a, pixel, AreaNoDuplication{});
}
kernel void rypath_area_temporal_finish_duplication_v1(constant wire::Parameters &p [[buffer(0)]],
                                                       constant AreaArguments &a [[buffer(1)]],
                                                       constant DuplicationArguments &d
                                                       [[buffer(2)]],
                                                       uint pixel [[thread_position_in_grid]]) {
    area_temporal_finish_body(a, pixel, AreaDuplication{&d, &p});
}
// Diagnostic extraction of the ACTUAL Enhanced fresh local-selected atom.
// No history/temporal/suffix publication or extra categorical draw occurs here.
kernel void rypath_area_fresh_finalize_v1(constant AreaArguments &a [[buffer(1)]],
                                          uint pixel [[thread_position_in_grid]]) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    if (!area_enhanced(a) || a.config->phase != aw::fresh || a.config->candidate != 0 ||
        a.config->history_used || a.config->mutation_steps || a.config->spatial_mode) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    const auto accumulator = a.accumulators[pixel];
    AreaNumber Z;
    if (accumulator.confidence != 1 || !area_finish(a, pixel, accumulator, 1, Z)) {
        AREA_FAIL(a, aw::invalid_count);
        return;
    }
    wire::Point weighted;
    if (!area_weighted_xyz(a, pixel, Z, accumulator.xyz, area_T(accumulator), weighted))
        return;
    a.raw_xyz[pixel] = weighted;
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
template <typename Publication>
inline void area_finalize_body(constant AreaArguments &a, uint pixel,
                               const thread Publication &publication) {
    if (pixel >= a.config->pixel_count || !area_config(a, pixel))
        return;
    if (a.config->phase != aw::spatial_final || a.config->candidate != 0) {
        AREA_FAIL(a, aw::invalid_config);
        return;
    }
    auto accumulator = a.accumulators[pixel];
    if (area_count(a, accumulator) != (a.config->spatial_mode ? 1u : 4u)) {
        AREA_DETAIL(a, aw::invalid_count, accumulator.state.status,
                    area_state_values(accumulator.state));
        return;
    }
    const aw::Temporal center = a.temporal[pixel];
    if (!area_temporal_valid(a, pixel, center))
        return;
    AreaNumber center_score{0, 0};
    confidence::Spatial spatial_confidence{};
    if (area_enhanced(a))
        spatial_confidence = confidence::spatial_begin({center.confidence, 1});
    for (uint direction = 1; direction <= 4; ++direction) {
        const uint neighbor = area_neighbor_direction(a, pixel, direction);
        if (area_failed(a))
            return;
        const uint reverse_direction = area_reverse_direction(a, direction);
        const bool selected_canonical =
            !a.config->spatial_mode || direction == a.config->canonical_atlas;
        // An edge singleton reads the center directly; an atlas pair reverses
        // within this SAME candidate, while legacy offsets use their opposite.
        const AreaNumber reverse_target =
            !selected_canonical ? AreaNumber{0, 0}
            : neighbor == pixel
                ? area_T(center)
                : area_cross_load(a, area_cross_index(a, reverse_direction, neighbor));
        uint neighbor_confidence = 0;
        if (area_enhanced(a) && neighbor != pixel) {
            const auto source = a.temporal[neighbor];
            if (!area_temporal_valid(a, pixel, source))
                return;
            neighbor_confidence = source.confidence;
        }
        if (area_enhanced(a) &&
            confidence::add_neighbor(spatial_confidence,
                                     {neighbor_confidence, uint(neighbor != pixel)}) !=
                confidence::Status::ok) {
            AREA_FAIL(a, aw::invalid_count);
            return;
        }
        if (!selected_canonical)
            continue;
        const auto part = area_enhanced(a)
                              ? enhanced_path::canonical_score(
                                    area_Z(center), area_T(center),
                                    neighbor == pixel ? AreaNumber{0, 0} : reverse_target,
                                    float(center.confidence),
                                    neighbor == pixel ? 1.f : float(neighbor_confidence),
                                    a.config->spatial_mode ? 1.f : .25f)
                              : area_legacy(reuse_mis::spatial_pair_score(
                                    center.normalizer, center.target, reverse_target.fraction));
        if (part.status != reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_value, part.status,
                        as_type<uint4>(float4(center.normalizer, center.target,
                                              reverse_target.fraction, part.value.fraction)),
                        uint4(direction, neighbor, reverse_direction, 0));
            return;
        }
        const auto sum =
            direction == 1 ? part
            : area_enhanced(a)
                ? enhanced_path::add_scores(center_score, part.value)
                : area_legacy(reuse_mis::add_scores(center_score.fraction, part.value.fraction));
        if (sum.status != reservoir::Status::ok) {
            AREA_DETAIL(a, aw::invalid_value, sum.status,
                        as_type<uint4>(float4(center_score.fraction, part.value.fraction,
                                              sum.value.fraction, 0)),
                        uint4(direction, neighbor, reverse_direction, 0));
            return;
        }
        center_score = sum.value;
    }
    if (a.config->spatial_mode) {
        const uint donor = area_neighbor_direction(a, pixel, a.config->forward_atlas);
        const uint sampled_confidence = donor == pixel ? 0u : a.temporal[donor].confidence;
        if (accumulator.confidence != sampled_confidence || area_failed(a)) {
            AREA_FAIL(a, aw::invalid_count);
            return;
        }
        // Confidence describes all structurally admitted domains, not the two
        // Monte Carlo term choices, even when a sampled score or shift is null.
        accumulator.confidence = spatial_confidence.confidence - center.confidence;
    }
    if (area_enhanced(a) &&
        (spatial_confidence.status != confidence::Status::ok ||
         accumulator.confidence != spatial_confidence.confidence - center.confidence)) {
        AREA_FAIL(a, aw::invalid_count);
        return;
    }
    if (!area_update(a, pixel, accumulator, center.payload, center.xyz, center_score,
                     area_T(center), center.selected_slot,
                     area_enhanced(a) ? center.confidence : 0u, center.record,
                     {center.target_h_fraction, center.target_h_exponent}) ||
        !area_add_output(a, pixel, center_score, center.xyz, area_T(center)))
        return;
    AreaNumber normalizer;
    if (!area_finish(a, pixel, accumulator, a.config->spatial_mode ? 2u : aw::spatial_candidates,
                     normalizer))
        return;
    if (a.config->output_mode == aw::selected_output) {
        wire::Point weighted;
        if (!area_weighted_xyz(a, pixel, normalizer, accumulator.xyz, area_T(accumulator),
                               weighted))
            return;
        a.raw_xyz[pixel] = weighted;
    }
    // Output marginalization never changes the selected sample reused later.
    if (!publication.final(a, pixel, accumulator))
        return;
    ew::Reference record{};
    if (area_enhanced(a)) {
        record = {1u - a.config->history_source_bank, pixel};
        if (!area_copy_record(a, pixel, accumulator.selected_slot, accumulator.record, record))
            return;
    }
    a.history_destination[pixel] = {accumulator.payload,
                                    normalizer.fraction,
                                    accumulator.target,
                                    1,
                                    accumulator.selected_slot,
                                    accumulator.xyz,
                                    accumulator.confidence,
                                    record,
                                    accumulator.target_h_fraction,
                                    accumulator.target_h_exponent,
                                    normalizer.exponent,
                                    accumulator.target_exponent};
}

kernel void rypath_area_finalize(constant AreaArguments &a [[buffer(1)]],
                                 uint pixel [[thread_position_in_grid]]) {
    area_finalize_body(a, pixel, AreaNoDuplication{});
}
kernel void rypath_area_finalize_duplication_v1(constant wire::Parameters &p [[buffer(0)]],
                                                constant AreaArguments &a [[buffer(1)]],
                                                constant DuplicationArguments &d [[buffer(2)]],
                                                uint pixel [[thread_position_in_grid]]) {
    area_finalize_body(a, pixel, AreaDuplication{&d, &p});
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
    const auto target =
        area_enhanced(a)
            ? enhanced_path::target({source.xyz.x, source.xyz.y, source.xyz.z},
                                    a.config->target_scale,
                                    {source.target_h_fraction, source.target_h_exponent})
            : area_legacy(reuse_mis::importance_target(source.xyz.x, source.xyz.y, source.xyz.z,
                                                       a.config->target_scale));
    if (!rypath_replay::valid(source.payload) || !area_confidence_valid(a, source.confidence) ||
        !area_owned_value_valid(a, source.selected_slot, source.record,
                                {source.target_h_fraction, source.target_h_exponent}) ||
        (area_enhanced(a) && source.record.bank != ew::fresh_bank &&
         source.record.bank != ew::temporal_bank) ||
        !area_selected_values(a, source.selected_slot, area_Z(source), area_T(source),
                              source.xyz) ||
        !area_slot_valid(a, source.selected_slot) || source.reserved[0] != 0 ||
        source.reserved[1] != 0 || target.status != reservoir::Status::ok ||
        !enhanced_reservoir::same(target.value, area_T(source))) {
        AREA_DETAIL(a, aw::invalid_mutation, target.status,
                    as_type<uint4>(float4(source.normalizer, source.target, target.value.fraction,
                                          a.config->target_scale)),
                    as_type<uint4>(float4(source.xyz.x, source.xyz.y, source.xyz.z, 0)),
                    area_payload_head(source.payload), area_payload_tail(source.payload));
        return false;
    }
    return true;
}
inline bool area_mutation_rejected(constant AreaArguments &a, uint pixel) {
    const uint maximum = a.config->pixel_count * a.config->mutation_steps;
    if (atomic_fetch_add_explicit(&a.counters->mutations, 1u, memory_order_relaxed) >= maximum) {
        AREA_FAIL(a, aw::invalid_mutation);
        return false;
    }
    area_consume_evaluation(a, pixel);
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
    const auto proposal =
        mutation::propose(source, entropy.value, mutation::Policy(a.config->mutation_policy));
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
        p.pixel_count != a.config->pixel_count || p.sampler_kind != wire::independent_sampler ||
        (area_contribution(a) && (p.packet_transport != 2 || p.bidirectional != 0 ||
                                  p.max_depth != a.config->contribution_depth))) {
        AREA_DETAIL(
            a, aw::invalid_config, 0, uint4(p.width, p.height, p.pixel_begin, p.pixel_count),
            uint4(a.config->width, a.config->height, a.config->pixel_count, p.sampler_kind));
        return;
    }
    const auto source = a.temporal[pixel];
    if (!area_mutation_source(a, pixel, source))
        return;
    if (area_null(a, source.selected_slot)) {
        a.replay[pixel] = source.payload;
        area_prepare_evaluation(a, pixel, source.selected_slot, source.record);
        return;
    }
    const auto proposal = area_mutation_proposal(a, pixel, source.payload);
    if (proposal.status != mutation::Status::ok)
        return;
    a.replay[pixel] = proposal.payload;
    // Symmetric mutation changes U only; j remains fixed on the counting domain.
    area_prepare_evaluation(a, pixel, source.selected_slot, source.record);
}
struct AreaMutationResult {
    AreaNumber normalizer{0, 0};
    uint words_used = 0;
    bool accepted = false;
};
inline bool area_mutation_decision(constant AreaArguments &a, uint pixel,
                                   const thread aw::Temporal &source, AreaNumber target,
                                   thread AreaMutationResult &result) {
    if (area_enhanced(a)) {
        auto pending = enhanced_reservoir::prepare_normalizer_acceptance(area_Z(source),
                                                                         area_T(source), target);
        if (pending.status != mutation::Status::ok) {
            AREA_DETAIL(a, aw::invalid_mutation, pending.status,
                        area_numbers(area_Z(source), area_T(source)),
                        area_numbers(target, pending.accepted_normalizer));
            return false;
        }
        const enhanced_entropy::Identity identity{pixel, a.config->serial_low,
                                                  a.config->serial_high, a.config->seed_low,
                                                  a.config->seed_high};
        for (uint ordinal = 0;
             !pending.decision.complete && ordinal < enhanced_reservoir::maximum_random_words;
             ++ordinal) {
            const auto entropy = enhanced_entropy::word(
                identity, enhanced_entropy::mutation_site(a.config->candidate), ordinal);
            if (entropy.status != enhanced_entropy::Status::ok ||
                enhanced_reservoir::feed_random_word(pending, entropy.value) !=
                    mutation::Status::ok) {
                AREA_DETAIL(a, aw::invalid_mutation, pending.status,
                            area_numbers(area_Z(source), target),
                            uint4(ordinal, entropy.value, uint(entropy.status), 0));
                return false;
            }
        }
        const auto checked = enhanced_reservoir::finalize_normalizer_acceptance(pending);
        if (checked.status != mutation::Status::ok) {
            AREA_DETAIL(a, aw::invalid_mutation, checked.status,
                        area_numbers(area_Z(source), target));
            return false;
        }
        result = {checked.normalizer, checked.words_used, checked.accepted};
        return true;
    }
    // Legacy FLOAT preparation/entropy/correction order is unchanged.
    auto pending =
        reuse_mis::prepare_normalizer_acceptance(source.normalizer, source.target, target.fraction);
    if (pending.status != mutation::Status::ok) {
        AREA_DETAIL(a, aw::invalid_mutation, pending.status, area_numbers(area_Z(source), target));
        return false;
    }
    const auto identity = area_mutation_identity(a, pixel);
    for (uint ordinal = 0; !pending.decision.complete && ordinal < reservoir::maximum_random_words;
         ++ordinal) {
        const auto entropy = mutation::acceptance_word(identity, a.config->candidate, ordinal);
        if (entropy.status != mutation::Status::ok ||
            reuse_mis::feed_random_word(pending, entropy.value) != mutation::Status::ok) {
            AREA_DETAIL(a, aw::invalid_mutation, pending.status,
                        area_numbers(area_Z(source), target));
            return false;
        }
    }
    const auto checked = reuse_mis::finalize_normalizer_acceptance(pending);
    if (checked.status != mutation::Status::ok) {
        AREA_DETAIL(a, aw::invalid_mutation, checked.status, area_numbers(area_Z(source), target));
        return false;
    }
    result = {{checked.normalizer, 0}, checked.words_used, checked.accepted};
    return true;
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
    ew::Reference record{};
    reuse_mis::Scaled h{};
    if (!area_owned_result(a, pixel, record, h))
        return;
    if (area_null(a, source.selected_slot)) {
        uint selected_slot;
        AreaNumber unused_normalizer;
        const auto evaluated_null = a.replay[pixel];
        if (!area_same_payload(evaluated_null, source.payload) || !area_zero(a.leaf_xyz[pixel]) ||
            !area_evaluated_slot(a, pixel, false, AreaNumber{0, 0}, selected_slot,
                                 unused_normalizer) ||
            selected_slot != cw::enhanced_null_slot) {
            AREA_FAIL(a, aw::invalid_mutation);
            return;
        }
        if (area_mutation_rejected(a, pixel))
            area_consume_evaluation(a, pixel);
        return; // A null realization has no atom to mutate; retain its domain.
    }
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
    const auto target =
        area_enhanced(a) ? enhanced_path::target({xyz.x, xyz.y, xyz.z}, a.config->target_scale, h)
                         : area_legacy(reuse_mis::importance_target(xyz.x, xyz.y, xyz.z,
                                                                    a.config->target_scale));
    if (target.status != reservoir::Status::ok) {
        AREA_DETAIL(a, aw::invalid_mutation, target.status,
                    as_type<uint4>(float4(xyz.x, xyz.y, xyz.z, a.config->target_scale)));
        return;
    }
    uint selected_slot;
    AreaNumber unused_normalizer;
    if (!area_evaluated_slot(a, pixel, false, target.value, selected_slot, unused_normalizer))
        return;
    if (selected_slot != source.selected_slot) {
        AREA_DETAIL(a, aw::invalid_mutation, 0, uint4(source.selected_slot, selected_slot, 0, 0));
        return;
    }
    if (area_enhanced(a) && enhanced_reservoir::zero(target.value)) {
        if (area_mutation_rejected(a, pixel))
            area_consume_evaluation(a, pixel);
        return; // Exact zero acceptance; no positive-target MH division or draw.
    }
    AreaMutationResult result{};
    if (!area_mutation_decision(a, pixel, source, target.value, result))
        return;
    // Counts are diagnostics, never an acceptance/stopping criterion. Valid
    // dispatches attempt exactly pixel_count*mutation_steps; configuration
    // validation proves this product fits the shared 32-bit counters.
    const uint maximum = a.config->pixel_count * a.config->mutation_steps;
    if (atomic_fetch_add_explicit(&a.counters->mutations, 1u, memory_order_relaxed) >= maximum) {
        AREA_DETAIL(a, aw::invalid_mutation, 0,
                    uint4(maximum, result.words_used, result.accepted, 0),
                    as_type<uint4>(float4(source.normalizer, source.target, target.value.fraction,
                                          result.normalizer.fraction)));
        return;
    }
    if (result.accepted) {
        if (atomic_fetch_add_explicit(&a.counters->accepted, 1u, memory_order_relaxed) >= maximum) {
            AREA_DETAIL(a, aw::invalid_mutation, 0,
                        uint4(maximum, result.words_used, result.accepted, 0),
                        as_type<uint4>(float4(source.normalizer, source.target,
                                              target.value.fraction, result.normalizer.fraction)));
            return;
        }
        ew::Reference destination{};
        if (area_enhanced(a)) {
            destination = {ew::temporal_bank, pixel};
            if (!area_copy_record(a, pixel, selected_slot, record, destination))
                return;
        }
        a.temporal[pixel] = {proposal.payload,
                             result.normalizer.fraction,
                             xyz,
                             target.value.fraction,
                             source.selected_slot,
                             {0, 0},
                             source.confidence,
                             destination,
                             h.fraction,
                             h.exponent,
                             result.normalizer.exponent,
                             target.value.exponent};
    }
    // Rejection retains the exact old payload/Z/XYZ/target. The host always
    // launches every configured step, including after rejections/equal targets.
    area_consume_evaluation(a, pixel);
}

#undef AREA_DETAIL
#undef AREA_FAIL
