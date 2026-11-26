// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_suffix_shared.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
namespace s = rypath_enhanced_suffix;
namespace h = rypath_hero;
using V = std::array<long double, 4>;
void need(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(long double actual, long double expected, long double tolerance, const char *message) {
    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
long double scalar(h::Number n) {
    return std::ldexp(static_cast<long double>(n.fraction), n.exponent);
}
h::Density density(h::Vector p) {
    h::Density out{};
    for (unsigned k = 0; k < 4; ++k)
        out.lane[k] = h::number(p[k]);
    return out;
}
long double total(V v) { return (v[0] + v[1]) + (v[2] + v[3]); }
void independent_step(V &P, V &beta, h::Vector n, h::Vector pdf) {
    const auto before = total(P);
    for (unsigned k = 0; k < 4; ++k)
        P[k] *= pdf[k];
    const auto after = total(P);
    need(after > 0, "Independent rational branch support");
    for (unsigned k = 0; k < 4; ++k)
        beta[k] = beta[k] * n[k] * before / after;
}
void ordinary_products() {
    const std::array<h::Vector, 3> n{{{.5f, .25f, 0, .75f}, {2, .5f, 3, 0}, {.25f, 2, 1, .5f}}};
    const std::array<h::Vector, 3> pdf{
        {{.25f, .5f, .125f, .75f}, {.5f, .125f, .75f, 1}, {.5f, 1, .25f, .125f}}};
    for (auto input : {h::Vector{1, 3, 2, 4}, h::Vector{7, 0, 5, 1}}) {
        auto state = s::begin();
        V P{}, beta{2, 5, 0, 7}, N{1, 1, 1, 1}, D{1, 1, 1, 1}, MIS{};
        for (unsigned k = 0; k < 4; ++k)
            P[k] = input[k];
        for (unsigned event = 0; event < 3; ++event) {
            if (event == 2) {
                need(s::snapshot_mis(state) == s::Status::ok, "Snapshot before final sample");
                MIS = D;
            }
            need(s::append(state, n[event], pdf[event]) == s::Status::ok,
                 "Ordinary coefficient append");
            independent_step(P, beta, n[event], pdf[event]);
            for (unsigned k = 0; k < 4; ++k) {
                N[k] *= n[event][k];
                D[k] *= pdf[event][k];
            }
        }
        const auto result = s::recombine(state, density(input), {2, 5, 0, 7});
        need(result.status == s::Status::ok, "Multievent recombination supported");
        for (unsigned k = 0; k < 4; ++k) {
            near(scalar(state.N[k]), N[k], 0, "Products retain physical numerators, never N/D");
            near(scalar(state.D_to_atom.lane[k]), D[k], 0, "Likelihood products independent");
            near(result.beta[k], beta[k], std::abs(beta[k]) * 2e-6L + 1e-35L,
                 "Telescoping equals independent long-double event recurrence");
            near(scalar(result.to_atom.lane[k]), P[k], 0, "Atom density snapshot");
            near(scalar(result.to_MIS.lane[k]), input[k] * MIS[k], 0,
                 "MIS excludes preceding sample and differs from atom density");
        }
        need(scalar(result.to_atom.lane[0]) != scalar(result.to_MIS.lane[0]),
             "Cannot substitute terminal density for MIS prefix");
    }
    // One retained event has the exact operation association of hero::advance.
    const auto prefix = density({1, 3, 0, 0});
    const h::Vector beta{2, 5, 0, 0};
    auto one = s::begin();
    s::append(one, n[0], pdf[0]);
    const auto actual = s::recombine(one, prefix, beta);
    const auto original = h::advance(prefix, beta, n[0], pdf[0]);
    need(original.valid && actual.status == s::Status::ok, "One-event reference valid");
    for (unsigned k = 0; k < 4; ++k)
        need(h::bits(actual.beta[k]) == h::bits(original.beta[k]),
             "One-event hero rounding reference");
}
void coverage_and_zero() {
    auto prefix = density({1, 3, 0, 0});
    auto state = s::begin();
    s::append(state, {1, 1, 0, 0}, {.5f, .25f, 0, 0});
    const auto good = s::recombine(state, prefix, {2, 0, 0, 0});
    need(good.status == s::Status::ok, "Zero-beta lane still has density support");
    near(good.beta[0], 32.L / 5, 1e-6L, "Density coverage zero-beta32/5 oracle");
    auto wrong = state;
    wrong.D_to_atom.lane[1] = {};
    const auto lost = s::recombine(wrong, prefix, {2, 0, 0, 0});
    need(lost.status == s::Status::ok && lost.beta[0] == 16,
         "Wrong zero-beta density deletion16 negative");
    state.D_atom_coverage &= ~2u;
    state.D_to_atom.lane[1] = {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<int>::max()};
    need(s::recombine(state, prefix, {2, 0, 0, 0}).status == s::Status::cache_miss,
         "Missing D coverage needed even when beta0: same payload must regenerate");
    prefix.lane[1] = {};
    need(s::recombine(state, prefix, {2, 0, 0, 0}).status == s::Status::ok,
         "Unused missing density poison is never read");
    auto numerator = s::begin(1, 15);
    numerator.N[1] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<int>::min()};
    need(s::recombine(numerator, density({1, 0, 0, 0}), {1, 2, 0, 0}).status ==
             s::Status::cache_miss,
         "N coverage needed for beta even when its Pin0");
    need(s::recombine(numerator, density({1, 0, 0, 0}), {1, 0, 0, 0}).status == s::Status::ok,
         "Unneeded N poison ignored");
    auto zeroN = s::begin();
    s::append(zeroN, {0, 0, 0, 0}, {1, 1, 1, 1});
    const auto black = s::recombine(zeroN, density({1, 2, 3, 4}), {1, 2, 3, 4});
    need(black.status == s::Status::ok && black.beta == h::Vector{},
         "Physical numerator zero with nonzero D is valid black");
    for (auto pdf : {h::Vector{1, 0, 0, 0}, h::Vector{0, 1, 0, 0}}) {
        auto mixed_zero = s::begin();
        s::append(mixed_zero, {0, 1, 0, 0}, pdf);
        const auto out = s::recombine(mixed_zero, density({1, 1, 0, 0}), {1, 1, 0, 0});
        need(out.status == s::Status::ok && out.beta[0] == 0 && out.beta[1] == 2,
             "Independent zeroN/nonzeroD and zeroD/nonzeroN never form lane-wise0/0");
    }
    auto zeroD = s::begin();
    s::append(zeroD, {1, 1, 1, 1}, {0, 0, 0, 0});
    need(s::recombine(zeroD, density({1, 2, 3, 4}), {1, 2, 3, 4}).status ==
             s::Status::outside_support,
         "Zero common denominator is outside support, never black/clamped division");
    auto noMIS = s::begin(15, 1);
    noMIS.D_atom_coverage = 15;
    noMIS.D_to_atom = h::initial_density();
    need(s::recombine(noMIS, density({1, 1, 0, 0}), {1, 0, 0, 0}).status == s::Status::cache_miss &&
             s::recombine(noMIS, density({1, 1, 0, 0}), {1, 0, 0, 0}, false).status ==
                 s::Status::ok,
         "MIS coverage separate and optional only for caller with no MIS query");
}
void collapse_and_coating() {
    auto state = s::begin();
    s::append(state, {.5f, .25f, 1, 1}, {.25f, .5f, 1, 1});
    s::snapshot_mis(state);
    s::collapse(state);
    s::coated(state, 4);
    const auto result = s::recombine(state, density({1, 3, 0, 0}), {2, 7, 0, 0});
    need(result.status == s::Status::ok && result.beta[0] == 64 && result.beta[1] == 0,
         "Collapse+hidden weight uses beta0*N0*sumPin/(Pin0*D0)");
    need(scalar(result.to_atom.lane[1]) == 0 && scalar(result.to_MIS.lane[1]) == 1.5L,
         "Pre-collapse MIS snapshot retains companion density");
    need(result.beta[0] != 256, "No unconditional extra factor4");
    auto proxy = state;
    s::append(proxy, {1, 0, 0, 0}, {.125f, 0, 0, 0});
    need(s::recombine(proxy, density({1, 3, 0, 0}), {2, 7, 0, 0}).beta[0] == 512,
         "Putting coating directional proxy inD is wrong8x negative");
    s::collapse(state); // Already hero-only: no second compensation.
    s::coated(state, .5f);
    s::snapshot_mis(state);
    const auto second = s::recombine(state, density({1, 3, 0, 0}), {2, 7, 0, 0});
    need(second.status == s::Status::ok && second.beta[0] == 32 &&
             scalar(second.to_MIS.lane[1]) == 0,
         "Second coating and post-collapse MIS snapshot");
    need(s::recombine(state, density({0, 3, 0, 0}), {2, 7, 0, 0}).status ==
             s::Status::outside_support,
         "Collapse cannot invent hero likelihood support");
    auto uncollapsed = s::begin();
    need(s::coated(uncollapsed, 1) == s::Status::invalid_state,
         "Coating operation requires explicit collapse pattern");
}
void dynamic_range_and_faults() {
    auto state = s::begin();
    for (unsigned i = 0; i < 64; ++i)
        need(s::append(state, {0x1p-100f, 1, 1, 1}, {0x1p-100f, 1, 1, 1}) == s::Status::ok,
             "Deep independent exponents remain represented");
    const auto result = s::recombine(state, density({1, 0, 0, 0}), {3, 0, 0, 0});
    need(result.status == s::Status::ok && result.beta[0] == 3 && state.N[0].exponent < -6000 &&
             state.D_to_atom.lane[0].exponent < -6000,
         "Deep numerator/density cancellation without materializing N/D");
    auto regrowth = s::begin();
    for (unsigned i = 0; i < 160; ++i)
        s::append(regrowth, {1, 1, 1, 1}, {1, .5f, 1, 1});
    const auto halfway = s::recombine(regrowth, density({1, 1, 0, 0}), {1, 1, 0, 0});
    need(halfway.status == s::Status::ok && halfway.beta[0] == 2 && halfway.beta[1] == 2 &&
             h::materialize(regrowth.D_to_atom.lane[1]).value == 0,
         "Tiny likelihood is retained even when a FLOAT posterior would vanish");
    for (unsigned i = 0; i < 160; ++i)
        s::append(regrowth, {1, 1, 1, 1}, {1, 2, 1, 1});
    const auto regrown = s::recombine(regrowth, density({1, 1, 0, 0}), {1, 1, 0, 0});
    need(regrown.status == s::Status::ok && regrown.beta[0] == 1 && regrown.beta[1] == 1,
         "Likelihood regrowth changes common denominator without posterior truncation");
    const auto bounded = s::multiply({.5f, int(s::maximum_exponent)}, h::number(0x1p120f));
    need(bounded.status == s::Status::invalid_arithmetic,
         "Exponent-domain overflow fails after safe bounded addition, never int UB");
    state = s::begin();
    s::append(state, {0x1p-100f, 1, 1, 1}, {1, 1, 1, 1});
    const auto tiny = s::recombine(state, density({1, 0, 0, 0}), {0x1p-49f, 0, 0, 0});
    need(tiny.status == s::Status::ok && h::bits(tiny.beta[0]) == 1,
         "Gradual underflow minimumsubnormal");
    for (float n : {.5f, 1.5f, 2.5f}) {
        auto ties = s::begin();
        s::append(ties, {n, 1, 1, 1}, {1, 1, 1, 1});
        const auto out = s::recombine(ties, density({1, 0, 0, 0}),
                                      {std::numeric_limits<float>::denorm_min(), 0, 0, 0});
        const unsigned expected = n == .5f ? 0u : 2u;
        need(out.status == s::Status::ok && h::bits(out.beta[0]) == expected,
             "Output ties-to-even subnormal reference");
    }
    auto carry = s::begin();
    s::append(carry, {8388607.5f, 1, 1, 1}, {1, 1, 1, 1});
    const auto rounded = s::recombine(carry, density({1, 0, 0, 0}),
                                      {std::numeric_limits<float>::denorm_min(), 0, 0, 0});
    need(rounded.status == s::Status::ok && h::bits(rounded.beta[0]) == 0x00800000u,
         "Gradual-underflow tie carries into smallest normal");
    auto invalid = s::begin();
    invalid.N[0] = {.5f, std::numeric_limits<int>::max()};
    need(s::recombine(invalid, h::initial_density(), {1, 1, 1, 1}).status ==
             s::Status::invalid_state,
         "Poisoned exponent rejected before int addition");
    for (float bad :
         {-1.f, -std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN()}) {
        invalid = s::begin();
        need(s::append(invalid, {bad, 1, 1, 1}, {1, 1, 1, 1}) == s::Status::invalid_arithmetic &&
                 s::snapshot_mis(invalid) == s::Status::invalid_arithmetic,
             "Invalid observed numerator is sticky, not null/cachemiss");
    }
    invalid = s::begin();
    need(s::append(invalid, {1, 1, 1, 1}, {-1, 1, 1, 1}) == s::Status::invalid_arithmetic,
         "Negative PDF forbidden");
    auto subnormal = s::begin();
    s::append(subnormal, {std::numeric_limits<float>::denorm_min(), 1, 1, 1},
              {std::numeric_limits<float>::denorm_min(), 1, 1, 1});
    need(s::recombine(subnormal, density({1, 0, 0, 0}), {2, 0, 0, 0}).beta[0] == 2,
         "Positive subnormal n/pdf retained independently");
    auto overflow = s::begin();
    s::append(overflow, {0x1p120f, 1, 1, 1}, {1, 1, 1, 1});
    need(s::recombine(overflow, density({1, 0, 0, 0}), {0x1p120f, 0, 0, 0}).status ==
             s::Status::invalid_arithmetic,
         "Unrepresentable output is failure, never clamped");
}
} // namespace
int main() {
    try {
        ordinary_products();
        coverage_and_zero();
        collapse_and_coating();
        dynamic_range_and_faults();
        std::cout << "Enhanced spectral suffix coefficient tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
