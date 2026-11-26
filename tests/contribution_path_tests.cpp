// SPDX-License-Identifier: GPL-3.0-only
#include "contribution_path_shared.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace c = rypath_contribution_path;
namespace r = rypath_reuse_mis;
namespace m = rypath_reservoir;
void need(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(long double actual, long double expected, long double tolerance, const char *message) {
    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
struct Rational {
    std::int64_t n = 0, d = 1;
    Rational(std::int64_t numerator = 0, std::int64_t denominator = 1)
        : n(numerator), d(denominator) {
        need(d != 0, "Rational denominator");
        if (d < 0) {
            n = -n;
            d = -d;
        }
        const auto divisor = std::gcd(n, d);
        n /= divisor;
        d /= divisor;
    }
    friend Rational operator+(Rational a, Rational b) { return {a.n * b.d + b.n * a.d, a.d * b.d}; }
    friend Rational operator*(Rational a, Rational b) { return {a.n * b.n, a.d * b.d}; }
    friend bool operator==(Rational a, Rational b) { return a.n == b.n && a.d == b.d; }
    long double value() const { return static_cast<long double>(n) / d; }
};
using V = std::array<long double, 3>;
V vector(c::XYZ xyz) { return {xyz.x, xyz.y, xyz.z}; }
struct Atom {
    c::XYZ xyz;
    unsigned active = 0;
};
using Tree = std::array<Atom, 5>;
using Trees = std::array<Tree, 3>;
Trees toy_trees() {
    Trees out{};
    // U0: first-hit emission, first NEE, later emission, black second NEE,
    // terminal emission at the depth limit. Slots interleave E0,N0,E1,N1,E2.
    out[0] = {{{{4, 0, 0}, 1}, {{0, 4, -4}, 1}, {{0, 0, 4}, 1}, {{}, 0}, {{4, 4, 0}, 1}}};
    // U1: a black hit plus NEE, then arrival emission before termination.
    // Inactive later slots still belong to the common replay domain.
    out[1] = {{{{}, 1}, {{8, -4, 0}, 1}, {{4, 0, 0}, 1}, {{}, 0}, {{}, 0}}};
    // U2: immediate environment; every later contribution is inactive.
    out[2] = {{{{0, 0, 8}, 1}, {{}, 0}, {{}, 0}, {{}, 0}, {{}, 0}}};
    return out;
}
// Hand-derived integer scores: support floor one plus quarter signed L1.
// The enumeration oracle does not call production target/selection functions.
constexpr std::array<std::array<int, 5>, 3> toy_targets{
    {{2, 3, 2, 1, 3}, {1, 4, 2, 1, 1}, {3, 1, 1, 1, 1}}};
constexpr float scale_with_unit_floor = 1'000'000.f;
c::Descriptor descriptor(unsigned depth) {
    const auto result = c::make_descriptor(c::DepthMode::finite, depth);
    need(result.status == c::Status::ok, "Finite descriptor");
    return result.value;
}
long double dyadic(m::DyadicProbability p) {
    return std::ldexp(static_cast<long double>(p.significand), -static_cast<int>(p.bit_count));
}
c::State choose(c::State state, c::PreparedUpdate pending, bool select_new) {
    need(pending.status == c::Status::ok, "Prepared contribution slot");
    if (pending.decision.complete) {
        need(select_new && pending.decision.selected, "First slot is certain");
    } else {
        const bool rare_success = select_new == pending.decision.select_new_on_success;
        for (unsigned i = 0; !pending.decision.complete && i < m::maximum_random_words; ++i)
            need(c::feed_random_word(pending, rare_success ? 0u : 0xffffffffu) == c::Status::ok,
                 "External contribution-selection entropy");
    }
    const auto committed = c::commit_update(state, pending);
    need(committed.status == c::Status::ok && committed.selected == select_new,
         "Contribution selection commit");
    return state;
}
struct Outcome {
    c::State state;
    long double probability = 1;
};
std::vector<Outcome> enumerate(const Tree &tree) {
    std::vector<Outcome> current{{c::begin(descriptor(2), scale_with_unit_floor), 1}};
    for (unsigned j = 0; j < tree.size(); ++j) {
        std::vector<Outcome> next;
        for (const auto &input : current) {
            const auto pending = c::prepare_update(input.state, j, tree[j].xyz, tree[j].active);
            need(pending.status == c::Status::ok, "Toy slot preparation");
            if (pending.decision.complete) {
                next.push_back({choose(input.state, pending, true), input.probability});
            } else {
                const long double rare = dyadic(pending.decision.rare_probability);
                const long double p_new = pending.decision.select_new_on_success ? rare : 1 - rare;
                next.push_back({choose(input.state, pending, true), input.probability * p_new});
                next.push_back(
                    {choose(input.state, pending, false), input.probability * (1 - p_new)});
            }
        }
        current = std::move(next);
    }
    return current;
}
void slots_and_failures() {
    const auto zero = descriptor(0);
    need(zero.slot_count == 1 && c::slot(zero, c::Kind::endpoint, 0).index == 0,
         "Depth zero retains immediate emission/environment");
    need(c::slot(zero, c::Kind::direct, 0).status == c::Status::invalid_slot,
         "No N_D contribution");
    need(c::make_descriptor(c::DepthMode::unbounded, 0).status == c::Status::unsupported_depth,
         "Unbounded depth is explicit and unsupported");
    need(c::make_descriptor(c::DepthMode(0), 0).status == c::Status::invalid_descriptor,
         "Unknown depth mode");
    const auto largest = c::make_descriptor(c::DepthMode::finite, 0x7fffffffu);
    need(largest.status == c::Status::ok && largest.value.slot_count == 0xffffffffu,
         "All representable finite slot counts are admitted");
    need(c::slot(largest.value, c::Kind::endpoint, 0x7fffffffu).index == 0xfffffffeu,
         "Largest endpoint slot does not alias no-selection");
    need(c::make_descriptor(c::DepthMode::finite, 0x80000000u).status == c::Status::count_overflow,
         "Slot-count overflow is rejected before multiplication");
    for (unsigned depth : {0u, 1u, 2u, 65u, 1000u}) {
        const auto d = descriptor(depth);
        for (unsigned j = 0; j < d.slot_count; ++j) {
            const auto decoded = c::slot(d, j);
            need(decoded.status == c::Status::ok && decoded.depth == j / 2 &&
                     decoded.kind == (j % 2 ? c::Kind::direct : c::Kind::endpoint) &&
                     c::slot(d, decoded.kind, decoded.depth).index == j,
                 "Independent interleaved E/N indexing");
        }
        need(c::slot(d, d.slot_count).status == c::Status::invalid_slot, "Index upper bound");
    }
    auto bad = descriptor(2);
    ++bad.version;
    need(!c::valid_descriptor(bad), "Version mismatch");
    bad = descriptor(2);
    --bad.slot_count;
    need(!c::valid_descriptor(bad), "Declared slot count must match depth");
    auto state = c::begin(descriptor(2), scale_with_unit_floor);
    need(c::finalize(state).status == c::Status::incomplete, "Empty tree cannot publish");
    need(c::prepare_update(state, 1, {}, 0).status == c::Status::invalid_slot,
         "Missing/out-of-order slot is rejected");
    need(c::prepare_update(state, 0, {1, 0, 0}, 0).status == c::Status::invalid_value,
         "Inactive slot cannot silently discard stale contribution");
    need(c::prepare_update(state, 0, {}, 2).status == c::Status::invalid_value,
         "Invalid active flag");
    need(c::prepare_update(state, 0, {INFINITY, 0, 0}, 1).status == c::Status::invalid_value,
         "Nonfinite slot fails visibly");
    const auto first = c::prepare_update(state, 0, {}, 0);
    need(first.target == 1, "Inactive slot retains actual positive floor");
    state = choose(state, first, true);
    need(c::prepare_update(state, 0, {}, 0).status == c::Status::invalid_slot, "Duplicate slot");
    need(c::finalize(state).status == c::Status::incomplete, "Truncated stream cannot publish");
    auto stale = first;
    need(c::commit_update(state, stale).status == c::Status::invalid_state,
         "Stale commit rejected");
    need(c::finalize(state).status == c::Status::invalid_state, "Failure is sticky");
    state = c::begin(descriptor(2), scale_with_unit_floor);
    state = choose(state, c::prepare_update(state, 0, {}, 0), true);
    const auto unfinished = c::prepare_update(state, 1, {}, 0);
    need(c::commit_update(state, unfinished).status == c::Status::incomplete,
         "Unresolved random decision cannot publish");
}
void finite_tree_enumeration() {
    const auto trees = toy_trees();
    std::array<Rational, 3> exact_mean{};
    V measured{}, truth{}, divided_by_M{};
    long double total_mass = 0;
    for (unsigned u = 0; u < trees.size(); ++u) {
        int sum = 0;
        for (auto target : toy_targets[u])
            sum += target;
        for (unsigned j = 0; j < trees[u].size(); ++j) {
            const Rational probability(toy_targets[u][j], sum);
            const Rational weight(sum, toy_targets[u][j]);
            need(probability * weight == Rational(1), "Exact rational counting-slot indicator");
            for (unsigned k = 0; k < 3; ++k) {
                const auto component = vector(trees[u][j].xyz)[k];
                exact_mean[k] = exact_mean[k] + Rational(1, 3) * probability * weight *
                                                    Rational(static_cast<std::int64_t>(component));
                truth[k] += component / 3;
            }
        }
        std::array<long double, 5> indicators{};
        long double probability_mass = 0;
        for (const auto &outcome : enumerate(trees[u])) {
            const auto reservoir = c::finalize(outcome.state);
            need(reservoir.status == c::Status::ok && reservoir.normalizer == sum,
                 "Normalizer is total target, with no division by slot count");
            need(reservoir.target == toy_targets[u][reservoir.selected_slot],
                 "Selected target uses independent rational toy score");
            const auto output = c::readout(reservoir);
            need(output.status == c::Status::ok, "Signed vector readout");
            const auto xyz = vector(output.value);
            const long double p = outcome.probability / trees.size();
            for (unsigned k = 0; k < 3; ++k) {
                measured[k] += p * xyz[k];
                divided_by_M[k] += p * xyz[k] / 5;
            }
            indicators[reservoir.selected_slot] +=
                outcome.probability * reservoir.normalizer / reservoir.target;
            probability_mass += outcome.probability;
        }
        near(probability_mass, 1, 1e-12L, "Enumerated represented decision law has unit mass");
        for (auto indicator : indicators) {
            near(indicator, 1, 2e-6L, "Every U/slot indicator retains its counting measure");
            total_mass += indicator / trees.size();
        }
    }
    near(total_mass, 5, 1e-5L, "Counting reference measure has mass five, not one");
    for (unsigned k = 0; k < 3; ++k) {
        near(exact_mean[k].value(), truth[k], 1e-12L, "Rational tree-sum oracle");
        near(measured[k], truth[k], 2e-5L, "Variable-termination emission/NEE signed mean");
    }
    need(std::abs(divided_by_M[0] - truth[0]) > 1,
         "Negative control catches accidental /M normalization");

    // The source is black at E2 after early termination, while that same fixed
    // slot contributes at the destination. All source slots remain selectable.
    long double borrowed = 0;
    for (const auto &outcome : enumerate(trees[1])) {
        const auto reservoir = c::finalize(outcome.state);
        const c::XYZ destination = reservoir.selected_slot == 4 ? c::XYZ{8, -4, 0} : c::XYZ{};
        const auto estimate = c::readout(reservoir.normalizer, destination, reservoir.target);
        need(estimate.status == c::Status::ok, "Destination indicator readout");
        borrowed += outcome.probability * estimate.value.x;
    }
    near(borrowed, 8, 2e-5L, "Source-inactive/destination-positive support is retained");
    Rational dropped_zero_mean;
    int active_target_sum = 0;
    for (unsigned j = 0; j < 5; ++j)
        if (trees[1][j].active)
            active_target_sum += toy_targets[1][j];
    for (unsigned j = 0; j < 5; ++j)
        if (trees[1][j].active)
            dropped_zero_mean =
                dropped_zero_mean + Rational(toy_targets[1][j], active_target_sum) *
                                        Rational(active_target_sum, toy_targets[1][j]) *
                                        Rational(j == 4 ? 8 : 0);
    need(dropped_zero_mean == Rational(0) && dropped_zero_mean != Rational(8),
         "Negative control catches dropped inactive-source support");
}
void generalized_fresh_MIS() {
    const auto tree = toy_trees()[0];
    constexpr std::array<int, 5> fresh{2, 3, 2, 1, 3}, history{1, 2, 5, 4, 2};
    constexpr int Zf = 11, Zh = 14;
    V mean{}, forgot_fresh_Z{}, truth{};
    for (const auto &atom : tree)
        for (unsigned k = 0; k < 3; ++k)
            truth[k] += vector(atom.xyz)[k];
    // Independent source draws on a five-atom counting domain, then analytically
    // marginalize the temporal categorical choice. This tests proper canonical
    // UCWs, not merely agreement with the score implementation.
    for (unsigned f = 0; f < 5; ++f)
        for (unsigned h = 0; h < 5; ++h) {
            const Rational probability = Rational(fresh[f], Zf) * Rational(history[h], Zh);
            const auto sf = r::temporal_fresh_score(float(Zf), float(fresh[f]), float(history[f]));
            const auto sh =
                r::temporal_history_score(float(Zh), float(fresh[h]), float(history[h]));
            const auto wrong = r::temporal_fresh_score(float(fresh[f]), float(history[f]));
            need(sf.status == r::Status::ok && sh.status == r::Status::ok &&
                     wrong.status == r::Status::ok,
                 "Temporal score domain");
            near(sf.value, static_cast<long double>(Zf) * fresh[f] / (fresh[f] + 20 * history[f]),
                 1e-6L, "General fresh score versus independent rational expression");
            const auto a = c::readout(sf.value, tree[f].xyz, float(fresh[f]));
            const auto b = c::readout(sh.value, tree[h].xyz, float(fresh[h]));
            const auto bad = c::readout(wrong.value, tree[f].xyz, float(fresh[f]));
            need(a.status == c::Status::ok && b.status == c::Status::ok &&
                     bad.status == c::Status::ok,
                 "Temporal marginal vector output");
            for (unsigned k = 0; k < 3; ++k) {
                mean[k] += probability.value() * (vector(a.value)[k] + vector(b.value)[k]);
                forgot_fresh_Z[k] +=
                    probability.value() * (vector(bad.value)[k] + vector(b.value)[k]);
            }
        }
    for (unsigned k = 0; k < 3; ++k)
        near(mean[k], truth[k], 1e-5L,
             "Canonical tree reservoir plus history integrates all atoms");
    need(std::abs(forgot_fresh_Z[0] - truth[0]) > .1L,
         "Negative control catches retaining weight-one fresh formula");

    const std::array<float, 6> values{1.f,
                                      7.f,
                                      .125f,
                                      0x1p-80f,
                                      std::numeric_limits<float>::min(),
                                      std::numeric_limits<float>::max()};
    for (float current : values)
        for (float old : values) {
            const auto legacy = r::temporal_fresh_score(current, old);
            const auto general = r::temporal_fresh_score(current, current, old);
            need(legacy.status == general.status && m::bits(legacy.value) == m::bits(general.value),
                 "Weight-one overload retains original operation bits");
        }
    const float hi = std::numeric_limits<float>::max(), lo = std::numeric_limits<float>::min();
    const auto hdr = r::temporal_fresh_score(hi, lo, lo);
    need(hdr.status == r::Status::ok, "General fresh score avoids overflowing UCW intermediate");
    near(hdr.value / static_cast<long double>(hi), 1.L / 21, 1e-8L, "HDR fresh score oracle");
    need(r::temporal_fresh_score(0, 1, 1).status == r::Status::invalid_candidate &&
             r::temporal_fresh_score(1, 0, 1).status == r::Status::invalid_target,
         "Invalid fresh normalizer and targets fail visibly");
}
long double gamma(unsigned operations, long double unit_roundoff) {
    const auto product = operations * unit_roundoff;
    need(product < 1, "Rounding-error envelope is defined");
    return product / (1 - product);
}
void bounded_error(long double actual, long double expected, long double bound,
                   const std::string &label) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > bound) {
        std::ostringstream detail;
        detail << std::setprecision(20) << label << ": actual=" << actual
               << ", expected=" << expected << ", absolute bound=" << bound;
        throw std::runtime_error(detail.str());
    }
}
void nonuniform_long_stream() {
    constexpr unsigned count = 131;
    constexpr long double u = 0x1p-24L;
    // For one represented WRS update, fl(S+t) has relative error <=u and the
    // smaller-event division has relative error <=u. Each conditional UCW
    // factor S_new*p_new/t or S_new*p_old/S_old therefore differs from one by
    // <=3u (use smaller<=larger); exponent extraction/scaling is exact here.
    // Four rounding units per slot conservatively bound their product. Extra
    // units cover final readout, target formation and the ideal-ratio comparison.
    // These bounds are fixed analytically, not fitted to measured errors.
    const long double oracle_error =
        gamma(8 * count + 64, std::numeric_limits<long double>::epsilon() / 2);
    const long double target_error = gamma(6, u) + oracle_error;
    const long double normalizer_error = gamma(count + 8, u) + oracle_error;
    const long double indicator_error = gamma(4 * count + 4, u) + oracle_error;
    const long double probability_error = gamma(5 * count + 24, u) + oracle_error;
    const long double mean_error = gamma(4 * count + 12, u) + oracle_error;
    std::array<Atom, count> increasing{};
    // Three inactive floor-only slots, then 64 equal-score pairs spanning
    // 2^-126..2^100. X and Z cancel within each pair; Y has a nonzero signed
    // sum. The lowest pairs also contain finite subnormal lane components.
    for (unsigned pair = 0; pair < 64; ++pair) {
        const int exponent = -126 + int(226 * pair / 63);
        const float magnitude = std::ldexp(1.f, exponent);
        const float y = (pair & 1u) ? -magnitude / 2 : magnitude / 2;
        const float z = (pair % 3) ? -magnitude / 4 : magnitude / 4;
        increasing[3 + 2 * pair] = {{magnitude, y, z}, 1};
        increasing[4 + 2 * pair] = {{-magnitude, y, -z}, 1};
    }
    for (bool decreasing : {false, true}) {
        const std::string label = decreasing ? "131 decreasing" : "131 increasing";
        std::array<Atom, count> atoms{};
        std::array<float, count> targets{};
        std::array<long double, count> probabilities{}, ideal_targets{};
        V truth{}, absolute_sum{}, expectation{};
        const float scale = std::numeric_limits<float>::min();
        const long double floor =
            std::max(static_cast<long double>(scale), static_cast<long double>(1e-6f) * scale);
        long double ideal_sum = 0;
        auto state = c::begin(descriptor(65), scale);
        for (unsigned j = 0; j < count; ++j) {
            atoms[j] = increasing[decreasing ? count - 1 - j : j];
            const auto components = vector(atoms[j].xyz);
            long double energy = 0;
            for (unsigned k = 0; k < 3; ++k) {
                energy += std::abs(components[k]) / 4;
                truth[k] += components[k];
                absolute_sum[k] += std::abs(components[k]);
            }
            ideal_targets[j] = floor + energy;
            ideal_sum += ideal_targets[j];
            const auto pending = c::prepare_update(state, j, atoms[j].xyz, atoms[j].active);
            need(pending.status == c::Status::ok, "Nonuniform slot preparation");
            targets[j] = pending.target;
            bounded_error(targets[j] / ideal_targets[j], 1, target_error,
                          label + " target slot " + std::to_string(j));
            if (j != 0) {
                need(decreasing ? targets[j] <= targets[j - 1] : targets[j] >= targets[j - 1],
                     "Chronological target ordering");
                const long double rare = dyadic(pending.decision.rare_probability);
                // Preserve the directly represented rare branch. Computing
                // p_old as 1-(1-rare) would erase HDR floor-slot probabilities.
                const long double p_old = pending.decision.select_new_on_success ? 1 - rare : rare;
                const long double p_new = pending.decision.select_new_on_success ? rare : 1 - rare;
                for (unsigned old = 0; old < j; ++old)
                    probabilities[old] *= p_old;
                probabilities[j] = p_new;
            } else
                probabilities[j] = 1;
            // Only score_sum affects later transition probabilities. A single
            // always-new trajectory obtains it; all selected-slot probabilities
            // are propagated above, without enumerating 2^130 histories.
            state = choose(state, pending, true);
        }
        const auto result = c::finalize(state);
        need(result.status == c::Status::ok, "Nonuniform complete reservoir");
        bounded_error(result.normalizer / ideal_sum, 1, normalizer_error, label + " normalizer");
        long double probability_mass = 0, worst_indicator = 0, worst_probability = 0;
        for (unsigned j = 0; j < count; ++j) {
            need(probabilities[j] > 0, "Every HDR/floor slot retains positive probability");
            const long double indicator = probabilities[j] * result.normalizer / targets[j];
            const long double ideal_probability = ideal_targets[j] / ideal_sum;
            bounded_error(indicator, 1, indicator_error,
                          label + " UCW indicator slot " + std::to_string(j));
            bounded_error(probabilities[j] / ideal_probability, 1, probability_error,
                          label + " ideal probability slot " + std::to_string(j));
            worst_indicator = std::max(worst_indicator, std::abs(indicator - 1));
            worst_probability =
                std::max(worst_probability, std::abs(probabilities[j] / ideal_probability - 1));
            probability_mass += probabilities[j];
            const auto readout = c::readout(result.normalizer, atoms[j].xyz, targets[j]);
            need(readout.status == c::Status::ok, "HDR per-slot signed readout");
            const auto value = vector(readout.value);
            for (unsigned k = 0; k < 3; ++k) {
                need(value[k] == 0 || std::abs(value[k]) >= std::numeric_limits<float>::min(),
                     "Nonuniform readout stays normal or zero");
                expectation[k] += probabilities[j] * value[k];
            }
        }
        bounded_error(probability_mass, 1, oracle_error, label + " probability mass");
        for (unsigned k = 0; k < 3; ++k)
            bounded_error(expectation[k], truth[k], mean_error * absolute_sum[k],
                          label + " signed mean component " + std::to_string(k));
        std::cout << label << ": max relative indicator error " << worst_indicator << " (bound "
                  << indicator_error << "), probability error " << worst_probability << " (bound "
                  << probability_error << ")\n";
    }
}
void black_large_and_numerical_cases() {
    auto state = c::begin(descriptor(65), scale_with_unit_floor);
    for (unsigned j = 0; j < 131; ++j)
        state = choose(state, c::prepare_update(state, j, {}, 0), true);
    const auto black = c::finalize(state);
    need(black.status == c::Status::ok && state.processed == 131 && black.normalizer == 131 &&
             black.selected_slot == 130,
         "True contribution count survives repeated two-input WRS rebasing");
    const auto output = c::readout(black);
    need(output.status == c::Status::ok && c::zero(output.value), "All-black tree is valid");

    const auto tiny = std::numeric_limits<float>::denorm_min();
    const auto subnormal = c::readout(2, {tiny, -2 * tiny, -0.f}, 1);
    need(subnormal.status == c::Status::ok && m::bits(subnormal.value.x) == 2u &&
             m::bits(subnormal.value.y) == 0x80000004u && m::bits(subnormal.value.z) == 0x80000000u,
         "Signed gradual-underflow and signed-zero readout");
    const float hi = std::numeric_limits<float>::max(), lo = std::numeric_limits<float>::min();
    const auto cancellation = c::readout(hi, {lo, -lo, 0}, lo);
    need(cancellation.status == c::Status::ok && cancellation.value.x == hi &&
             cancellation.value.y == -hi,
         "Do not materialize overflowing conceptual Z/target");
    state = c::begin(descriptor(1), 1);
    state = choose(state, c::prepare_update(state, 0, {hi, hi, hi}, 1), true);
    const auto overflow = c::prepare_update(state, 1, {hi, hi, hi}, 1);
    need(overflow.status == c::Status::arithmetic_failure, "Overflowing local normalizer fails");
    need(c::commit_update(state, overflow).status == c::Status::arithmetic_failure &&
             c::finalize(state).status == c::Status::arithmetic_failure,
         "No partial reservoir publication after arithmetic failure");
}
} // namespace

int main() {
    try {
        slots_and_failures();
        finite_tree_enumeration();
        generalized_fresh_MIS();
        nonuniform_long_stream();
        black_large_and_numerical_cases();
        std::cout << "Contribution paths: counting slots, rational UCWs, signed trees, support and "
                     "fresh MIS checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
