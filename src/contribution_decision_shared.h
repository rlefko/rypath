// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"

namespace rypath_contribution_decision {
using Word = rypath_replay::Word;
using Counter = rypath_replay::Words4;
using Key = rypath_replay::Words2;
enum : Word {
    maximum_depth = 65,
    maximum_slot = 2 * maximum_depth,
    maximum_random_words = 9,
    word_stride = 16
};
// Submission serial, rather than the user sample label, identifies local WRS
// entropy. No replay coordinates, queue/physical slot, or selected payload enter.
struct Identity {
    Word pixel, serial_low, serial_high, seed_low, seed_high;
};
inline bool valid_parameters(Word depth, Word slot, Word ordinal) {
    return depth <= maximum_depth && slot <= 2 * depth && ordinal < maximum_random_words;
}
inline bool valid_address(Word slot, Word ordinal) {
    return valid_parameters(maximum_depth, slot, ordinal);
}
// Preconditions for counter()/word(): valid_address(slot, ordinal). The stride
// retains the exact original shader algorithm; unused ordinals 9..15 stay reserved.
// Addresses are injective over the declared slot/word bounds. The 32-bit Philox
// output projection is not claimed to be collision-free or truly independent.
inline Counter counter(Identity identity, Word slot, Word ordinal) {
    return {identity.pixel, identity.serial_low, identity.serial_high,
            slot * word_stride + ordinal};
}
inline Key key(Identity identity) {
    return {identity.seed_low ^ Word(rypath_replay::DecisionDomain::contribution),
            identity.seed_high ^ 0x63707431u};
}
inline Word word(Identity identity, Word slot, Word ordinal) {
    return rypath_replay::philox(counter(identity, slot, ordinal), key(identity)).x;
}
static_assert(sizeof(Identity) == 20 && alignof(Identity) == 4 && sizeof(Counter) == 16 &&
                  sizeof(Key) == 8 && maximum_random_words <= word_stride,
              "Contribution decision identity/address ABI");
} // namespace rypath_contribution_decision
