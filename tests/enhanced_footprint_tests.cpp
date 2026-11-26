// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_footprint_shared.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
namespace f = rypath_enhanced_footprint;
namespace r = rypath_reuse_mis;
void need(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
f::Number number(float v) { return v == 0 ? f::Number{0, 0} : r::decompose_positive(v); }
long double value(f::Number n) {
    return std::ldexp(static_cast<long double>(n.fraction), n.exponent);
}
f::Input fixture() {
    f::Input v;
    v.predecessor = v.pivot = f::Family::rough_conductor;
    v.predecessor_alpha = .2f;
    v.primary_distance_squared = number(1);
    v.primary_cosine = 1;
    v.edge_distance_squared = number(1);
    v.predecessor_pdf = number(1);
    v.cosine_at_pivot = 1;
    v.pivot_outgoing_pdf = number(1);
    v.cosine_at_predecessor = 1;
    return v;
}
bool oracle(f::Input v) {
    // Independent long-double equation with true pi, no production scaled calls.
    const long double pi = std::acos(-1.L);
    const long double threshold = .0002L * 4 * pi * value(v.primary_distance_squared) /
                                  static_cast<long double>(v.primary_cosine);
    const long double forward =
        value(v.edge_distance_squared) / (value(v.predecessor_pdf) * v.cosine_at_pivot);
    const bool inverse = v.pivot == f::Family::rough_conductor;
    const long double backward = inverse
                                     ? value(v.edge_distance_squared) /
                                           (value(v.pivot_outgoing_pdf) * v.cosine_at_predecessor)
                                     : std::numeric_limits<long double>::infinity();
    const bool rough = v.predecessor != f::Family::rough_conductor || v.predecessor_alpha >= .2f;
    return rough && forward >= threshold && backward >= threshold;
}
void numerical_and_units() {
    for (float primary : {.01f, 1.f, 100.f})
        for (float edge : {.00001f, .1f, 10.f})
            for (float p : {.01f, 1.f, 100.f})
                for (float inverse : {.02f, 2.f, 200.f}) {
                    auto v = fixture();
                    v.primary_distance_squared = number(primary);
                    v.edge_distance_squared = number(edge);
                    v.predecessor_pdf = number(p);
                    v.pivot_outgoing_pdf = number(inverse);
                    v.primary_cosine = .5f;
                    v.cosine_at_pivot = .25f;
                    v.cosine_at_predecessor = .75f;
                    const auto actual = f::classify(v);
                    need(actual.status == f::Status::ok &&
                             (actual.decision == f::Decision::eligible) == oracle(v),
                         "Independent long-double dual-footprint grid");
                    for (int exponent : {-300, -100, 100, 300}) {
                        auto scaled = v;
                        // Uniform world length rescaling scales BOTH squared
                        // distances identically; angle PDFs/cosines unchanged.
                        scaled.primary_distance_squared.exponent += exponent;
                        scaled.edge_distance_squared.exponent += exponent;
                        const auto result = f::classify(scaled);
                        need(result.status == f::Status::ok && result.decision == actual.decision,
                             "World units do not alter criterion or under/overflow");
                    }
                }
    // Nested denominator regression: footprint .001 lies BETWEEN wrong tiny
    // threshold .0002/(4pi) and correct threshold .0002*4pi. Old interpretation
    // would accept it, while actual paper Eq5 rejects (ratio=(4pi)^2≈157.91).
    auto v = fixture();
    v.pivot = f::Family::diffuse;
    v.edge_distance_squared = number(.001f);
    const auto actual = f::classify(v);
    const long double wrong = .0002L / (4 * std::acos(-1.L));
    need(value(v.edge_distance_squared) > wrong && actual.decision == f::Decision::ineligible,
         "Wrong4pi denominator changes eligibility and must fail regression");
}
void boundaries_and_inverse() {
    auto v = fixture();
    v.pivot = f::Family::diffuse;
    const auto threshold = f::primary_threshold(v.primary_distance_squared, v.primary_cosine);
    need(threshold.status == f::Status::ok, "Primary threshold valid");
    v.edge_distance_squared = threshold.area;
    need(f::classify(v).decision == f::Decision::eligible, "Represented equality is inclusive");
    v.edge_distance_squared.fraction = std::nextafter(v.edge_distance_squared.fraction, 0.f);
    need(f::classify(v).decision == f::Decision::ineligible, "One represented ulp below rejects");
    v.edge_distance_squared = threshold.area;
    v.edge_distance_squared.fraction = std::nextafter(v.edge_distance_squared.fraction, 1.f);
    need(f::classify(v).decision == f::Decision::eligible, "One represented ulp above accepts");
    v = fixture();
    v.predecessor_alpha = std::nextafter(.2f, 0.f);
    need(f::classify(v).decision == f::Decision::ineligible,
         "Predecessor roughness below threshold");
    v.predecessor_alpha = .2f;
    need(f::classify(v).decision == f::Decision::eligible, "Predecessor roughness equality");
    v.pivot_outgoing_pdf = number(1000);
    auto result = f::classify(v);
    need(result.forward_pass && !result.inverse_pass && result.decision == f::Decision::ineligible,
         "Forward pass alone cannot replace inverse test");
    for (auto family : {f::Family::diffuse, f::Family::terminal_emission}) {
        v.pivot = family;
        v.pivot_outgoing_pdf = {std::numeric_limits<float>::quiet_NaN(), 0};
        v.cosine_at_predecessor = std::numeric_limits<float>::quiet_NaN();
        need(f::classify(v).decision == f::Decision::eligible,
             "Diffuse/terminal-emission bypasses UNUSED inverse query entirely");
    }
    v = fixture();
    v.predecessor = f::Family::diffuse;
    v.predecessor_alpha = std::numeric_limits<float>::quiet_NaN();
    need(f::classify(v).decision == f::Decision::eligible, "Diffuse has no NDF-width query");
}
void support_and_faults() {
    auto v = fixture();
    for (unsigned field = 0; field < 6; ++field) {
        auto z = v;
        if (field == 0)
            z.primary_distance_squared = {0, 0};
        if (field == 1)
            z.primary_cosine = 0;
        if (field == 2)
            z.edge_distance_squared = {0, 0};
        if (field == 3)
            z.predecessor_pdf = {0, 0};
        if (field == 4)
            z.pivot_outgoing_pdf = {0, 0};
        if (field == 5)
            z.cosine_at_pivot = 0;
        need(f::classify(z).status == f::Status::ok &&
                 f::classify(z).decision == f::Decision::zero_support,
             "Zero geometry/proposal support is not arithmetic failure or infinity acceptance");
    }
    for (auto invalid :
         {f::Number{.5f, std::numeric_limits<int>::max()}, f::Number{1, 0}, f::Number{-.5f, 1}}) {
        auto bad = v;
        bad.predecessor_pdf = invalid;
        need(f::classify(bad).status == f::Status::invalid_input,
             "Poisoned normalized number rejected before exponent arithmetic");
    }
    for (unsigned field = 0; field < 3; ++field) {
        auto tiny = fixture();
        if (field == 0)
            tiny.primary_cosine = std::numeric_limits<float>::denorm_min();
        if (field == 1)
            tiny.cosine_at_pivot = std::numeric_limits<float>::denorm_min();
        if (field == 2)
            tiny.cosine_at_predecessor = std::numeric_limits<float>::denorm_min();
        const auto result = f::classify(tiny);
        need(result.status == f::Status::ok && result.decision != f::Decision::zero_support &&
                 (result.decision == f::Decision::eligible) == oracle(tiny),
             "Represented positive subnormal cosine is not silently flushed to absent support");
    }
    auto negative_tiny = fixture();
    negative_tiny.primary_cosine = -std::numeric_limits<float>::denorm_min();
    need(f::classify(negative_tiny).status == f::Status::invalid_input,
         "Negative subnormal cosine is invalid before floating comparisons");
    v.primary_cosine = std::numeric_limits<float>::quiet_NaN();
    need(f::classify(v).status == f::Status::invalid_input, "NaN is not geometric rejection");
    v = fixture();
    v.pivot = f::Family::unsupported;
    need(f::classify(v).decision == f::Decision::unsupported,
         "Unsupported material remains a declared generic-map case");
}
void first_eligible() {
    auto pass = fixture(), fail = fixture();
    fail.predecessor_alpha = .1f;
    f::FirstEligible source, destination;
    need(f::consider(source, 2, fail).decision == f::Decision::ineligible &&
             f::consider(source, 3, pass).decision == f::Decision::eligible,
         "Source retains first eligible3");
    need(f::consider(destination, 2, pass).decision == f::Decision::eligible,
         "Destination has an EARLIER eligible2");
    need(!f::reciprocal_first(source, destination),
         "Both passing at oldk3 does not permit ignoring earlier destination pivot");
    f::FirstEligible equal;
    f::consider(equal, 2, fail);
    f::consider(equal, 3, pass);
    need(f::reciprocal_first(source, equal), "Matching adaptive labels accepted");
    need(f::consider(equal, 4, pass).status == f::Status::invalid_state,
         "Cannot silently select a later pivot after first");
    f::FirstEligible gap;
    need(f::consider(gap, 3, pass).status == f::Status::invalid_state &&
             !f::reciprocal_first(gap, source),
         "Skipped earlier-eligibility tests fail closed");
    f::FirstEligible last;
    pass.pivot = f::Family::terminal_emission;
    for (unsigned k = 2; k <= 66; ++k)
        need(f::consider(last, k, k == 66 ? pass : fail).status == f::Status::ok,
             "Full bounded ordered search");
    need(last.selected == 66 && last.next == 67, "D65 terminal-vertex first eligible boundary");
}
} // namespace
int main() {
    try {
        numerical_and_units();
        boundaries_and_inverse();
        support_and_faults();
        first_eligible();
        std::cout << "Enhanced paper dual-footprint tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
