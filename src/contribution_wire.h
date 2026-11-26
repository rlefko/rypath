// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"

namespace rypath {
namespace contribution_wire {
using Word = render_wire::Word;
enum Mode : Word { summed = 0, contribution_path = 1 };
// A request/result for one full replay. no_selection requests fresh local WRS;
// otherwise requested_slot fixes j throughout destination replay or mutation.
// Prepare writes {request,no_selection,0,0}; transport publishes ready=1 only
// after writing selected F_j to leaf_xyz. Fresh normalizer is sum_j target_j;
// imported evaluations retain normalizer=+0, never a newly resampled local sum.
// Consumers clear ready after successful use. A new prepare must reset the
// complete record, so a consumed result cannot masquerade as a pending request.
struct Evaluation {
    Word requested_slot, selected_slot;
    float normalizer;
    Word ready;
};
static_assert(sizeof(Evaluation) == 16 && __builtin_offsetof(Evaluation, normalizer) == 8 &&
                  __builtin_offsetof(Evaluation, ready) == 12,
              "Contribution evaluation host/shader ABI");
} // namespace contribution_wire
} // namespace rypath
