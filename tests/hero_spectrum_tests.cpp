// SPDX-License-Identifier: GPL-3.0-only
#include "hero_spectrum_shared.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace h = rypath_hero;
using V = h::Vector;
using Exact = std::array<long double, 4>;
void need(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(long double actual, long double expected, const char *message,
          long double relative = 4e-6L) {
    const auto tolerance = relative * std::abs(expected);
    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
long double sum(const Exact &a) { return a[0] + a[1] + a[2] + a[3]; }
Exact exact(V a) { return {a[0], a[1], a[2], a[3]}; }
long double mean(V a) { return sum(exact(a)) / 4; }
long double decode(h::Number a) {
    return std::ldexp(static_cast<long double>(a.fraction), a.exponent);
}
void density_matches(const h::Density &actual, const Exact &probability) {
    for (unsigned k = 0; k < 4; ++k)
        near(decode(actual.lane[k]), probability[k], "Cumulative likelihood vs direct product");
}
void beta_matches(V actual, const Exact &physical, const Exact &probability) {
    for (unsigned k = 0; k < 4; ++k)
        near(actual[k], 4 * physical[k] / sum(probability), "Eq.11 direct full-path denominator");
}
V ones() { return {1, 1, 1, 1}; }

// Enumerate two binary scattering choices and four possible actual heroes.
// The oracle forms full-path products and integrates them directly; it never
// computes the expected value by duplicating advance()'s recurrence.
void finite_paths(bool black_hero) {
    const std::array<V, 2> first{{{.75f, .25f, 0, 1}, {.25f, .75f, 1, 0}}};
    const std::array<std::array<V, 2>, 2> second{
        {{{{.125f, .75f, .5f, .25f}, {.875f, .25f, .5f, .75f}}},
         {{{.875f, .25f, 1, 0}, {.125f, .75f, 0, 1}}}}};
    const std::array<V, 2> n1{{{0, .5f, .25f, .75f}, {.125f, .25f, .75f, .5f}}};
    const std::array<V, 2> n2{{{.5f, .25f, .125f, .75f}, {.25f, .75f, .5f, .125f}}};
    long double expected = 0, measured = 0, local_only = 0;
    for (unsigned x = 0; x < 2; ++x) {
        V a = first[x], na = n1[x];
        if (black_hero) {
            a[0] = 0; // This hero has a null/absorbed outcome of probability one.
            na[0] = 0;
        }
        const auto prefix = h::advance(h::initial_density(), ones(), na, a);
        need(prefix.valid, "Valid finite prefix");
        for (unsigned y = 0; y < 2; ++y) {
            const auto b = second[x][y], nb = n2[y];
            Exact probability{}, physical{};
            for (unsigned k = 0; k < 4; ++k) {
                probability[k] = static_cast<long double>(a[k]) * b[k];
                physical[k] = static_cast<long double>(na[k]) * nb[k];
            }
            const auto result = h::advance(prefix.density, prefix.beta, nb, b);
            need(result.valid, "Valid finite continuation");
            density_matches(result.density, probability);
            beta_matches(result.beta, physical, probability);
            expected += sum(physical) / 4;
            // Actual hero choice is uniform. Each hero then samples its own
            // two decisions. Include zero-probability branches explicitly.
            for (unsigned hero = 0; hero < 4; ++hero) {
                measured += probability[hero] / 4 * mean(result.beta);
                // Negative control: averaging just the local second PDFs
                // loses the first event's wavelength-dependent posterior.
                local_only +=
                    probability[hero] / 4 * (4 * sum(physical) / (sum(exact(a)) * sum(exact(b))));
            }
        }
    }
    near(measured, expected, "Enumerated four-hero two-bounce integral");
    need(std::abs(local_only - expected) > .005L,
         "Fixture must detect local-only spectral density weighting");
}

void joint_light_bsdf() {
    const V prefix_pdf{.75f, .125f, .5f, .25f};
    const V prefix_numerator{.25f, .5f, .75f, 0}; // Black physical hero in one rotation.
    const auto prefix = h::advance(h::initial_density(), ones(), prefix_numerator, prefix_pdf);
    need(prefix.valid, "Joint MIS prefix");
    const std::array<V, 2> light{{{.875f, .125f, .5f, 0}, {.125f, .875f, .5f, 1}}};
    const std::array<V, 2> bsdf{{{.125f, .75f, .25f, 1}, {.875f, .25f, .75f, 0}}};
    const std::array<V, 2> contribution{{{.125f, .75f, .5f, .25f}, {.75f, .25f, .125f, .5f}}};
    long double expected = 0, measured = 0, hero_only = 0;
    for (unsigned direction = 0; direction < 2; ++direction) {
        Exact physical{}, a{}, b{};
        for (unsigned k = 0; k < 4; ++k) {
            physical[k] =
                static_cast<long double>(prefix_numerator[k]) * contribution[direction][k];
            a[k] = static_cast<long double>(prefix_pdf[k]) * light[direction][k];
            b[k] = static_cast<long double>(prefix_pdf[k]) * bsdf[direction][k];
        }
        const auto wl = h::balance(prefix.density, light[direction], bsdf[direction]);
        const auto wb = h::balance(prefix.density, bsdf[direction], light[direction]);
        need(wl.valid && wb.valid, "Valid joint MIS weights");
        near(wl.value, sum(a) / (sum(a) + sum(b)), "Eq.12 light full-technique sum");
        near(wb.value, sum(b) / (sum(a) + sum(b)), "Eq.12 BSDF full-technique sum");
        near(static_cast<long double>(wl.value) + wb.value, 1, "Joint MIS partition");
        const auto l =
            h::advance(prefix.density, prefix.beta, contribution[direction], light[direction]);
        const auto s =
            h::advance(prefix.density, prefix.beta, contribution[direction], bsdf[direction]);
        need(l.valid && s.valid, "Both connection strategies have mixture support");
        beta_matches(l.beta, physical, a);
        beta_matches(s.beta, physical, b);
        const auto weighted_l =
            h::balance_contribution(prefix.density, l.beta, light[direction], bsdf[direction]);
        const auto weighted_s =
            h::balance_contribution(prefix.density, s.beta, bsdf[direction], light[direction]);
        need(weighted_l.valid && weighted_s.valid, "Scaled joint contribution weights");
        expected += sum(physical) / 4;
        for (unsigned hero = 0; hero < 4; ++hero) {
            measured += a[hero] / 4 * mean(weighted_l.value);
            measured += b[hero] / 4 * mean(weighted_s.value);
            const auto denominator =
                static_cast<long double>(light[direction][hero]) + bsdf[direction][hero];
            const auto wrong_light = light[direction][hero] / denominator;
            const auto wrong_bsdf = bsdf[direction][hero] / denominator;
            hero_only += a[hero] / 4 * mean(l.beta) * wrong_light;
            hero_only += b[hero] / 4 * mean(s.beta) * wrong_bsdf;
        }
    }
    near(measured, expected, "Joint wavelength/light/BSDF enumerated integral");
    need(std::abs(hero_only - expected) > .005L, "Fixture must reject hero-only light/BSDF MIS");
}

void delta_collapse_and_past() {
    const V p{.125f, .25f, .75f, 1}, f{.25f, .5f, .75f, .125f};
    const auto prefix = h::advance(h::initial_density(), ones(), f, p);
    need(prefix.valid, "Delta test prefix");
    const auto saved_beta = prefix.beta;
    const auto saved_density = prefix.density;
    const V aligned_pdf{.25f, .5f, .75f, 1}, aligned_f{.5f, .25f, .125f, .75f};
    const auto aligned = h::advance(prefix.density, prefix.beta, aligned_f, aligned_pdf);
    need(aligned.valid, "Aligned Dirac retains four masses");
    Exact joint{}, physical{};
    for (unsigned k = 0; k < 4; ++k) {
        joint[k] = static_cast<long double>(p[k]) * aligned_pdf[k];
        physical[k] = static_cast<long double>(f[k]) * aligned_f[k];
    }
    density_matches(aligned.density, joint);
    beta_matches(aligned.beta, physical, joint);
    const V singular_pdf{.25f, 0, 0, 0}, singular_f{.5f, 0, 0, 0};
    const auto singular = h::advance(prefix.density, prefix.beta, singular_f, singular_pdf);
    need(singular.valid, "Misaligned Dirac has hero-only support");
    near(singular.beta[0], 4.L * f[0] * singular_f[0] / (p[0] * singular_pdf[0]),
         "Dirac mixture applies posterior compensation exactly once");
    for (unsigned k = 1; k < 4; ++k)
        need(singular.beta[k] == 0 && singular.density.lane[k].fraction == 0,
             "Misaligned companion contribution and mass are zero");
    const auto collapsed = h::collapse(prefix.density, prefix.beta);
    need(collapsed.valid, "Explicit hero fallback");
    near(collapsed.beta[0], 4.L * f[0] / p[0], "Explicit collapse uses cumulative hero posterior");
    need(std::abs(collapsed.beta[0] - prefix.beta[0] * 4) > 1,
         "Unequal-density fixture detects unconditional factor four");
    const auto after = h::advance(collapsed.density, collapsed.beta, singular_f, singular_pdf);
    need(after.valid, "Hero continuation after explicit collapse");
    near(after.beta[0], singular.beta[0], "Explicit and implicit Dirac collapse agree");
    // Accumulated contributions use their production-time mixture weights.
    // Model a contribution at the prefix and a later singular contribution;
    // no final wavelength-PDF/mask may erase the earlier companion channels.
    const V emission{.5f, .75f, .25f, 1};
    long double past = 0, closed_past = 0;
    for (unsigned k = 0; k < 4; ++k) {
        past += prefix.beta[k] * emission[k] / 4;
        closed_past += static_cast<long double>(f[k]) * emission[k] / sum(exact(p));
        need(std::bit_cast<std::uint32_t>(prefix.beta[k]) ==
                 std::bit_cast<std::uint32_t>(saved_beta[k]),
             "Future operations leave earlier throughput bytes unchanged");
        need(prefix.density.lane[k].fraction == saved_density.lane[k].fraction &&
                 prefix.density.lane[k].exponent == saved_density.lane[k].exponent,
             "Future operations leave earlier density unchanged");
    }
    const auto future = singular.beta[0] / 4;
    near(past + future,
         closed_past + static_cast<long double>(f[0]) * singular_f[0] / (p[0] * singular_pdf[0]),
         "Earlier plus later contribution uses each event's own weights");
    need(std::abs(past - static_cast<long double>(prefix.beta[0]) * emission[0]) > .01L,
         "Fixture detects retroactive final-mask factor-four replacement");
}

void density_regrowth() {
    auto density = h::initial_density();
    V beta = ones();
    // This fixture keeps physical FLOAT throughput representable throughout.
    // Scaled likelihood support is stronger than the ordinary binary32 beta
    // contract: a physical beta that rounds to zero cannot subsequently regrow.
    Exact probability{1, 1, 1, 1}, physical{1, 1, 1, 1};
    const float tiny = std::ldexp(1.f, -100);
    for (unsigned event = 0; event < 9; ++event) {
        const V pdf = event < 4 ? V{1, tiny, 1, 1} : V{tiny, 1, tiny, tiny};
        const V numerator = event < 4 ? ones() : V{tiny, tiny, tiny, tiny};
        for (unsigned k = 0; k < 4; ++k) {
            probability[k] *= pdf[k];
            physical[k] *= numerator[k];
        }
        const auto out = h::advance(density, beta, numerator, pdf);
        need(out.valid, "Scaled likelihood stress remains representable");
        density = out.density;
        beta = out.beta;
        density_matches(density, probability);
        beta_matches(beta, physical, probability);
        if (event == 3) {
            need(probability[1] < std::numeric_limits<float>::denorm_min() &&
                     density.lane[1].fraction > 0,
                 "Counterfactual likelihood survives below FLOAT subnormal support");
        }
    }
    const auto weight = h::balance(density, V{0, 1, 0, 0}, V{1, 0, 1, 1});
    need(weight.valid && weight.value > .999999f,
         "Previously subnormal counterfactual can regrow to dominate");
    near(weight.value, probability[1] / sum(probability), "Regrown likelihood direct denominator");
}

void numeric_conversion() {
    // Independent host IEEE conversion of exactly represented scaled values.
    // Cover gradual underflow, ties and the subnormal-to-normal carry, without
    // reproducing the implementation's integer shift/remainder algorithm.
    for (int exponent = -175; exponent <= 130; ++exponent)
        for (float fraction :
             {.5f, std::nextafter(.5f, 1.f), .625f, .75f, std::nextafter(1.f, 0.f)}) {
            const double exact_value = std::ldexp(double(fraction), exponent);
            const float expected = float(exact_value);
            const auto result = h::materialize({fraction, exponent});
            if (std::isfinite(expected)) {
                need(result.valid && std::bit_cast<std::uint32_t>(result.value) ==
                                         std::bit_cast<std::uint32_t>(expected),
                     "Scaled conversion agrees with independent IEEE RNE bits");
            } else {
                need(!result.valid, "Unrepresentable final value rejects");
            }
        }
    const auto p = h::initial_density();
    const auto maximum = std::numeric_limits<float>::max();
    const V twice{2, 2, 2, 2};
    const V large{maximum, maximum, maximum, maximum};
    const auto recovered = h::advance(p, large, twice, twice);
    need(recovered.valid, "Avoid intermediate overflow when final throughput is finite");
    for (unsigned k = 0; k < 4; ++k)
        need(recovered.beta[k] == maximum, "Finite extreme throughput retains its bits");
    need(!h::advance(p, large, twice, ones()).valid, "True final throughput overflow rejects");
    const auto tiny_hero = h::advance(p, ones(), ones(), V{std::ldexp(1.f, -125), 1, 1, 1});
    need(tiny_hero.valid && !h::collapse(tiny_hero.density, large).valid,
         "Unrepresentable compensated hero-only throughput rejects");
}

void scaled_balance_contribution() {
    const auto prefix = h::initial_density();
    const float large = std::ldexp(1.f, 120), small = std::ldexp(1.f, -125),
                competing = std::ldexp(1.f, 25);
    const V value{large, large, large, large}, sampled{small, small, small, small},
        other{competing, competing, competing, competing};
    const auto advanced = h::advance(prefix, value, sampled, sampled);
    need(advanced.valid, "Tiny-weight regression has finite sampled throughput");
    const auto ordinary_weight = h::balance(prefix, sampled, other);
    need(ordinary_weight.valid && ordinary_weight.value == 0,
         "Regression exercises an unrepresentable standalone MIS weight");
    const auto weighted = h::balance_contribution(prefix, advanced.beta, sampled, other);
    need(weighted.valid, "Scaled MIS contribution recovers a normal final value");
    const float expected =
        float(double(large) * double(small) / (double(small) + double(competing)));
    need(std::isnormal(expected), "Regression final contribution is normal");
    for (unsigned k = 0; k < 4; ++k)
        need(std::bit_cast<std::uint32_t>(weighted.value[k]) ==
                 std::bit_cast<std::uint32_t>(expected),
             "Do not round a tiny MIS weight before applying large throughput");
    const auto fp = [](std::uint32_t word) { return std::bit_cast<float>(word); };
    const V boundary{fp(1), fp(3), fp(5), fp(0x00ffffffu)};
    const auto half = h::balance_contribution(prefix, boundary, ones(), ones());
    need(half.valid, "Gradual-underflow final contributions are valid");
    const std::array<std::uint32_t, 4> rounded{0, 2, 2, 0x00800000u};
    for (unsigned k = 0; k < 4; ++k)
        need(std::bit_cast<std::uint32_t>(half.value[k]) == rounded[k],
             "Final contribution ties round to even, including normal-boundary carry");
    const V maximum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    const V zero{};
    const auto identity = h::balance_contribution(prefix, maximum, ones(), zero);
    const auto black = h::balance_contribution(prefix, maximum, zero, ones());
    need(identity.valid && black.valid, "Unit/zero weights accept finite HDR values");
    for (unsigned k = 0; k < 4; ++k)
        need(identity.value[k] == maximum[k] && black.value[k] == 0,
             "Unit weight preserves HDR and zero weight produces exact zero");
    need(!h::balance_contribution(prefix, ones(), zero, zero).valid,
         "Empty joint sampling support rejects weighted contributions");
    for (float invalid : {-1.f, INFINITY, NAN}) {
        V bad = ones();
        bad[2] = invalid;
        need(!h::balance_contribution(prefix, bad, ones(), zero).valid &&
                 !h::balance_contribution(prefix, bad, zero, ones()).valid &&
                 !h::balance_contribution(prefix, ones(), bad, ones()).valid &&
                 !h::balance_contribution(prefix, ones(), ones(), bad).valid,
             "Nonnegative finite contribution/probability contract is enforced before weighting");
    }
}

void boundaries_and_rejections() {
    for (unsigned i = 0; i < 16384; ++i) {
        const float u = float(i) / 16384;
        const auto sample = h::sample_uniform(u);
        for (unsigned k = 0; k < 4; ++k) {
            const long double shifted = std::fmod(static_cast<long double>(u) + k / 4.L, 1.L);
            near(sample.lambda[k], 360 + 470 * shifted, "Uniform shifted wavelength oracle", 3e-7L);
            need(sample.lambda[k] >= 360 && sample.lambda[k] < 830 && sample.pdf[k] == 1.f / 470.f,
                 "Uniform wrapped wavelength bounds and PDF");
        }
    }
    const auto endpoint = h::sample_uniform(std::nextafter(1.f, 0.f));
    for (unsigned k = 0; k < 4; ++k)
        need(endpoint.lambda[k] >= 360 && endpoint.lambda[k] < 830,
             "Largest uniform retains open wavelength upper bound");
    for (float u : {-1.f, 1.f, INFINITY, NAN}) {
        const auto sample = h::sample_uniform(u);
        for (unsigned k = 0; k < 4; ++k)
            need(!std::isfinite(sample.lambda[k]) && !std::isfinite(sample.pdf[k]),
                 "Invalid uniform is rejected rather than repaired");
    }
    const auto p = h::initial_density();
    const V zero{};
    need(!h::advance(p, ones(), ones(), zero).valid, "No mixture sampling support rejects");
    need(!h::balance(p, zero, zero).valid, "Empty joint support rejects");
    need(h::balance(p, zero, ones()).valid && h::balance(p, zero, ones()).value == 0,
         "Zero selected-technique mass has exact zero balance weight");
    for (float invalid : {-1.f, INFINITY, NAN}) {
        V bad = ones();
        bad[2] = invalid;
        need(!h::advance(p, ones(), ones(), bad).valid &&
                 !h::advance(p, ones(), bad, ones()).valid &&
                 !h::advance(p, bad, ones(), ones()).valid && !h::balance(p, bad, ones()).valid,
             "Negative/nonfinite probability, numerator or throughput rejects");
    }
    auto dead = p;
    dead.lane[0] = {};
    need(!h::collapse(dead, ones()).valid, "Impossible actual hero cannot be collapsed");
}
int main() {
    try {
        finite_paths(false);
        finite_paths(true);
        joint_light_bsdf();
        delta_collapse_and_past();
        density_regrowth();
        numeric_conversion();
        scaled_balance_contribution();
        boundaries_and_rejections();
        std::cout << "Hero spectrum: independent finite-path/MIS/Dirac/support/wavelength checks "
                     "passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
