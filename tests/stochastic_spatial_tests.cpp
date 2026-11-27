// SPDX-License-Identifier: GPL-3.0-only
#include "area_evaluation_context.hpp"
#include "enhanced_confidence_shared.h"
#include "enhanced_path_shared.h"
#include "stochastic_spatial_shared.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace st = rypath_stochastic_spatial;
namespace ep = rypath_enhanced_path;
namespace cf = rypath_enhanced_confidence;
namespace er = rypath_enhanced_reservoir;
namespace reuse = rypath_reuse_mis;
static void need(bool b, const char *why) {
    if (!b)
        throw std::runtime_error(why);
}
static ep::Number num(float v) { return v == 0 ? ep::Number{0, 0} : reuse::decompose_positive(v); }
static double value(ep::Number v) { return std::ldexp(double(v.fraction), v.exponent); }
static double score(ep::NumberResult r) {
    need(r.status == rypath_reservoir::Status::ok, "score failed");
    return value(r.value);
}
// Recursively propagate the ACTUAL selected normalizer, rather than replacing
// history by a fresh exact Z. All source marginals are UCW; their draws are
// deliberately perfectly correlated. Four fixed involutions use identity/swap
// maps on two atoms. Enumerate every categorical outcome for three generations.
static unsigned recursion() {
    struct State {
        unsigned atom;
        double Z;
        long double probability;
    };
    unsigned cases = 0;
    for (unsigned present = 0; present < 16; ++present)
        for (bool null_atom : {false, true})
            for (bool diagonal : {false, true}) {
                const double F[2] = {null_atom ? 0. : -2., 3.};
                const double T[2] = {null_atom ? 0. : 1., 3.};
                std::vector<State> states{{0, T[0] / .25, .25L}, {1, T[1] / .75, .75L}};
                for (unsigned generation = 0; generation < 3; ++generation) {
                    std::vector<State> next;
                    long double conditional = 0, selected = 0, probability = 0;
                    for (const auto &state : states)
                        for (unsigned forward = 0; forward < 4; ++forward)
                            for (unsigned canonical = 0; canonical < 4; ++canonical) {
                                if (diagonal && forward != canonical)
                                    continue;
                                const long double choice = diagonal ? .25L : .0625L;
                                const float center_confidence = float(3 + generation);
                                const bool donor_present = (present >> forward) & 1u;
                                const bool canonical_present = (present >> canonical) & 1u;
                                const unsigned donor_atom = state.atom ^ (forward & 1u);
                                const unsigned inverse_atom = state.atom ^ (canonical & 1u);
                                const double source_Z =
                                    T[state.atom] == 0 ? 0 : state.Z * (forward + 2);
                                const double donor_score =
                                    !donor_present ? 0
                                                   : score(ep::pair_score(
                                                         {num(float(source_Z)),
                                                          num(float(T[state.atom] * (forward + 2))),
                                                          num(float(T[donor_atom])),
                                                          float(1 + forward + generation),
                                                          center_confidence, 1.f}));
                                const double center_score = score(ep::canonical_score(
                                    num(float(state.Z)), num(float(T[state.atom])),
                                    num(float(canonical_present ? T[inverse_atom] * (canonical + 2)
                                                                : 0)),
                                    center_confidence, float(1 + canonical + generation), 1.f));
                                const double S = donor_score + center_score;
                                const long double weight = state.probability * choice;
                                conditional +=
                                    weight * ((T[donor_atom] == 0
                                                   ? 0
                                                   : donor_score * F[donor_atom] / T[donor_atom]) +
                                              (T[state.atom] == 0
                                                   ? 0
                                                   : center_score * F[state.atom] / T[state.atom]));
                                if (S == 0) {
                                    next.push_back({0, 0, weight});
                                    probability += weight;
                                } else {
                                    for (unsigned branch = 0; branch < 2; ++branch) {
                                        const double term =
                                            branch == 0 ? donor_score : center_score;
                                        if (term == 0)
                                            continue;
                                        const unsigned atom = branch == 0 ? donor_atom : state.atom;
                                        const long double p = weight * term / S;
                                        next.push_back({atom, S, p});
                                        selected += p * S * F[atom] / T[atom];
                                        probability += p;
                                    }
                                }
                                ++cases;
                            }
                    const double integral = F[0] + F[1];
                    need(std::abs(probability - 1) < 1e-12L, "recursive probabilities lost mass");
                    need(std::abs(conditional - integral) < 2e-5L,
                         "recursive conditional mean lost UCW");
                    need(std::abs(selected - integral) < 2e-5L,
                         "recursive selected history lost UCW");
                    need(std::abs(selected - conditional) < 1e-12L,
                         "recursive categorical readout mismatch");
                    states = std::move(next);
                }
            }
    return cases;
}
int main() {
    try {
        // Distinct independent frame entropy sites, exact index bounds, serial high
        // word and seed sensitivity. Histograms are a smoke check, not independence proof.
        unsigned histogram[4][4]{};
        for (unsigned serial = 0; serial < 65536; ++serial) {
            auto s = st::draw(serial, 7, 61002, 0);
            need(st::valid(s), "invalid draw");
            need(st::evaluations(s) == (s.forward == s.canonical ? 1u : 2u),
                 "evaluation accounting");
            ++histogram[s.forward - 1][s.canonical - 1];
        }
        for (auto &row : histogram)
            for (auto n : row)
                need(n > 3600 && n < 4600, "joint-index smoke");
        need(st::evaluations({0, 1}) == 0 && st::evaluations({1, 5}) == 0, "invalid IDs admitted");
        const auto reference = st::draw(0, 0, 61002, 0);
        bool high_changed = false, seed_changed = false;
        for (unsigned i = 1; i < 32; ++i) {
            auto h = st::draw(0, i, 61002, 0), s = st::draw(0, 0, 61002, i);
            high_changed |= h.forward != reference.forward || h.canonical != reference.canonical;
            seed_changed |= s.forward != reference.forward || s.canonical != reference.canonical;
        }
        need(high_changed && seed_changed, "entropy identity omitted high word");
        for (bool history : {false, true})
            for (bool cached : {false, true})
                for (unsigned m : {0u, 1u, 2u, 4u}) {
                    if (cached && !history)
                        continue;
                    for (unsigned f = 1; f <= 4; ++f)
                        for (unsigned c = 1; c <= 4; ++c) {
                            const auto n = st::evaluations({f, c});
                            const auto p = 37ull;
                            const auto total =
                                rypath::logical_area_evaluations(p, history, m) - (4 - n) * p;
                            const auto hits = cached ? 2 * p : 0;
                            need(rypath::valid_enhanced_area_evaluation_accounting(
                                     p, history, m, cached, total - hits, hits, 0, total, n),
                                 "actual leaf accounting");
                            need(!rypath::valid_enhanced_area_evaluation_accounting(
                                     p, history, m, cached, total - hits, hits, 0, total + 1, n),
                                 "damaged accounting accepted");
                        }
                }
        // Enumerate all center/donor draws for a two-atom physical domain and every
        // border-presence mask. Different source PMFs and target functions give
        // independent UCW inputs Z=T/q. Also enumerate a physical zero/null atom.
        // Expected output is integral F, including signed F, not positive target.
        unsigned cases = 0;
        for (unsigned present = 0; present < 16; ++present)
            for (bool null_atom : {false, true}) {
                long double total_output = 0, total_selected = 0;
                for (unsigned draws = 0; draws < 32; ++draws) {
                    unsigned atom[5];
                    double q[5], target[5], Z[5];
                    long double probability = 1;
                    for (unsigned source = 0; source < 5; ++source) {
                        atom[source] = (draws >> source) & 1u;
                        const double p0 = (source % 3 + 1) * .125;
                        q[source] = atom[source] ? 1 - p0 : p0;
                        probability *= q[source];
                        target[source] = null_atom && atom[source] == 0
                                             ? 0
                                             : double(1 + source + 2 * atom[source]);
                        Z[source] = target[source] / q[source];
                    }
                    const double F[2] = {null_atom ? 0. : -2., 3.};
                    double donor[4], canonical[4], donor_F_over_T[4];
                    auto confidence = cf::spatial_begin({3, 1});
                    for (unsigned k = 0; k < 4; ++k) {
                        const bool admitted = (present >> k) & 1u;
                        const float other = float(k + 1);
                        need(cf::add_neighbor(confidence, {admitted ? k + 1 : 0,
                                                           unsigned(admitted)}) == cf::Status::ok,
                             "structural confidence");
                        const double destT =
                            null_atom && atom[k + 1] == 0 ? 0 : double(1 + 2 * atom[k + 1]);
                        const double inverseT =
                            null_atom && atom[0] == 0 ? 0 : double(2 + k + 2 * atom[0]);
                        donor[k] = admitted ? score(ep::pair_score(
                                                  {num(float(Z[k + 1])), num(float(target[k + 1])),
                                                   num(float(destT)), other, 3.f, 1.f}))
                                            : 0;
                        canonical[k] = score(ep::canonical_score(
                            num(float(Z[0])), num(float(target[0])),
                            num(float(admitted ? inverseT : 0)), 3.f, admitted ? other : 1.f, 1.f));
                        donor_F_over_T[k] = destT == 0 ? 0 : F[atom[k + 1]] / destT;
                    }
                    const double center_F_over_T = target[0] == 0 ? 0 : F[atom[0]] / target[0];
                    double oracle = 0;
                    for (unsigned k = 0; k < 4; ++k)
                        oracle +=
                            (donor[k] * donor_F_over_T[k] + canonical[k] * center_F_over_T) / 4;
                    double conditional = 0, selected = 0;
                    for (unsigned f = 0; f < 4; ++f)
                        for (unsigned c = 0; c < 4; ++c) {
                            const double S = donor[f] + canonical[c];
                            const double mean =
                                donor[f] * donor_F_over_T[f] + canonical[c] * center_F_over_T;
                            conditional += mean / 16;
                            // Exhaust both ordinary categorical-reservoir outcomes. The
                            // selected UCW is S/T_selected. No division by2 or confidence.
                            if (S > 0)
                                selected += (donor[f] / S * S * donor_F_over_T[f] +
                                             canonical[c] / S * S * center_F_over_T) /
                                            16;
                            ++cases;
                        }
                    need(std::abs(conditional - oracle) < 1e-10,
                         "conditional mean differs from all-pair sum");
                    need(std::abs(selected - oracle) < 1e-10,
                         "selected UCW differs from conditional mean");
                    total_output += probability * conditional;
                    total_selected += probability * selected;
                    unsigned expected = 3;
                    for (unsigned k = 0; k < 4; ++k)
                        if ((present >> k) & 1u)
                            expected += k + 1;
                    need(confidence.confidence == expected,
                         "sampled work changed structural confidence");
                }
                const double integral = (null_atom ? 0. : -2.) + 3.;
                need(std::abs(total_output - integral) < 2e-6, "UCW physical mean failed");
                need(std::abs(total_selected - integral) < 2e-6, "selected physical mean failed");
            }
        const auto recursive_cases = recursion();
        std::cout << "Recursive independent/diagonal cases " << recursive_cases << "\n";
        std::cout << "Stochastic pair enumeration passed " << cases
                  << " conditional/selected cases\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
