// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/reuse_mis_shared.h"
namespace reuse_mis_probe {
using Word = rypath_replay::Word;
struct Query {
    float x, y, z, target_scale, Z, current_target, previous_target, component, normalizer;
    Word word_count, words[9];
};
struct Result {
    rypath_reservoir::ScalarResult target, fresh, history, pair, contribution;
    rypath_reservoir::DyadicProbability ratio;
    float prepared_normalizer, final_normalizer;
    Word prepared_status, final_status, complete, selected, compared_bits, accepted, words_used;
};
static_assert(sizeof(Query) == 76 && sizeof(Result) == 84, "Reuse MIS arithmetic CPU/MSL ABI");
} // namespace reuse_mis_probe
