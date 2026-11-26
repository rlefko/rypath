// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_confidence_shared.h"
#include "enhanced_path_shared.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace c = rypath_enhanced_confidence;
namespace e = rypath_enhanced_path;
namespace m = rypath_reservoir;
void need(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
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
            const auto center = e::canonical_score(2, 2, inverse, fresh, history);
            const auto neighbor = e::pair_score({Zold, old == 0 ? 0.f : 2.f, 2, history, fresh});
            need(center.status == m::Status::ok && neighbor.status == m::Status::ok,
                 "Complementary coefficient example is numerically valid");
            correct += (center.value + neighbor.value) / 4.L;
            const long double wrong_center = old == 0 ? 2 : (inverse == 0 ? 2 : 1);
            deletion += (wrong_center + neighbor.value) / 4.L;
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
    const auto center = e::canonical_score(2, 2, 4, fresh, history);
    const auto shifted = e::pair_score({4, 4, 2, history, fresh});
    near(center.value, 4.L / 82, 1e-8L, "Fresh complementary score with preweight cap20");
    near(shifted.value, 160.L / 82, 3e-7L, "History shifted score with same capped coefficient");
    near(static_cast<long double>(center.value) + shifted.value, 2, 3e-7L,
         "Complementary partition sums to complete integrand");
    const auto wrong_history = e::pair_score({4, 4, 2, 105, fresh});
    need(std::abs(static_cast<long double>(center.value) + wrong_history.value - 2) > .03L,
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
        faults_and_limits();
        std::cout << "Enhanced strict structural-confidence tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
