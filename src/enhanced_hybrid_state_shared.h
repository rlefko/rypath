// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "enhanced_chart_shared.h"
#include "enhanced_footprint_shared.h"

// Scalar-only persistent hybrid preparation. This owns no scene pointers,
// vertices or RNG state. Chart activation is separate from the WRS winner.
namespace rypath_enhanced_hybrid {
namespace ew = rypath::enhanced_wire;
namespace fp = rypath_enhanced_footprint;
using Word = ew::Word;
using Number = rypath_reuse_mis::Scaled;
struct PredecessorSnapshot {
    float beta[4]{};
    float wo[3]{};
    Word path_id = ew::invalid;
    float normal_offset = 0;
};
struct PredecessorCertificate {
    Number edge_distance_squared{0, 0};
    float cosine_at_predecessor = 0;
    Word base_admissible = 0;
    // Frozen payload may contain these additionally prepared lanes while the
    // actual predecessor keeps its original active mask for physical replay.
    Word query_mask = 0;
};
struct HybridState {
    ew::Record active_record = ew::empty_record();
    Number active_h{.5f, 1}, active_generation_h{.5f, 1};
    fp::FirstEligible first;
    Number primary_distance_squared{0, 0};
    float primary_cosine = 0;
    Word primary_ready = 0;
    PredecessorSnapshot predecessor;
    PredecessorCertificate certificate;
    Word predecessor_valid = 0, outside = 0;
};
static_assert(sizeof(PredecessorSnapshot) == 36 && sizeof(PredecessorCertificate) == 20 &&
                  sizeof(HybridState) == 156 && alignof(HybridState) == 4,
              "Hybrid chart preparation CPU/Metal scalar layout");
} // namespace rypath_enhanced_hybrid
