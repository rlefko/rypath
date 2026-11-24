// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/replay_shared.h"
#include "../src/reservoir_shared.h"
namespace area_math_probe {
using Word = rypath_replay::Word;
#if defined(__METAL_VERSION__)
constant constexpr Word maximum_candidates = 32;
#else
inline constexpr Word maximum_candidates = 32;
#endif
struct Candidate {
    float weight, target;
    Word word_count;
    Word words[9];
};
struct Query {
    rypath_replay::Words4 counter;
    rypath_replay::Words2 key;
    rypath_replay::FreshIdentity identity;
    rypath_replay::TailAddress address;
    rypath_reservoir::State initial;
    Word candidate_count, uniform_word;
    float xyz[3], epsilon, bounded_scale;
    Candidate candidates[maximum_candidates];
};
struct Step {
    rypath_reservoir::DyadicProbability rare;
    Word words_used, compared_bits, complete, selected, status;
    float score_sum;
};
struct Result {
    rypath_replay::Words4 philox;
    rypath_replay::ReplayState fresh;
    rypath_replay::Words4 tail;
    float uniform_value;
    rypath_reservoir::ScalarResult target;
    float bounded_value;
    Word bounded_status;
    rypath_reservoir::State state;
    rypath_reservoir::ScalarResult normalized;
    Step steps[maximum_candidates];
};
static_assert(sizeof(Candidate) == 48 && sizeof(Query) == 1644 && sizeof(Step) == 32 &&
                  sizeof(Result) == 1136,
              "Area math probe host/MSL ABI");
} // namespace area_math_probe
