// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"
namespace rypath_enhanced_decision {
using Word = rypath_replay::Word;
using Counter = rypath_replay::Words4;
using Key = rypath_replay::Words2;
enum : Word { law_version = 1, maximum_gate = 63, domain = 0x65647231u };
struct Identity {
    Word pixel, serial_low, serial_high, seed_low, seed_high;
};
inline bool valid_address(Word gate) { return gate <= maximum_gate; }
// No canonical U, physical queue slot, user sample label, WRS word or mutation
// coordinate enters. Distinct Philox key domain from all existing decisions.
inline Counter counter(Identity identity, Word gate) {
    return {identity.pixel, identity.serial_low, identity.serial_high, gate};
}
inline Key key(Identity identity) {
    return {identity.seed_low ^ domain, identity.seed_high ^ 0x6f727231u};
}
// Precondition: valid_address(gate). A q=1 gate need not request this word.
inline Word word(Identity identity, Word gate) {
    return rypath_replay::philox(counter(identity, gate), key(identity)).x;
}
} // namespace rypath_enhanced_decision
