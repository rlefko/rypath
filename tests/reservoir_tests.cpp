// SPDX-License-Identifier: GPL-3.0-only
#include "reservoir_shared.h"

#include <array>
#include <bit>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
namespace reservoir = rypath_reservoir;
void require(bool value, std::string_view label) {
    if (!value)
        throw std::runtime_error(std::string(label));
}
void near(long double value, long double expected, std::string_view label) {
    // Enumerated branch probabilities are exact rational expressions evaluated
    // in long double. Only the production weights use binary32 arithmetic.
    constexpr long double tolerance = 2e-6L;
    require(std::isfinite(value) &&
                std::abs(value - expected) <= tolerance * (1 + std::abs(expected)),
            std::string(label) + ": actual=" + std::to_string(static_cast<double>(value)) +
                " expected=" + std::to_string(static_cast<double>(expected)));
}
using Targets = std::array<long double, 3>;
constexpr Targets target_a{1, 2, 4}, target_b{3, 1, 5};
struct Outcome {
    unsigned value = 0;
    long double probability = 0, exact_weight = 0;
    float returned_weight = 0;
};
struct Candidate {
    float weight;
    long double target;
};
long double update(reservoir::State &state, Candidate candidate, long double selector) {
    const auto [weight, target] = candidate;
    const long double score = static_cast<long double>(weight) * static_cast<float>(target);
    const long double ideal_new = score / (static_cast<long double>(state.score_sum) + score);
    const bool want_new = selector < ideal_new;
    auto pending = reservoir::prepare_update(state, weight, static_cast<float>(target));
    require(pending.status == reservoir::Status::ok, "Valid finite-domain preparation failed");
    long double probability_new = 1;
    if (!pending.complete) {
        const long double rare =
            std::ldexp(static_cast<long double>(pending.rare_probability.significand),
                       -static_cast<int>(pending.rare_probability.bit_count));
        require(rare > 0 && rare <= .5L,
                "Nonempty selector must represent the smaller supported event");
        require(pending.select_new_on_success == (score <= state.score_sum),
                "Smaller-event orientation");
        const long double ideal_rare = std::min(ideal_new, 1 - ideal_new);
        require(std::abs(rare - ideal_rare) <= 3e-7L * ideal_rare,
                "Dyadic probability versus independent rational ratio");
        probability_new = pending.select_new_on_success ? rare : 1 - rare;
        const bool want_rare = want_new == pending.select_new_on_success;
        for (unsigned word = 0; !pending.complete && word < reservoir::maximum_random_words; ++word)
            require(reservoir::feed_random_word(pending, want_rare ? 0u : 0xffffffffu) ==
                        reservoir::Status::ok,
                    "Finite-domain random-word comparison failed");
    }
    require(pending.complete && pending.selected == want_new,
            "Selector interval chose wrong payload");
    const auto result = reservoir::commit_update(state, pending);
    require(result.status == reservoir::Status::ok && state.status == reservoir::Status::ok,
            "Valid finite-domain commit failed");
    return probability_new;
}
reservoir::Status rejected_update(reservoir::State &state, float weight, float target) {
    return reservoir::commit_update(state, reservoir::prepare_update(state, weight, target)).status;
}
float finish(const reservoir::State &state, unsigned count, unsigned selected) {
    require(state.candidate_count == count && state.selected_index == selected,
            "Every scheduled candidate must count exactly once");
    const auto result = reservoir::finalize(state);
    require(result.status == reservoir::Status::ok && std::isfinite(result.value) &&
                result.value > 0,
            "Normalized selected UCW must be finite positive");
    return result.value;
}
std::vector<Outcome> fresh_pool() {
    std::vector<Outcome> outcomes;
    // Each candidate pair has mass 1/9 in the normalized uniform base measure.
    // Split the selector's unit interval analytically at A[j]/(A[i]+A[j]);
    // midpoint calls exercise both intervals without relying on production probabilities.
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            const long double p_new = target_a[j] / (target_a[i] + target_a[j]);
            for (unsigned choose_new = 0; choose_new < 2; ++choose_new) {
                reservoir::State state;
                update(state, {1, target_a[i]}, .375L);
                const auto actual_p =
                    update(state, {1, target_a[j]}, choose_new ? p_new / 2 : (1 + p_new) / 2);
                const unsigned value = choose_new ? j : i;
                const auto weight = finish(state, 2, choose_new);
                outcomes.push_back({value, (choose_new ? actual_p : 1 - actual_p) / 9,
                                    (target_a[i] + target_a[j]) / (2 * target_a[value]), weight});
            }
        }
    return outcomes;
}
void indicator_masses(const std::vector<Outcome> &outcomes, std::string_view label) {
    std::array<long double, 3> mass{};
    long double probability = 0, signed_integral = 0;
    constexpr std::array<long double, 3> h{-3, 5, 11};
    for (const auto &outcome : outcomes) {
        probability += outcome.probability;
        mass[outcome.value] += outcome.probability * outcome.returned_weight;
        signed_integral += outcome.probability * outcome.returned_weight * h[outcome.value];
    }
    near(probability, 1, "Enumerated selector probability partition");
    // Three independent indicator functions span every h on this finite domain.
    // The dyadic selector intervals are integrated exactly. Agreement with the
    // ideal 1/3 proper-weighting identity allows the declared binary32 target,
    // probability and normalizer rounding; it is not a claim of exact float unbiasedness.
    for (const auto value : mass)
        near(value, 1.L / 3, label);
    near(signed_integral, 13.L / 3, "Arbitrary signed h integral");
}
std::vector<Outcome> reuse_pool(const std::vector<Outcome> &old, bool duplicate) {
    std::vector<Outcome> outcomes;
    for (const auto &source : old)
        for (unsigned fresh = 0; fresh < 3; ++fresh) {
            const long double old_score = source.exact_weight * target_b[source.value];
            const unsigned imported_count = duplicate ? 2 : 1;
            const long double old_mass = imported_count * old_score;
            const long double p_fresh = target_b[fresh] / (old_mass + target_b[fresh]);
            // The duplicated imports are correlated copies of ONE old selected
            // sample. Enumerate both second-import selector intervals anyway.
            for (unsigned second_import = 0; second_import < imported_count; ++second_import)
                for (unsigned choose_fresh = 0; choose_fresh < 2; ++choose_fresh) {
                    reservoir::State state;
                    update(state, {source.returned_weight, target_b[source.value]}, .375L);
                    if (duplicate)
                        update(state, {source.returned_weight, target_b[source.value]},
                               second_import ? .25L : .75L);
                    const auto actual_p = update(state, {1, target_b[fresh]},
                                                 choose_fresh ? p_fresh / 2 : (1 + p_fresh) / 2);
                    const unsigned selected =
                        choose_fresh ? imported_count : (duplicate && second_import ? 1 : 0);
                    const unsigned value = choose_fresh ? fresh : source.value;
                    const auto weight = finish(state, imported_count + 1, selected);
                    const auto probability = source.probability / (3 * imported_count) *
                                             (choose_fresh ? actual_p : 1 - actual_p);
                    outcomes.push_back(
                        {value, probability,
                         (old_mass + target_b[fresh]) / ((imported_count + 1) * target_b[value]),
                         weight});
                }
        }
    return outcomes;
}
void black_paths() {
    long double correct = 0, skipped_control = 0;
    constexpr std::array<long double, 2> f{0, 2};
    // target_xyz's positive support floor keeps black candidates valid: targets 1 and 3.
    constexpr std::array<long double, 2> target{1, 3};
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            const auto p_new = target[j] / (target[i] + target[j]);
            for (unsigned choose_new = 0; choose_new < 2; ++choose_new) {
                reservoir::State state;
                update(state, {1, target[i]}, .375L);
                const auto actual_p =
                    update(state, {1, target[j]}, choose_new ? p_new / 2 : (1 + p_new) / 2);
                const auto selected = choose_new ? j : i;
                const auto weight = finish(state, 2, choose_new);
                correct += (choose_new ? actual_p : 1 - actual_p) * f[selected] * weight / 4;
            }
            // Deliberately biased control: compact away black paths and divide
            // by the surviving K. Three of four pairs then return 2 instead of
            // preserving the full K=2 estimator's black outcomes.
            if (i || j) {
                reservoir::State skipped;
                if (i)
                    update(skipped, {1, target[i]}, .375L);
                if (j)
                    update(skipped, {1, target[j]}, .375L);
                const auto weight = reservoir::finalize(skipped);
                require(weight.status == reservoir::Status::ok, "Black-skip control setup");
                skipped_control += 2 * weight.value / 4;
            }
        }
    near(correct, 1, "Complete K=2 black-path mean");
    near(skipped_control, 1.5L, "Deliberate surviving-K biased control");
}
std::array<unsigned, reservoir::maximum_random_words> random_prefix(long double unit_value) {
    std::array<unsigned, reservoir::maximum_random_words> words{};
    for (auto &word : words) {
        unit_value = std::ldexp(unit_value, 32);
        const auto digit = std::floor(unit_value);
        word = static_cast<unsigned>(digit);
        unit_value -= digit;
    }
    return words;
}
void feed_prefix(reservoir::PreparedUpdate &pending, long double unit_value) {
    const auto words = random_prefix(unit_value);
    for (const auto word : words) {
        if (pending.complete)
            break;
        require(reservoir::feed_random_word(pending, word) == reservoir::Status::ok,
                "Dyadic prefix comparison failed");
    }
    require(pending.complete && pending.words_used <= reservoir::maximum_random_words,
            "Every valid dyadic decision must terminate within nine independent words");
}
void selector_boundaries() {
    // Exact rational 1/4, its complement, 1/2, and both HDR orderings.
    // The largest exponent gap requires nine words rather than a float selector.
    for (const auto &[old_target, new_target] :
         {std::array<float, 2>{3, 1},
          {1, 3},
          {1, 1},
          {1e-6f, 100},
          {100, 1e-6f},
          {std::numeric_limits<float>::min(), std::numeric_limits<float>::max()},
          {std::numeric_limits<float>::max(), std::numeric_limits<float>::min()}}) {
        reservoir::State original;
        auto initial = reservoir::prepare_update(original, 1, old_target);
        require(initial.complete && initial.selected && initial.words_used == 0 &&
                    reservoir::commit_update(original, initial).status == reservoir::Status::ok,
                "First supported candidate selects without random words");
        const auto prototype = reservoir::prepare_update(original, 1, new_target);
        require(prototype.status == reservoir::Status::ok && !prototype.complete,
                "HDR sum absorption must preserve a supported random decision");
        const long double smaller =
            std::min(static_cast<long double>(old_target), static_cast<long double>(new_target));
        const long double ideal_probability =
            smaller / (static_cast<long double>(old_target) + new_target);
        const auto significand = prototype.rare_probability.significand;
        const auto exponent = -static_cast<int>(prototype.rare_probability.bit_count);
        const auto probability = std::ldexp(static_cast<long double>(significand), exponent);
        require(probability > 0 && probability <= .5L &&
                    std::abs(probability - ideal_probability) <= 3e-7L * ideal_probability,
                "HDR smaller event retains positive support and correct rational probability");
        for (int offset = -1; offset <= 1; ++offset) {
            auto state = original;
            auto pending = prototype;
            // Decode the supplied dyadic as a long-double number, then create
            // base-2^32 digits independently of the production bit comparator.
            feed_prefix(pending,
                        std::ldexp(static_cast<long double>(significand) + offset, exponent));
            const bool rare_success = offset < 0;
            require(pending.selected == (rare_success == prototype.select_new_on_success),
                    "Exact threshold equality belongs to failure; lower prefix succeeds");
            if (offset == 0)
                require(pending.compared_bits == prototype.rare_probability.bit_count &&
                            pending.words_used == (prototype.rare_probability.bit_count + 31) / 32,
                        "Equal prefix consumes the complete represented dyadic");
            const auto result = reservoir::commit_update(state, pending);
            require(result.status == reservoir::Status::ok && state.candidate_count == 2 &&
                        state.selected_index == (pending.selected ? 1u : 0u),
                    "HDR candidate count and selected payload");
            const auto weight = reservoir::finalize(state);
            // The extreme rare selection's mathematically valid UCW exceeds
            // float range. This is an explicit output failure, not lost support.
            const auto selected_target = pending.selected ? new_target : old_target;
            const long double ideal_weight = (static_cast<long double>(old_target) + new_target) /
                                             (2 * static_cast<long double>(selected_target));
            if (ideal_weight > std::numeric_limits<float>::max())
                require(weight.status == reservoir::Status::arithmetic_failure,
                        "Unrepresentable selected UCW fails visibly");
            else
                require(weight.status == reservoir::Status::ok && weight.value > 0,
                        "Representable HDR selected UCW remains publishable");
        }
    }
}
struct CoefficientBounds {
    long double lower = 1, upper = 1;
};
CoefficientBounds coefficient_bounds(unsigned count) {
    // Independent evaluation of the documented conditional coefficient bounds.
    constexpr long double unit_roundoff = 0x1p-24L;
    return {std::pow(1 - 3 * unit_roundoff, static_cast<int>(count - 1)) *
                std::pow(1 - unit_roundoff, 3),
            std::pow(1 + 3 * unit_roundoff, static_cast<int>(count - 1)) *
                std::pow(1 + unit_roundoff, 3)};
}
long double decision_probability(const reservoir::PreparedUpdate &pending) {
    if (pending.complete)
        return 1;
    const auto rare = std::ldexp(static_cast<long double>(pending.rare_probability.significand),
                                 -static_cast<int>(pending.rare_probability.bit_count));
    return pending.select_new_on_success ? rare : 1 - rare;
}
void choose(reservoir::State &state, reservoir::PreparedUpdate pending, bool select_new) {
    require(pending.status == reservoir::Status::ok, "Analytical branch preparation failed");
    if (!pending.complete) {
        const bool rare_success = select_new == pending.select_new_on_success;
        for (unsigned i = 0; !pending.complete && i < reservoir::maximum_random_words; ++i)
            require(reservoir::feed_random_word(pending, rare_success ? 0u : 0xffffffffu) ==
                        reservoir::Status::ok,
                    "Analytical branch random words failed");
    }
    require(pending.complete && pending.selected == select_new &&
                reservoir::commit_update(state, pending).status == reservoir::Status::ok,
            "Analytical branch commit failed");
}
void capped_absorption() {
    const unsigned count = reservoir::maximum_candidate_count;
    require(count >= 5 && count <= 32,
            "Local reservoir cap must remain within the qualified bound");
    const auto bounds = coefficient_bounds(count);
    require(bounds.upper - 1 < 5.723e-6L && 1 - bounds.lower <= bounds.upper - 1,
            "Positive/negative coefficient bounds at the local cap");
    std::array<long double, 2> indicator_mass{};
    long double max_coefficient_error = 0;
    for (const bool first_a : {false, true}) {
        std::vector<float> target(count);
        std::vector<unsigned> value(count);
        std::vector<long double> p_new(count);
        reservoir::State state;
        for (unsigned candidate = 0; candidate < count; ++candidate) {
            // U is uniform A/B. X0=U and every later Xi=1-U: each candidate is
            // proper but maximally correlated. In the A-first arm, B scores are
            // individually absorbed by float addition to the initial 2^24 score.
            value[candidate] = (candidate == 0 ? first_a : !first_a) ? 0 : 1;
            target[candidate] = value[candidate] == 0 ? 0x1p24f : 1.f;
            auto pending = reservoir::prepare_update(state, 1, target[candidate]);
            require(pending.status == reservoir::Status::ok, "Capped HDR stream preparation");
            p_new[candidate] = decision_probability(pending);
            choose(state, pending, candidate == 0);
        }
        require(state.candidate_count == count, "Absorbed scores still count all scheduled paths");
        long double branch_probability_sum = 0;
        for (unsigned selected = 0; selected < count; ++selected) {
            long double probability = p_new[selected];
            for (unsigned later = selected + 1; later < count; ++later)
                probability *= 1 - p_new[later];
            branch_probability_sum += probability;
            // Score accumulation is selection-independent. Every index has
            // positive probability, so this enumerates every reachable final
            // selected-target state without enumerating 2^(K-1) selector histories.
            auto selected_state = state;
            selected_state.selected_index = selected;
            selected_state.selected_target = target[selected];
            const auto weight = finish(selected_state, count, selected);
            const long double coefficient = count * probability * weight;
            constexpr auto oracle_slack = 64 * std::numeric_limits<long double>::epsilon();
            require(
                coefficient >= bounds.lower - oracle_slack &&
                    coefficient <= bounds.upper + oracle_slack,
                "Every absorbed-stream conditional coefficient lies within its analytical bound");
            max_coefficient_error = std::max(max_coefficient_error, std::abs(coefficient - 1));
            indicator_mass[value[selected]] += probability * weight / 2;
        }
        near(branch_probability_sum, 1, "All capped absorption selection branches accounted");
    }
    const long double eta = bounds.upper - 1;
    for (const auto mass : indicator_mass)
        require(std::abs(mass - .5L) <= .5L * eta, "Capped correlated indicator measure bound");
    const auto signed_mean = -3 * indicator_mass[0] + 5 * indicator_mass[1];
    require(std::abs(signed_mean - 1) <= 4 * eta, "Capped correlated signed-h measure bound");
    reservoir::State historical_limit;
    historical_limit.score_sum = 1;
    historical_limit.selected_target = 1;
    historical_limit.selected_index = 0;
    historical_limit.candidate_count = 1u << 24;
    require(!reservoir::valid_state(historical_limit) &&
                rejected_update(historical_limit, 1, 1) == reservoir::Status::invalid_state,
            "Historical 2^24-candidate counterexample is outside the accepted local contract");
    std::cout << std::setprecision(12) << "Capped absorption K=" << count
              << " mean=" << indicator_mass[0] + indicator_mass[1]
              << " max coefficient error=" << max_coefficient_error << " eta=" << eta << '\n';
}
using Distribution = std::map<std::pair<unsigned, std::uint32_t>, long double>;
Distribution distribution(const std::vector<Outcome> &outcomes) {
    Distribution result;
    for (const auto &outcome : outcomes)
        result[{outcome.value, std::bit_cast<std::uint32_t>(outcome.returned_weight)}] +=
            outcome.probability;
    return result;
}
Distribution temporal(const Distribution &history, const Targets &target) {
    Distribution result;
    for (const auto &[key, source_probability] : history) {
        const auto [old_value, weight_bits] = key;
        const auto old_weight = std::bit_cast<float>(weight_bits);
        for (unsigned fresh = 0; fresh < 3; ++fresh) {
            reservoir::State state;
            choose(
                state,
                reservoir::prepare_update(state, old_weight, static_cast<float>(target[old_value])),
                true);
            const auto pending =
                reservoir::prepare_update(state, 1, static_cast<float>(target[fresh]));
            require(pending.status == reservoir::Status::ok,
                    "Multigeneration temporal preparation");
            const auto p_fresh = decision_probability(pending);
            for (const bool select_fresh : {false, true}) {
                auto selected = state;
                choose(selected, pending, select_fresh);
                const auto weight = finish(selected, 2, select_fresh ? 1 : 0);
                const unsigned value = select_fresh ? fresh : old_value;
                result[{value, std::bit_cast<std::uint32_t>(weight)}] +=
                    source_probability / 3 * (select_fresh ? p_fresh : 1 - p_fresh);
            }
        }
    }
    return result;
}
Distribution fixed_spatial(const Distribution &temporal_result, const Targets &target) {
    Distribution result;
    for (const auto &[key, probability] : temporal_result) {
        const auto [value, weight_bits] = key;
        const auto weight = std::bit_cast<float>(weight_bits);
        reservoir::State state;
        // Fixed five-site stencil with perfectly correlated identical inputs.
        // Every selection history has the same payload, selected target and
        // normalizer, so its probability mass collapses exactly to one outcome.
        for (unsigned candidate = 0; candidate < 5; ++candidate)
            choose(state,
                   reservoir::prepare_update(state, weight, static_cast<float>(target[value])),
                   candidate == 0);
        const auto spatial_weight = finish(state, 5, 0);
        result[{value, std::bit_cast<std::uint32_t>(spatial_weight)}] += probability;
    }
    return result;
}
long double measure_error(const Distribution &outcomes, long double bound, std::string_view label) {
    std::array<long double, 3> mass{};
    long double probability = 0;
    for (const auto &[key, branch_mass] : outcomes) {
        const auto [value, weight_bits] = key;
        mass[value] += branch_mass * std::bit_cast<float>(weight_bits);
        probability += branch_mass;
    }
    constexpr auto oracle_slack = 256 * std::numeric_limits<long double>::epsilon();
    require(std::abs(probability - 1) <= oracle_slack,
            "Dynamic distribution preserves every selector branch");
    long double max_relative_error = 0;
    for (const auto value : mass) {
        max_relative_error = std::max(max_relative_error, std::abs(3 * value - 1));
        require(std::abs(value - 1.L / 3) <= bound / 3 + oracle_slack, label);
    }
    const auto signed_mean = -3 * mass[0] + 5 * mass[1] + 11 * mass[2];
    require(std::abs(signed_mean - 13.L / 3) <= bound * 19.L / 3 + oracle_slack,
            "Multigeneration signed h bound uses integral of |h|, not cancellation");
    return max_relative_error;
}
void multigeneration() {
    const auto eta2 = coefficient_bounds(2).upper - 1;
    const auto eta5 = coefficient_bounds(5).upper - 1;
    auto current = distribution(fresh_pool());
    long double bound = eta2;
    measure_error(current, bound, "Initial fresh-pool indicator bound");
    long double observed = 0;
    for (unsigned generation = 0; generation < 5; ++generation) {
        const auto &target = generation % 2 == 0 ? target_b : target_a;
        auto refreshed = temporal(current, target);
        // Every stage imports ONE history and ONE fixed-support fresh proposal.
        // In ideal arithmetic that halves inherited indicator error; local
        // floating coefficient error expands it only by the documented eta2.
        bound = eta2 + (1 + eta2) * bound / 2;
        observed = measure_error(refreshed, bound, "Fresh/history temporal contraction bound");
        current = fixed_spatial(refreshed, target);
        bound = eta5 + (1 + eta5) * bound;
        observed =
            std::max(observed, measure_error(current, bound, "Fixed spatial indicator bound"));
    }
    const long double stationary_bound =
        (eta5 + (1 + eta5) * eta2) / (1 - (1 + eta5) * (1 + eta2) / 2);
    require(bound <= stationary_bound && stationary_bound < 3e-6L,
            "Fresh refresh gives a finite bounded-error recurrence");
    std::cout << "Temporal/spatial generations=5 states=" << current.size()
              << " last observed relative error=" << observed << " propagated bound=" << bound
              << " stationary bound=" << stationary_bound << '\n';
}
void malformed_selector_counters() {
    reservoir::State state;
    choose(state, reservoir::prepare_update(state, 1, 1), true);
    auto incomplete = reservoir::prepare_update(state, 1, 1);
    incomplete.compared_bits = 23;
    incomplete.words_used = 0;
    require(reservoir::feed_random_word(incomplete, 0) == reservoir::Status::invalid_state &&
                reservoir::commit_update(state, incomplete).status ==
                    reservoir::Status::invalid_state,
            "Incomplete selector counters must correspond to whole consumed words");
    state = {};
    choose(state, reservoir::prepare_update(state, 1, 1), true);
    auto complete = reservoir::prepare_update(state, 1, 1);
    complete.complete = true;
    complete.words_used = 1;
    complete.compared_bits = 277;
    complete.rare_probability.bit_count = 277;
    require(reservoir::commit_update(state, complete).status == reservoir::Status::invalid_state,
            "Completed comparison cannot claim more bits than supplied random words");
}
void contracts() {
    reservoir::State empty;
    require(reservoir::valid_state(empty) &&
                reservoir::finalize(empty).status == reservoir::Status::empty,
            "Empty pool is distinct from a zero contribution");
    const auto black = reservoir::target_xyz(0, 0, 0, 1);
    const auto signed_xyz = reservoir::target_xyz(-2, 3, -4, 1);
    require(black.status == reservoir::Status::ok && black.value == 1 &&
                signed_xyz.status == reservoir::Status::ok && signed_xyz.value == 10,
            "Positive support floor and absolute XYZ target");
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();
    for (const float invalid :
         {0.f, -1.f, nan, infinity, std::numeric_limits<float>::denorm_min()}) {
        reservoir::State weight_state, target_state;
        require(rejected_update(weight_state, invalid, 1) == reservoir::Status::invalid_candidate,
                "Invalid normalized candidate weight rejected");
        require(rejected_update(weight_state, 1, 1) == reservoir::Status::invalid_candidate &&
                    reservoir::finalize(weight_state).status ==
                        reservoir::Status::invalid_candidate,
                "Invalid candidate is a sticky failure, not a discarded black sample");
        require(rejected_update(target_state, 1, invalid) == reservoir::Status::invalid_candidate,
                "Zero/invalid target cannot erase support silently");
        require(reservoir::target_xyz(1, 2, 3, invalid).status == reservoir::Status::invalid_target,
                "Invalid support floor rejected");
    }
    reservoir::State undecided;
    update(undecided, {1, 1}, .375L);
    auto pending = reservoir::prepare_update(undecided, 1, 1);
    require(reservoir::commit_update(undecided, pending).status == reservoir::Status::incomplete &&
                reservoir::finalize(undecided).status == reservoir::Status::incomplete,
            "An undecided selector must fail visibly and stickily");
    reservoir::State stale;
    auto first = reservoir::prepare_update(stale, 1, 1);
    require(reservoir::commit_update(stale, first).status == reservoir::Status::ok,
            "First candidate needs no random words");
    require(reservoir::commit_update(stale, first).status == reservoir::Status::invalid_state,
            "A committed preparation cannot be replayed against changed state");
    for (const unsigned invalid_bits : {0u, 23u, 278u}) {
        reservoir::State state;
        update(state, {1, 1}, .375L);
        auto malformed_selector = reservoir::prepare_update(state, 1, 1);
        malformed_selector.rare_probability.bit_count = invalid_bits;
        require(reservoir::feed_random_word(malformed_selector, 0) ==
                    reservoir::Status::invalid_state,
                "Dyadic selector bounds reject invalid encodings");
        require(reservoir::commit_update(state, malformed_selector).status ==
                    reservoir::Status::invalid_state,
                "Invalid selector encoding cannot publish a candidate");
    }
    for (const float invalid : {nan, infinity}) {
        require(reservoir::target_xyz(invalid, 0, 0, 1).status == reservoir::Status::invalid_target,
                "Nonfinite XYZ rejected");
    }
    require(reservoir::target_xyz(std::numeric_limits<float>::max(),
                                  std::numeric_limits<float>::max(), 0, 1)
                    .status == reservoir::Status::arithmetic_failure,
            "Unrepresentable target fails instead of silently clamping");
    reservoir::State malformed;
    malformed.candidate_count = 1;
    require(rejected_update(malformed, 1, 1) == reservoir::Status::invalid_state,
            "Malformed state must fail before payload selection");
    reservoir::State count_limit;
    count_limit.score_sum = 1;
    count_limit.selected_target = 1;
    count_limit.candidate_count = reservoir::maximum_candidate_count;
    count_limit.selected_index = 0;
    require(rejected_update(count_limit, 1, 1) == reservoir::Status::count_overflow &&
                count_limit.candidate_count == reservoir::maximum_candidate_count &&
                reservoir::finalize(count_limit).status == reservoir::Status::count_overflow,
            "Count overflow must fail without wrapping or publishing a partial estimator");
    reservoir::State overflow;
    overflow.score_sum = std::numeric_limits<float>::max();
    overflow.selected_target = std::numeric_limits<float>::min();
    overflow.candidate_count = 1;
    overflow.selected_index = 0;
    require(reservoir::finalize(overflow).status == reservoir::Status::arithmetic_failure,
            "Unrepresentable normalized UCW must fail visibly");
}
} // namespace
int main() {
    try {
        const auto fresh = fresh_pool();
        indicator_masses(fresh, "Two-fresh target-A indicator measure");
        indicator_masses(reuse_pool(fresh, false), "Temporal old+fresh target-B indicator measure");
        indicator_masses(reuse_pool(fresh, true), "Duplicated imported sample indicator measure");
        black_paths();
        contracts();
        selector_boundaries();
        capped_absorption();
        multigeneration();
        malformed_selector_counters();
        std::cout << "Reservoir finite-domain validation passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
