// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_reservoir_shared.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace {
namespace er = rypath_enhanced_reservoir;
namespace old = rypath_reservoir;
namespace reuse = rypath_reuse_mis;
using N = er::Number;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
N number(float f, int exponent = 0) {
    int shift = 0;
    const auto fraction = std::frexp(f, &shift);
    return {fraction, exponent + shift};
}
// Independent host representation: no production sum/product/ratio helper.
N reference_sum(N a, N b) {
    if (a.fraction == 0)
        return b;
    if (b.fraction == 0)
        return a;
    const int e = std::max(a.exponent, b.exponent);
    const double sum = std::ldexp(double(a.fraction), a.exponent - e) +
                       std::ldexp(double(b.fraction), b.exponent - e);
    int shift = 0;
    const float f = std::frexp(static_cast<float>(sum), &shift);
    return {f, e + shift};
}
old::DyadicProbability reference_ratio(N small, N large) {
    int exponent = 0;
    const float fraction = std::frexp(small.fraction / large.fraction, &exponent);
    return {(std::bit_cast<unsigned>(fraction) & 0x7fffffu) | 0x800000u,
            unsigned(24 - (small.exponent - large.exponent + exponent))};
}
// Exact bit-vector threshold +/-1 in units 2^-B, independent of selector code.
std::vector<unsigned> threshold_words(old::DyadicProbability p, int offset) {
    std::vector<unsigned> bits(p.bit_count, 0);
    for (unsigned k = 0; k < 24; ++k)
        bits[p.bit_count - 24 + k] = (p.significand >> (23 - k)) & 1u;
    if (offset != 0) {
        bool carry = true;
        for (unsigned k = p.bit_count; k > 0 && carry; --k) {
            const auto previous = bits[k - 1];
            bits[k - 1] ^= 1u;
            carry = offset > 0 ? previous == 1 : previous == 0;
        }
        require(!carry, "threshold adjustment in range");
    }
    std::vector<unsigned> words((p.bit_count + 31) / 32, 0);
    for (unsigned k = 0; k < p.bit_count; ++k)
        words[k / 32] |= bits[k] << (31 - k % 32);
    return words;
}
void consume(er::PreparedUpdate &p, const std::vector<unsigned> &words) {
    for (const auto word : words) {
        if (p.decision.complete)
            break;
        require(er::feed_random_word(p, word) == er::Status::ok, "reservoir feed");
    }
    require(p.decision.complete, "reservoir complete");
}
er::State first(N score, N target = {.5f, 1}) {
    er::State s;
    const auto p = er::prepare_score(s, score, target);
    const auto r = er::commit_update(s, p);
    require(r.status == er::Status::ok && r.selected && s.candidate_count == 1,
            "first positive selects without entropy");
    return s;
}
void arithmetic() {
    static_assert(sizeof(N) == 8);
    static_assert(er::maximum_probability_bits == 8216 && er::maximum_random_words == 257);
    for (int exponent : {-4096, -149, 0, 128, 4096}) {
        const N a{.625f, exponent}, b{.875f, exponent - (exponent == -4096 ? 0 : 1)};
        const auto sum = er::checked_add(a, b);
        if (exponent == 4096)
            require(sum.status == er::Status::arithmetic_failure, "sum overflow visible");
        else
            require(sum.status == er::Status::ok && er::same(sum.value, reference_sum(a, b)),
                    "independent rounded scaled sum");
        require(er::same(er::checked_add(a, {0, 0}).value, a), "right zero identity bits");
        require(er::same(er::checked_add({0, 0}, a).value, a), "left zero identity bits");
    }
    const auto tiny = er::from_float(std::numeric_limits<float>::denorm_min());
    require(tiny.status == er::Status::ok && tiny.value.fraction == .5f &&
                tiny.value.exponent == -148,
            "raw subnormal decoded");
    require(er::from_float(-std::numeric_limits<float>::denorm_min()).status != er::Status::ok,
            "negative subnormal rejected by bits");
    require(er::zero(er::from_float(-0.f).value), "input signed zero canonicalized");
    require(!er::valid_number({-0.f, 0}) && !er::valid_number({0, 1}), "serialized zero canonical");
    require(!er::positive({std::numeric_limits<float>::denorm_min(), 0}),
            "mantissa never denormal");
    require(er::checked_multiply({.5f, -4096}, {.5f, -4096}).status != er::Status::ok,
            "product underflow fails not clips");
    require(er::checked_divide({.5f, 4096}, {.5f, -4096}).status != er::Status::ok,
            "quotient out of declared range");
    require(er::checked_add({.5f, std::numeric_limits<int>::max()}, {.5f, 0}).status !=
                er::Status::ok,
            "poison exponent rejected before integer addition");
    require(er::checked_divide({0, 0}, {.5f, -4096}).status == er::Status::ok &&
                er::checked_divide({.5f, 0}, {0, 0}).status != er::Status::ok,
            "zero numerator versus invalid denominator");
    require(er::from_float(std::numeric_limits<float>::infinity()).status != er::Status::ok &&
                er::from_float(std::numeric_limits<float>::quiet_NaN()).status != er::Status::ok,
            "nonfinite float rejected");
    // Exact native failing operands, with BOTH denominator terms and quarter.
    const float Z = std::bit_cast<float>(0x03d76154u);
    const float T = std::bit_cast<float>(0x33f892fcu);
    const float inverse = std::bit_cast<float>(0x3b258c2cu);
    const auto numerator = er::checked_multiply(number(Z), number(T));
    const auto denominator = er::checked_add(number(T), number(inverse));
    require(numerator.status == er::Status::ok && denominator.status == er::Status::ok,
            "Pavilion numerator/denominator scaled");
    const auto ratio = er::checked_divide(numerator.value, denominator.value);
    const auto score = er::checked_multiply(ratio.value, number(.25f));
    const long double expected =
        static_cast<long double>(Z) * T / (static_cast<long double>(T) + inverse) / 4;
    const long double actual =
        std::ldexp(static_cast<long double>(score.value.fraction), score.value.exponent);
    require(score.status == er::Status::ok && score.value.exponent < -125 &&
                std::abs(actual - expected) <= 5e-7L * expected,
            "Pavilion exact-word quarter MIS score retained");
    const auto selected = first(score.value, number(T));
    require(er::valid_state(selected), "legal Z below target");
}
void selectors() {
    for (const int gap : {0, 1, 24, 125, 1024, 4096, 8192}) {
        const N large{.875f, gap == 8192 ? 4096 : 0};
        const N small{.5f, gap == 8192 ? -4096 : -gap};
        auto source = first(large);
        const auto expected_sum = reference_sum(large, small);
        const auto expected_ratio = reference_ratio(small, expected_sum);
        for (int offset : {-1, 0, 1}) {
            auto p = er::prepare_score(source, small, {.75f, -4000});
            require(p.status == er::Status::ok && er::same(p.score_sum, expected_sum) &&
                        er::same_probability(p.decision.rare_probability, expected_ratio),
                    "independent scaled selector ratio");
            consume(p, threshold_words(expected_ratio, offset));
            require(p.decision.selected == (offset < 0), "strict dyadic threshold choice");
            auto s = source;
            const auto r = er::commit_update(s, p);
            require(r.status == er::Status::ok && r.selected == (offset < 0) &&
                        s.selected_index == (offset < 0 ? 1u : 0u),
                    "selected payload index");
            if (gap == 8192)
                require(r.words_used == 257, "extreme rare event reaches word257");
        }
        // Complement event: a huge new score must retain the old candidate on
        // rare success, rather than round 1-p to one and erase the old path.
        source = first(small);
        auto p = er::prepare_score(source, large, {.5f, 1});
        require(!p.decision.select_new_on_success, "complement orientation");
        consume(p, threshold_words(expected_ratio, -1));
        require(!p.decision.selected, "tiny old event survives");
    }
    // Exhaust all four equiprobable two-bit intervals for scores1 and3.
    // Targets2/4 and responses2/8 give E[S*Fselected/Tselected]=7.
    long double mean = 0;
    for (unsigned k = 0; k < 4; ++k) {
        auto exact = first(number(1), number(2));
        auto decision = er::prepare_score(exact, number(3), number(4));
        consume(decision, {k << 30});
        const auto result = er::commit_update(exact, decision);
        require(result.status == er::Status::ok, "dyadic physical mean commit");
        const long double S = std::ldexp(static_cast<long double>(exact.score_sum.fraction),
                                         exact.score_sum.exponent);
        const long double T = std::ldexp(static_cast<long double>(exact.selected_target.fraction),
                                         exact.selected_target.exponent);
        const long double F = exact.selected_index == 0 ? 2 : 8;
        mean += S * F / T / 4;
    }
    require(mean == 7, "independent dyadic weighted readout mean");
    auto s = first({.5f, 0});
    auto p = er::prepare_score(s, {.5f, -1000}, {.5f, 1});
    auto bad = p;
    bad.score_sum.fraction = .75f;
    require(er::feed_random_word(bad, 0) == er::Status::invalid_state, "stale pending sum");
    bad = p;
    bad.decision.compared_bits = 1;
    require(er::feed_random_word(bad, 0) == er::Status::invalid_state, "forged progress");
    auto incomplete = s;
    require(er::commit_update(incomplete, p).status == er::Status::incomplete,
            "incomplete is failure");
    consume(p, {0xffffffffu});
    require(er::commit_update(s, p).status == er::Status::ok, "normal commit");
    require(er::commit_update(s, p).status == er::Status::invalid_state,
            "duplicate commit rejected");
    s = first({.5f, 0});
    s.candidate_count = er::maximum_positive_candidates;
    require(er::prepare_score(s, {.5f, 0}, {.5f, 0}).status == er::Status::count_overflow,
            "positive candidate cap");
    s = {};
    require(er::prepare_score(s, {0, 0}, {.5f, 0}).status != er::Status::ok &&
                er::prepare_score(s, {.5f, 0}, {0, 0}).status != er::Status::ok,
            "nulls handled by caller not positive selector");
}
void normal_range_parity() {
    for (float a : {.125f, 1.f, 17.25f})
        for (float b : {.25f, 3.f, 1048576.f})
            for (unsigned word : {0u, 0xffffffffu}) {
                auto scaled = first(number(a));
                auto p = er::prepare_score(scaled, number(b), number(1));
                old::State original{a, 1, 1, 0, old::Status::ok};
                auto q = old::prepare_update(original, b, 1);
                require(p.status == er::Status::ok && q.status == old::Status::ok &&
                            er::same(p.score_sum, number(q.score_sum)) &&
                            er::same_probability(p.decision.rare_probability, q.rare_probability) &&
                            p.decision.select_new_on_success == q.select_new_on_success,
                        "normal-range original selector policy parity");
                while (!p.decision.complete)
                    er::feed_random_word(p, word);
                while (!q.complete)
                    old::feed_random_word(q, word);
                const auto x = er::commit_update(scaled, p);
                const auto y = old::commit_update(original, q);
                require(x.status == er::Status::ok && y.status == old::Status::ok &&
                            x.selected == y.selected && x.words_used == y.words_used,
                        "normal-range selector result parity");
            }
    for (float source : {.7123456f, .9234567f})
        for (float proposal : {.7123456f, .9234567f})
            for (unsigned word : {0u, 0xffffffffu}) {
                auto p = er::prepare_normalizer_acceptance(number(1.234567f), number(source),
                                                           number(proposal));
                auto q = reuse::prepare_normalizer_acceptance(1.234567f, source, proposal);
                while (!p.decision.complete)
                    er::feed_random_word(p, word);
                while (!q.decision.complete)
                    reuse::feed_random_word(q, word);
                const auto x = er::finalize_normalizer_acceptance(p);
                const auto y = reuse::finalize_normalizer_acceptance(q);
                require(x.status == er::MutationStatus::ok &&
                            y.status == reuse::MutationStatus::ok &&
                            er::same(x.normalizer, number(y.normalizer)) &&
                            x.accepted == y.accepted && x.words_used == y.words_used,
                        "normal-range actual-law MH policy parity");
            }
}
void mh() {
    // Declared numerical tolerance: six elementary roundings, <=8 float eps
    // relative against independent long-double accepted-flow calculation.
    constexpr long double tolerance = 8.L * std::numeric_limits<float>::epsilon();
    for (const int exponent : {-4000, -149, 0, 4000}) {
        const N Z{.625f, exponent};
        const auto equal = er::prepare_normalizer_acceptance(Z, {.75f, 4096}, {.75f, 4096});
        const auto e = er::finalize_normalizer_acceptance(equal);
        require(e.status == er::MutationStatus::ok && e.accepted && e.words_used == 0 &&
                    er::same(e.normalizer, Z),
                "equal MH bit preservation");
        for (const bool uphill : {false, true}) {
            const N source{.7123456f, uphill ? -4000 : 4000};
            const N proposal{.9234567f, uphill ? 4000 : -4000};
            auto p = er::prepare_normalizer_acceptance(Z, source, proposal);
            require(p.status == er::MutationStatus::ok, "scaled MH prepare");
            const N small = uphill ? source : proposal, large = uphill ? proposal : source;
            const auto q = reference_ratio(small, large);
            require(er::same_probability(p.smaller_ratio, q), "MH independent actual ratio");
            if (!uphill)
                for (const auto w : threshold_words(q, -1)) {
                    if (p.decision.complete)
                        break;
                    require(er::feed_random_word(p, w) == er::MutationStatus::ok, "MH tiny feed");
                }
            const auto r = er::finalize_normalizer_acceptance(p);
            require(r.status == er::MutationStatus::ok && r.accepted, "MH accepted");
            // Exponents of Z cancel, allowing a robust relative reference even
            // on macOS where long double has binary64 exponent range.
            const long double actual = std::ldexp(static_cast<long double>(r.normalizer.fraction),
                                                  r.normalizer.exponent - Z.exponent) /
                                       Z.fraction;
            const long double ratio_fraction =
                static_cast<long double>(small.fraction / large.fraction);
            const long double correction = uphill
                                               ? ratio_fraction * large.fraction / small.fraction
                                               : small.fraction / (ratio_fraction * large.fraction);
            require(std::abs(actual - correction) <= tolerance * correction,
                    "actual rounded-law MH correction");
            require(actual > .999L && actual < 1.001L,
                    "negative control: must not multiply Z by extreme target ratio");
            if (!uphill) {
                auto rejection = er::prepare_normalizer_acceptance(Z, source, proposal);
                require(er::feed_random_word(rejection, 0xffffffffu) == er::MutationStatus::ok,
                        "MH rejection feed");
                const auto rejected = er::finalize_normalizer_acceptance(rejection);
                require(rejected.status == er::MutationStatus::ok && !rejected.accepted &&
                            er::same(rejected.normalizer, Z),
                        "MH rejection preserves bits");
            }
            auto stale = p;
            stale.accepted_normalizer.exponent += 1;
            require(er::finalize_normalizer_acceptance(stale).status != er::MutationStatus::ok,
                    "MH stale correction rejected");
        }
    }
    auto poison = er::prepare_normalizer_acceptance({.5f, std::numeric_limits<int>::max()},
                                                    {.5f, 0}, {.5f, 0});
    require(poison.status == er::MutationStatus::invalid_weight, "MH exponent poison");
    require(er::prepare_normalizer_acceptance({.5f, 0}, {0, 0}, {.5f, 0}).status ==
                er::MutationStatus::invalid_target,
            "MH zero target is outside positive transition helper");
}
} // namespace
int main() {
    try {
        arithmetic();
        selectors();
        normal_range_parity();
        mh();
        std::cout << "Enhanced scaled arithmetic/reservoir/MH tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
