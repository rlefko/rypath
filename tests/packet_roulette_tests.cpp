// SPDX-License-Identifier: GPL-3.0-only
#include "packet_roulette_shared.h"
#include "packet_spectrum_shared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace rr = rypath_packet_roulette;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}

void dyadic_expectation() {
    const rr::Vector arrival{.5f, .25f, 0, 0};
    const rr::Vector original{.25f, .125f, 0, 0};
    std::array<double, 4> average{};
    double current_and_future = 0;
    unsigned survivors = 0;
    for (unsigned i = 0; i < 8; ++i) {
        auto future = original;
        const bool survives = rr::continue_path(future, {}, 2, (float(i) + .5f) / 8);
        current_and_future += arrival[0] + (survives ? future[0] : 0);
        if (survives) {
            ++survivors;
            require(future == rr::Vector{1, .5f, 0, 0}, "Shared packet survival factor");
            for (unsigned lane = 0; lane < 4; ++lane)
                average[lane] += future[lane] / 8;
        }
    }
    require(survivors == 2 && average == std::array<double, 4>{.25, .125, 0, 0},
            "Exhaustive dyadic Bernoulli mean preserves each lane");
    require(current_and_future / 8 == .75 && arrival == rr::Vector{.5f, .25f, 0, 0},
            "Roulette affects future throughput, never the current NEE/emission term");
    auto boundary = original;
    require(rr::continue_path(boundary, {}, 2, .75f), "Equality at q survives");
    boundary = original;
    require(!rr::continue_path(boundary, {}, 2, std::nextafter(.75f, 0.f)) && boundary == original,
            "u below q terminates without rewriting stored throughput");
}

void eta_and_numerical_fallback() {
    require(rr::opposite_hemispheres(1e-20f, -1e-20f) &&
                rr::opposite_hemispheres(-1e-20f, 1e-20f) &&
                !rr::opposite_hemispheres(1e-20f, 1e-20f) &&
                !rr::opposite_hemispheres(-1e-20f, -1e-20f) &&
                !rr::opposite_hemispheres(0.f, -1.f) && !rr::opposite_hemispheres(-0.f, 1.f),
            "Transmission uses nonzero signs even when their product is subnormal");
    rr::EtaScale state;
    rr::account_transmission(state, 2, true);
    require(state.enabled && state.value == 4, "Entering event uses sampled relative eta squared");
    auto beta = rr::Vector{.25f, .25f, .25f, .25f};
    require(rr::continue_path(beta, state, 2, 0) && beta[0] == .25f,
            "Lossless refractive eta scaling prevents artificial absorption");
    rr::account_transmission(state, .5f, true);
    require(state.enabled && state.value == 1, "Exiting reciprocal eta cancels entering eta");
    rr::account_transmission(state, std::numeric_limits<float>::infinity(), false);
    require(state.enabled && state.value == 1, "Reflection/TIR does not consume eta metadata");
    rr::account_transmission(state, 1, true);
    require(state.enabled && state.value == 1, "Thin/diffuse transmission has unit outer eta");
    for (float eta : {0.f, -1.f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN(), 0x1p65f, 0x1p-65f}) {
        rr::EtaScale unsafe;
        rr::account_transmission(unsafe, eta, true);
        require(!unsafe.enabled && unsafe.value == 1, "Unsafe eta disables the heuristic only");
        rr::account_transmission(unsafe, 2, true);
        auto unchanged = rr::Vector{.25f, .125f, 0, 0};
        require(rr::continue_path(unchanged, unsafe, 2, 0) && unchanged[0] == .25f &&
                    !unsafe.enabled,
                "Unsafe cumulative eta disables roulette for the rest of the path");
    }
    rr::EtaScale cumulative{0x1p120f, true};
    rr::account_transmission(cumulative, 16, true);
    require(!cumulative.enabled && cumulative.value == 0x1p120f, "Cumulative eta overflow");
    cumulative = {0x1p-120f, true};
    rr::account_transmission(cumulative, .0625f, true);
    require(!cumulative.enabled && cumulative.value == 0x1p-120f, "Cumulative eta underflow");

    for (auto value : {rr::Vector{0x1p-26f, 0, 0, 0},
                       rr::Vector{.25f, std::numeric_limits<float>::denorm_min(), 0, 0}}) {
        const auto original = value;
        for (float u : {0.f, .5f, std::nextafter(1.f, 0.f)}) {
            value = original;
            require(rr::continue_path(value, {}, 2, u) && value == original,
                    "Tiny probability/unsafe survivor fallback is independent of the outcome");
        }
    }
    beta = {2, 1, 0, 0};
    const auto original = beta;
    require(rr::continue_path(beta, {std::numeric_limits<float>::max(), true}, 2, 0) &&
                beta == original,
            "Unrepresentable rr maximum skips this event");
    beta = {.25f, .125f, 0, 0};
    require(rr::continue_path(beta, {}, 1, 0) && beta[0] == .25f, "First scatter is ineligible");
}

void future_dispersion_and_permutations() {
    // This conditional finite-state regression deliberately allows later lambda
    // termination to change earlier finite-sample spectral interpretation. It is
    // not a proof of whole-render unbiasedness; a real dispersion mean fixture
    // remains part of renderer qualification.
    std::array<unsigned, 4> permutation{0, 1, 2, 3};
    const rr::Vector spectrum{.25f, .125f, .0625f, .03125f};
    do {
        rr::Vector shuffled{};
        for (unsigned lane = 0; lane < 4; ++lane)
            shuffled[lane] = spectrum[permutation[lane]];
        for (unsigned i = 0; i < 8; ++i) {
            auto a = spectrum, b = shuffled;
            const float u = (float(i) + .5f) / 8;
            const bool alive = rr::continue_path(a, {}, 2, u);
            require(alive == rr::continue_path(b, {}, 2, u),
                    "Pre-dispersion roulette decision is lane-permutation symmetric");
            if (alive)
                for (unsigned lane = 0; lane < 4; ++lane)
                    require(b[lane] == a[permutation[lane]], "Shared survival permutes lanes");
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    auto lambda = rypath_packet_spectrum::sample_visible(.5f);
    auto killed = spectrum;
    require(!rr::continue_path(killed, {}, 2, 0) && !lambda.secondary_terminated(),
            "A path killed before dispersion does not invent wavelength termination");
    auto alive = spectrum;
    require(rr::continue_path(alive, {}, 2, .875f), "Later dispersive surface is reachable");
    lambda.terminate_secondary();
    require(lambda.secondary_terminated() && lambda.pdf[0] > 0,
            "Surviving path retains the existing later hero-wavelength policy");
}
} // namespace

int main() {
    try {
        dyadic_expectation();
        eta_and_numerical_fallback();
        future_dispersion_and_permutations();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
