// SPDX-License-Identifier: GPL-3.0-only
#include "mutation_shared.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace mutation = rypath_mutation;
namespace replay = rypath_replay;
namespace reservoir = rypath_reservoir;
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
// Apple Silicon long double has binary64 precision. Compensated accumulation
// and a precision-derived oracle margin keep branch-enumeration arithmetic
// separate from the much larger production binary32 weight-error envelope.
constexpr long double oracle_margin = 64 * std::numeric_limits<long double>::epsilon();
struct Sum {
    long double sum = 0, correction = 0;
    void add(long double value) {
        const auto next = sum + value;
        correction +=
            std::abs(sum) >= std::abs(value) ? (sum - next) + value : (value - next) + sum;
        sum = next;
    }
    long double value() const { return sum + correction; }
};
bool same(const replay::ReplayState &a, const replay::ReplayState &b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}
long double dyadic(reservoir::DyadicProbability value) {
    return std::ldexp(static_cast<long double>(value.significand),
                      -static_cast<int>(value.bit_count));
}
long double acceptance(const mutation::PreparedAcceptance &pending) {
    require(pending.status == mutation::Status::ok, "Acceptance preparation failed");
    return pending.proposal_target >= pending.source_target ? 1 : dyadic(pending.smaller_ratio);
}
mutation::AcceptanceResult finish(mutation::PreparedAcceptance pending, bool accept) {
    if (pending.decision.complete) {
        require(accept && pending.decision.selected, "Certain acceptance must remain accepted");
    } else {
        for (unsigned i = 0; !pending.decision.complete && i < reservoir::maximum_random_words; ++i)
            require(mutation::feed_random_word(pending, accept ? 0u : 0xffffffffu) ==
                        mutation::Status::ok,
                    "Acceptance word comparison failed");
    }
    const auto result = mutation::finalize_acceptance(pending);
    require(result.status == mutation::Status::ok && result.accepted == accept,
            "Acceptance decision/finalization failed");
    return result;
}

void proposal_symmetry() {
    const auto original = replay::fresh({0xffffffffu, 17, 3, 5, 7});
    for (unsigned block = 0; block < 4; ++block)
        for (unsigned signs = 0; signs < 4; ++signs)
            for (const auto scale : {mutation::ProposalScale{}, mutation::ProposalScale{1, 30},
                                     mutation::ProposalScale{31, 0}}) {
                replay::Words4 words{block, signs, 0xffffffffu, 0};
                const auto moved = mutation::propose(original, words, scale);
                require(moved.status == mutation::Status::ok && !same(moved.payload, original),
                        "Every valid proposal has a nonzero integer offset");
                words.y ^= 3;
                const auto reverse = mutation::propose(moved.payload, words, scale);
                require(reverse.status == mutation::Status::ok && same(reverse.payload, original),
                        "Equal-probability sign reversal must exactly invert modulo 2^32");
                require(moved.payload.version == original.version, "Proposal preserves version");
                if (block != unsigned(mutation::Block::tail_key))
                    require(moved.payload.tail_key[0] == original.tail_key[0] &&
                                moved.payload.tail_key[1] == original.tail_key[1],
                            "Coordinate mutations preserve the keyed indirect tape");
            }
    // Independent finite-domain transition count on a four-state closed lattice.
    unsigned transitions[4][4]{};
    for (unsigned x = 0; x < 4; ++x)
        for (unsigned sign = 0; sign < 2; ++sign)
            for (unsigned magnitude = 1; magnitude <= 2; ++magnitude) {
                auto source = original;
                source.wavelength = x << 30;
                const auto moved = mutation::propose(
                    source, {unsigned(mutation::Block::wavelength), sign, magnitude - 1, 0},
                    {1, 30});
                require(moved.status == mutation::Status::ok, "Closed-lattice proposal failed");
                const unsigned expected = (x + (sign ? 4 - magnitude : magnitude)) % 4;
                require(moved.payload.wavelength == expected << 30,
                        "Closed-lattice destination versus independent modular oracle");
                ++transitions[x][expected];
            }
    for (unsigned x = 0; x < 4; ++x)
        for (unsigned y = 0; y < 4; ++y)
            require(transitions[x][y] == transitions[y][x], "Exact discrete proposal symmetry");
    for (const auto scale : {mutation::ProposalScale{0, 0}, mutation::ProposalScale{32, 0},
                             mutation::ProposalScale{1, 32}, mutation::ProposalScale{2, 30}})
        require(mutation::propose(original, {0, 0, 0, 0}, scale).status ==
                    mutation::Status::invalid_scale,
                "Reject a scale that could produce a zero or undefined wrapped offset");
    auto invalid = original;
    ++invalid.version;
    require(mutation::propose(invalid, {0, 0, 0, 0}).status == mutation::Status::invalid_payload,
            "Reject incompatible replay payload");
    for (unsigned count = 0; count <= 8; ++count)
        require(mutation::valid_step_count(count) ==
                    (count == 0 || count == 1 || count == 2 || count == 4),
                "Only fixed 0/1/2/4 mutation steps are legal");
}

void counter_domains() {
    const mutation::MutationIdentity identity{0xfedcba98u, 0xffffffffu, 0x87654321u, 123, 456};
    std::set<std::array<unsigned, 4>> counters;
    for (unsigned step = 0; step < 4; ++step)
        for (const auto domain : {mutation::Entropy::proposal, mutation::Entropy::acceptance})
            for (unsigned block = 0; block < (domain == mutation::Entropy::proposal ? 1u : 3u);
                 ++block) {
                const auto counter = mutation::decision_counter(identity, step, domain, block);
                require(counter.status == mutation::Status::ok, "Valid decision counter");
                const auto c = counter.value;
                require(c.x == identity.pixel && c.y == identity.serial_low &&
                            c.z == identity.serial_high,
                        "Preserve full pixel and accepted-submission serial identity");
                require(counters.insert({c.x, c.y, c.z, c.w}).second,
                        "Proposal/acceptance/step/block counters must not alias");
                require(c.w != unsigned(replay::DecisionDomain::selection) &&
                            c.w != unsigned(replay::FreshDomain::tail_key) &&
                            c.w != unsigned(replay::FreshDomain::film) &&
                            c.w != unsigned(replay::FreshDomain::lens) &&
                            c.w != unsigned(replay::FreshDomain::wavelength) &&
                            c.w != unsigned(replay::TailDomain::transport) &&
                            c.w != unsigned(replay::TailDomain::material) &&
                            c.w != unsigned(replay::TailDomain::medium) &&
                            c.w != unsigned(replay::TailDomain::dielectric),
                        "Mutation does not alias existing RNG domains");
                const auto generated = mutation::decision_words(identity, step, domain, block);
                const auto expected = replay::philox(c, {identity.seed_low, identity.seed_high});
                require(generated.status == mutation::Status::ok &&
                            std::memcmp(&generated.value, &expected, sizeof(expected)) == 0,
                        "Counter-to-Philox mapping");
            }
    for (unsigned ordinal = 0; ordinal < reservoir::maximum_random_words; ++ordinal) {
        const auto block =
            mutation::decision_words(identity, 3, mutation::Entropy::acceptance, ordinal / 4);
        const auto value = block.value;
        const unsigned lanes[]{value.x, value.y, value.z, value.w};
        const auto actual = mutation::acceptance_word(identity, 3, ordinal);
        require(actual.status == mutation::Status::ok && actual.value == lanes[ordinal % 4],
                "Acceptance word ordinal preserves block and lane");
    }
    auto next = identity;
    ++next.serial_high;
    require(mutation::decision_counter(next, 0, mutation::Entropy::proposal, 0).value.z !=
                mutation::decision_counter(identity, 0, mutation::Entropy::proposal, 0).value.z,
            "High submission-serial bits cannot be dropped");
    require(mutation::decision_words(identity, 4, mutation::Entropy::proposal).status ==
                mutation::Status::invalid_step,
            "Bound mutation step ordinal");
    require(mutation::decision_words(identity, 0, mutation::Entropy::proposal, 1).status ==
                    mutation::Status::invalid_domain &&
                mutation::acceptance_word(identity, 0, 9).status ==
                    mutation::Status::invalid_domain &&
                mutation::decision_words(identity, 0, mutation::Entropy(99)).status ==
                    mutation::Status::invalid_domain,
            "Reject out-of-domain entropy addresses");
}

using Targets = std::array<float, 4>;
struct Outcome {
    unsigned state;
    long double probability;
    float weight;
};
std::vector<Outcome> step(const std::vector<Outcome> &source, Targets targets, bool correct) {
    std::vector<Outcome> output;
    for (const auto &old : source)
        for (unsigned sign = 0; sign < 2; ++sign)
            for (unsigned magnitude = 1; magnitude <= 2; ++magnitude) {
                const unsigned next = (old.state + (sign ? 4 - magnitude : magnitude)) % 4;
                const auto pending =
                    mutation::prepare_acceptance(old.weight, targets[old.state], targets[next]);
                const long double a = acceptance(pending);
                const long double ideal =
                    std::min(1.L, static_cast<long double>(targets[next]) / targets[old.state]);
                require(std::abs(a - ideal) <= std::ldexp(1.L, -24) * ideal,
                        "Represented MH acceptance versus independent long-double ratio");
                if (a < 1) {
                    const auto rejected = finish(pending, false);
                    require(rejected.weight == old.weight, "Rejected UCW remains unchanged");
                    output.push_back({old.state, old.probability * (1 - a) / 4, old.weight});
                }
                const auto accepted = finish(pending, true);
                output.push_back(
                    {next, old.probability * a / 4, correct ? accepted.weight : old.weight});
            }
    return output;
}
void measure_oracles() {
    // Exact initial probability*weight=1/4 in each state, deliberately not at
    // the target's stationary distribution: no startup/burn-in assumption.
    const std::vector<Outcome> initial{{0, .125L, 2}, {1, .125L, 2}, {2, .25L, 1}, {3, .5L, .5f}};
    for (const auto targets : {Targets{1, 2, 4, 8}, Targets{1, 3, 7, 10}})
        for (const unsigned count : {0u, 1u, 2u, 4u}) {
            auto outcomes = initial;
            for (unsigned i = 0; i < count; ++i)
                outcomes = step(outcomes, targets, true);
            std::array<Sum, 4> mass{};
            Sum probability, signed_integral;
            constexpr std::array<int, 4> h{-7, 2, 11, -3};
            for (const auto &outcome : outcomes) {
                probability.add(outcome.probability);
                mass[outcome.state].add(outcome.probability * outcome.weight);
                signed_integral.add(outcome.probability * outcome.weight * h[outcome.state]);
            }
            require(std::abs(probability.value() - 1) <= oracle_margin,
                    "Transition probability partition");
            const auto bound = std::pow(1.L + std::ldexp(1.L, -24), count) - 1;
            for (const auto value : mass)
                require(std::abs(value.value() - .25L) <= bound / 4 + oracle_margin,
                        "Finite-state indicator UCW within the accumulated normal-rounding bound");
            require(std::abs(signed_integral.value() - .75L) <= bound * 23 / 4 + 23 * oracle_margin,
                    "Arbitrary signed integrand preserves weighted measure");
        }
    const auto control = step(initial, {1, 3, 7, 10}, false);
    std::array<long double, 4> wrong_mass{};
    for (const auto &outcome : control)
        wrong_mass[outcome.state] += outcome.probability * outcome.weight;
    require(std::any_of(wrong_mass.begin(), wrong_mass.end(),
                        [](auto value) { return std::abs(value - .25L) > .01L; }),
            "Omitting the UCW correction must fail the indicator oracle");

    constexpr Targets targets{1, 3, 7, 10};
    for (unsigned x = 0; x < 4; ++x)
        for (unsigned y = 0; y < 4; ++y) {
            const auto forward = mutation::prepare_acceptance(1, targets[x], targets[y]);
            const auto reverse = mutation::prepare_acceptance(1, targets[y], targets[x]);
            const auto a = acceptance(forward), b = acceptance(reverse);
            const long double flow_a = targets[x] * a, flow_b = targets[y] * b;
            require(std::abs(flow_a - flow_b) <= std::ldexp(1.L, -24) * std::max(flow_a, flow_b),
                    "Target detailed balance within declared probability rounding");
            const auto corrected = finish(forward, true);
            require(std::abs(a * corrected.weight - b) <= std::ldexp(1.L, -24) * b,
                    "Actual dyadic reverse-flow correction, independently of target rounding");
        }
}

using RandomWords = std::array<unsigned, reservoir::maximum_random_words>;
RandomWords prefix(unsigned numerator, unsigned bit_count) {
    RandomWords words{};
    for (unsigned i = 0; i < bit_count; ++i) {
        const unsigned leading = bit_count - 24;
        const unsigned bit = i < leading ? 0 : (numerator >> (bit_count - i - 1)) & 1u;
        words[i / 32] |= bit << (31 - i % 32);
    }
    return words;
}
mutation::AcceptanceResult decide(mutation::PreparedAcceptance pending, RandomWords words) {
    for (const auto word : words) {
        if (pending.decision.complete)
            break;
        require(mutation::feed_random_word(pending, word) == mutation::Status::ok,
                "Boundary word feed");
    }
    const auto result = mutation::finalize_acceptance(pending);
    require(result.status == mutation::Status::ok, "Boundary decision completion");
    return result;
}
void extreme_ratios_and_boundaries() {
    const float minimum = std::numeric_limits<float>::min();
    const float maximum = std::numeric_limits<float>::max();
    for (const auto values :
         {std::array<float, 3>{1, 2, 1}, {1, 100, 1e-6f}, {minimum, maximum, minimum}}) {
        const auto pending = mutation::prepare_acceptance(values[0], values[1], values[2]);
        require(pending.status == mutation::Status::ok && !pending.decision.complete,
                "Supported downhill ratio, including below-float-normal acceptance");
        const auto ratio = pending.smaller_ratio;
        const auto below = decide(pending, prefix(ratio.significand - 1, ratio.bit_count));
        const auto equal = decide(pending, prefix(ratio.significand, ratio.bit_count));
        auto above_words = prefix(ratio.significand + 1, ratio.bit_count);
        if (ratio.significand == 0x00ffffffu)
            above_words.fill(0xffffffffu);
        const auto above = decide(pending, above_words);
        require(below.accepted && !equal.accepted && !above.accepted,
                "Exact dyadic below/equal/above acceptance boundaries");
        require(equal.weight == values[0] && above.weight == values[0],
                "Boundary rejection preserves weight");
        const long double expected = static_cast<long double>(values[0]) / dyadic(ratio);
        require(std::abs(below.weight - expected) <= std::ldexp(1.L, -24) * expected,
                "Extreme exponent cancellation preserves representable corrected UCW");
        if (values[1] == maximum)
            require(equal.words_used == 9 && ratio.bit_count == 277,
                    "Extreme ratio equality needs bounded nine-word support");
    }
    const auto uphill = mutation::prepare_acceptance(maximum, minimum, maximum);
    require(uphill.status == mutation::Status::ok && uphill.decision.complete &&
                uphill.smaller_ratio.bit_count == 277,
            "Certain uphill acceptance retains tiny reverse probability");
    const auto corrected = finish(uphill, true);
    require(corrected.words_used == 0 && reservoir::positive_normal(corrected.weight),
            "Uphill correction cancels extreme ratio without intermediate underflow");
    const long double expected = static_cast<long double>(maximum) * dyadic(uphill.smaller_ratio);
    require(std::abs(corrected.weight - expected) <= std::ldexp(1.L, -24) * expected,
            "Uphill actual reverse-probability weight");
    const auto equal = finish(mutation::prepare_acceptance(3, 7, 7), true);
    require(equal.weight == 3 && equal.words_used == 0, "Equal targets are an exact unit move");
}

void normal_exponent_domain() {
    const auto value = [](unsigned bits) { return std::bit_cast<float>(bits); };
    const float minimum = std::numeric_limits<float>::min();
    const float maximum = std::numeric_limits<float>::max();
    const float target = value(0x3f813881u);

    // Reviewer witness: fractional multiplication then subnormal ldexp used to
    // round back to FLT_MIN, with error 1.4784242 * 2^-24. Normal output alone
    // therefore does not establish a single normal rounding operation.
    auto witness = mutation::prepare_acceptance(value(0x00813880u), 1, target);
    require(witness.smaller_ratio.significand == (0x3f7d94e6u & 0x007fffffu) + 0x00800000u &&
                witness.smaller_ratio.bit_count == 24,
            "Boundary witness uses the independently specified actual dyadic probability");
    require(witness.status == mutation::Status::arithmetic_failure &&
                witness.decision.words_used == 0 &&
                mutation::feed_random_word(witness, 0xffffffffu) ==
                    mutation::Status::arithmetic_failure &&
                mutation::finalize_acceptance(witness).status ==
                    mutation::Status::arithmetic_failure,
            "Reject scaled subnormal witness before observing acceptance entropy");

    const auto adjacent = mutation::prepare_acceptance(value(0x00813881u), 1, target);
    require(adjacent.status == mutation::Status::ok, "Adjacent normal boundary must stay valid");
    const auto accepted = finish(adjacent, true);
    require(accepted.weight == minimum, "Adjacent valid scaled fraction produces exact FLT_MIN");
    const long double exact =
        static_cast<long double>(value(0x00813881u)) * static_cast<long double>(value(0x3f7d94e6u));
    require(std::abs(accepted.weight - exact) <= std::ldexp(1.L, -24) * exact,
            "Adjacent normal witness obeys the one-operation error bound");
    require(mutation::prepare_acceptance(value(0x00ffffffu), 1, 2).status ==
                mutation::Status::arithmetic_failure,
            "Reject exact half-ULP-below-minimum scaling even when ldexp could round upward");
    const auto minimum_exact = finish(mutation::prepare_acceptance(value(0x01000000u), 1, 2), true);
    require(minimum_exact.weight == minimum, "Exact lower exponent boundary is valid");

    // Dividing by 1/2 scales by two. These adjacent source floats land at
    // FLT_MAX and at the first overflowing exponent respectively. The latter
    // must fail before ldexp even on a toward-zero backend that saturates it.
    const auto maximum_exact = finish(mutation::prepare_acceptance(value(0x7effffffu), 2, 1), true);
    require(maximum_exact.weight == maximum, "Exact upper exponent boundary is valid");
    auto overflow = mutation::prepare_acceptance(value(0x7f000000u), 2, 1);
    require(overflow.status == mutation::Status::arithmetic_failure &&
                overflow.decision.words_used == 0 &&
                mutation::feed_random_word(overflow, 0xffffffffu) ==
                    mutation::Status::arithmetic_failure &&
                mutation::finalize_acceptance(overflow).status ==
                    mutation::Status::arithmetic_failure,
            "First overflowing final exponent fails before a rejection can hide it");
}

void failure_contracts() {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    for (const auto bad : {0.f, -1.f, nan, inf, std::numeric_limits<float>::denorm_min()}) {
        require(mutation::prepare_acceptance(bad, 1, 1).status == mutation::Status::invalid_weight,
                "Reject invalid UCW without substitution");
        require(
            mutation::prepare_acceptance(1, bad, 1).status == mutation::Status::invalid_target &&
                mutation::prepare_acceptance(1, 1, bad).status == mutation::Status::invalid_target,
            "Reject invalid source/proposal targets");
    }
    const auto minimum = std::numeric_limits<float>::min();
    const auto maximum = std::numeric_limits<float>::max();
    for (const auto values : {std::array<float, 3>{1, minimum, maximum}, {1, maximum, minimum}}) {
        auto failed = mutation::prepare_acceptance(values[0], values[1], values[2]);
        require(failed.status == mutation::Status::arithmetic_failure &&
                    mutation::feed_random_word(failed, 0xffffffffu) ==
                        mutation::Status::arithmetic_failure &&
                    mutation::finalize_acceptance(failed).status ==
                        mutation::Status::arithmetic_failure,
                "Unrepresentable accepted weight fails before observing rejection entropy");
    }
    auto pending = mutation::prepare_acceptance(1, 3, 1);
    require(mutation::finalize_acceptance(pending).status == mutation::Status::incomplete,
            "Incomplete acceptance is an explicit error");
    pending.decision.compared_bits = 23;
    require(mutation::feed_random_word(pending, 0) == mutation::Status::invalid_decision,
            "Reject malformed entropy counters");
    pending = mutation::prepare_acceptance(1, 3, 1);
    require(mutation::feed_random_word(pending, 0) == mutation::Status::ok,
            "Valid acceptance decision");
    ++pending.accepted_weight;
    require(mutation::finalize_acceptance(pending).status == mutation::Status::invalid_decision,
            "Reject inconsistent cached accepted weight");
    pending = mutation::prepare_acceptance(1, 3, 1);
    require(mutation::feed_random_word(pending, 0) == mutation::Status::ok,
            "Valid acceptance for malformed completed-counter control");
    pending.decision.compared_bits = 277;
    require(mutation::finalize_acceptance(pending).status == mutation::Status::invalid_decision,
            "Reject completed decision claiming more bits than its supplied words");
}
} // namespace

int main() {
    try {
        proposal_symmetry();
        counter_domains();
        measure_oracles();
        extreme_ratios_and_boundaries();
        normal_exponent_domain();
        failure_contracts();
        std::cout
            << "Replay mutation proposal, weighted-measure, balance and boundary oracles pass\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
