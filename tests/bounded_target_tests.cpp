// SPDX-License-Identifier: GPL-3.0-only
#include "reservoir_shared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace r = rypath_reservoir;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
constexpr long double metal_unit = 0x1p-23L;
constexpr long double proxy_max = static_cast<long double>(1.1f);
float target(std::array<float, 3> xyz, float scale) {
    const auto value = r::bounded_target_xyz(xyz[0], xyz[1], xyz[2], scale);
    require(value.status == r::Status::ok && std::isfinite(value.value) && value.value >= 1 &&
                value.value <= 1.1f,
            "Bounded proxy accepts finite XYZ and preserves [1,1.1f]");
    long double full = 0, flushed = 0;
    for (const auto component : xyz) {
        full += std::abs(static_cast<long double>(component));
        if (component == 0 || std::isnormal(component))
            flushed += std::abs(static_cast<long double>(component));
    }
    const auto ideal = 1 + .1L * full / (static_cast<long double>(scale) + full);
    const auto ftz = 1 + .1L * flushed / (static_cast<long double>(scale) + flushed);
    require(value.value >= ftz - 8 * metal_unit && value.value <= ideal + 8 * metal_unit,
            "Independent long-double bounded proxy oracle (subnormal FTZ bracket)");
    if (full == 0)
        require(value.value == 1, "Black paths have exact unit target support");
    return value.value;
}
void range_and_inputs() {
    const std::array<float, 9> values{0,
                                      -0.f,
                                      std::numeric_limits<float>::min(),
                                      0x1p-127f,
                                      1e-6f,
                                      -.7f,
                                      2.1f,
                                      std::numeric_limits<float>::max(),
                                      -std::numeric_limits<float>::max()};
    for (const auto x : values)
        for (const auto y : values)
            for (const auto z : values)
                for (const auto scale : {std::numeric_limits<float>::min(), 1e-6f, 1.f, 3.7f,
                                         std::numeric_limits<float>::max()})
                    target({x, y, z}, scale);
    for (const auto invalid :
         {0.f, -1.f, std::numeric_limits<float>::denorm_min(),
          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        require(r::bounded_target_xyz(1, 2, 3, invalid).status == r::Status::invalid_target,
                "Invalid target scale rejected");
    for (const auto invalid :
         {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN()})
        for (unsigned coordinate = 0; coordinate < 3; ++coordinate) {
            std::array<float, 3> xyz{};
            xyz[coordinate] = invalid;
            require(r::bounded_target_xyz(xyz[0], xyz[1], xyz[2], 1).status ==
                        r::Status::invalid_target,
                    "Nonfinite XYZ rejected");
        }
    const auto invariant = target({.7f, -1.3f, 2.1f}, 3.7f);
    for (const int exponent : {-80, -20, 20, 80})
        require(target({std::ldexp(.7f, exponent), std::ldexp(-1.3f, exponent),
                        std::ldexp(2.1f, exponent)},
                       std::ldexp(3.7f, exponent)) == invariant,
                "Power-of-two XYZ/scale unit changes preserve target");
}
struct PoolResult {
    float weight;
    long double probability;
};
PoolResult pool(const std::vector<float> &weights, const std::vector<float> &targets,
                unsigned selected) {
    r::State state;
    std::vector<long double> replace, keep;
    for (unsigned i = 0; i < weights.size(); ++i) {
        auto pending = r::prepare_update(state, weights[i], targets[i]);
        require(pending.status == r::Status::ok, "Bounded candidate preparation");
        long double rare = 0;
        if (!pending.complete)
            rare = std::ldexp(static_cast<long double>(pending.rare_probability.significand),
                              -static_cast<int>(pending.rare_probability.bit_count));
        replace.push_back(i == 0 ? 1 : pending.select_new_on_success ? rare : 1 - rare);
        keep.push_back(i == 0 ? 0 : pending.select_new_on_success ? 1 - rare : rare);
        const bool want_new = i == 0 || i == selected;
        const bool want_rare = want_new == pending.select_new_on_success;
        for (unsigned word = 0; !pending.complete && word < r::maximum_random_words; ++word)
            require(r::feed_random_word(pending, want_rare ? 0u : 0xffffffffu) == r::Status::ok,
                    "Bounded selector interval");
        require(pending.complete && pending.selected == want_new &&
                    r::commit_update(state, pending).status == r::Status::ok,
                "Bounded selection commit");
    }
    const auto result = r::finalize(state);
    require(result.status == r::Status::ok && state.candidate_count == weights.size() &&
                state.selected_index == selected,
            "Bounded final candidate count/weight");
    auto probability = replace[selected];
    for (unsigned i = selected + 1; i < weights.size(); ++i)
        probability *= keep[i];
    return {result.value, probability};
}
struct Outcome {
    unsigned value;
    float weight;
    long double probability;
};
void measure(const std::vector<Outcome> &outcomes, long double bound) {
    std::array<long double, 3> mass{};
    long double probability = 0;
    for (const auto &entry : outcomes) {
        mass[entry.value] += entry.probability * entry.weight;
        probability += entry.probability;
    }
    require(std::abs(probability - 1) < 1e-12L,
            "Enumerated bounded selector intervals partition unity");
    for (const auto value : mass)
        require(std::abs(value - 1.L / 3) <= bound / 3 + 1e-13L,
                "Bounded target indicator measure");
    require(std::abs(-3 * mass[0] + 5 * mass[1] + 11 * mass[2] - 13.L / 3) <=
                bound * 19.L / 3 + 1e-13L,
            "Bounded target arbitrary signed-h measure");
}
void finite_measure() {
    const auto large = std::numeric_limits<float>::max();
    const std::array<float, 3> a{target({0, 0, 0}, 1), target({1, 2, 4}, 1),
                                 target({large, large, large}, 1)};
    const std::array<float, 3> b{target({large, large, large}, 2), target({0, 0, 0}, 2),
                                 target({2, 1, 0}, 2)};
    std::vector<Outcome> history;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            for (unsigned selected = 0; selected < 2; ++selected) {
                const auto result = pool({1, 1}, {a[i], a[j]}, selected);
                history.push_back({selected ? j : i, result.weight, result.probability / 9});
            }
    const auto eta2 = (1 + 3 * metal_unit) * std::pow(1 + metal_unit, 3) - 1;
    const auto eta5 = std::pow(1 + 3 * metal_unit, 4) * std::pow(1 + metal_unit, 3) - 1;
    long double bound = eta2;
    measure(history, bound);
    for (unsigned generation = 0; generation < 3; ++generation) {
        const auto &t = generation % 2 == 0 ? b : a;
        std::vector<Outcome> next;
        for (const auto &old : history)
            for (unsigned fresh = 0; fresh < 3; ++fresh)
                for (unsigned selected = 0; selected < 2; ++selected) {
                    const auto temporal = pool({old.weight, 1}, {t[old.value], t[fresh]}, selected);
                    const auto value = selected ? fresh : old.value;
                    // Fixed five correlated spatial imports: selected payload is the
                    // same for every selector history; exact marginalization is valid.
                    const auto spatial = pool(std::vector<float>(5, temporal.weight),
                                              std::vector<float>(5, t[value]), 0);
                    next.push_back(
                        {value, spatial.weight, old.probability * temporal.probability / 3});
                }
        history = std::move(next);
        bound = eta5 + (1 + eta5) * (eta2 + (1 + eta2) * bound / 2);
        measure(history, bound);
    }
    std::cout << "Bounded target finite-domain generations=3 outcomes=" << history.size()
              << " error bound=" << static_cast<double>(bound) << '\n';
}
float inward(long double value, bool lower) {
    float endpoint = static_cast<float>(value);
    if (lower && endpoint < value)
        endpoint = std::nextafter(endpoint, std::numeric_limits<float>::infinity());
    if (!lower && endpoint > value)
        endpoint = std::nextafter(endpoint, 0.f);
    return endpoint;
}
void endpoint_pools(const std::vector<std::array<float, 2>> &weight_endpoints, long double lower,
                    long double upper) {
    const unsigned count = static_cast<unsigned>(weight_endpoints.size());
    for (unsigned wmask = 0; wmask < (1u << count); ++wmask)
        for (unsigned tmask = 0; tmask < (1u << count); ++tmask) {
            std::vector<float> weights(count), targets(count);
            for (unsigned i = 0; i < count; ++i) {
                weights[i] = weight_endpoints[i][(wmask >> i) & 1];
                targets[i] = ((tmask >> i) & 1) ? 1.1f : 1.f;
            }
            for (unsigned selected = 0; selected < count; ++selected) {
                const auto result = pool(weights, targets, selected);
                require(result.weight >= lower && result.weight <= upper,
                        "Every endpoint-pool branch stays inside global UCW recurrence");
            }
        }
}
void global_weight_bound() {
    // Up to five inputs: one product, four sum roundings and the separated
    // normalization are conservatively covered by eight relative Metal units.
    const auto ratio_upper = proxy_max * std::pow(1 + metal_unit, 8);
    const auto ratio_lower = std::pow(1 - metal_unit, 8) / proxy_max;
    const auto upper_fixed = ratio_upper * ratio_upper / (2 - ratio_upper * ratio_upper);
    const auto lower_fixed = ratio_lower * ratio_lower / (2 - ratio_lower * ratio_lower);
    require(upper_fixed < 1.533L && lower_fixed > .704L,
            "Bounded target global stationary UCW envelope");
    long double lower = 1, upper = 1;
    for (unsigned generation = 0; generation < 12; ++generation) {
        const auto temporal_lower = ratio_lower * (1 + lower) / 2,
                   temporal_upper = ratio_upper * (1 + upper) / 2;
        endpoint_pools({{1, 1}, {inward(lower, true), inward(upper, false)}}, temporal_lower,
                       temporal_upper);
        lower = ratio_lower * temporal_lower;
        upper = ratio_upper * temporal_upper;
        endpoint_pools(std::vector<std::array<float, 2>>(
                           5, {inward(temporal_lower, true), inward(temporal_upper, false)}),
                       lower, upper);
    }
    std::cout << "Bounded target global UCW stationary interval ["
              << static_cast<double>(lower_fixed) << ',' << static_cast<double>(upper_fixed)
              << "] endpoint generations=12\n";
}
} // namespace
int main() {
    try {
        range_and_inputs();
        finite_measure();
        global_weight_bound();
        std::cout << "Bounded target validation passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
