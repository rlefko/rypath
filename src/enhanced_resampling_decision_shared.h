// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"

// Enhanced-only law 1. Scaled selection can consume 257 words; legacy 16-word
// contribution/Area site strides and 3-block MH acceptance are not extended.
// All legacy generators and the independent discovery stream remain unchanged.
namespace rypath_enhanced_resampling_decision {
using Word = rypath_replay::Word;
using Counter = rypath_replay::Words4;
using Key = rypath_replay::Words2;
enum : Word {
    law_version = 1,
    maximum_random_words = 257,
    maximum_ordinal = maximum_random_words - 1,
    word_stride = 512,
    maximum_slot = 130,
    maximum_phase = 5,
    maximum_candidate = 4,
    maximum_step = 3,
    fresh_key_domain = 0x65667231u,
    area_key_domain = 0x65617231u,
    acceptance_key_domain = 0x656d7231u,
    high_key_domain = 0x73777331u
};
enum class Role : Word { fresh_local, area_selection, mutation_acceptance };
enum class Status : Word { ok, invalid_role, invalid_site, invalid_ordinal };
struct Identity {
    Word pixel, serial_low, serial_high, seed_low, seed_high;
};
struct Site {
    Role role;
    // fresh_local: slot; area_selection: phase; mutation_acceptance: step.
    Word index;
    // Only area_selection uses candidate. Other roles require the zero sentinel.
    Word candidate;
};
struct Address {
    Counter counter{0, 0, 0, 0};
    Key key{0, 0};
    Status status = Status::ok;
};
struct WordResult {
    Word value = 0;
    Status status = Status::ok;
};
inline Site fresh_site(Word slot) { return {Role::fresh_local, slot, 0}; }
inline Site area_site(Word phase, Word candidate) {
    return {Role::area_selection, phase, candidate};
}
inline Site mutation_site(Word step) { return {Role::mutation_acceptance, step, 0}; }
inline bool valid_role(Role role) {
    return role == Role::fresh_local || role == Role::area_selection ||
           role == Role::mutation_acceptance;
}
inline bool valid_site(Site site) {
    switch (site.role) {
    case Role::fresh_local:
        return site.index <= maximum_slot && site.candidate == 0;
    case Role::area_selection:
        // Exactly 8 categorical sites. The 13 valid Area configuration pairs are
        // not all entropy consumers: spatial_begin and mutation use no Area WRS.
        return (site.index == 0 && site.candidate == 0) ||
               (site.index == 1 && site.candidate == 1) ||
               (site.index == 2 && site.candidate >= 1 && site.candidate <= 4) ||
               (site.index == 4 && site.candidate == 0) || (site.index == 5 && site.candidate == 0);
    case Role::mutation_acceptance:
        return site.index <= maximum_step && site.candidate == 0;
    }
    return false;
}
inline bool valid_address(Site site, Word ordinal) {
    return valid_site(site) && ordinal <= maximum_ordinal;
}
inline Address address(Identity identity, Site site, Word ordinal) {
    if (!valid_role(site.role))
        return {{0, 0, 0, 0}, {0, 0}, Status::invalid_role};
    if (!valid_site(site))
        return {{0, 0, 0, 0}, {0, 0}, Status::invalid_site};
    if (ordinal > maximum_ordinal)
        return {{0, 0, 0, 0}, {0, 0}, Status::invalid_ordinal};
    const Word local_site =
        site.role == Role::area_selection ? site.index * 5u + site.candidate : site.index;
    const Word domain = site.role == Role::fresh_local      ? fresh_key_domain
                        : site.role == Role::area_selection ? area_key_domain
                                                            : acceptance_key_domain;
    return {{identity.pixel, identity.serial_low, identity.serial_high,
             local_site * word_stride + ordinal},
            {identity.seed_low ^ domain, identity.seed_high ^ high_key_domain},
            Status::ok};
}
inline WordResult word(Identity identity, Site site, Word ordinal) {
    const auto value = address(identity, site, ordinal);
    if (value.status != Status::ok)
        return {0, value.status};
    return {rypath_replay::philox(value.counter, value.key).x, Status::ok};
}
// Injectivity is of the complete(key,counter), for a fixed run seed. Roles use
// distinct XOR key masks, while each role's site*512+ordinal is injective over
// the checked bounds. Counter.w<=130*512+256=66816, so multiplication cannot
// overflow. Full pixel and 64-bit serial occupy separate unmodified words.
// A 32-bit projected output may collide; pseudorandom independence is the usual
// Philox modeling assumption, not a promise of collision-free output values.
// No payload, queue slot, user sample label or selected ancestry enters Identity.
// Across DIFFERENT seeds, domain-XOR keys need not be globally disjoint.
static_assert(sizeof(Identity) == 20 && alignof(Identity) == 4 && sizeof(Counter) == 16 &&
                  sizeof(Key) == 8 && maximum_random_words <= word_stride &&
                  maximum_slot * word_stride + maximum_ordinal == 66816,
              "Enhanced resampling entropy identity/address bounds");
} // namespace rypath_enhanced_resampling_decision
