// SPDX-License-Identifier: GPL-3.0-only
#include "area_wire.h"
#include "enhanced_confidence_shared.h"
#include "enhanced_path_shared.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace c = rypath_enhanced_confidence;
namespace e = rypath_enhanced_path;
namespace m = rypath_reservoir;
namespace er = rypath_enhanced_reservoir;
void need(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
e::Number number(float value) {
    const auto result = er::from_float(value);
    need(result.status == er::Status::ok, "Confidence score input is representable");
    return result.value;
}
long double value(e::Number number) {
    return std::ldexp(static_cast<long double>(number.fraction), number.exponent);
}
void near(long double actual, long double expected, long double tolerance, const char *message) {
    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
void recurrence() {
    const auto first = c::temporal({});
    need(first.status == c::Status::ok && first.fresh.confidence == 1 &&
             first.history.present == 0 && first.history.confidence == 0 &&
             first.output.present == 1 && first.output.confidence == 1,
         "No history initializes fresh confidence1");
    for (unsigned prior : std::array<unsigned, 6>{1u, 19u, 20u, 21u, 105u, c::maximum_confidence}) {
        const auto t = c::temporal({prior, 1});
        const auto admitted = prior < 20 ? prior : 20;
        need(t.status == c::Status::ok && t.history.confidence == admitted &&
                 t.output.confidence == admitted + 1 && t.fresh.confidence == 1,
             "Temporal caps input then adds fresh confidence");
        const auto a = c::coefficient(t.fresh), b = c::coefficient(t.history);
        need(a.status == c::Status::ok && b.status == c::Status::ok && a.value == 1 &&
                 b.value == float(admitted),
             "MIS coefficients use capped input confidence before weighting");
        // Cap AFTER weighting would use raw105 in MIS, or cap the combined
        // result to20; neither produces the prescribed history20/output21.
        if (prior == 105)
            need(b.value != float(prior) && t.output.confidence != 20,
                 "Preweight/input-cap timing negative controls");
    }
    // Independent integer recurrence: center +4 admitted neighbors, each carrying
    // min(20,previous)+1. Spatial confidence reaches105, then caps back to21 in
    // the next temporal pass. Winning ancestry or null/real realization is absent.
    unsigned reference = 1;
    c::Source previous{};
    for (unsigned frame = 0; frame < 100; ++frame) {
        const unsigned temporal_expected =
            (previous.present ? (reference < 20 ? reference : 20) : 0) + 1;
        const auto t = c::temporal(previous);
        auto spatial = c::spatial_begin(t.output);
        for (unsigned n = 0; n < 4; ++n)
            need(c::add_neighbor(spatial, t.output) == c::Status::ok, "Spatial neighbor admitted");
        const auto out = c::spatial_finish(spatial);
        reference = temporal_expected * 5;
        need(out.status == c::Status::ok && out.source.confidence == reference &&
                 spatial.admitted == 5 && spatial.considered == 5,
             "Spatial confidence sums center and all structurally admitted inputs");
        previous = out.source;
    }
    need(reference == 105, "Fixed-cap recurrence stays bounded without clipping spatial sum");
}
void structural_not_sampled() {
    const auto temporal = c::temporal({9, 1});
    need(temporal.history.confidence == 9 && temporal.output.confidence == 10,
         "Realized null retains temporal domain confidence");
    auto source = c::spatial_begin({3, 1});
    // These labels describe the consuming estimator's outcomes. The confidence
    // API intentionally has NO outcome parameter, so all keep the SAME source.
    enum class Outcome { mapped_positive, mapped_zero, undefined_inverse, selected_null };
    for (auto outcome : {Outcome::mapped_positive, Outcome::mapped_zero, Outcome::undefined_inverse,
                         Outcome::selected_null}) {
        (void)outcome;
        auto state = source;
        need(c::add_neighbor(state, {7, 1}) == c::Status::ok &&
                 c::spatial_finish(state).source.confidence == 10,
             "Pointwise shift rejection/zero/null does not remove a structural domain");
    }
    need(c::add_neighbor(source, {}) == c::Status::ok && source.considered == 2 &&
             source.admitted == 1 && c::spatial_finish(source).source.confidence == 3,
         "Preselected geometric donor rejection excludes structural confidence");
    // Independent two-state source-domain oracle, four equiprobable pairs:
    // destination F=2 at U0/U1; previous F=0/2. Coefficients1/1 remain present
    // regardless of previous selection. Removing its domain on NULL yields9/4.
    const auto coefficients = c::temporal({1, 1});
    const float fresh = c::coefficient(coefficients.fresh).value;
    const float history = c::coefficient(coefficients.history).value;
    long double correct = 0, deletion = 0;
    for (unsigned current = 0; current < 2; ++current)
        for (unsigned old = 0; old < 2; ++old) {
            const float inverse = current == 0 ? 0.f : 2.f;
            const float Zold = old == 0 ? 0.f : 2.f;
            const auto center =
                e::canonical_score(number(2), number(2), number(inverse), fresh, history);
            const auto neighbor = e::pair_score(
                {number(Zold), number(old == 0 ? 0.f : 2.f), number(2), history, fresh});
            need(center.status == m::Status::ok && neighbor.status == m::Status::ok,
                 "Complementary coefficient example is numerically valid");
            correct += (value(center.value) + value(neighbor.value)) / 4.L;
            const long double wrong_center = old == 0 ? 2 : (inverse == 0 ? 2 : 1);
            deletion += (wrong_center + value(neighbor.value)) / 4.L;
        }
    near(correct, 2, 0, "Structural null retains unbiased complementary partition");
    near(deletion, 2.25L, 0, "Deleting a null source confidence is biased");
}
void complementary_and_cap_timing() {
    const auto t = c::temporal({105, 1});
    const float fresh = c::coefficient(t.fresh).value;
    const float history = c::coefficient(t.history).value;
    // At one common chart point use target2/4 and UCWs1; both source normalizers
    // equal own target. Their weighted destination integrand is2. An independent
    // exact-rational calculation gives shares2/82 and80/82, not2/422,420/422.
    const auto center = e::canonical_score(number(2), number(2), number(4), fresh, history);
    const auto shifted = e::pair_score({number(4), number(4), number(2), history, fresh});
    near(value(center.value), 4.L / 82, 1e-8L, "Fresh complementary score with preweight cap20");
    near(value(shifted.value), 160.L / 82, 3e-7L,
         "History shifted score with same capped coefficient");
    near(value(center.value) + value(shifted.value), 2, 3e-7L,
         "Complementary partition sums to complete integrand");
    const auto wrong_history = e::pair_score({number(4), number(4), number(2), 105, fresh});
    need(std::abs(value(center.value) + value(wrong_history.value) - 2) > .03L,
         "Cap only one side/after weighting breaks complementary partition");
    const auto spatial = c::spatial_begin(t.output);
    need(c::spatial_finish(spatial).source.confidence == 21,
         "Output confidence does not depend on winner or target/UCW");
    // Four equal spatial pair weights partition center's coefficient. Confidence
    // itself is NOT divided by4; it is summed once per admitted source domain.
    auto merged = spatial;
    for (unsigned i = 0; i < 4; ++i)
        need(c::add_neighbor(merged, {5, 1}) == c::Status::ok, "Admit spatial confidence5");
    need(c::spatial_finish(merged).source.confidence == 41,
         "Quarter MIS partition is not a confidence-count divisor");
}
void fixed_cap_controls() {
    for (unsigned cap : {1u, 4u, 20u}) {
        const auto absent = c::temporal({}, cap);
        need(absent.status == c::Status::ok && absent.fresh.confidence == 1 &&
                 absent.history.present == 0 && absent.history.confidence == 0 &&
                 absent.output.confidence == 1,
             "Every fixed cap preserves canonical absence");
        for (unsigned prior :
             std::array<unsigned, 5>{1u, cap, cap + 1u, 105u, c::maximum_confidence}) {
            const auto t = c::temporal({prior, 1}, cap);
            const unsigned bounded = prior < cap ? prior : cap;
            need(t.status == c::Status::ok && t.history.confidence == bounded &&
                     t.fresh.confidence == 1 && t.output.confidence == bounded + 1,
                 "Fixed cap enters prior coefficient before fresh addition");
        }
        c::Source prior{};
        unsigned expected = 0;
        for (unsigned frame = 0; frame < 100; ++frame) {
            const unsigned temporal_expected = (prior.present ? std::min(expected, cap) : 0) + 1;
            const auto t = c::temporal(prior, cap);
            auto spatial = c::spatial_begin(t.output);
            for (unsigned neighbor = 0; neighbor < 4; ++neighbor)
                need(c::add_neighbor(spatial, t.output) == c::Status::ok,
                     "Fixed-cap spatial structural source");
            prior = c::spatial_finish(spatial).source;
            expected = 5 * temporal_expected;
            need(prior.confidence == expected, "Independent fixed-cap recursive confidence");
        }
        need(expected == 5 * (cap + 1), "Cap1/4/20 stable recurrence keeps full spatial sum");
        const auto t = c::temporal({105, 1}, cap);
        const float fresh = c::coefficient(t.fresh).value;
        const float history = c::coefficient(t.history).value;
        const auto center = e::canonical_score(number(2), number(2), number(4), fresh, history);
        const auto shifted = e::pair_score({number(4), number(4), number(2), history, fresh});
        const long double denominator = 2 + 4.L * cap;
        near(value(center.value), 4.L / denominator, 3e-7L,
             "Independent fresh coefficient uses chosen fixed cap");
        near(value(shifted.value), 8.L * cap / denominator, 3e-7L,
             "Independent positive history coefficient uses same chosen fixed cap");
        near(value(center.value) + value(shifted.value), 2, 3e-7L,
             "Both fixed-cap MIS terms retain the complementary partition");
        const auto wrong = e::pair_score({number(4), number(4), number(2), 105.f, fresh});
        need(std::abs(value(center.value) + value(wrong.value) - 2) > .03L,
             "Cap on only reverse/fresh term fails for each supported experiment");
        // A realized zero/null does not delete the prior structural domain.
        const auto zero = e::pair_score({number(0), number(0), number(2), history, fresh});
        need(zero.status == m::Status::ok && er::zero(zero.value) && t.history.present == 1 &&
                 t.history.confidence == cap && t.output.confidence == cap + 1,
             "Fixed cap retains null prior domain");
    }
}

struct CapInput {
    unsigned maximum, count;
};
long double cap_oracle(CapInput input) {
    // Independent real-valued Lin section5 formula. No production pow/helper,
    // confidence encoding, clamp or recurrence is reused by this oracle.
    const long double D = static_cast<long double>(input.count) / 288.L;
    return static_cast<long double>(input.maximum) +
           (1.L - static_cast<long double>(input.maximum)) * std::pow(D, .1L);
}
void fractional_cap_and_wire() {
    namespace f = c::fractional;
    namespace a = rypath::area_wire;
    constexpr long double tolerance = 128.L * std::numeric_limits<float>::epsilon();
    for (unsigned H : {1u, 4u, 20u})
        for (unsigned count : {0u, 1u, 36u, 287u, 288u}) {
            const auto expected = cap_oracle({H, count});
            const auto t = f::temporal({105.f, 1}, H, count, false);
            need(t.status == c::Status::ok && t.fresh.confidence == 1 && t.history.present == 1 &&
                     t.output.present == 1,
                 "Adaptive prior is a structural domain");
            near(t.history.confidence, expected, tolerance,
                 "Independent fractional cap: count/288 and alpha0.1");
            near(t.output.confidence, expected + 1, tolerance,
                 "Adaptive cap applies BEFORE adding fresh1");
            near(t.history.confidence, expected, tolerance,
                 "MIS consumes the fractional history coefficient");
            if (H > 1 && (count == 1 || count == 36))
                need(std::abs(t.history.confidence - std::floor(t.history.confidence)) > .1f,
                     "Noninteger cap cannot be rounded, truncated or cast to Word");
            if (count == 0)
                need(t.history.confidence == float(H), "Zero duplication retains H exactly");
            if (count == 288)
                need(t.history.confidence == 1.f, "Full duplication reaches minimum1 exactly");
            const auto word = a::encode_confidence(t.history.confidence, 1);
            need(word == std::bit_cast<unsigned>(t.history.confidence) &&
                     std::bit_cast<unsigned>(a::decode_confidence(word, 1)) == word,
                 "Adaptive wire is FLOAT bits with no quantization");
        }
    for (unsigned old : std::array<unsigned, 6>{1u, 4u, 20u, 21u, 105u, c::maximum_confidence}) {
        need(a::encode_confidence(float(old), 0) == old &&
                 a::decode_confidence(old, 0) == float(old),
             "Fixed wire words retain their old INTEGER bytes");
        const auto integer = c::temporal({old, 1}, 20);
        const auto floating = f::temporal({float(old), 1}, 20, 0, false);
        need(floating.status == c::Status::ok &&
                 floating.history.confidence == float(integer.history.confidence) &&
                 floating.output.confidence == float(integer.output.confidence),
             "Fixed float recurrence matches existing strict integer oracle");
    }
    need(!f::valid_source({a::decode_confidence(20, 1), 1}) &&
             !c::valid_source({a::encode_confidence(20.f, 1), 1}),
         "Wrong confidence encoding cannot masquerade as a valid source");
}
void fractional_complementary_weights() {
    namespace f = c::fractional;
    for (unsigned count : {1u, 36u, 288u}) {
        const auto expected_M = cap_oracle({20, count});
        const auto t = f::temporal({105.f, 1}, 20, count, false);
        need(t.status == c::Status::ok && f::valid_source(t.fresh) && f::valid_source(t.history),
             "Adaptive sources validate before MIS");
        const float fresh = t.fresh.confidence, history = t.history.confidence;
        const auto center = e::canonical_score(number(2), number(2), number(4), fresh, history);
        const auto shifted = e::pair_score({number(4), number(4), number(2), history, fresh});
        const long double denominator = 2 + 4 * expected_M;
        near(value(center.value), 4 / denominator, 2e-6L,
             "Fresh MIS uses the SAME fractional source cap");
        near(value(shifted.value), 8 * expected_M / denominator, 2e-6L,
             "Historical MIS uses fractional cap with correct denominator orientation");
        near(value(center.value) + value(shifted.value), 2, 2e-6L,
             "Conditional complementary pair partitions the integrand");
        // This local algebra does not prove the sample-dependent policy unbiased:
        // Lin section5 explicitly introduces bias across sample-dependent domains.
        const auto wrong = e::pair_score({number(4), number(4), number(2), 105.f, fresh});
        need(std::abs(value(center.value) + value(wrong.value) - 2) > .03L,
             "Capping only the fresh/reverse term breaks the complementary pair");
        if (count != 288) {
            const auto rounded =
                e::canonical_score(number(2), number(2), number(4), 1, std::floor(history));
            need(std::abs(value(rounded.value) - value(center.value)) > .001L,
                 "Rounding both coefficients could preserve partition but changes weights");
        }
    }
}
void fractional_propagation_and_null() {
    namespace f = c::fractional;
    f::Source previous{};
    long double expected = 0;
    constexpr std::array<unsigned, 6> counts{0, 1, 36, 288, 0, 36};
    for (unsigned frame = 0; frame < 48; ++frame) {
        const auto count = counts[frame % counts.size()];
        const auto t = f::temporal(previous, 20, count, false);
        const long double temporal =
            (previous.present ? std::min(expected, cap_oracle({20, count})) : 0.L) + 1.L;
        auto spatial = f::spatial_begin(t.output);
        for (unsigned neighbor = 0; neighbor < 4; ++neighbor)
            need(f::add_neighbor(spatial, t.output) == c::Status::ok,
                 "Fractional spatial structural input admitted");
        const auto result = f::spatial_finish(spatial);
        expected = 5 * temporal;
        need(t.status == c::Status::ok && result.status == c::Status::ok && spatial.admitted == 5 &&
                 spatial.considered == 5,
             "Fractional recurrence retains all spatial domains");
        near(result.source.confidence, expected, 1e-4L,
             "Independent temporal/spatial recurrence retains fractional mass");
        previous = result.source;
    }
    const auto null = f::temporal({7.25f, 1}, 20, 0xffffu, true);
    need(null.status == c::Status::ok && null.history.confidence == 7.25f &&
             null.history.present == 1 && null.output.confidence == 8.25f,
         "Null history has no duplication label yet retains its fractional domain");
    for (unsigned count : {0u, 1u, 36u, 288u, 0xffffu}) {
        const auto cold = f::temporal({}, 20, count, false);
        need(cold.status == c::Status::ok && cold.fresh.confidence == 1 &&
                 cold.history.present == 0 && cold.output.confidence == 1,
             "Cut or missing donor ignores unavailable map and starts fresh1");
    }
    const auto zero = e::pair_score({number(0), number(0), number(2), 7.25f, 1});
    need(zero.status == m::Status::ok && er::zero(zero.value),
         "Null target has zero score without deleting structural confidence");
    auto structural = f::spatial_begin({2.25f, 1});
    need(f::add_neighbor(structural, {7.25f, 1}) == c::Status::ok &&
             f::add_neighbor(structural, {}) == c::Status::ok &&
             f::spatial_finish(structural).source.confidence == 9.5f && structural.admitted == 2 &&
             structural.considered == 3,
         "Spatial fractional confidence includes a null domain but excludes rejected donor");
}
void fractional_faults() {
    namespace f = c::fractional;
    for (float value : {0.f, .5f, -1.f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        const f::Source invalid{value, 1};
        need(!f::valid_source(invalid) &&
                 f::temporal(invalid, 20, 0, false).status == c::Status::invalid_source,
             "Adaptive invalid confidence fails, never clamps into a valid domain");
    }
    for (unsigned count : {0u, 1u, 36u, 288u})
        need(f::temporal({5.f, 1}, 20, count, true).status == c::Status::invalid_source,
             "A null draw requires the missing-label count sentinel");
    for (unsigned count : {289u, 0xffffu, 0xffffffffu})
        need(f::temporal({5.f, 1}, 20, count, false).status == c::Status::invalid_source,
             "Present nonnull history rejects missing or out-of-range counts");
    for (unsigned cap : std::array<unsigned, 3>{0u, 21u, c::maximum_confidence})
        need(f::temporal({5.f, 1}, cap, 36, false).status == c::Status::invalid_cap,
             "Adaptive policy rejects unsupported H rather than silently clipping");
    auto overflow = f::spatial_begin({float(c::maximum_confidence), 1});
    need(f::add_neighbor(overflow, {1.f, 1}) == c::Status::count_overflow &&
             overflow.confidence == float(c::maximum_confidence) &&
             f::spatial_finish(overflow).status == c::Status::count_overflow,
         "Fractional sum overflow cannot round back to maximum and report success");
}

void faults_and_limits() {
    need(c::coefficient({c::maximum_confidence, 1}).value == 16777216.f,
         "Maximum coefficient is exactly representable binary32");
    for (auto bad : {c::Source{0, 1}, c::Source{1, 0}, c::Source{1, 2},
                     c::Source{c::maximum_confidence + 1, 1}}) {
        need(!c::valid_source(bad) && c::temporal(bad).status == c::Status::invalid_source &&
                 c::coefficient(bad).status == c::Status::invalid_source,
             "Malformed sources fail instead of becoming absent domains");
    }
    need(c::temporal({1, 1}, 0).status == c::Status::invalid_cap &&
             c::temporal({1, 1}, c::maximum_confidence).status == c::Status::invalid_cap,
         "Invalid cap cannot overflow cap plus fresh1");
    need(c::temporal({c::maximum_confidence, 1}, c::maximum_confidence - 1).output.confidence ==
             c::maximum_confidence,
         "Largest accepted temporal output remains exact");
    need(c::spatial_begin({}).status == c::Status::invalid_source,
         "Canonical center must be structurally present even for null");
    auto overflow = c::spatial_begin({c::maximum_confidence, 1});
    need(c::add_neighbor(overflow, {1, 1}) == c::Status::count_overflow &&
             overflow.confidence == c::maximum_confidence &&
             c::spatial_finish(overflow).status == c::Status::count_overflow &&
             c::add_neighbor(overflow, {}) == c::Status::count_overflow,
         "Confidence overflow is sticky, never wrapped or saturated success");
    auto full = c::spatial_begin({1, 1});
    for (unsigned i = 1; i < c::maximum_spatial_sources; ++i)
        need(c::add_neighbor(full, {}) == c::Status::ok,
             "Rejected donors retain bounded observations");
    need(c::add_neighbor(full, {}) == c::Status::count_overflow,
         "Scheduling observation count cannot exceed declared bound");
    c::Spatial forged{1, 0, 1, c::Status::ok};
    need(c::spatial_finish(forged).status == c::Status::invalid_state &&
             c::add_neighbor(forged, {}) == c::Status::invalid_state,
         "Malformed accumulator cannot be finalized");
}
} // namespace
int main() {
    try {
        recurrence();
        structural_not_sampled();
        complementary_and_cap_timing();
        fixed_cap_controls();
        faults_and_limits();
        fractional_cap_and_wire();
        fractional_complementary_weights();
        fractional_propagation_and_null();
        fractional_faults();
        std::cout << "Enhanced strict structural-confidence tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
