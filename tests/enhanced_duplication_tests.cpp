// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_duplication_oracle.hpp"
#include "enhanced_duplication_shared.h"
#include "enhanced_duplication_wire.h"
#include <cstring>
#include <iostream>
namespace {
namespace d = rypath_enhanced_duplication;
namespace ref = duplication_oracle;
void need(bool value, const char *why) { ref::require(value, why); }
void finite_maps() {
    const auto constant = ref::count(ref::fixture(17, 17, 0), 17, 17, 1);
    need(constant.counts[0] == 80 && constant.counts[8] == 152 &&
             constant.counts[8 * 17 + 8] == 288,
         "17x17 constant corner80/edge152/center288 excludes center and OOB");
    need(80 * 18 == 5 * 288 && 152 * 36 == 19 * 288,
         "Border duplication denominator remains288, never visible-neighbor count");
    const auto distinct = ref::count(ref::fixture(17, 17, 1), 17, 17, 2);
    for (auto count : distinct.counts)
        need(count == 0, "Distinct valid births are zero repetition, not missing");
    const auto stripe = ref::count(ref::fixture(17, 17, 2), 17, 17, 1);
    need(stripe.counts[0] == 8 && stripe.counts[8 * 17 + 8] == 16,
         "Independent vertical stripe counts");
    const auto checker = ref::count(ref::fixture(17, 17, 3), 17, 17, 2);
    need(checker.counts[0] == 40 && checker.counts[8] == 76 && checker.counts[8 * 17 + 8] == 144,
         "Independent checker finite border counts");
    const auto one = ref::count(ref::fixture(1, 1, 0), 1, 1, 1);
    need(one.counts[0] == 0 && one.histogram[0] == 1,
         "1x1 valid center has zero eligible neighbors");
    auto missing = ref::fixture(17, 17, 0);
    missing[0].valid = false;
    missing[0].null = true;
    const auto partial = ref::count(missing, 17, 17, 1);
    need(partial.counts[0] == ref::missing && partial.histogram[289] == 1 &&
             partial.counts[1] == 88,
         "Null center is missing and a null neighbor contributes no match");
    for (auto &p : missing) {
        p.valid = false;
        p.null = true;
    }
    const auto all_null = ref::count(missing, 17, 17, 1);
    need(all_null.histogram[289] == 289 && all_null.histogram[290] == 0 && all_null.sum == 0,
         "All-null coverage is separate from zero repetition");
    missing[7].null = false;
    const auto unknown = ref::count(missing, 17, 17, 1);
    need(unknown.histogram[289] == 288 && unknown.histogram[290] == 1,
         "Unknown positive is a separate protocol bucket");
    for (auto dims : {std::array{19u, 11u}, std::array{3u, 1u}, std::array{1u, 9u}}) {
        const auto map = ref::count(ref::fixture(dims[0], dims[1], 0), dims[0], dims[1], 1);
        unsigned total = 0;
        for (auto n : map.histogram)
            total += n;
        need(total == dims[0] * dims[1],
             "Odd and partial-tile finite maps cover every output once");
    }
}
void identities() {
    const d::Label zero{0, 0, 0, 0};
    need(d::valid_label(zero, 1) && !d::canonical_invalid(zero),
         "Pixel0/sample0/high0/j0 is a valid exact birth label");
    const auto invalid = d::invalid_label();
    need(d::canonical_invalid(invalid) && !d::valid_label(invalid, 289),
         "Missing birth has canonical UINT_MAX,0,0,0 sentinel");
    auto high = zero;
    high.sample_high = 1;
    need(!d::equal_label(zero, high, d::initial_seed) &&
             !d::equal_label(zero, high, d::initial_atom),
         "Full64bit sample separates equal low words");
    auto another_atom = zero;
    another_atom.initial_j = 1;
    need(d::equal_label(zero, another_atom, d::initial_seed) &&
             !d::equal_label(zero, another_atom, d::initial_atom),
         "Seed family deliberately ignores initial atom while atom family retains it");
    auto malformed = invalid;
    malformed.sample_low = 1;
    need(!d::canonical_invalid(malformed) && !d::valid_label(malformed, 289),
         "Noncanonical missing label cannot become known positive provenance");
    const auto u = rypath_replay::fresh({0, 0, 0, 0, 0});
    need(rypath_replay::valid(u), "Zero session seed and sample labels remain legal replay inputs");
    auto moved = u;
    moved.film[0] ^= 1;
    moved.lens[1] ^= 8;
    moved.tail_key[1] ^= 16;
    const auto before = d::current_key(u, 0), after = d::current_key(moved, 0);
    std::array<unsigned, 8> expected{};
    std::memcpy(expected.data(), &u, 7 * sizeof(unsigned));
    expected[7] = 0;
    need(std::memcmp(before.words, expected.data(), sizeof(before)) == 0,
         "Current key is exactly seven U words plus j, with version validated separately");
    need(!d::equal_key(before, after, d::current) && d::equal_label(zero, zero, d::initial_atom),
         "Mutation changes current coordinates without resetting the retained birth family");
    auto atoms = ref::fixture(17, 17, 0);
    for (unsigned p = 0; p < atoms.size(); ++p) {
        atoms[p].birth[3] = p % 2;
        atoms[p].current[0] = p;
    }
    const auto seed = ref::count(atoms, 17, 17, 1), atom = ref::count(atoms, 17, 17, 2),
               current = ref::count(atoms, 17, 17, 3);
    need(seed.counts[8 * 17 + 8] == 288 && atom.counts[8 * 17 + 8] == 144 &&
             current.counts[8 * 17 + 8] == 0,
         "Ancestry seed, ancestry atom and current(U,j) equality are explicitly different maps");
    need(d::equal_key(d::label_key(zero), d::label_key(another_atom), d::initial_seed) &&
             !d::equal_key(d::label_key(zero), d::label_key(another_atom), d::initial_atom),
         "GPU label-key mode projection preserves exact semantics");
}
} // namespace
int main() {
    try {
        finite_maps();
        identities();
        std::cout << "Duplication independent finite-map and exact-label controls passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
