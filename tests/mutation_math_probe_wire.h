// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/mutation_shared.h"
namespace mutation_probe {
using Word = rypath_replay::Word;
struct Query {
    rypath_replay::ReplayState source;
    rypath_replay::Words4 entropy;
    rypath_mutation::MutationIdentity identity;
    rypath_mutation::ProposalScale scale;
    float weight, source_target, proposal_target;
    Word step, word_count, acceptance_ordinal, requested_steps;
    Word words[9];
};
struct Result {
    rypath_mutation::ProposalResult proposal;
    rypath_replay::Words4 counter, proposal_words;
    Word counter_status, proposal_words_status;
    Word acceptance_words[9], acceptance_status[9];
    Word ordinal_value, ordinal_status;
    rypath_reservoir::DyadicProbability ratio;
    Word prepared_status, complete, selected, compared_bits;
    float prepared_weight, final_weight;
    Word final_status, accepted, words_used, valid_step_count;
};
static_assert(sizeof(Query) == 140 && sizeof(Result) == 208, "Mutation math probe CPU/MSL ABI");
} // namespace mutation_probe
