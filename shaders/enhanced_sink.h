// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/contribution_decision_shared.h"
#include "../src/contribution_wire.h"
#include "../src/enhanced_decision_shared.h"
#include "../src/enhanced_path_shared.h"
#include "../src/wavefront_wire.h"
#include "frame_sensor.h"

namespace rypath_enhanced_runtime {
namespace ep = rypath_enhanced_path;
namespace cw = rypath::contribution_wire;
namespace rw = rypath::render_wire;
namespace wf = rypath::wavefront_wire;
struct State {
    ep::State selection;
    ep::DiscoveryState discovery;
    uint requested_slot, next_slot, pixel;
    ep::XYZ requested_xyz;
    float arrival_probability, direct_probability;
    uint rr_cut, failed;
};
static_assert(sizeof(State) == 104 && alignof(State) == 4, "Enhanced sidecar ABI");
static_assert(ep::null_slot == cw::enhanced_null_slot &&
                  ep::law_version == cw::enhanced_discovery_law,
              "Enhanced law/tag agreement");
inline State begin(const thread cw::Evaluation &request, device const wf::Config &config,
                   uint pixel) {
    State state{};
    const auto descriptor =
        ep::cp::make_descriptor(ep::cp::DepthMode::finite, config.contribution_depth);
    state.selection = ep::begin(descriptor.value, config.contribution_target_scale);
    state.requested_slot = request.requested_slot;
    state.pixel = pixel;
    state.arrival_probability = state.direct_probability = 1;
    state.failed =
        uint(descriptor.status != ep::Status::ok || state.selection.status != ep::Status::ok ||
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
        if (fresh()) {
            const float probability =
                (index & 1u) ? state.direct_probability : state.arrival_probability;
            auto pending =
                ep::prepare_update(state.selection, index, {xyz.x, xyz.y, xyz.z}, probability);
            for (uint ordinal = 0; pending.status == ep::Status::ok && !pending.decision.complete &&
                                   ordinal < ep::math::maximum_random_words;
                 ++ordinal)
                ep::feed_random_word(pending, rypath_contribution_decision::word(
                                                  {pixel, config.contribution_serial_low,
                                                   config.contribution_serial_high,
                                                   parameters.seed_low, parameters.seed_high},
                                                  index, ordinal));
            if (ep::commit_update(state.selection, pending).status != ep::Status::ok) {
                state.failed = 1;
                return;
            }
        } else if (index == state.requested_slot) {
            state.requested_xyz = {xyz.x, xyz.y, xyz.z};
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
        result = {state.requested_slot, state.requested_slot, 0.f, 1};
        auto value = state.requested_xyz;
        if (fresh()) {
            const auto stop = state.rr_cut ? ep::StopReason::rr_cut
                              : state.next_slot == state.selection.descriptor.slot_count
                                  ? ep::StopReason::depth_end
                                  : ep::StopReason::physical_end;
            const auto selected = ep::finalize(state.selection, stop);
            if (selected.status != ep::Status::ok)
                return false;
            result.selected_slot = selected.selected_slot;
            result.normalizer = selected.normalizer;
            value = selected.xyz;
        }
        xyz = float3(value.x, value.y, value.z);
        return all(isfinite(xyz));
    }
};
} // namespace rypath_enhanced_runtime
