// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/contribution_decision_shared.h"
#include "../src/contribution_path_shared.h"
#include "../src/contribution_wire.h"
#include "../src/wavefront_wire.h"
#include "frame_sensor.h"

namespace rypath_contribution_runtime {
namespace cp = rypath_contribution_path;
namespace cw = rypath::contribution_wire;
namespace rw = rypath::render_wire;
namespace wf = rypath::wavefront_wire;
struct State {
    cp::State selection;
    uint requested_slot, next_slot;
    cp::XYZ requested_xyz;
    uint failed, reserved;
};
static_assert(sizeof(cp::State) == 52 && sizeof(State) == 80 && alignof(State) == 4,
              "Contribution sidecar ABI");
inline State begin(const thread cw::Evaluation &request, device const wf::Config &config) {
    State state{};
    const auto descriptor = cp::make_descriptor(cp::DepthMode::finite, config.contribution_depth);
    state.selection = cp::begin(descriptor.value, config.contribution_target_scale);
    state.requested_slot = request.requested_slot;
    state.failed =
        uint(descriptor.status != cp::Status::ok || state.selection.status != cp::Status::ok ||
             request.ready != 0 || request.selected_slot != cp::math::no_selection ||
             as_type<uint>(request.normalizer) != 0 ||
             (request.requested_slot != cp::math::no_selection &&
              request.requested_slot >= descriptor.value.slot_count));
    return state;
}
// Local selection is independent of the replay payload and queue position. The
// fixed slot and rejection-word ordinal have disjoint injective tag ranges for
// D<=65 and maximum_random_words<=16. Submission serial survives repeated user
// sample indices. As with replay generation, independence is pseudorandom.
inline uint selection_word(constant rw::Parameters &p, device const wf::Config &config, uint pixel,
                           uint slot, uint ordinal) {
    return rypath_contribution_decision::word({pixel, config.contribution_serial_low,
                                               config.contribution_serial_high, p.seed_low,
                                               p.seed_high},
                                              slot, ordinal);
}
struct Sink {
    thread State &state;
    device const rw::Point *cie;
    constant rw::Parameters &parameters;
    device const wf::Config &config;
    uint pixel;

    bool selected_prefix() const thread {
        return config.contribution_replay == cw::prefix_replay &&
               state.requested_slot != cp::math::no_selection;
    }
    bool wanted(uint slot) const thread {
        return !selected_prefix() || slot == state.requested_slot;
    }
    bool stop_after(uint depth) const thread {
        return selected_prefix() && depth >= state.requested_slot / 2u;
    }
    void append(cp::XYZ xyz, uint active) thread {
        if (state.failed)
            return;
        if (state.requested_slot == cp::math::no_selection) {
            auto pending = cp::prepare_update(state.selection, state.next_slot, xyz, active);
            for (uint ordinal = 0; pending.status == cp::Status::ok && !pending.decision.complete &&
                                   ordinal < cp::math::maximum_random_words;
                 ++ordinal)
                cp::feed_random_word(
                    pending, selection_word(parameters, config, pixel, state.next_slot, ordinal));
            const auto result = cp::commit_update(state.selection, pending);
            if (result.status != cp::Status::ok) {
                state.failed = 1;
                return;
            }
        } else if (state.next_slot == state.requested_slot) {
            state.requested_xyz = xyz;
        }
        ++state.next_slot;
    }
    void pad_to(uint end) thread {
        if (state.requested_slot != cp::math::no_selection) {
            // The requested value is initialized to zero; an omitted atom stays
            // zero. Imports never draw a new contribution or fresh normalizer.
            state.next_slot = end;
            return;
        }
        while (!state.failed && state.next_slot < end)
            append({}, 0);
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
        // Every atom uses the original four wavelength PDFs, including after a
        // hero collapse. Its contribution already contains hypothesis/MIS weights.
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
        pad_to(index);
        append({xyz.x, xyz.y, xyz.z}, 1);
    }
    bool finish(const thread cw::Evaluation &request, thread float3 &xyz,
                thread cw::Evaluation &result) thread {
        if (state.failed || state.reserved != 0 || request.ready != 0 ||
            request.requested_slot != state.requested_slot ||
            request.selected_slot != cp::math::no_selection ||
            as_type<uint>(request.normalizer) != 0 ||
            state.next_slot > state.selection.descriptor.slot_count)
            return false;
        pad_to(state.selection.descriptor.slot_count);
        if (state.failed)
            return false;
        result = {state.requested_slot, state.requested_slot, 0.f, 1};
        cp::XYZ value = state.requested_xyz;
        if (state.requested_slot == cp::math::no_selection) {
            const auto reservoir = cp::finalize(state.selection);
            if (reservoir.status != cp::Status::ok)
                return false;
            result.selected_slot = reservoir.selected_slot;
            result.normalizer = reservoir.normalizer;
            value = reservoir.xyz;
        }
        xyz = float3(value.x, value.y, value.z);
        return all(isfinite(xyz));
    }
};
static_assert(cp::math::maximum_random_words <= rypath_contribution_decision::maximum_random_words,
              "Contribution entropy tag bound");
} // namespace rypath_contribution_runtime
