// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/contribution_wire.h"
#include "../src/enhanced_decision_shared.h"
#include "../src/enhanced_hybrid_state_shared.h"
#include "../src/enhanced_path_shared.h"
#include "../src/enhanced_resampling_decision_shared.h"
#include "../src/enhanced_suffix_tape_shared.h"
#include "../src/enhanced_wire.h"
#include "../src/wavefront_wire.h"
#include "frame_sensor.h"

namespace rypath_enhanced_runtime {
namespace ep = rypath_enhanced_path;
namespace cw = rypath::contribution_wire;
namespace rw = rypath::render_wire;
namespace wf = rypath::wavefront_wire;
namespace ew = rypath::enhanced_wire;
namespace decision = rypath_enhanced_resampling_decision;
namespace chart = rypath_enhanced_chart;
namespace hybrid = rypath_enhanced_hybrid;
namespace tape = rypath_enhanced_suffix_tape;
struct State {
    ep::State selection;
    ep::DiscoveryState discovery;
    uint requested_slot, next_slot, pixel;
    ep::XYZ requested_xyz;
    float arrival_probability, direct_probability;
    uint rr_cut, failed;
    ep::reuse::Scaled requested_h{.5f, 1};
    hybrid::HybridState hybrid;
    uint suffix_count = 0, suffix_coverage = 0;
};
static_assert(sizeof(State) == 292 && alignof(State) == 4, "Enhanced sidecar ABI");
static_assert(decision::maximum_random_words == ep::er::maximum_random_words,
              "Scaled selector entropy budget");
static_assert(ep::null_slot == cw::enhanced_null_slot &&
                  ep::law_version == cw::enhanced_discovery_law,
              "Enhanced law/tag agreement");
inline bool pending_owned(const thread ew::Evaluation &e, uint pixels, uint pixel, bool fresh) {
    const bool same = e.source.bank == e.result.bank && e.source.pixel == e.result.pixel;
    return e.ready == 0 && e.normalizer_exponent == 0 && as_type<uint>(e.target_h_fraction) == 0 &&
           e.target_h_exponent == 0 && ew::valid(e.result, pixels) &&
           (fresh
                ? ew::absent(e.source) && e.result.bank == ew::fresh_bank && e.result.pixel == pixel
                : ew::valid(e.source, pixels) &&
                      (same || (e.result.bank == ew::fresh_bank && e.result.pixel == pixel)));
}
inline bool suffix_capture_owned(const thread ew::Evaluation &e, uint pixels, uint pixel,
                                 bool fresh) {
    return pending_owned(e, pixels, pixel, fresh) &&
           (e.suffix_context_low || e.suffix_context_high) && e.result.bank == ew::fresh_bank &&
           e.result.pixel == pixel &&
           (fresh || e.source.bank != e.result.bank || e.source.pixel != e.result.pixel);
}
inline bool unstarted_hybrid(const thread hybrid::HybridState &h) {
    if (!chart::canonical_generic(h.active_record) || !ep::unit_density(h.active_h) ||
        !ep::unit_density(h.active_generation_h) || h.first.next != 2 ||
        h.first.selected != hybrid::fp::no_pivot || h.first.status != hybrid::fp::Status::ok ||
        !ep::er::zero(h.primary_distance_squared) || as_type<uint>(h.primary_cosine) ||
        h.primary_ready || h.predecessor.path_id != ew::invalid ||
        as_type<uint>(h.predecessor.normal_offset) || h.predecessor_valid || h.outside ||
        !ep::er::zero(h.certificate.edge_distance_squared) ||
        as_type<uint>(h.certificate.cosine_at_predecessor) || h.certificate.base_admissible ||
        h.certificate.query_mask)
        return false;
    for (uint k = 0; k < 4; ++k)
        if (as_type<uint>(h.predecessor.beta[k]))
            return false;
    for (uint k = 0; k < 3; ++k)
        if (as_type<uint>(h.predecessor.wo[k]))
            return false;
    return true;
}
inline State begin(const thread cw::Evaluation &request, device const wf::Config &config,
                   uint pixel, const thread ew::Evaluation &owned) {
    State state{};
    const auto descriptor =
        ep::cp::make_descriptor(ep::cp::DepthMode::finite, config.contribution_depth);
    state.selection = ep::begin(descriptor.value, config.contribution_target_scale);
    state.requested_slot = request.requested_slot;
    state.pixel = pixel;
    state.arrival_probability = state.direct_probability = 1;
    state.failed =
        uint(!pending_owned(owned, config.pixel_count, pixel,
                            request.requested_slot == ep::fresh_slot) ||
             descriptor.status != ep::Status::ok || state.selection.status != ep::Status::ok ||
             request.ready != 0 || request.selected_slot != ep::fresh_slot ||
             as_type<uint>(request.normalizer) != 0 ||
             (request.requested_slot != ep::fresh_slot && request.requested_slot != ep::null_slot &&
              request.requested_slot >= descriptor.value.slot_count));
    return state;
}
struct Sink {
    thread State &state;
    device const rw::Point *cie;
    constant rw::Parameters &parameters;
    device const wf::Config &config;
    uint pixel;
    device ew::Record *records;
    device ew::Evaluation *evaluations;
    device tape::Entry *suffixes = nullptr;
    device wf::Counters *counters = nullptr;
    ew::Record atom_record = ew::empty_record();
    ep::reuse::Scaled atom_h{.5f, 1}, atom_generation_h{.5f, 1};

    thread Sink &cache_sink() thread { return *this; }
    uint suffix_capture_index() thread {
        const auto receipt = evaluations[pixel];
        if (!suffix_capture_owned(receipt, config.pixel_count, pixel, fresh()))
            return ew::invalid;
        const ulong index = ulong(receipt.result.bank) * config.pixel_count + receipt.result.pixel;
        if (suffixes == nullptr || index >= ew::invalid) {
            state.failed = 1;
            return ew::invalid;
        }
        return uint(index);
    }
    void invalidate_suffix() thread {
        const uint owner = suffix_capture_index();
        if (owner != ew::invalid)
            suffixes[owner].version = 0;
    }
    bool stage_suffix(float4 numerator, float4 likelihood, uint prepared, bool supported) thread {
        if (!has_interior())
            return false;
        const uint owner = suffix_capture_index();
        if (owner == ew::invalid || state.suffix_count >= tape::maximum_operations || !supported)
            return false;
        if (!(prepared & 1u) || (prepared & ~tape::all_lanes)) {
            state.failed = 1;
            return false;
        }
        // Append beyond every previously selected atom's consumed prefix.
        // The current atom still uses suffix_count BEFORE this continuation.
        device auto &operation = suffixes[owner].operations[state.suffix_count];
        for (uint k = 0; k < 4; ++k) {
            operation.numerator[k] = numerator[k];
            operation.sampled_likelihood[k] = likelihood[k];
        }
        return true;
    }
    void advance_suffix(bool staged, uint prepared) thread {
        if (!has_interior() || suffix_capture_index() == ew::invalid)
            return;
        if (!staged || state.suffix_count >= tape::maximum_operations) {
            state.suffix_count = tape::maximum_operations + 1u;
            return; // Older selected cache entries remain valid.
        }
        const uint shift = tape::operation_prepared_shift(state.suffix_count);
        state.suffix_coverage |= (prepared | (prepared << 4u)) << shift;
        ++state.suffix_count;
    }
    bool has_interior() const thread {
        return state.hybrid.active_record.tag == chart::interior_zomega;
    }
    bool activate_hybrid(const thread ew::Record &record, ep::reuse::Scaled h_eval,
                         ep::reuse::Scaled h_gen) thread {
        if (state.failed || state.hybrid.outside || has_interior() ||
            record.tag != chart::interior_zomega ||
            record.event_ordinal >= config.contribution_depth ||
            !chart::valid_chart(record, 2u * (record.event_ordinal + 1u),
                                config.contribution_depth) ||
            !ep::valid_density(h_eval) || !ep::valid_density(h_gen) ||
            !hybrid::fp::valid_first(state.hybrid.first) ||
            state.hybrid.first.selected != record.event_ordinal + 1u) {
            state.failed = 1;
            return false;
        }
        state.hybrid.active_record = record;
        state.hybrid.active_h = h_eval;
        state.hybrid.active_generation_h = h_gen;
        state.suffix_count = state.suffix_coverage = 0;
        return true;
    }
    bool mapping_import(uint event) const thread {
        return !has_interior() && !fresh() && state.requested_slot == 2u * event + 1u;
    }
    void reset_atom() thread {
        atom_record = has_interior() ? state.hybrid.active_record : ew::empty_record();
        atom_h = has_interior() ? state.hybrid.active_h : ep::reuse::Scaled{.5f, 1};
        atom_generation_h =
            has_interior() ? state.hybrid.active_generation_h : ep::reuse::Scaled{.5f, 1};
    }
    void set_atom(const thread ew::Record &record, ep::reuse::Scaled h,
                  ep::reuse::Scaled generation_h) thread {
        if (has_interior()) {
            state.failed = 1; // Terminal-light coordinates cannot replace an active interior chart.
            return;
        }
        atom_record = record;
        atom_h = h;
        atom_generation_h = generation_h;
    }
    ew::Record imported_record() thread {
        if (fresh() || state.requested_slot == ep::null_slot)
            return ew::empty_record();
        const auto receipt = evaluations[pixel];
        if (!pending_owned(receipt, config.pixel_count, pixel, false)) {
            state.failed = 1;
            return ew::empty_record();
        }
        const auto record =
            records[ulong(receipt.source.bank) * config.pixel_count + receipt.source.pixel];
        if (record.tag == chart::terminal_surface ||
            !chart::valid_chart(record, state.requested_slot, config.contribution_depth)) {
            state.failed = 1;
            return ew::empty_record();
        }
        return record;
    }
    ew::Record support_record() thread { return imported_record(); }
    bool fresh() const thread { return state.requested_slot == ep::fresh_slot; }
    bool selected_prefix() const thread {
        return !fresh() && config.contribution_replay == cw::prefix_replay;
    }
    bool wanted(uint slot) const thread {
        return !selected_prefix() || slot == state.requested_slot;
    }
    bool stop_after(uint depth) const thread {
        return selected_prefix() && depth >= state.requested_slot / 2u;
    }
    void begin_event() thread {
        reset_atom();
        state.arrival_probability = state.direct_probability = state.discovery.probability;
    }
    bool continue_discovery(float4 beta, const thread rypath_bdpt::HeroTrack &track,
                            uint depth) thread {
        if (!fresh() || depth <= 1)
            return true;
        if (state.rr_cut || depth > 65 || state.failed) {
            state.failed = 1;
            return false;
        }
        float desired = 1;
        if (track.roulette_enabled) {
            float maximum = 0;
            bool eligible = true;
            for (uint k = 0; k < 4; ++k) {
                const float value = beta[k] * track.eta_scale[k];
                if (!isfinite(value) || value < 0 ||
                    (beta[k] > 0 && !rypath_packet_roulette::positive_normal(value))) {
                    eligible = false;
                    break;
                }
                maximum = max(maximum, value);
            }
            if (eligible) {
                const rypath_packet_roulette::EtaScale unit{};
                const auto decision = rypath_packet_roulette::prepare(float4(maximum), unit, depth);
                if (decision.eligible)
                    desired = 1 - decision.termination;
            }
        }
        const auto gate = ep::prepare_gate(state.discovery, desired);
        const uint word =
            gate.threshold == (1u << 24)
                ? 0u
                : rypath_enhanced_decision::word({pixel, config.contribution_serial_low,
                                                  config.contribution_serial_high,
                                                  parameters.seed_low, parameters.seed_high},
                                                 depth - 2u);
        const auto result = ep::commit_gate(state.discovery, gate, word);
        if (result.status != ep::Status::ok) {
            state.failed = 1;
            return false;
        }
        state.direct_probability = result.probability;
        state.rr_cut = uint(!result.survived);
        // Canonical physical beta is NEVER divided by this discovery q.
        return result.survived;
    }
    static bool same_record(const thread ew::Record &a, const thread ew::Record &b) {
        if (a.version != b.version || a.tag != b.tag || a.light_id != b.light_id ||
            a.event_ordinal != b.event_ordinal || a.instance != b.instance ||
            a.primitive != b.primitive || a.kind != b.kind || a.reserved != b.reserved)
            return false;
        for (uint k = 0; k < 4; ++k)
            if (a.coordinate_bits[k] != b.coordinate_bits[k])
                return false;
        return true;
    }
    template <typename Wavelengths>
    bool project_interior(float4 term, const thread Wavelengths &lambda, ep::reuse::Scaled h,
                          thread float3 &xyz) thread {
        if (!ep::valid_density(h))
            return false;
        float sums[3]{0, 0, 0};
        for (uint lane = 0; lane < 4; ++lane) {
            const float wavelength = lambda.lambda[lane], pdf = lambda.pdf[lane];
            const uint term_bits = as_type<uint>(term[lane]);
            if (!(wavelength >= 360 && wavelength <= 830) || !chart::finite(pdf) ||
                (as_type<uint>(pdf) & 0x80000000u) || (as_type<uint>(pdf) & 0x7fffffffu) == 0 ||
                ((term_bits & 0x80000000u) && (term_bits & 0x7fffffffu)))
                return false;
            const float3 response = rypath_frame_sensor::cie(cie, wavelength, false);
            if (!all(isfinite(response)))
                return false;
            for (uint channel = 0; channel < 3; ++channel) {
                const uint response_bits = as_type<uint>(response[channel]);
                const uint sign = (response_bits ^ term_bits) & 0x80000000u;
                float value = ep::reuse::from_bits(sign);
                if ((term_bits & 0x7fffffffu) && (response_bits & 0x7fffffffu)) {
                    const auto t = ep::reuse::decompose_positive(term[lane]);
                    const auto c = ep::reuse::decompose_positive(
                        ep::reuse::from_bits(response_bits & 0x7fffffffu));
                    auto projected = ep::reuse::quotient(ep::reuse::product(t, c),
                                                         ep::reuse::decompose_positive(pdf));
                    projected.exponent -= 2;
                    projected = ep::reuse::product(projected, h);
                    // v4 defines F from represented incoming C and the owned
                    // chart scale BEFORE the sole final FLOAT conversion. No
                    // materialize-then-undo product, clamp or positive floor.
                    const auto materialized = ep::reuse::materialize_contribution(projected);
                    if (materialized.status != ep::math::Status::ok)
                        return false;
                    value = ep::reuse::from_bits(as_type<uint>(materialized.value) | sign);
                }
                const auto sum = ep::reuse::signed_sum(sums[channel], value);
                if (sum.status != ep::math::Status::ok)
                    return false;
                sums[channel] = sum.value;
            }
        }
        xyz = {sums[0], sums[1], sums[2]};
        return true;
    }
    template <typename Wavelengths>
    bool add(uint index, float4 term, const thread Wavelengths &lambda) thread {
        if (state.failed)
            return false;
        if (index < state.next_slot || index >= state.selection.descriptor.slot_count ||
            !all(isfinite(term)) || any(term < 0)) {
            state.failed = 1;
            return false;
        }
        bool selected = false;
        const bool direct = (index & 1u) != 0;
        const bool interior = has_interior();
        const auto record = interior ? state.hybrid.active_record
                            : direct ? atom_record
                                     : ew::empty_record();
        const auto h = interior ? state.hybrid.active_h
                       : direct ? atom_h
                                : ep::reuse::Scaled{.5f, 1};
        const auto generation_h = interior ? state.hybrid.active_generation_h
                                  : direct ? atom_generation_h
                                           : ep::reuse::Scaled{.5f, 1};
        if (record.tag == chart::terminal_surface ||
            !chart::valid_chart(record, index, config.contribution_depth) ||
            !ep::valid_density(h) || !ep::valid_density(generation_h) || state.hybrid.outside > 1 ||
            (fresh() && state.hybrid.outside)) {
            state.failed = 1;
            return false;
        }
        if (!fresh() && index == state.requested_slot) {
            const auto requested = imported_record();
            if (state.failed)
                return false;
            const bool requested_interior = requested.tag == chart::interior_zomega;
            if (state.hybrid.outside || requested_interior != interior ||
                (requested_interior && !same_record(requested, state.hybrid.active_record))) {
                state.requested_xyz = {};
                state.requested_h = {.5f, 1};
                state.next_slot = index + 1u;
                return false; // Partial chart map: absence is zero, never chart substitution.
            }
        }
        float3 xyz = 0;
        if (interior) {
            if (!project_interior(term, lambda, h, xyz)) {
                state.failed = 1;
                return false;
            }
        } else {
            for (uint lane = 0; lane < 4; ++lane) {
                const float wavelength = lambda.lambda[lane], pdf = lambda.pdf[lane];
                if (!(wavelength >= 360 && wavelength <= 830 && isfinite(pdf) && pdf > 0)) {
                    state.failed = 1;
                    return false;
                }
                xyz += rypath_frame_sensor::cie(cie, wavelength, false) * (term[lane] / pdf / 4.f);
            }
            if (!all(isfinite(xyz))) {
                state.failed = 1;
                return false;
            }
        }
        if (fresh()) {
            const float probability =
                (index & 1u) ? state.direct_probability : state.arrival_probability;
            auto pending = ep::prepare_update(state.selection, index, {xyz.x, xyz.y, xyz.z},
                                              probability, h, generation_h);
            for (uint ordinal = 0;
                 pending.status == ep::Status::ok && !pending.decision.decision.complete &&
                 ordinal < ep::er::maximum_random_words;
                 ++ordinal) {
                const auto word = decision::word({pixel, config.contribution_serial_low,
                                                  config.contribution_serial_high,
                                                  parameters.seed_low, parameters.seed_high},
                                                 decision::fresh_site(index), ordinal);
                if (word.status != decision::Status::ok) {
                    state.failed = 1;
                    return false;
                }
                ep::feed_random_word(pending, word.value);
            }
            const auto updated = ep::commit_update(state.selection, pending);
            if (updated.status != ep::Status::ok) {
                state.failed = 1;
                return false;
            }
            if (updated.selected) {
                selected = true;
                const auto receipt = evaluations[pixel];
                if (!pending_owned(receipt, config.pixel_count, pixel, true)) {
                    state.failed = 1;
                    return false;
                }
                records[ulong(receipt.result.bank) * config.pixel_count + receipt.result.pixel] =
                    record;
            }
        } else if (index == state.requested_slot) {
            selected = suffix_capture_index() != ew::invalid;
            state.requested_xyz = {xyz.x, xyz.y, xyz.z};
            state.requested_h = h;
        }
        state.next_slot = index + 1u;
        return selected && !state.failed;
    }
    bool finish(const thread cw::Evaluation &request, thread float3 &xyz,
                thread cw::Evaluation &result) thread {
        if (state.failed || state.hybrid.outside > 1 || (fresh() && state.hybrid.outside) ||
            state.pixel != pixel || request.ready != 0 ||
            request.requested_slot != state.requested_slot ||
            request.selected_slot != ep::fresh_slot || as_type<uint>(request.normalizer) != 0 ||
            state.next_slot > state.selection.descriptor.slot_count)
            return false;
        auto receipt = evaluations[pixel];
        if (!pending_owned(receipt, config.pixel_count, pixel, fresh()))
            return false;
        result = {state.requested_slot, state.requested_slot, 0.f, 1};
        auto value = state.requested_xyz;
        auto h = state.requested_h;
        if (fresh()) {
            const auto stop = state.rr_cut ? ep::StopReason::rr_cut
                              : state.next_slot == state.selection.descriptor.slot_count
                                  ? ep::StopReason::depth_end
                                  : ep::StopReason::physical_end;
            const auto selected = ep::finalize(state.selection, stop);
            if (selected.status != ep::Status::ok)
                return false;
            result.selected_slot = selected.selected_slot;
            result.normalizer = selected.normalizer.fraction;
            receipt.normalizer_exponent = selected.normalizer.exponent;
            value = selected.xyz;
            h = selected.selected_h;
            if (selected.selected_slot == ep::null_slot) {
                invalidate_suffix();
                records[ulong(receipt.result.bank) * config.pixel_count + receipt.result.pixel] =
                    ew::empty_record();
            } else {
                const auto record =
                    records[ulong(receipt.result.bank) * config.pixel_count + receipt.result.pixel];
                if (record.tag == chart::terminal_surface ||
                    !chart::valid_chart(record, selected.selected_slot, config.contribution_depth))
                    return false;
            }
        } else if (state.requested_slot != ep::null_slot) {
            const auto record = imported_record();
            if (state.failed)
                return false;
            if (receipt.result.bank != receipt.source.bank ||
                receipt.result.pixel != receipt.source.pixel)
                records[ulong(receipt.result.bank) * config.pixel_count + receipt.result.pixel] =
                    record;
        }
        xyz = float3(value.x, value.y, value.z);
        if (!all(isfinite(xyz)) || !ep::valid_density(h))
            return false;
        receipt.target_h_fraction = h.fraction;
        receipt.target_h_exponent = h.exponent;
        receipt.ready = 1;
        evaluations[pixel] = receipt;
        return true;
    }
};
} // namespace rypath_enhanced_runtime
