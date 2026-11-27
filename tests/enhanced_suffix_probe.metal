// SPDX-License-Identifier: GPL-3.0-only
#include <metal_stdlib>
using namespace metal;
#include "../src/enhanced_suffix_tape_shared.h"

// Portable arithmetic only: no wavefront, Resources, AS, BSDF or light query.
namespace enhanced_suffix_probe {
namespace tape = rypath_enhanced_suffix_tape;
inline void store(device uint *out, const thread tape::Result &value) {
    out[0] = uint(value.status);
    out[1] = value.operations_applied;
    out[2] = value.early_zero;
    for (uint k = 0; k < 4; ++k) {
        out[4 + k] = as_type<uint>(value.value[k]);
        out[8 + k] = as_type<uint>(value.beta[k]);
        out[12 + 2 * k] = as_type<uint>(value.density.lane[k].fraction);
        out[13 + 2 * k] = as_type<uint>(value.density.lane[k].exponent);
        out[20 + 2 * k] = as_type<uint>(value.mis_density.lane[k].fraction);
        out[21 + 2 * k] = as_type<uint>(value.mis_density.lane[k].exponent);
    }
}
} // namespace enhanced_suffix_probe
kernel void rypath_enhanced_suffix_math(device const rypath_enhanced_suffix_tape::Entry *entries
                                        [[buffer(0)]],
                                        device const uint *queries [[buffer(1)]],
                                        device uint *output [[buffer(2)]],
                                        constant uint &count [[buffer(3)]],
                                        uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    namespace tape = rypath_enhanced_suffix_tape;
    const auto entry = entries[index];
    const auto q = queries + 40u * index;
    auto out = output + 112u * index;
    for (uint k = 0; k < 112; ++k)
        out[k] = 0;
    tape::Input input{};
    for (uint k = 0; k < 4; ++k) {
        input.beta[k] = as_type<float>(q[k]);
        input.density.lane[k] = {as_type<float>(q[4 + 2 * k]), as_type<int>(q[5 + 2 * k])};
        input.pre_pivot_density.lane[k] = {as_type<float>(q[12 + 2 * k]),
                                           as_type<int>(q[13 + 2 * k])};
        input.immediate_bsdf_pdf[k] = as_type<float>(q[20 + k]);
    }
    input.immediate_pdf_ready = q[24];
    input.immediate_pdf_coverage = q[25];
    input.selected_slot = q[26];
    input.pivot_event = q[27];
    input.context_low = q[28];
    input.context_high = q[29];
    const auto ready = tape::readout(entry, input);
    input.immediate_pdf_ready = 0;
    const auto pending = tape::readout(entry, input);
    auto resumed = pending;
    if (pending.status == tape::Status::needs_immediate_pdf) {
        resumed = tape::complete_immediate(entry, pending, input.immediate_bsdf_pdf, q[24], q[25]);
        out[2] = 1;
    }
    out[0] = 0x53554631u;
    out[1] = sizeof(tape::Entry);
    enhanced_suffix_probe::store(out + 4, ready);
    enhanced_suffix_probe::store(out + 36, pending);
    enhanced_suffix_probe::store(out + 68, resumed);
}
