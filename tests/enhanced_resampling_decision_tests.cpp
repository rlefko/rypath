// SPDX-License-Identifier: GPL-3.0-only
#include "contribution_decision_shared.h"
#include "enhanced_decision_shared.h"
#include "enhanced_resampling_decision_shared.h"
#include "mutation_shared.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
namespace d = rypath_enhanced_resampling_decision;
namespace legacy = rypath_contribution_decision;
namespace discovery = rypath_enhanced_decision;
namespace mutation = rypath_mutation;
using Words = std::array<std::uint32_t, 4>;
using Key = std::array<std::uint32_t, 2>;
using Address = std::array<std::uint32_t, 6>;
void need(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
Address tuple(d::Counter counter, d::Key key) {
    return {key.x, key.y, counter.x, counter.y, counter.z, counter.w};
}
// Independent array-based Philox4x32-10 reference. Published Random123 v1.14.0
// kat_vectors below bind it before it is used as the new routing oracle.
Words reference(Words value, Key key) {
    for (unsigned round = 0; round < 10; ++round) {
        const std::uint64_t a = std::uint64_t(value[0]) * 0xd2511f53u;
        const std::uint64_t b = std::uint64_t(value[2]) * 0xcd9e8d57u;
        value = {std::uint32_t(b >> 32) ^ value[1] ^ key[0], std::uint32_t(b),
                 std::uint32_t(a >> 32) ^ value[3] ^ key[1], std::uint32_t(a)};
        key[0] += 0x9e3779b9u;
        key[1] += 0xbb67ae85u;
    }
    return value;
}
void known_answers() {
    need(reference({0, 0, 0, 0}, {0, 0}) ==
             Words{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u},
         "Independent Philox reference must match the published zero vector");
    need(reference({0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu},
                   {0xffffffffu, 0xffffffffu}) ==
             Words{0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu},
         "Independent Philox reference must match the published maximum vector");
    need(reference({0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u},
                   {0xa4093822u, 0x299f31d0u}) ==
             Words{0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u},
         "Independent Philox reference must match the published pi vector");
    // Cancel the NEW key mask at a legal all-zero counter; this tests the
    // checked word API against a hardcoded answer, not another generator call.
    const d::Identity zero{0, 0, 0, 0x65667231u, 0x73777331u};
    const auto word = d::word(zero, d::fresh_site(0), 0);
    need(word.status == d::Status::ok && word.value == 0x6627e8d5u,
         "Enhanced checked word must produce the pinned all-zero Philox answer");
}
std::vector<d::Site> sites() {
    std::vector<d::Site> result;
    for (unsigned slot = 0; slot <= 130; ++slot)
        result.push_back(d::fresh_site(slot));
    // Independent list of actual categorical Area call sites, not every valid
    // scheduling pair. Neither spatial_begin nor phase3 requests Area entropy.
    constexpr std::array<std::array<unsigned, 2>, 8> area{
        {{0, 0}, {1, 1}, {2, 1}, {2, 2}, {2, 3}, {2, 4}, {4, 0}, {5, 0}}};
    for (const auto &pair : area)
        result.push_back(d::area_site(pair[0], pair[1]));
    for (unsigned step = 0; step < 4; ++step)
        result.push_back(d::mutation_site(step));
    need(result.size() == 143, "Declared entropy domain has exactly 143 sites");
    return result;
}
void exhaustive_addresses() {
    const d::Identity identity{0x92345678u, 0xffffffffu, 0x87654321u, 0x13579bdfu, 0xfedcba98u};
    std::set<Address> seen;
    for (const auto site : sites()) {
        need(d::valid_site(site), "Every independently enumerated site must validate");
        const unsigned code =
            site.role == d::Role::area_selection ? site.index * 5 + site.candidate : site.index;
        const unsigned mask = site.role == d::Role::fresh_local      ? 0x65667231u
                              : site.role == d::Role::area_selection ? 0x65617231u
                                                                     : 0x656d7231u;
        for (unsigned ordinal = 0; ordinal <= 256; ++ordinal) {
            const auto address = d::address(identity, site, ordinal);
            need(address.status == d::Status::ok && address.counter.x == identity.pixel &&
                     address.counter.y == identity.serial_low &&
                     address.counter.z == identity.serial_high &&
                     address.counter.w == code * 512 + ordinal &&
                     address.key.x == (identity.seed_low ^ mask) &&
                     address.key.y == (identity.seed_high ^ 0x73777331u),
                 "Every address must retain full identity and its declared site/role domain");
            need(seen.insert(tuple(address.counter, address.key)).second,
                 "No two role/site/word decisions may reuse a Philox key/counter");
            const auto expected = reference(
                {identity.pixel, identity.serial_low, identity.serial_high, code * 512 + ordinal},
                {identity.seed_low ^ mask, identity.seed_high ^ 0x73777331u});
            const auto word = d::word(identity, site, ordinal);
            need(word.status == d::Status::ok && word.value == expected[0],
                 "Every checked word must match the independent known-answer-bound reference");
        }
    }
    need(seen.size() == 143 * 257, "Exhaustive 143x257 address count");
    const auto last = d::address(identity, d::fresh_site(130), 256);
    need(last.counter.w == 66816, "Largest declared counter remains far below Word overflow");
    for (const auto site : sites()) {
        need(d::valid_address(site, 0) && d::valid_address(site, 256) &&
                 !d::valid_address(site, 257),
             "Word ordinal boundary is inclusive 0..256 only");
        need(d::word(identity, site, 257).status == d::Status::invalid_ordinal &&
                 d::word(identity, site, 0xffffffffu).status == d::Status::invalid_ordinal,
             "Overflowing/poisoned ordinals must fail before constructing an address");
    }
}
void validation_and_old_aliases() {
    const d::Identity identity{11, 13, 17, 19, 23};
    for (unsigned phase = 0; phase <= 5; ++phase)
        for (unsigned candidate = 0; candidate <= 4; ++candidate) {
            const bool expected =
                (phase == 0 && candidate == 0) || (phase == 1 && candidate == 1) ||
                (phase == 2 && candidate >= 1) || ((phase == 4 || phase == 5) && candidate == 0);
            need(d::valid_site(d::area_site(phase, candidate)) == expected,
                 "Unused Area scheduling pairs must not acquire a categorical stream");
        }
    for (const auto site :
         {d::fresh_site(131), d::fresh_site(0xffffffffu), d::area_site(6, 0), d::area_site(2, 5),
          d::area_site(3, 0), d::area_site(2, 0), d::mutation_site(4),
          d::Site{d::Role::fresh_local, 0, 1}, d::Site{d::Role::mutation_acceptance, 0, 1}}) {
        const auto word = d::word(identity, site, 0);
        need(!d::valid_site(site) && word.status == d::Status::invalid_site && word.value == 0,
             "Poisoned/unused/reserved site coordinates must fail closed");
    }
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const d::Site unknown{static_cast<d::Role>(0xffffffffu), 0, 0};
    need(d::word(identity, unknown, 0).status == d::Status::invalid_role,
         "Unknown role is distinct from an invalid site in a known role");
    const auto fresh_a = d::address(identity, d::fresh_site(0), 16);
    const auto fresh_b = d::address(identity, d::fresh_site(1), 0);
    need(fresh_a.counter.w != fresh_b.counter.w,
         "Negative control: old slot*16 aliases slot0/word16 with slot1/word0");
    const auto area_a = d::address(identity, d::area_site(2, 1), 16);
    const auto area_b = d::address(identity, d::area_site(2, 2), 0);
    need(area_a.counter.w != area_b.counter.w,
         "Negative control: old candidate*16 aliases consecutive valid Area sites");
}
void disjoint_legacy_and_discovery() {
    const d::Identity identity{7, 0xffffffffu, 0x98765432u, 0x12345678u, 0xabcdef01u};
    const legacy::Identity old{identity.pixel, identity.serial_low, identity.serial_high,
                               identity.seed_low, identity.seed_high};
    const discovery::Identity gate{identity.pixel, identity.serial_low, identity.serial_high,
                                   identity.seed_low, identity.seed_high};
    const mutation::MutationIdentity mh{identity.pixel, identity.serial_low, identity.serial_high,
                                        identity.seed_low, identity.seed_high};
    std::set<Address> reserved;
    for (unsigned slot = 0; slot <= 130; ++slot)
        for (unsigned ordinal = 0; ordinal < 9; ++ordinal)
            reserved.insert(tuple(legacy::counter(old, slot, ordinal), legacy::key(old)));
    for (const auto site : sites())
        if (site.role == d::Role::area_selection)
            for (unsigned ordinal = 0; ordinal < 9; ++ordinal)
                reserved.insert(
                    tuple({identity.pixel, identity.serial_low, identity.serial_high,
                           site.index * 128 + site.candidate * 16 + ordinal},
                          {identity.seed_low ^ 0x72737331u, identity.seed_high ^ 0x61726531u}));
    for (unsigned step = 0; step < 4; ++step)
        for (const auto role : {mutation::Entropy::proposal, mutation::Entropy::acceptance})
            for (unsigned block = 0; block < (role == mutation::Entropy::proposal ? 1u : 3u);
                 ++block) {
                const auto address = mutation::decision_counter(mh, step, role, block);
                need(address.status == mutation::Status::ok, "Existing mutation address fixture");
                reserved.insert(tuple(address.value, {identity.seed_low, identity.seed_high}));
            }
    for (unsigned ordinal = 0; ordinal < 64; ++ordinal)
        reserved.insert(tuple(discovery::counter(gate, ordinal), discovery::key(gate)));
    std::set<Key> new_keys;
    for (const auto site : sites()) {
        const auto first = d::address(identity, site, 0);
        new_keys.insert({first.key.x, first.key.y});
        need((first.key.x != legacy::key(old).x || first.key.y != legacy::key(old).y) &&
                 (first.key.x != discovery::key(gate).x || first.key.y != discovery::key(gate).y) &&
                 (first.key.x != (identity.seed_low ^ 0x72737331u) ||
                  first.key.y != (identity.seed_high ^ 0x61726531u)) &&
                 (first.key.x != identity.seed_low || first.key.y != identity.seed_high),
             "Every new role key must be disjoint from legacy/discovery for this fixed seed");
        for (unsigned ordinal = 0; ordinal <= 256; ++ordinal) {
            const auto address = d::address(identity, site, ordinal);
            need(!reserved.contains(tuple(address.counter, address.key)),
                 "No new key/counter may overlap any retained legacy/proposal/discovery address");
        }
    }
    need(new_keys.size() == 3, "All three Enhanced resampling roles have distinct fixed-seed keys");
    // Counter words alone intentionally coincide at site0/ordinal0; key domains
    // distinguish them. The contract is full-address separation, not disjoint w.
    const auto fresh = d::address(identity, d::fresh_site(0), 0);
    const auto area = d::address(identity, d::area_site(0, 0), 0);
    need(fresh.counter.w == area.counter.w && fresh.key.x != area.key.x,
         "Role keys must separate equal local counter coordinates");
}
void full_identity() {
    const d::Identity identity{0xffffffffu, 0xffffffffu, 0xfffffffeu, 0xffffffffu, 0xffffffffu};
    const auto baseline = d::address(identity, d::mutation_site(3), 256);
    auto next = identity;
    next.serial_low = 0;
    next.serial_high = 0xffffffffu;
    const auto rollover = d::address(next, d::mutation_site(3), 256);
    need(baseline.counter.x == 0xffffffffu && baseline.counter.y == 0xffffffffu &&
             baseline.counter.z == 0xfffffffeu && rollover.counter.y == 0 &&
             rollover.counter.z == 0xffffffffu &&
             tuple(baseline.counter, baseline.key) != tuple(rollover.counter, rollover.key),
         "Pixel and serial rollover above 32 bits must remain injective");
    const auto a = d::word(identity, d::fresh_site(130), 256);
    const auto b = d::word(identity, d::fresh_site(130), 256);
    need(a.status == d::Status::ok && a.value == b.value,
         "Repeated complete identity/site/ordinal is deterministic");
}
} // namespace
int main() try {
    known_answers();
    exhaustive_addresses();
    validation_and_old_aliases();
    disjoint_legacy_and_discovery();
    full_identity();
    std::cout << "Enhanced 143-site 257-word entropy injectivity/reference checks passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
