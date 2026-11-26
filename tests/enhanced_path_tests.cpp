// SPDX-License-Identifier: GPL-3.0-only
#include "contribution_decision_shared.h"
#include "enhanced_decision_shared.h"
#include "enhanced_path_shared.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
namespace e = rypath_enhanced_path;
namespace d = rypath_enhanced_decision;
namespace c = rypath_contribution_path;
namespace m = rypath_reservoir;
void need(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(long double actual, long double expected, long double tolerance, const char *message) {
    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
e::Descriptor descriptor(unsigned depth) {
    return c::make_descriptor(c::DepthMode::finite, depth).value;
}
e::PreparedUpdate pending(const e::State &state, unsigned index, e::XYZ xyz, float probability) {
    auto p = e::prepare_update(state, index, xyz, probability);
    need(p.status == e::Status::ok, "prepare valid observation");
    return p;
}
void observe(e::State &s, e::XYZ xyz, float probability, unsigned word = 0) {
    auto p = pending(s, s.observed, xyz, probability);
    for (unsigned i = 0; !p.decision.complete && i < m::maximum_random_words; ++i)
        e::feed_random_word(p, word);
    need(e::commit_update(s, p).status == e::Status::ok, "commit valid observation");
}
// Independent long-double physical target; no production scaled helper.
long double alpha(e::XYZ xyz, float scale = 1) {
    if (xyz.x == 0 && xyz.y == 0 && xyz.z == 0)
        return 0;
    return std::max(static_cast<long double>(std::numeric_limits<float>::min()),
                    1e-6L * static_cast<long double>(scale)) +
           ((std::abs(static_cast<long double>(xyz.x)) / 4 +
             std::abs(static_cast<long double>(xyz.y)) / 4) +
            std::abs(static_cast<long double>(xyz.z)) / 4);
}
void targets_and_nulls() {
    need(e::null_slot != e::fresh_slot, "null is not a fresh request");
    for (auto xyz : {e::XYZ{}, e::XYZ{-0.f, 0, -0.f}})
        need(m::bits(e::target(xyz, 1).value) == 0, "exact signed black target");
    const float tiny = std::numeric_limits<float>::denorm_min();
    need(e::target({tiny, 0, 0}, 1).value > 0, "nonzero subnormal retains support");
    for (auto xyz :
         {e::XYZ{4, -8, 16}, e::XYZ{0x1p120f, -0x1p120f, 0x1p120f}, e::XYZ{tiny, 0, 0}}) {
        const auto t = e::target(xyz, 1);
        need(t.status == m::Status::ok, "finite signed HDR target");
        near(t.value, alpha(xyz), alpha(xyz) * 4e-7L, "independent target");
    }
    need(e::target({}, 0).status != m::Status::ok, "black does not bypass invalid scale");
    need(e::target({std::numeric_limits<float>::infinity(), 0, 0}, 1).status != m::Status::ok,
         "nonfinite is failure not null");
    for (unsigned depth : {0u, 1u, 65u}) {
        auto s = e::begin(descriptor(depth), 1);
        for (unsigned i = 0; i < s.descriptor.slot_count; ++i) {
            auto p = pending(s, i, {-0.f, 0, 0}, 1);
            need(p.decision.complete && p.decision.words_used == 0, "zero consumes no entropy");
            need(e::commit_update(s, p).status == e::Status::ok, "zero observation committed");
        }
        const auto r = e::finalize(s, e::StopReason::depth_end);
        need(r.status == e::Status::ok && r.selected_slot == e::null_slot && r.normalizer == 0 &&
                 r.target == 0 && s.observed == s.descriptor.slot_count,
             "all-black explicit null retains observations");
        const auto out = e::readout(r);
        need(out.status == e::Status::ok && m::bits(out.value.x) == 0, "canonical null readout");
    }
    need(e::readout(1, {}, 1).status != e::Status::ok, "no fabricated nonnull black");
    need(e::readout(0, {1, 0, 0}, 0).status != e::Status::ok, "null has no nonzero XYZ");
}
void ordered_and_failures() {
    auto s = e::begin(descriptor(65), 1);
    observe(s, {}, 1);
    observe(s, {4, -2, 0}, .5f);
    const auto selected = s.selected_slot;
    const auto sum = s.score_sum;
    observe(s, {}, .25f);
    need(s.selected_slot == selected && m::bits(s.score_sum) == m::bits(sum) && s.observed == 3,
         "zero after real preserves selection and logical cursor");
    need(e::finalize(s, e::StopReason::rr_cut).status == e::Status::ok, "odd RR prefix accepted");
    need(e::finalize(s, e::StopReason::depth_end).status == e::Status::incomplete,
         "partial path is not depth complete");
    auto p = e::prepare_update(s, 4, {1, 0, 0}, 1);
    need(p.status == e::Status::invalid_slot, "observation hole fails");
    p = e::prepare_update(s, 2, {}, 1);
    need(p.status == e::Status::invalid_slot, "duplicate zero fails");
    p = e::prepare_update(s, 3, {}, 0);
    need(p.status != e::Status::ok, "zero does not bypass discovery validation");
    p = pending(s, 3, {1, 0, 0}, 1);
    need(e::commit_update(s, p).status == e::Status::incomplete, "missing entropy fails sticky");
    need(e::finalize(s, e::StopReason::physical_end).status == e::Status::incomplete,
         "failure never finalizes black");
    auto early = e::begin(descriptor(1), 1);
    need(e::finalize(early, e::StopReason::physical_end).status != e::Status::ok,
         "must observe E0 even black");
    observe(early, {}, 1);
    need(e::finalize(early, e::StopReason::physical_end).status == e::Status::ok,
         "known physical end needs no fake future observations");
    observe(early, {1, 0, 0}, 1);
    need(e::finalize(early, e::StopReason::rr_cut).status != e::Status::ok,
         "RR cutoff cannot follow N");
    auto bad = e::begin(descriptor(0), 1);
    p = pending(bad, 0, {}, 1);
    p.decision.selected = true;
    need(e::commit_update(bad, p).status != e::Status::ok, "tampered zero decision fails");
    auto overflow = e::begin(descriptor(1), 1);
    const auto huge = e::XYZ{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    p = e::prepare_update(overflow, 0, huge, 0x1p-32f);
    need(p.status == e::Status::arithmetic_failure, "HDR score overflow is whole-stream failure");
    need(e::commit_update(overflow, p).status == e::Status::arithmetic_failure,
         "overflow cannot drop candidate");
    auto stale = e::begin(descriptor(1), 1);
    p = pending(stale, 0, {1, 0, 0}, 1);
    observe(stale, {}, 1);
    need(e::commit_update(stale, p).status == e::Status::invalid_state, "stale prepared update");
}
long double probability(const m::PreparedUpdate &p) {
    if (p.complete)
        return p.selected ? 1 : 0;
    const long double rare = std::ldexp(static_cast<long double>(p.rare_probability.significand),
                                        -static_cast<int>(p.rare_probability.bit_count));
    return p.select_new_on_success ? rare : 1 - rare;
}
void wrs_distribution() {
    // Enumerate every selection branch analytically using its represented dyadic
    // decision probability. No sampling error or production readout oracle.
    const std::array<e::XYZ, 5> xyz{{{}, {4, -2, 0}, {}, {1, 0, 3}, {-2, 7, 1}}};
    const std::array<float, 5> a{{1, .5f, .25f, .25f, .125f}};
    struct Branch {
        e::State state;
        long double mass;
    };
    std::array<Branch, 32> branches{};
    unsigned count = 1;
    branches[0] = {e::begin(descriptor(2), 1), 1};
    long double expected_sum = 0;
    for (unsigned i = 0; i < xyz.size(); ++i) {
        expected_sum += alpha(xyz[i]) / a[i];
        std::array<Branch, 32> next{};
        unsigned n = 0;
        for (unsigned j = 0; j < count; ++j) {
            const auto p = pending(branches[j].state, i, xyz[i], a[i]);
            const long double select = p.alpha == 0 ? 0 : probability(p.decision);
            for (unsigned outcome = 0; outcome < 2; ++outcome) {
                const long double chance = outcome ? select : 1 - select;
                if (chance == 0)
                    continue;
                auto state = branches[j].state;
                // Independent state recurrence, not production commit. The
                // committed complete-state path is checked separately below.
                ++state.observed;
                if (p.alpha != 0)
                    state.score_sum = p.decision.score_sum;
                if (outcome) {
                    state.selected_slot = i;
                    state.selected_xyz = xyz[i];
                    state.selected_target = p.alpha;
                }
                next[n++] = {state, branches[j].mass * chance};
            }
        }
        branches = next;
        count = n;
    }
    std::array<long double, 3> average{};
    long double total_mass = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto &b = branches[i];
        need(e::valid_state(b.state), "independent recurrence produces valid state");
        near(b.state.score_sum, expected_sum, expected_sum * 4e-7L, "ordered normalizer");
        const auto r = e::finalize(b.state, e::StopReason::depth_end);
        const auto out = e::readout(r);
        need(out.status == e::Status::ok, "signed readout valid");
        const long double W = static_cast<long double>(r.normalizer) / r.target;
        near(out.value.x, W * r.xyz.x, std::abs(W * r.xyz.x) * 3e-7L + 1e-30L,
             "signed scaled readout against independent division");
        average[0] += b.mass * out.value.x;
        average[1] += b.mass * out.value.y;
        average[2] += b.mass * out.value.z;
        total_mass += b.mass;
    }
    near(total_mass, 1, 1e-15L, "selection masses normalize");
    std::array<long double, 3> wanted{};
    for (unsigned i = 0; i < xyz.size(); ++i) {
        wanted[0] += static_cast<long double>(xyz[i].x) / a[i];
        wanted[1] += static_cast<long double>(xyz[i].y) / a[i];
        wanted[2] += static_cast<long double>(xyz[i].z) / a[i];
    }
    for (unsigned i = 0; i < 3; ++i)
        near(average[i], wanted[i], std::abs(wanted[i]) * 2e-6L,
             "per-slot HT conditional identity");
    // Exercise all nine entropy words with a threshold-prefix tie. The
    // independent bit expansion of the represented dyadic builds the exact
    // threshold; choose threshold-1 at its last bit to select the rare atom.
    auto state = e::begin(descriptor(1), std::numeric_limits<float>::min());
    observe(state, {std::numeric_limits<float>::max(), 0, 0}, 1);
    auto p = pending(state, 1, {std::numeric_limits<float>::denorm_min(), 0, 0}, 1);
    const auto rare = p.decision.rare_probability;
    need(rare.bit_count > 256, "nine-word rare probability not erased");
    const unsigned words = (rare.bit_count + 31) / 32;
    for (unsigned ordinal = 0; ordinal < words; ++ordinal) {
        unsigned word = 0;
        for (unsigned bit = 0; bit < 32; ++bit) {
            const unsigned position = ordinal * 32 + bit;
            if (position < rare.bit_count && position + 24 >= rare.bit_count) {
                const unsigned source_bit = rare.bit_count - position - 1;
                word |= ((rare.significand >> source_bit) & 1u) << (31 - bit);
            }
        }
        if (ordinal + 1 == words)
            word -= 1u << (words * 32 - rare.bit_count);
        e::feed_random_word(p, word);
        if (ordinal + 1 < words)
            need(!p.decision.complete, "threshold prefix keeps requesting entropy");
    }
    need(p.decision.words_used == 9 && e::commit_update(state, p).status == e::Status::ok &&
             state.selected_slot == 1,
         "ninth-word rare finite decision remains selectable");
}
void pair_scores_and_negative_controls() {
    for (float Z : {1.f, 8.f, 0x1p120f})
        for (float ts : {0.f, .25f, 8.f})
            for (float td : {0.f, 1.f, 16.f}) {
                const e::PairInput p{Z, ts, td, 20, 1, .25f};
                const auto s = e::pair_score(p);
                need(s.status == m::Status::ok, "zero-aware pair score");
                const long double expected =
                    td == 0 ? 0 : static_cast<long double>(Z) * 20 * td / (20.L * ts + td) / 4;
                near(s.value, expected, std::abs(expected) * 3e-7L, "independent pair formula");
            }
    need(e::pair_score({0, 0, 0, 20, 1}).value == 0, "selected null score zero");
    need(e::pair_score({1, 0, 4, 1, 1}).value == 1, "undefined inverse gets complementary share");
    // Independent two-state null-domain negative: destination F=2 in
    // both U states; source F/target=0 at U0 and2 at U1. Fresh has one atom,
    // so current Z=2, old Z is0 or2. Four independent (currentU,oldU) pairs.
    // The old null cannot remove its domain from the current inverse target.
    long double correct = 0, deleted = 0;
    for (unsigned current = 0; current < 2; ++current)
        for (unsigned old = 0; old < 2; ++old) {
            const float inverse = current == 0 ? 0.f : 2.f;
            const float Zold = old == 0 ? 0.f : 2.f;
            const auto center = e::canonical_score(2, 2, inverse, 1, 1);
            const auto shifted = e::pair_score({Zold, old == 0 ? 0.f : 2.f, 2, 1, 1});
            need(center.status == m::Status::ok && shifted.status == m::Status::ok,
                 "null-domain contributions valid");
            // The intentionally WRONG implementation deletes the inverse
            // domain whenever the independently drawn old reservoir is null.
            const long double wrong_center = old == 0 ? 2 : (inverse == 0 ? 2 : 1);
            correct += (center.value + shifted.value) / 4.L;
            deleted += (wrong_center + shifted.value) / 4.L;
        }
    near(correct, 2, 0, "null-domain reference");
    near(deleted, 2.25L, 0, "null deletion 9/4 negative witness");
    need(deleted != correct, "null cannot delete structural support");
    need(e::canonical_score(8, 2, 0, 1, 20, .25f).value == 2,
         "zero inverse gives full canonical pair share");
    need(e::canonical_score(0, 0, 4, 1, 20).value == 0, "null canonical score");
    need(e::add_scores(0, 3).value == 3 && e::add_scores(3, 0).value == 3 &&
             e::add_scores(0, 0).value == 0 && e::add_scores(2, 3).value == 5,
         "zero-aware score sum");
    // Fresh E0=0, future E1=4 discovered with independent probability1/2.
    // A killed path is an explicit null, NOT a fake sampled future j. Survival
    // has Z=alpha/.5 and UCW2, so E[readout]=4. Future canonical replay j=2
    // remains4 even if that atom was unobserved in a killed fresh attempt.
    auto killed = e::begin(descriptor(1), 1);
    observe(killed, {}, 1);
    const auto null = e::finalize(killed, e::StopReason::rr_cut);
    need(null.status == e::Status::ok && null.selected_slot == e::null_slot, "RR null no future j");
    auto survived = e::begin(descriptor(1), 1);
    observe(survived, {}, 1);
    observe(survived, {}, .5f);
    observe(survived, {4, 0, 0}, .5f);
    const auto out = e::readout(e::finalize(survived, e::StopReason::depth_end));
    near(.5L * out.value.x, 4, 1e-6L, "external discovery compensation");
    const e::XYZ canonical_later{4, 0, 0};
    need(e::target(canonical_later, 1).value > 0 && null.selected_slot != 2,
         "unobserved is not zero canonical response");
}
void future_padding_negative() {
    // Independent ideal UCW indicator oracle. Killed fresh paths (probability
    // 1/2) have observed only E0. Padding three floor1 atoms would draw an
    // UNOBSERVED E1 with probability1/3 and UCW3, creating spurious indicator
    // mass(1/2)*(1/3)*3=1/2. Correct surviving discovery contributes mass1.
    // Reevaluate that SAME fixed future atom as nonzero4: wrong mean6 vs4.
    const long double killed = .5L;
    const long double false_selection = 1.L / 3;
    const long double false_ucw = 3;
    const long double valid_future_indicator = .5L * 2;
    const long double wrong_indicator =
        valid_future_indicator + killed * false_selection * false_ucw;
    near(valid_future_indicator * 4, 4, 0, "future atom HT indicator");
    near(wrong_indicator * 4, 6, 0, "future-padding mapped nonzero negative");
    auto s = e::begin(descriptor(1), 1);
    observe(s, {1, 0, 0}, 1);
    const auto cut = e::finalize(s, e::StopReason::rr_cut);
    need(cut.status == e::Status::ok && cut.selected_slot == 0 && s.observed == 1,
         "positive early atom survives cutoff without inventing futurej");
    near(e::readout(cut).value.x, 1, 1e-7L, "arrival E uses pre-gate probability");
    // Survived branch: keep the same E0 probability1; only N0/E1 use1/2.
    observe(s, {}, .5f);
    auto p = pending(s, 2, {4, 0, 0}, .5f);
    const long double choose = probability(p.decision);
    const long double total = p.decision.score_sum;
    const long double survivor_mean =
        (1 - choose) * total / s.selected_target + choose * total / p.alpha * 4;
    near(.5L * e::readout(cut).value.x + .5L * survivor_mean, 5, 3e-6L,
         "arrival and later discovery probabilities kept distinct");
}
void long_stream_and_readout() {
    auto s = e::begin(descriptor(65), 1);
    long double total = 0;
    float float_sum = 0;
    unsigned positives = 0;
    for (unsigned i = 0; i < 131; ++i) {
        const e::XYZ xyz = i % 3 == 0 ? e::XYZ{} : e::XYZ{float(i + 1), -2, 0};
        const float a = i < 50 ? 1.f : .25f;
        const auto t = e::target(xyz, 1);
        total += alpha(xyz) / a;
        if (t.value > 0) {
            float_sum += t.value / a;
            ++positives;
        }
        observe(s, xyz, a, 0xffffffffu);
        need(s.observed == i + 1 && e::valid_state(s), "all131 ordered observations valid");
    }
    need(positives == 87 && m::bits(float_sum) == m::bits(s.score_sum),
         "zero skip retains original positive-score association");
    near(s.score_sum, total, total * 2e-6L, "independent long-stream normalizer");
    need(e::finalize(s, e::StopReason::depth_end).status == e::Status::ok, "D65 full stream");
    const float tiny = std::numeric_limits<float>::denorm_min();
    const auto output = e::readout(0x1p120f, {tiny, -tiny, -0.f}, 0x1p-120f);
    need(output.status == e::Status::ok && output.value.x == 0x1p91f &&
             output.value.y == -0x1p91f && m::bits(output.value.z) == 0x80000000u,
         "weight before materialization recovers subnormal HDR and signed zero");
    const auto overflow = e::readout(0x1p120f, {0x1p120f, 0, 0}, 0x1p-120f);
    need(overflow.status == e::Status::arithmetic_failure, "readout overflow is failure");
    auto p = e::prepare_update(s, 131, {}, 1);
    need(p.status == e::Status::invalid_slot, "past fixed-domain observation fails");
}

void chart_densities() {
    namespace r = rypath_reuse_mis;
    constexpr r::Scaled unit{.5f, 1};
    for (auto xyz : {e::XYZ{4, -2, 0}, e::XYZ{0x1p120f, -0x1p120f, 0},
                     e::XYZ{std::numeric_limits<float>::denorm_min(), 0, 0}}) {
        const auto expected = r::importance_target(xyz.x, xyz.y, xyz.z, 1);
        const auto actual = e::target(xyz, 1, unit);
        need(actual.status == expected.status && m::bits(actual.value) == m::bits(expected.value),
             "Unit evaluation density preserves original target bits");
        auto state = e::begin(descriptor(0), 1);
        const auto old = e::prepare_update(state, 0, xyz, .5f);
        const auto explicit_unit = e::prepare_update(state, 0, xyz, .5f, unit, unit);
        need(m::bits(old.alpha) == m::bits(explicit_unit.alpha) &&
                 m::bits(old.decision.score_sum) == m::bits(explicit_unit.decision.score_sum),
             "Default chart densities preserve score bits");
    }
    const e::XYZ F{0x1p-18f, -0x1p-20f, 0};
    for (auto h :
         {r::Scaled{.5f, -1024}, r::Scaled{.75f, -20}, r::Scaled{.5f, 4}, r::Scaled{.5f, 100}}) {
        const auto actual = e::target(F, 1, h);
        const long double density = std::ldexp(static_cast<long double>(h.fraction), h.exponent);
        const long double energy =
            (std::abs(static_cast<long double>(F.x)) + std::abs(static_cast<long double>(F.y))) / 4;
        const long double expected = energy + density * static_cast<long double>(1e-6f);
        need(actual.status == m::Status::ok, "Scaled h need not materialize to FLOAT");
        near(actual.value, expected, expected * 4e-7L, "Independent chart floor/energy target");
    }
    need(e::target({1, 0, 0}, 1, {.5f, 1024}).status == m::Status::arithmetic_failure,
         "Huge chart floor fails instead of overflowing or clipping");
    for (auto poison : {r::Scaled{0, 1}, r::Scaled{1, 0}, r::Scaled{-.5f, 1},
                        r::Scaled{std::numeric_limits<float>::quiet_NaN(), 1},
                        r::Scaled{.5f, std::numeric_limits<int>::max()},
                        r::Scaled{.5f, std::numeric_limits<int>::min()}}) {
        need(!e::valid_density(poison) && e::target({}, 1, poison).status != m::Status::ok,
             "Even zero cannot bypass poisoned h before exponent arithmetic");
        const auto state = e::begin(descriptor(0), 1);
        need(e::prepare_update(state, 0, {}, 1, unit, poison).status != e::Status::ok,
             "Zero observation still validates actual generation h");
    }
    auto state = e::begin(descriptor(0), 1);
    auto update = e::prepare_update(state, 0, {2, 0, 0}, 1, {.5f, 4}, {.5f, 2});
    need(update.status == e::Status::ok && update.decision.complete,
         "Different source generation/query density is legal");
    need(e::commit_update(state, update).status == e::Status::ok &&
             state.score_sum < state.selected_target && e::valid_state(state),
         "h_gen>1 legally produces Z below selected canonical target");
    need(e::same_density(state.selected_h, {.5f, 4}), "Selected evaluation density retained");
    const auto reservoir = e::finalize(state, e::StopReason::depth_end);
    need(e::same_density(reservoir.selected_h, state.selected_h) &&
             e::readout(reservoir).value.x == 1,
         "Weighted chart response divides by source generation, not queried h");
    auto stale = e::begin(descriptor(1), 1);
    observe(stale, {0x1p80f, 0, 0}, 1);
    update = e::prepare_update(stale, 1, {2, 0, 0}, 1, {.5f, 4}, {.5f, 2});
    stale.selected_h = {.5f, 2};
    need(e::valid_state(stale), "Absorbed floor leaves altered-h state numerically valid");
    need(e::commit_update(stale, update).status == e::Status::invalid_state,
         "Stale selected evaluation-density state fails");
    auto changed = e::begin(descriptor(0), 1);
    update = e::prepare_update(changed, 0, {2, 0, 0}, 1, unit, unit);
    update.h_gen = {.5f, 2};
    need(e::commit_update(changed, update).status == e::Status::invalid_state,
         "Commit recomputes generation proposal score from retained density");
    auto null = e::begin(descriptor(0), 1);
    update = e::prepare_update(null, 0, {}, 1, {.5f, 100}, {.5f, -100});
    need(e::commit_update(null, update).status == e::Status::ok && e::unit_density(null.selected_h),
         "Null h remains canonical unit regardless of zero candidate's chart");
    null.selected_h = {.5f, 2};
    need(!e::valid_state(null), "Noncanonical null h invalid");
    // MH acts on the CHART target. Here the old U targets would order the two
    // states oppositely, so the negative control distinguishes actual policy.
    const auto current = e::target({0x1p-18f, 0, 0}, 1, {.5f, 2});
    const auto proposal = e::target({0x1p-18f, 0, 0}, 1, {.5f, 0});
    const auto acceptance = r::prepare_normalizer_acceptance(.25f, current.value, proposal.value);
    const auto wrong_current = e::target({0x1p-19f, 0, 0}, 1);
    const auto wrong_proposal = e::target({0x1p-17f, 0, 0}, 1);
    const auto wrong =
        r::prepare_normalizer_acceptance(.25f, wrong_current.value, wrong_proposal.value);
    need(acceptance.status == r::MutationStatus::ok && !acceptance.decision.complete &&
             wrong.status == r::MutationStatus::ok && wrong.decision.complete &&
             wrong.decision.selected,
         "Fixed-label MH targets physical chart T, not old U target");
}
void chart_quadrature() {
    namespace r = rypath_reuse_mis;
    // Piecewise-constant continuous endpoint quadrature, NOT endpoint-average:
    // widths1/3,2/3; actual h_gen2,1/2 => generation masses2/3,1/3.
    // Correlated F=2,6 gives physical integral14/3; omitting hgen gives10/3.
    constexpr std::array<float, 2> F{{2, 6}};
    constexpr std::array<long double, 2> mass{{2.L / 3, 1.L / 3}};
    constexpr std::array<r::Scaled, 2> generation{{{.5f, 2}, {.5f, 0}}};
    constexpr std::array<r::Scaled, 2> evaluation{{{.5f, 4}, {.5f, 0}}};
    long double physical = 0, omitted = 0;
    for (unsigned point = 0; point < 2; ++point) {
        auto state = e::begin(descriptor(0), 1);
        auto p =
            e::prepare_update(state, 0, {F[point], 0, 0}, 1, evaluation[point], generation[point]);
        need(p.status == e::Status::ok && e::commit_update(state, p).status == e::Status::ok,
             "Quadrature fresh candidate valid");
        const auto out = e::readout(e::finalize(state, e::StopReason::depth_end));
        need(out.status == e::Status::ok, "Quadrature weighted readout");
        physical += mass[point] * out.value.x;
        omitted += mass[point] * F[point];
    }
    near(physical, 14.L / 3, 2e-6L, "Endpoint-density correlated physical mean");
    near(omitted, 10.L / 3, 1e-15L, "Omitted generation-density negative");
    need(std::abs(physical - omitted) > 1, "Endpoint correlation makes missing h observable");
    // Two atoms, independent endpoint draws, second observed with probability1/2.
    // Enumerate source quadrature, discovery outcome and represented WRS decision.
    // Expected sum is2*(14/3)=28/3; T_floor may differ by heval at either atom.
    long double two_atom_mean = 0;
    for (unsigned first = 0; first < 2; ++first)
        for (unsigned second = 0; second < 2; ++second) {
            auto source = e::begin(descriptor(1), 1);
            auto initial = e::prepare_update(source, 0, {F[first], 0, 0}, 1, evaluation[first],
                                             generation[first]);
            need(e::commit_update(source, initial).status == e::Status::ok, "First chart atom");
            const auto cut = e::readout(e::finalize(source, e::StopReason::rr_cut));
            const auto next = e::prepare_update(source, 1, {F[second], 0, 0}, .5f,
                                                evaluation[second], generation[second]);
            need(next.status == e::Status::ok && !next.decision.complete, "Chart WRS second atom");
            const long double select_new = probability(next.decision);
            long double survived = 0;
            for (unsigned word : {0u, 0xffffffffu}) {
                auto state = source;
                auto p = next;
                for (unsigned ordinal = 0;
                     !p.decision.complete && ordinal < m::maximum_random_words; ++ordinal)
                    e::feed_random_word(p, word);
                const auto result = e::commit_update(state, p);
                need(result.status == e::Status::ok, "Commit chart WRS branch");
                observe(state, {}, .5f);
                const auto out = e::readout(e::finalize(state, e::StopReason::depth_end));
                need(out.status == e::Status::ok, "Finalized chart WRS branch");
                survived += (result.selected ? select_new : 1 - select_new) * out.value.x;
            }
            two_atom_mean += mass[first] * mass[second] * (.5L * cut.value.x + .5L * survived);
        }
    near(two_atom_mean, 28.L / 3, 1e-5L,
         "Independent endpoint/discovery/WRS quadrature physical sum");
}

void discovery() {
    auto state = e::DiscoveryState{};
    auto p = e::prepare_gate(state, .1f);
    const auto threshold = static_cast<unsigned>(std::ceil(static_cast<double>(.1f) * 16777216));
    need(p.threshold == threshold && p.probability == static_cast<float>(threshold) * 0x1p-24f,
         "actual24-bit probability");
    auto yes = state, no = state;
    need(e::commit_gate(yes, p, (threshold - 1) << 8).survived, "last surviving word");
    need(!e::commit_gate(no, p, threshold << 8).survived, "first rejected word");
    for (float desired : {std::numeric_limits<float>::min(), 0x1p-24f, .1f, .5f, 1.f}) {
        const auto gate = e::prepare_gate({}, desired);
        const double units = static_cast<double>(desired) * 16777216;
        const auto wanted = static_cast<unsigned>(std::ceil(units));
        need(gate.status == e::Status::ok && gate.threshold == wanted,
             "actual ceil lattice includes minimum and unit endpoints");
    }
    auto tampered_state = e::DiscoveryState{};
    auto tampered = e::prepare_gate(tampered_state, .5f);
    tampered.probability = .75f;
    need(e::commit_gate(tampered_state, tampered, 0).status == e::Status::invalid_state,
         "gate probability receipt tampering fails");
    state = {};
    for (unsigned i = 0; i < 64; ++i) {
        p = e::prepare_gate(state, .5f);
        const double proposed = static_cast<double>(state.probability) * .5;
        const bool forced = proposed < (1 + std::ldexp(1., -17)) * std::ldexp(1., -32);
        need((p.threshold == (1u << 24)) == forced, "pre-draw budget rule");
        const auto result = e::commit_gate(state, p, 0xffffffffu);
        need(result.status == e::Status::ok && result.probability >= 0x1p-32f,
             "64-gate bounded cumulative probability");
        if (forced)
            need(result.survived, "q1 independent of unseen random word");
    }
    need(e::prepare_gate(state, 1).status != e::Status::ok, "gate bound");
    // Non-power-of-two actual thresholds exercise rounded probability products.
    // Compare all prefixes with independent long-double products of EXACT T/2^24;
    // FLOAT A is an approximation with gamma_n, not exact HT compensation.
    state = {};
    long double exact_product = 1;
    constexpr std::array<float, 7> desired{{.1f, .73f, .91f, .37f, .83f, .61f, .99f}};
    for (unsigned i = 0; i < 64; ++i) {
        const auto gate = e::prepare_gate(state, desired[i % desired.size()]);
        const auto proposed_threshold = static_cast<unsigned>(
            std::ceil(static_cast<double>(desired[i % desired.size()]) * 16777216));
        const float proposed_q = static_cast<float>(proposed_threshold) * 0x1p-24f;
        const float rounded_next = state.probability * proposed_q;
        const bool forced = rounded_next < (1.f + 0x1p-17f) * 0x1p-32f;
        need(gate.threshold == (forced ? 1u << 24 : proposed_threshold),
             "non-power q follows pre-draw budget receipt");
        exact_product *= static_cast<long double>(gate.threshold) / 16777216.L;
        const auto result = e::commit_gate(state, gate, 0);
        const long double n_u = static_cast<long double>(i + 1) * std::ldexp(1.L, -24);
        const long double gamma = n_u / (1 - n_u);
        need(result.status == e::Status::ok && exact_product >= std::ldexp(1.L, -32),
             "actual probability product respects bound without post-hoc clipping");
        near(result.probability, exact_product, gamma * exact_product,
             "rounded multi-gate A stays within gamma_n envelope");
    }

    need(e::prepare_gate({}, 0).status != e::Status::ok, "invalid q fails, no q1 rescue");
    need(e::prepare_gate({}, std::numeric_limits<float>::quiet_NaN()).status != e::Status::ok,
         "NaN q fails");
    // Random123 v1.14.0 Philox4x32-10 zero-key/zero-counter known answer.
    const d::Identity zero{0, 0, 0, d::domain, 0x6f727231u};
    need(d::word(zero, 0) == 0x6627e8d5u, "independent published Philox known answer");
    std::set<std::array<unsigned, 4>> addresses;
    for (unsigned high : {0u, 1u, 0xffffffffu})
        for (unsigned gate = 0; gate < 64; ++gate) {
            const d::Identity identity{17, 9, high, 42, 71};
            const auto address = d::counter(identity, gate);
            need(addresses.insert({address.x, address.y, address.z, address.w}).second,
                 "full high-serial/gate injection");
            const auto key = d::key(identity);
            const auto wrs = rypath_contribution_decision::key({17, 9, high, 42, 71});
            need(key.x != wrs.x || key.y != wrs.y, "discovery distinct from WRS key domain");
            need(key.x != 42 || key.y != 71, "discovery distinct from canonical tail key");
        }
    need(d::valid_address(63) && !d::valid_address(64), "discovery address bounds");
}
} // namespace
int main() {
    try {
        targets_and_nulls();
        ordered_and_failures();
        wrs_distribution();
        pair_scores_and_negative_controls();
        future_padding_negative();
        long_stream_and_readout();
        chart_densities();
        chart_quadrature();
        discovery();
        std::cout << "Enhanced zero-aware canonical math tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
