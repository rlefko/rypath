// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"
#if defined(__METAL_VERSION__)
#define RYPATH_CONFIDENCE_THREAD thread
#else
#define RYPATH_CONFIDENCE_THREAD
#endif

// Lin et al., ReSTIR PT Enhanced (2026), section2.2, PDF p3/article13:3:
// fresh confidence1; cap the previous temporal INPUT then add fresh1;
// spatial output sums structurally admitted source confidences, including center.
// https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/lin2026restirptenhanced.pdf
// This fixed-cap integer policy excludes section5's sample-dependent biased cap.
namespace rypath_enhanced_confidence {
using Word = rypath_replay::Word;
enum : Word {
    law_version = 1,
    default_temporal_cap = 20,
    maximum_confidence = 1u << 24,
    maximum_spatial_sources = 32
};
enum class Status : Word { ok, invalid_source, invalid_cap, invalid_state, count_overflow };
// Presence means the domain was admitted, NOT that its selected sample has
// positive target/Z. An absent or geometrically rejected donor is canonical0.
// Null draws and pointwise undefined/zero shifts never change this record.
struct Source {
    Word confidence = 0;
    Word present = 0;
};
inline bool valid_source(Source source) {
    return source.present == 0 ? source.confidence == 0
                               : source.present == 1 && source.confidence >= 1 &&
                                     source.confidence <= maximum_confidence;
}
struct Coefficient {
    float value = 0;
    Status status = Status::ok;
};
inline Coefficient coefficient(Source source) {
    if (!valid_source(source))
        return {0, Status::invalid_source};
    // Every accepted integer converts EXACTLY to binary32. Larger confidence is
    // an explicit numerical failure, never a rounded, saturated or wrapped sum.
    return {float(source.confidence), Status::ok};
}
struct Temporal {
    Source fresh, history, output;
    Status status = Status::ok;
};
// fixed_cap is an immutable policy parameter, never derived from a selected
// sample, duplication score, target or W. Adaptive/sample-dependent caps are
// outside this law even though their integer values could fit this argument.
inline Temporal temporal(Source previous, Word fixed_cap = default_temporal_cap) {
    Temporal result;
    if (!valid_source(previous)) {
        result.status = Status::invalid_source;
        return result;
    }
    if (fixed_cap == 0 || fixed_cap >= maximum_confidence) {
        result.status = Status::invalid_cap;
        return result;
    }
    result.fresh = {1, 1};
    if (previous.present) {
        const Word capped = previous.confidence < fixed_cap ? previous.confidence : fixed_cap;
        result.history = {capped, 1};
    }
    result.output = {result.history.confidence + 1, 1};
    return result;
}
struct Spatial {
    Word confidence = 0;
    Word considered = 0;
    Word admitted = 0;
    Status status = Status::ok;
};
inline bool valid_spatial(Spatial state) {
    return state.status == Status::ok && state.considered >= 1 &&
           state.considered <= maximum_spatial_sources && state.admitted >= 1 &&
           state.admitted <= state.considered && state.confidence >= state.admitted &&
           state.confidence <= maximum_confidence;
}
inline Spatial spatial_begin(Source center) {
    if (!valid_source(center) || !center.present)
        return {0, 0, 0, Status::invalid_source};
    return {center.confidence, 1, 1, Status::ok};
}
inline Status add_neighbor(RYPATH_CONFIDENCE_THREAD Spatial &state, Source neighbor) {
    if (state.status != Status::ok)
        return state.status;
    if (!valid_spatial(state))
        state.status = Status::invalid_state;
    else if (!valid_source(neighbor))
        state.status = Status::invalid_source;
    else if (state.considered == maximum_spatial_sources ||
             neighbor.confidence > maximum_confidence - state.confidence)
        state.status = Status::count_overflow;
    else {
        ++state.considered; // Even rejected donors remain scheduling observations.
        if (neighbor.present) {
            state.confidence += neighbor.confidence;
            ++state.admitted;
        }
    }
    return state.status;
}
struct Result {
    Source source;
    Status status = Status::ok;
};
inline Result spatial_finish(Spatial state) {
    if (state.status != Status::ok)
        return {{}, state.status};
    if (!valid_spatial(state))
        return {{}, Status::invalid_state};
    return {{state.confidence, 1}, Status::ok};
}
static_assert(sizeof(Source) == 8 && alignof(Source) == 4 && sizeof(Spatial) == 16 &&
                  sizeof(Temporal) == 28,
              "Strict confidence shared CPU/Metal word layout");
} // namespace rypath_enhanced_confidence
#undef RYPATH_CONFIDENCE_THREAD
