// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/contribution_wire.h"
#include "../src/enhanced_decision_shared.h"
#include "../src/enhanced_path_shared.h"
#include "../src/enhanced_resampling_decision_shared.h"
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
struct State {
    ep::State selection;
    ep::DiscoveryState discovery;
    uint requested_slot, next_slot, pixel;
    ep::XYZ requested_xyz;
    float arrival_probability, direct_probability;
    uint rr_cut, failed;
    ep::reuse::Scaled requested_h{.5f, 1};
};
static_assert(sizeof(State) == 128 && alignof(State) == 4, "Enhanced sidecar ABI");
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
    ew::Record atom_record = ew::empty_record();
    ep::reuse::Scaled atom_h{.5f, 1}, atom_generation_h{.5f, 1};

    bool mapping_import(uint event) const thread {
        return !fresh() && state.requested_slot == 2u * event + 1u;
    }
    void reset_atom() thread {
        atom_record = ew::empty_record();
        atom_h = atom_generation_h = {.5f, 1};
    }
    void set_atom(const thread ew::Record &record, ep::reuse::Scaled h,
                  ep::reuse::Scaled generation_h) thread {
        atom_record = record;
        atom_h = h;
        atom_generation_h = generation_h;
    }
    ew::Record imported_record() thread {
        if (fresh())
            return ew::empty_record();
        const auto receipt = evaluations[pixel];
        if (!pending_owned(receipt, config.pixel_count, pixel, false)) {
            state.failed = 1;
            return ew::empty_record();
        }
        const auto record =
            records[ulong(receipt.source.bank) * config.pixel_count + receipt.source.pixel];
        if (!rypath_bdpt::enhanced_light::valid_record(record, state.requested_slot / 2u) ||
            ((state.requested_slot & 1u) == 0 && record.tag != ew::generic)) {
            state.failed = 1;
            return ew::empty_record();
        }
        return record;
    }
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
    template <typename Wavelengths>
    void add(uint index, float4 term, const thread Wavelengths &lambda) thread {
        if (state.failed)
            return;
        if (index < state.next_slot || index >= state.selection.descriptor.slot_count ||
            !all(isfinite(term)) || any(term < 0)) {
            state.failed = 1;
            return;
        }
        float3 xyz = 0;
        for (uint lane = 0; lane < 4; ++lane) {
            const float wavelength = lambda.lambda[lane], pdf = lambda.pdf[lane];
            if (!(wavelength >= 360 && wavelength <= 830 && isfinite(pdf) && pdf > 0)) {
                state.failed = 1;
                return;
            }
            xyz += rypath_frame_sensor::cie(cie, wavelength, false) * (term[lane] / pdf / 4.f);
        }
        if (!all(isfinite(xyz))) {
            state.failed = 1;
            return;
        }
        const bool direct = (index & 1u) != 0;
        const auto h = direct ? atom_h : ep::reuse::Scaled{.5f, 1};
        const auto generation_h = direct ? atom_generation_h : ep::reuse::Scaled{.5f, 1};
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
                    return;
                }
                ep::feed_random_word(pending, word.value);
            }
            const auto updated = ep::commit_update(state.selection, pending);
            if (updated.status != ep::Status::ok) {
                state.failed = 1;
                return;
            }
            if (updated.selected) {
                const auto receipt = evaluations[pixel];
                if (!pending_owned(receipt, config.pixel_count, pixel, true)) {
                    state.failed = 1;
                    return;
                }
                records[ulong(receipt.result.bank) * config.pixel_count + receipt.result.pixel] =
                    direct ? atom_record : ew::empty_record();
            }
        } else if (index == state.requested_slot) {
            state.requested_xyz = {xyz.x, xyz.y, xyz.z};
            state.requested_h = h;
        }
        state.next_slot = index + 1u;
    }
    bool finish(const thread cw::Evaluation &request, thread float3 &xyz,
                thread cw::Evaluation &result) thread {
        if (state.failed || state.pixel != pixel || request.ready != 0 ||
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
            if (selected.selected_slot == ep::null_slot)
                records[ulong(receipt.result.bank) * config.pixel_count + receipt.result.pixel] =
                    ew::empty_record();
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
