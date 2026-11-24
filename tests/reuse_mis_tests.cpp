// SPDX-License-Identifier: GPL-3.0-only
#include "reuse_mis_shared.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace r = rypath_reuse_mis;
namespace m = rypath_reservoir;
namespace mh = rypath_mutation;
using V = std::array<long double, 3>;
void need(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
void near(long double a, long double b, long double tolerance, const char *message) {
    need(std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance, message);
}
float value(r::ScalarResult x) {
    need(x.status == r::Status::ok, "Expected valid scalar");
    return x.value;
}
float fp(std::uint32_t word) { return std::bit_cast<float>(word); }
constexpr float lo = std::numeric_limits<float>::min(), hi = std::numeric_limits<float>::max();

void scalar_cases() {
    near(value(r::importance_target(-4, 8, -12, 1)), 6.000001L, 1e-6L, "Signed quarter-L1 target");
    need(value(r::importance_target(0, 0, 0, lo)) == lo, "Support floor");
    need(value(r::importance_target(hi, -hi, hi, hi)) > hi * .749f, "Finite HDR headroom");
    need(value(r::importance_target(fp(1), -fp(2), fp(3), lo)) >= lo,
         "All finite subnormal components admitted");
    need(r::importance_target(0, 0, 0, 0).status != r::Status::ok, "Invalid scale");
    need(r::importance_target(INFINITY, 0, 0, 1).status != r::Status::ok, "Infinite input");
    need(r::importance_target(NAN, 0, 0, 1).status != r::Status::ok, "NaN input");
    near(value(r::temporal_fresh_score(7, 3)), 49.L / 67, 1e-7L, "Fresh source MIS");
    near(value(r::temporal_history_score(11, 7, 3)), 1540.L / 67, 3e-6L,
         "History fixed20 source MIS");
    near(value(r::spatial_pair_score(11, 7, 3)), 77.L / 40, 3e-7L, "Spatial fixed quarter");
    need(r::temporal_fresh_score(lo, hi).status == r::Status::arithmetic_failure,
         "Truly subnormal score fails");
    // Naive20*t and naive squared target overflow; final scores are normal.
    near(value(r::temporal_fresh_score(hi, hi)) / hi, 1.L / 21, 2e-8L,
         "Overflowing denominator avoided");
    near(value(r::temporal_history_score(hi, hi, hi)) / hi, 20.L / 21, 2e-7L, "History HDR result");
    near(value(r::spatial_pair_score(hi, lo, lo)) / hi, 1.L / 8, 2e-8L,
         "Tiny denominator, large normal result");
    need(value(r::signed_contribution(lo, hi, lo)) == hi, "Avoid intermediate product underflow");
    need(value(r::signed_contribution(hi, lo, hi)) == lo, "Avoid explicit underflowed ratio");
    need(value(r::signed_contribution(lo, -hi, lo)) == -hi, "Signed HDR recovery");
    need(value(r::signed_contribution(1, fp(1), lo)) == std::ldexp(1.f, -23),
         "Recover subnormal input to normal output");
    need(m::bits(value(r::signed_contribution(1, -0.f, 1))) == 0x80000000u,
         "Signed zero bits preserved");
    need(r::signed_contribution(lo, .5f, 1).status == r::Status::arithmetic_failure,
         "True subnormal output fails");
    need(r::signed_contribution(hi, 2, 1).status == r::Status::arithmetic_failure,
         "True overflow fails");
    need(r::add_scores(hi, hi).status == r::Status::arithmetic_failure, "Score sum overflow fails");
    need(r::spatial_pair_score(0, 1, 1).status != r::Status::ok,
         "Zero score input cannot silently drop");
}

struct Moments {
    V mean{};
    long double second = 0;
    std::array<long double, 2> selected_law{};
    void add(long double probability, V output, unsigned selected) {
        for (unsigned c = 0; c < 3; ++c) {
            mean[c] += probability * output[c];
            second += probability * output[c] * output[c];
        }
        selected_law[selected] += probability;
    }
};
V contribution(long double Z, V F, long double t) {
    for (auto &x : F)
        x *= Z / t;
    return F;
}
long double mse(const Moments &s, V truth) {
    long double out = s.second;
    for (unsigned c = 0; c < 3; ++c)
        out -= 2 * truth[c] * s.mean[c] - truth[c] * truth[c];
    return out;
}
// Independent finite measure: two states of mass1/2. Enumerate every candidate
// tuple and selection result; no spatial variance or stochastic standard errors.
void temporal_finite() {
    const std::array<long double, 2> t{2, 9}, previous{13, 1};
    const std::array<V, 2> F{V{3, -4, 1}, V{-1, 8, 7}};
    const V truth{1, 2, 4};
    const long double Z = (previous[0] + previous[1]) / 2;
    Moments selected, conditional, wrongK, adaptive;
    for (unsigned fresh = 0; fresh < 2; ++fresh)
        for (unsigned history = 0; history < 2; ++history) {
            const long double p = .5L * previous[history] / (2 * Z);
            const std::array<unsigned, 2> U{fresh, history};
            const std::array<long double, 2> scores{
                t[fresh] * t[fresh] / (t[fresh] + 20 * previous[fresh]),
                Z * 20 * t[history] / (t[history] + 20 * previous[history])};
            near(value(r::temporal_fresh_score(float(t[fresh]), float(previous[fresh]))), scores[0],
                 2e-7L, "Finite fresh score oracle");
            near(value(r::temporal_history_score(float(Z), float(t[history]),
                                                 float(previous[history]))),
                 scores[1], 2e-6L, "Finite history score oracle");
            const long double S = scores[0] + scores[1];
            V C{};
            for (unsigned i = 0; i < 2; ++i) {
                const auto output = contribution(S, F[U[i]], t[U[i]]);
                selected.add(p * scores[i] / S, output, U[i]);
                auto wrong = output;
                for (auto &x : wrong)
                    x /= 2;
                wrongK.add(p * scores[i] / S, wrong, U[i]);
                for (unsigned c = 0; c < 3; ++c)
                    C[c] += scores[i] * F[U[i]][c] / t[U[i]];
            }
            // Conditional output changes ONLY the displayed estimator, never the
            // selected history law/payload/Z. Enumerate the same selection decisions.
            for (unsigned i = 0; i < 2; ++i)
                conditional.add(p * scores[i] / S, C, U[i]);
            // Negative control: candidate-dependent source coefficient is not a
            // common pointwise partition and changes the expected measure.
            const long double a_f = fresh == 0 ? 1 : 20, a_h = a_f;
            const long double sf = t[fresh] * t[fresh] / (t[fresh] + a_f * previous[fresh]);
            const long double sh = Z * a_h * t[history] / (t[history] + a_h * previous[history]);
            V bad{};
            for (unsigned c = 0; c < 3; ++c)
                bad[c] = sf * F[fresh][c] / t[fresh] + sh * F[history][c] / t[history];
            adaptive.add(p, bad, 0);
        }
    for (unsigned c = 0; c < 3; ++c) {
        near(selected.mean[c], truth[c], 1e-15L, "Temporal partition expectation");
        near(conditional.mean[c], truth[c], 1e-15L, "Conditional vector expectation");
        near(wrongK.mean[c], truth[c] / 2, 1e-15L, "Wrong /K negative control");
    }
    need(mse(conditional, truth) <= mse(selected, truth) + 1e-15L,
         "Rao-Blackwell vector MSE cannot increase");
    for (unsigned u = 0; u < 2; ++u)
        near(selected.selected_law[u], conditional.selected_law[u], 1e-18L,
             "Selected history law unchanged");
    need(std::abs(adaptive.mean[1] - truth[1]) > .01L,
         "Sample-dependent coefficient negative control detects bias");
}

void spatial_finite(bool correlated) {
    const std::array<long double, 2> center{1, 11};
    const std::array<std::array<long double, 2>, 4> neighbor{
        {{20, 1}, {1, 17}, {.01L, 40}, {13, .02L}}};
    const std::array<V, 2> F{V{4, -3, 0}, V{0, 7, -2}};
    const V truth{2, 2, -1};
    Moments selected, conditional;
    // A shared uniform chooses all source samples in correlated mode. Enumerate
    // its exact breakpoints; marginals stay correct with opposite visibility.
    std::array<long double, 7> cuts{0, 1, center[0] / (center[0] + center[1]), 0, 0, 0, 0};
    for (unsigned j = 0; j < 4; ++j)
        cuts[3 + j] = neighbor[j][0] / (neighbor[j][0] + neighbor[j][1]);
    for (unsigned i = 0; i < 7; ++i)
        for (unsigned j = i + 1; j < 7; ++j)
            if (cuts[j] < cuts[i])
                std::swap(cuts[i], cuts[j]);
    const unsigned cases = correlated ? 6 : 32;
    for (unsigned mask = 0; mask < cases; ++mask) {
        std::array<unsigned, 5> U{};
        long double probability = 1;
        if (correlated) {
            probability = cuts[mask + 1] - cuts[mask];
            const long double u = (cuts[mask + 1] + cuts[mask]) / 2;
            U[0] = u < center[0] / (center[0] + center[1]) ? 0 : 1;
            for (unsigned j = 0; j < 4; ++j)
                U[j + 1] = u < neighbor[j][0] / (neighbor[j][0] + neighbor[j][1]) ? 0 : 1;
        } else {
            for (unsigned j = 0; j < 5; ++j) {
                U[j] = (mask >> j) & 1;
                const auto &t = j == 0 ? center : neighbor[j - 1];
                probability *= t[U[j]] / (t[0] + t[1]);
            }
        }
        std::array<long double, 5> scores{};
        const long double Zc = (center[0] + center[1]) / 2;
        for (unsigned j = 0; j < 4; ++j) {
            scores[0] += Zc * center[U[0]] / (4 * (center[U[0]] + neighbor[j][U[0]]));
            const long double Zj = (neighbor[j][0] + neighbor[j][1]) / 2;
            scores[j + 1] =
                Zj * center[U[j + 1]] / (4 * (center[U[j + 1]] + neighbor[j][U[j + 1]]));
            near(value(r::spatial_pair_score(float(Zj), float(center[U[j + 1]]),
                                             float(neighbor[j][U[j + 1]]))),
                 scores[j + 1], 2e-6L, "Spatial pair oracle");
        }
        long double S = 0;
        V C{};
        for (unsigned j = 0; j < 5; ++j) {
            S += scores[j];
            for (unsigned c = 0; c < 3; ++c)
                C[c] += scores[j] * F[U[j]][c] / center[U[j]];
        }
        for (unsigned j = 0; j < 5; ++j) {
            selected.add(probability * scores[j] / S, contribution(S, F[U[j]], center[U[j]]), U[j]);
            conditional.add(probability * scores[j] / S, C, U[j]);
        }
    }
    for (unsigned c = 0; c < 3; ++c)
        near(selected.mean[c], truth[c], 3e-15L,
             "Spatial partition expectation, including correlated sources");
    need(mse(conditional, truth) <= mse(selected, truth) + 1e-14L,
         "Spatial conditional vector second moment");
    for (unsigned i = 0; i < 2; ++i)
        near(selected.selected_law[i], conditional.selected_law[i], 1e-18L,
             "Spatial history law unchanged");
    // Boundary-clamped duplicate sources: all five candidates are one sample.
    for (float t : {1.f, 11.f}) {
        const float pair = value(r::spatial_pair_score(6, t, t));
        float center_score = value(r::add_scores(pair, pair));
        center_score = value(r::add_scores(center_score, center_score));
        near(center_score + 4 * pair, 6, 1e-6L, "Duplicate clamped candidates conserve normalizer");
    }
}

r::NormalizerAcceptanceResult decide(r::PreparedNormalizerAcceptance p, bool accept) {
    for (unsigned i = 0; !p.decision.complete && i < 9; ++i)
        need(r::feed_random_word(p, accept ? 0u : 0xffffffffu) == mh::Status::ok,
             "Acceptance entropy feed");
    auto result = r::finalize_normalizer_acceptance(p);
    need(result.status == mh::Status::ok, "Complete normalizer decision");
    return result;
}
void acceptance_cases() {
    const std::array<float, 8> targets{lo, fp(0x00800001), .03125f, .7f, 1.f, 3.f, 127.f, hi};
    for (float old : targets)
        for (float next : targets) {
            auto p = r::prepare_normalizer_acceptance(7, old, next);
            need(p.status == mh::Status::ok, "Extreme target ratio admits Z");
            const bool up = next > old;
            int es, el;
            const float sf = std::frexp(std::min(old, next), &es),
                        lf = std::frexp(std::max(old, next), &el);
            const float ratio = float((long double)sf / lf),
                        product = float((long double)ratio * lf);
            const float correction =
                old == next ? 1 : float(up ? (long double)product / sf : (long double)sf / product);
            const float expected = float((long double)7 * correction);
            auto yes = decide(p, true);
            need(yes.accepted && m::bits(yes.normalizer) == m::bits(expected),
                 "Long-double rounded operation oracle");
            if (next < old) {
                const auto no = decide(p, false);
                need(!no.accepted && m::bits(no.normalizer) == m::bits(7.f),
                     "Reject retains normalizer bits");
                const long double actual = std::ldexp((long double)p.smaller_ratio.significand,
                                                      -int(p.smaller_ratio.bit_count));
                const long double oracle = std::ldexp((long double)ratio, es - el);
                near(actual, oracle, 0, "Exact dyadic probability vs independent exponent oracle");
            }
        }
    auto equal = decide(r::prepare_normalizer_acceptance(fp(0x3f800001), 3, 3), true);
    need(equal.words_used == 0 && m::bits(equal.normalizer) == 0x3f800001,
         "Equal target retains exact bits");
    need(r::prepare_normalizer_acceptance(lo, lo, hi).status == mh::Status::ok,
         "Z avoids old UCW underflow preflight");
    need(mh::prepare_acceptance(lo, lo, hi).status == mh::Status::arithmetic_failure,
         "Negative control old UCW API fails differently");
    need(r::prepare_normalizer_acceptance(0, 1, 2).status == mh::Status::invalid_weight,
         "Invalid Z");
    need(r::prepare_normalizer_acceptance(1, fp(1), 2).status == mh::Status::invalid_target,
         "Subnormal target rejected");
    auto p = r::prepare_normalizer_acceptance(1, 3, 1);
    need(r::finalize_normalizer_acceptance(p).status == mh::Status::incomplete,
         "Incomplete entropy rejected");
    p.accepted_normalizer = 2;
    need(r::feed_random_word(p, 0) == mh::Status::invalid_decision,
         "Tampered preflight rejected before entropy");
    p = r::prepare_normalizer_acceptance(1, 1, 3);
    p.decision.selected = false;
    need(r::finalize_normalizer_acceptance(p).status == mh::Status::invalid_decision,
         "Tampered deterministic branch");
    bool failed_hi = false, failed_lo = false;
    for (unsigned i = 1; i < 50000; ++i) {
        const float a = fp(0x3f000000u | ((i * 2654435761u) & 0x007fffffu));
        const float b = fp(0x3f800000u | ((i * 2246822519u) & 0x007fffffu));
        failed_hi |=
            r::prepare_normalizer_acceptance(hi, a, b).status == mh::Status::arithmetic_failure;
        failed_lo |=
            r::prepare_normalizer_acceptance(lo, a, b).status == mh::Status::arithmetic_failure;
    }
    need(failed_hi && failed_lo, "Accepted normalizer overflow/subnormal preflight witnesses");
    for (unsigned i = 1; i < 4000; ++i) {
        const float a =
            std::ldexp(fp(0x3f000000u | ((i * 2654435761u) & 0x007fffffu)), int(i % 200) - 100);
        const float b = std::ldexp(fp(0x3f000000u | ((i * 2246822519u) & 0x007fffffu)),
                                   int((i * 53) % 200) - 100);
        const float Z = std::ldexp(fp(0x3f000000u | ((i * 3266489917u) & 0x007fffffu)),
                                   int((i * 37) % 200) - 100);
        int es, el;
        const float sf = std::frexp(std::min(a, b), &es), lf = std::frexp(std::max(a, b), &el);
        const float ratio = float((long double)sf / lf), p = float((long double)ratio * lf);
        const float C = a == b ? 1 : float(b > a ? (long double)p / sf : (long double)sf / p);
        const auto accepted = decide(r::prepare_normalizer_acceptance(Z, a, b), true);
        need(m::bits(accepted.normalizer) == m::bits(float((long double)Z * C)),
             "Wide normalizer/correction long-double oracle");
    }
}

void normalizer_two_state() {
    const std::array<float, 2> t{1.f, 11.25f};
    const std::array<V, 2> F{V{3, -4, 1}, V{-1, 8, 7}};
    const V truth{1, 2, 4};
    const float Z = (t[0] + t[1]) / 2;
    Moments actual, ideal;
    for (unsigned source = 0; source < 2; ++source) {
        const unsigned dest = source ^ 1;
        const long double mass = (long double)t[source] / (2 * Z);
        const auto pending = r::prepare_normalizer_acceptance(Z, t[source], t[dest]);
        const long double A = t[dest] >= t[source]
                                  ? 1
                                  : std::ldexp((long double)pending.smaller_ratio.significand,
                                               -int(pending.smaller_ratio.bit_count));
        const long double exact_A = std::min(1.L, (long double)t[dest] / t[source]);
        const auto accepted = decide(pending, true);
        V output{};
        for (unsigned c = 0; c < 3; ++c)
            output[c] =
                value(r::signed_contribution(accepted.normalizer, float(F[dest][c]), t[dest]));
        actual.add(mass * A, output, dest);
        actual.add(mass * (1 - A), contribution(Z, F[source], t[source]), source);
        ideal.add(mass * exact_A, contribution(Z, F[dest], t[dest]), dest);
        ideal.add(mass * (1 - exact_A), contribution(Z, F[source], t[source]), source);
    }
    for (unsigned c = 0; c < 3; ++c) {
        near(ideal.mean[c], truth[c], 3e-15L, "Exact two-state MH partition expectation");
        near(actual.mean[c], truth[c], 5e-6L,
             "Actual rounded dyadic normalizer vector expectation");
    }
    near(actual.second, ideal.second, 2e-4L,
         "Actual dyadic second moment vs exact two-state kernel");
}
struct Joint {
    unsigned u;
    long double Z, p;
};
using Law = std::vector<Joint>;
long double dyadic(m::DyadicProbability q) {
    return std::ldexp((long double)q.significand, -int(q.bit_count));
}
Law reuse_generation(const Law &law, const std::array<float, 2> &old, const std::array<float, 2> &t,
                     bool actual) {
    Law out;
    for (const auto &h : law)
        for (unsigned fresh = 0; fresh < 2; ++fresh) {
            long double sf =
                (long double)t[fresh] * t[fresh] / ((long double)t[fresh] + 20 * old[fresh]);
            long double sh = h.Z * 20 * t[h.u] / ((long double)t[h.u] + 20 * old[h.u]);
            long double Z = sf + sh, pnew = sh / Z;
            if (actual) {
                sf = value(r::temporal_fresh_score(t[fresh], old[fresh]));
                sh = value(r::temporal_history_score(float(h.Z), t[h.u], old[h.u]));
                m::State source;
                source.score_sum = float(sf);
                source.selected_target = 1;
                source.candidate_count = 1;
                source.selected_index = 0;
                const auto selection = m::prepare_update(source, float(sh), 1);
                need(selection.status == m::Status::ok, "RandomZ selection preflight");
                Z = selection.score_sum;
                pnew = selection.select_new_on_success ? dyadic(selection.rare_probability)
                                                       : 1 - dyadic(selection.rare_probability);
            }
            out.push_back({fresh, Z, h.p * .5L * (1 - pnew)});
            out.push_back({h.u, Z, h.p * .5L * pnew});
        }
    return out;
}
Law move_generation(const Law &law, const std::array<float, 2> &t, bool actual) {
    Law out;
    for (const auto &state : law) {
        const unsigned next = state.u ^ 1;
        long double accept = std::min(1.L, (long double)t[next] / t[state.u]), Z = state.Z;
        if (actual) {
            const auto pending =
                r::prepare_normalizer_acceptance(float(state.Z), t[state.u], t[next]);
            need(pending.status == mh::Status::ok, "RandomZ mutation preflight");
            accept = t[next] >= t[state.u] ? 1 : dyadic(pending.smaller_ratio);
            Z = decide(pending, true).normalizer;
        }
        out.push_back({next, Z, state.p * accept});
        if (accept < 1)
            out.push_back({state.u, state.Z, state.p * (1 - accept)});
    }
    return out;
}
Moments joint_moments(const Law &law, const std::array<float, 2> &t) {
    const std::array<V, 2> F{V{3, -4, 1}, V{-1, 8, 7}};
    Moments result;
    for (const auto &state : law)
        result.add(state.p, contribution(state.Z, F[state.u], t[state.u]), state.u);
    return result;
}
void multigeneration_random_Z() {
    std::array<float, 2> old{3, 7};
    Law actual{{0, 3, .5L}, {1, 7, .5L}}, ideal = actual;
    // Fixed error envelope:128binary32 eps over three stages, against absolute
    // signal/moment. This exceeds the fixed normal arithmetic+selector operation
    // count; no empirical SE, observed residual or adaptive threshold is used.
    constexpr long double budget = 128 * std::numeric_limits<float>::epsilon();
    const V truth{1, 2, 4}, absolute_signal{2, 6, 4};
    for (const auto &t : {std::array<float, 2>{11, 2}, {.125f, 17}, {5, 3}}) {
        actual = reuse_generation(actual, old, t, true);
        ideal = reuse_generation(ideal, old, t, false);
        for (unsigned phase = 0; phase < 2; ++phase) {
            const auto a = joint_moments(actual, t), b = joint_moments(ideal, t);
            for (unsigned c = 0; c < 3; ++c) {
                near(b.mean[c], truth[c], 2e-12L, "Independent randomZ finite oracle expectation");
                near(a.mean[c], truth[c], budget * absolute_signal[c],
                     "Actual randomZ multigeneration reuse/MH expectation");
            }
            near(a.second, b.second, budget * b.second, "RandomZ finite second moment envelope");
            need(std::any_of(actual.begin(), actual.end(),
                             [&](const Joint &s) { return s.Z != actual.front().Z; }),
                 "History normalizers genuinely random");
            if (phase == 0) {
                actual = move_generation(actual, t, true);
                ideal = move_generation(ideal, t, false);
            }
        }
        old = t;
    }
}
void extreme_tail_cancellation() {
    const float floor = value(r::importance_target(0, 0, 0, lo));
    const float hdr = value(r::importance_target(hi, hi, hi, lo));
    const std::array<long double, 2> previous{floor, hdr}, current{hdr, floor};
    const long double Z = (previous[0] + previous[1]) / 2;
    long double mass = 0, clipped = 0, rare_probability = 0;
    for (unsigned f = 0; f < 2; ++f)
        for (unsigned h = 0; h < 2; ++h) {
            const long double p = .5L * previous[h] / (2 * Z);
            const std::array<long double, 2> scores{
                current[f] * current[f] / (current[f] + 20 * previous[f]),
                Z * 20 * current[h] / (current[h] + 20 * previous[h])};
            const long double total = scores[0] + scores[1];
            const std::array<unsigned, 2> U{f, h};
            for (unsigned i = 0; i < 2; ++i) {
                const long double probability = p * scores[i] / total,
                                  output = U[i] == 1 ? total / current[1] : 0;
                mass += probability * output;
                if (probability >= 0x1p-120L)
                    clipped += probability * output;
                else
                    rare_probability += probability;
            }
        }
    near(mass, .5L, 1e-15L, "Extreme epsilon/HDR exact tail cancellation");
    need(rare_probability < 8 * 0x1p-120L && clipped < .3L,
         "Dropping tiny events loses finite signal");
    need(r::temporal_fresh_score(floor, hdr).status == r::Status::arithmetic_failure,
         "Unrepresentable positive MIS score fails, never drops");
}
int main() {
    try {
        scalar_cases();
        temporal_finite();
        spatial_finite(false);
        spatial_finite(true);
        acceptance_cases();
        normalizer_two_state();
        multigeneration_random_Z();
        extreme_tail_cancellation();
        std::cout << "reuse MIS: scalar extremes, exact finite partitions/moments, correlated "
                     "pools and dyadic normalizer acceptance passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
