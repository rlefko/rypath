// SPDX-License-Identifier: GPL-3.0-only
#include "area_math_probe_wire.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace p = area_math_probe;
namespace r = rypath_reservoir;
namespace rr = rypath_replay;
void need(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
std::string error(NSError *failure) {
    return failure ? failure.localizedDescription.UTF8String : "No Metal error description";
}
using Words = std::array<std::uint32_t, 4>;
Words reference_philox(Words counter, std::array<std::uint32_t, 2> key) {
    // Test-side Random1234x32-10 reference, anchored below by three pinned KATs.
    // Fresh/tail domain construction is independent of the shared helper.
    for (unsigned round = 0; round < 10; ++round) {
        const std::uint64_t a = std::uint64_t(counter[0]) * 0xd2511f53u;
        const std::uint64_t b = std::uint64_t(counter[2]) * 0xcd9e8d57u;
        counter = {std::uint32_t(b >> 32) ^ counter[1] ^ key[0], std::uint32_t(b),
                   std::uint32_t(a >> 32) ^ counter[3] ^ key[1], std::uint32_t(a)};
        key[0] += 0x9e3779b9u;
        key[1] += 0xbb67ae85u;
    }
    return counter;
}
Words words(rr::Words4 value) { return {value.x, value.y, value.z, value.w}; }
rr::ReplayState reference_fresh(const rr::FreshIdentity &identity) {
    const auto block = [&](unsigned domain) {
        return reference_philox({identity.pixel, identity.sample_low, identity.sample_high, domain},
                                {identity.seed_low, identity.seed_high});
    };
    const auto tail = block(0x66726b31u), film = block(0x66726631u), lens = block(0x66726c31u),
               wavelength = block(0x66727731u);
    return {{tail[0], tail[1]}, {film[0], film[1]}, {lens[0], lens[1]}, wavelength[0], 1};
}
struct Case {
    p::Query query{};
    std::string label;
    unsigned selected = 0;
    int finite_i = -1, finite_j = -1, black_i = -1, black_j = -1;
    int absorption_arm = -1;
    int boundary_offset = 2;
    int temporal_value = -1;
    long double prior_probability = 0;
    bool exact_probability = false;
    r::Status expected = r::Status::ok;
    r::Status target_expected = r::Status::ok, normalized_expected = r::Status::ok;
    r::Status bounded_expected = r::Status::ok;
    unsigned expected_bit_count = 0;
    bool finalize_only = false;
};
Case base(std::string label) {
    Case result;
    result.label = std::move(label);
    result.query.identity = {0x80000001u, 29, 0x80000001u, 0xabcdef01u, 0x98765432u};
    result.query.address = {rr::TailDomain::medium, 0x80000000u, 0xfedcba98u, 0x80000001u};
    result.query.uniform_word = 0xffffffffu;
    result.query.xyz[0] = -2;
    result.query.xyz[1] = 3;
    result.query.xyz[2] = -4;
    result.query.epsilon = 1;
    result.query.bounded_scale = 1;
    return result;
}
void stream(Case &test, const std::vector<float> &weights, const std::vector<float> &targets,
            unsigned selected) {
    need(weights.size() == targets.size() && !targets.empty() &&
             targets.size() <= p::maximum_candidates,
         "Probe case dimensions");
    test.query.candidate_count = static_cast<unsigned>(targets.size());
    test.selected = selected;
    long double sum = 0;
    for (unsigned i = 0; i < targets.size(); ++i) {
        auto &candidate = test.query.candidates[i];
        candidate.weight = weights[i];
        candidate.target = targets[i];
        candidate.word_count = 9;
        const long double score = static_cast<long double>(weights[i]) * targets[i];
        const bool new_is_small = score <= sum;
        const bool choose_new = i == selected;
        std::fill(std::begin(candidate.words), std::end(candidate.words),
                  choose_new == new_is_small ? 0u : 0xffffffffu);
        sum += score;
    }
}
std::vector<Case> cases() {
    std::vector<Case> result;
    // Published Random123 v1.14.0 tests/kat_vectors, pinned by third_party/random123.
    const std::array<Words, 3> counter{
        {{0, 0, 0, 0}, {~0u, ~0u, ~0u, ~0u}, {0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u}}};
    const std::array<std::array<unsigned, 2>, 3> key{
        {{0, 0}, {~0u, ~0u}, {0xa4093822u, 0x299f31d0u}}};
    const std::array<Words, 3> known{{{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u},
                                      {0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu},
                                      {0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u}}};
    for (unsigned i = 0; i < 3; ++i) {
        need(reference_philox(counter[i], key[i]) == known[i], "Independent CPU Random123 KAT");
        auto test = base("Random123 KAT " + std::to_string(i));
        test.query.counter = {counter[i][0], counter[i][1], counter[i][2], counter[i][3]};
        test.query.key = {key[i][0], key[i][1]};
        test.query.uniform_word = i == 0 ? 0 : i == 1 ? 255 : ~0u;
        stream(test, {1}, {1}, 0);
        result.push_back(test);
    }
    constexpr std::array<float, 3> target{1, 2, 4};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            for (unsigned selected = 0; selected < 2; ++selected) {
                auto test = base("Finite domain A");
                stream(test, {1, 1}, {target[i], target[j]}, selected);
                test.finite_i = static_cast<int>(i);
                test.finite_j = static_cast<int>(j);
                result.push_back(test);
            }
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 2; ++j)
            for (unsigned selected = 0; selected < 2; ++selected) {
                auto test = base("Black positive floor");
                stream(test, {1, 1}, {i ? 2.000001f : 1e-6f, j ? 2.000001f : 1e-6f}, selected);
                test.black_i = static_cast<int>(i);
                test.black_j = static_cast<int>(j);
                test.query.xyz[0] = test.query.xyz[1] = test.query.xyz[2] = 0;
                test.query.epsilon = 1e-6f;
                result.push_back(test);
            }
    for (const auto target_pair : {std::array<float, 2>{3, 1}, {1, 3}, {1, 1}}) {
        const unsigned threshold = target_pair[0] == target_pair[1] ? 0x80000000u : 0x40000000u;
        for (int delta = -1; delta <= 1; ++delta) {
            auto test = base("Exact dyadic below/equal/above");
            const bool rare_success = delta < 0;
            const bool new_small = target_pair[1] <= target_pair[0];
            stream(test, {1, 1}, {target_pair[0], target_pair[1]},
                   rare_success == new_small ? 1 : 0);
            test.query.candidates[1].words[0] = threshold + delta;
            test.exact_probability = true;
            test.boundary_offset = delta;
            result.push_back(test);
        }
    }
    for (unsigned selected = 0; selected < 5; ++selected) {
        auto test = base("Five-input normalized pool");
        stream(test, {1, 1, 1, 1, 1}, {1, 2, 4, 3, 5}, selected);
        result.push_back(test);
    }
    for (unsigned arm = 0; arm < 2; ++arm)
        for (unsigned selected = 0; selected < 32; ++selected) {
            std::vector<float> targets(32, arm == 0 ? 1.f : 0x1p24f);
            targets[0] = arm == 0 ? 0x1p24f : 1.f;
            auto test = base("Capped correlated absorption");
            stream(test, std::vector<float>(32, 1), targets, selected);
            test.absorption_arm = static_cast<int>(arm);
            result.push_back(test);
        }
    for (const auto pair : {std::array<float, 2>{1e-6f, 100}, {100, 1e-6f}}) {
        for (unsigned selected = 0; selected < 2; ++selected) {
            auto test = base("HDR target order");
            stream(test, {1, 1}, {pair[0], pair[1]}, selected);
            result.push_back(test);
        }
    }
    for (const auto pair : {std::array<float, 2>{0x1p-126f, 0x1p127f}, {0x1p127f, 0x1p-126f}}) {
        for (unsigned selected = 0; selected < 2; ++selected) {
            auto test = base("HDR weight exponent gap");
            stream(test, {pair[0], pair[1]}, {1, 1}, selected);
            result.push_back(test);
        }
    }
    for (unsigned selected = 0; selected < 5; ++selected) {
        auto test = base("Inexact products and sums");
        stream(test, {0x1.000002p0f, .7f, 1.9f, 1.1f, .3f}, {0x1.000002p0f, 2.3f, .8f, 1.7f, 3.1f},
               selected);
        test.query.xyz[0] = .7f;
        test.query.xyz[1] = -1.3f;
        test.query.xyz[2] = 2.1f;
        test.query.epsilon = 1e-6f;
        result.push_back(test);
    }
    auto inexact_one = base("Inexact weight times target");
    stream(inexact_one, {0x1.000002p0f}, {0x1.000002p0f}, 0);
    result.push_back(inexact_one);
    auto absorbed_target = base("Target sum absorption");
    stream(absorbed_target, {1}, {1}, 0);
    absorbed_target.query.xyz[0] = 0x1p24f;
    absorbed_target.query.xyz[1] = absorbed_target.query.xyz[2] = 1;
    result.push_back(absorbed_target);
    for (const auto pair : {std::array<float, 2>{std::numeric_limits<float>::min(),
                                                 std::numeric_limits<float>::max()},
                            {std::numeric_limits<float>::max(), std::numeric_limits<float>::min()}})
        for (unsigned selected = 0; selected < 2; ++selected) {
            auto test = base("HDR weight exponent gap");
            stream(test, {pair[0], pair[1]}, {1, 1}, selected);
            test.expected_bit_count = 277;
            result.push_back(test);
        }
    for (unsigned kind = 0; kind < 3; ++kind) {
        auto test = base("Explicit score overflow or underflow");
        if (kind == 0)
            stream(test, {std::numeric_limits<float>::max()}, {2}, 0);
        else if (kind == 1)
            stream(test, {std::numeric_limits<float>::min()}, {.5f}, 0);
        else
            stream(test, {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
                   {1, 1}, 0);
        test.expected = r::Status::arithmetic_failure;
        result.push_back(test);
    }
    for (const float invalid :
         {0.f, std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::quiet_NaN(),
          std::numeric_limits<float>::infinity()}) {
        auto test = base("Explicit invalid candidate");
        stream(test, {1}, {1}, 0);
        test.query.candidates[0].weight = invalid;
        test.expected = r::Status::invalid_candidate;
        result.push_back(test);
        auto bad_floor = base("Explicit invalid target floor");
        stream(bad_floor, {1}, {1}, 0);
        bad_floor.query.epsilon = invalid;
        bad_floor.target_expected = r::Status::invalid_target;
        result.push_back(bad_floor);
    }
    auto target_overflow = base("Explicit target sum overflow");
    stream(target_overflow, {1}, {1}, 0);
    target_overflow.query.xyz[0] = target_overflow.query.xyz[1] = std::numeric_limits<float>::max();
    target_overflow.query.xyz[2] = 0;
    target_overflow.target_expected = r::Status::arithmetic_failure;
    result.push_back(target_overflow);
    auto final_overflow = base("Explicit final UCW overflow");
    final_overflow.finalize_only = true;
    final_overflow.query.initial = {std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::min(), 1, 0, r::Status::ok};
    final_overflow.normalized_expected = r::Status::arithmetic_failure;
    result.push_back(final_overflow);
    for (const auto xyz : {std::array<float, 3>{0, 0, 0},
                           {.7f, -1.3f, 2.1f},
                           {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max()},
                           {0x1p-127f, 0x1p-127f, 0x1p-127f}}) {
        for (const float scale :
             {std::numeric_limits<float>::min(), 1.f, 3.7f, std::numeric_limits<float>::max()}) {
            auto test = base("Bounded target scale/range");
            stream(test, {1}, {1}, 0);
            std::copy(xyz.begin(), xyz.end(), test.query.xyz);
            test.query.bounded_scale = scale;
            if (xyz[0] == std::numeric_limits<float>::max())
                test.target_expected = r::Status::arithmetic_failure;
            if (xyz[0] == 0x1p-127f)
                test.target_expected = r::Status::invalid_target;
            result.push_back(test);
        }
    }
    for (const float invalid :
         {0.f, -1.f, std::numeric_limits<float>::denorm_min(),
          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto test = base("Bounded invalid scale");
        stream(test, {1}, {1}, 0);
        test.query.bounded_scale = invalid;
        test.bounded_expected = r::Status::invalid_target;
        result.push_back(test);
    }
    for (const float invalid :
         {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto test = base("Bounded nonfinite XYZ");
        stream(test, {1}, {1}, 0);
        test.query.xyz[1] = invalid;
        test.target_expected = test.bounded_expected = r::Status::invalid_target;
        result.push_back(test);
    }
    auto incomplete = base("Incomplete selection fails explicitly");
    stream(incomplete, {1, 1}, {1, 1}, 0);
    incomplete.query.candidates[1].word_count = 0;
    incomplete.expected = r::Status::incomplete;
    result.push_back(incomplete);
    constexpr std::array<unsigned, 4> domain{0x72707431u, 0x72706431u, 0x72706d31u, 0x72707631u};
    for (unsigned i = 0; i < result.size(); ++i) {
        result[i].query.address.domain = static_cast<rr::TailDomain>(domain[i % 4]);
        result[i].query.address.ordinal ^= i << 25;
        result[i].query.identity.sample_high ^= i;
    }
    return result;
}
class Probe {
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLComputePipelineState> pipeline_;

  public:
    Probe(id<MTLDevice> device, const char *path) : device_(device) {
        NSError *failure = nil;
        NSString *library_path = [NSString stringWithUTF8String:path];
        if (library_path == nil)
            throw std::runtime_error("Area math library path must be valid UTF-8");
        auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:library_path]
                                           error:&failure];
        need(library != nil, "Area math library: " + error(failure));
        auto function = [library newFunctionWithName:@"rypath_area_math_probe"];
        need(function != nil, "Missing area math kernel");
        pipeline_ = [device newComputePipelineStateWithFunction:function error:&failure];
        queue_ = [device newCommandQueue];
        need(pipeline_ != nil && queue_ != nil, "Area math pipeline: " + error(failure));
    }
    std::vector<p::Result> run(const std::vector<Case> &tests) const {
        std::vector<p::Query> queries;
        for (const auto &test : tests)
            queries.push_back(test.query);
        auto input = [device_ newBufferWithBytes:queries.data()
                                          length:queries.size() * sizeof(p::Query)
                                         options:MTLResourceStorageModeShared];
        auto output = [device_ newBufferWithLength:(tests.size() + 1) * sizeof(p::Result)
                                           options:MTLResourceStorageModeShared];
        need(input != nil && output != nil, "Area math buffers");
        std::memset(output.contents, 0xa5, output.length);
        auto command = [queue_ commandBuffer];
        auto encoder = [command computeCommandEncoder];
        need(command != nil && encoder != nil, "Area math command encoding");
        [encoder setComputePipelineState:pipeline_];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        const auto count = static_cast<unsigned>(tests.size());
        [encoder setBytes:&count length:sizeof(count) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(
                                                  32, pipeline_.maxTotalThreadsPerThreadgroup),
                                              1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        need(command.status == MTLCommandBufferStatusCompleted,
             "Area math GPU command: " + error(command.error));
        const auto *bytes = static_cast<const unsigned char *>(output.contents);
        need(std::all_of(bytes + tests.size() * sizeof(p::Result), bytes + output.length,
                         [](unsigned char byte) { return byte == 0xa5; }),
             "Area math dispatch guard overwritten");
        std::vector<p::Result> results(tests.size());
        std::memcpy(results.data(), output.contents, results.size() * sizeof(p::Result));
        return results;
    }
};
long double probability(const p::Step &step, const p::Query &query, unsigned index,
                        bool select_new) {
    if (index == 0)
        return select_new ? 1 : 0;
    const auto rare = std::ldexp(static_cast<long double>(step.rare.significand),
                                 -static_cast<int>(step.rare.bit_count));
    long double old = 0;
    for (unsigned i = 0; i < index; ++i)
        old += static_cast<long double>(query.candidates[i].weight) * query.candidates[i].target;
    const auto score =
        static_cast<long double>(query.candidates[index].weight) * query.candidates[index].target;
    return select_new == (score <= old) ? rare : 1 - rare;
}
void check(const Case &test, const p::Result &output) {
    const auto &query = test.query;
    need(words(output.philox) == reference_philox(words(query.counter), {query.key.x, query.key.y}),
         test.label + ": Philox integer mismatch");
    const auto fresh = reference_fresh(query.identity);
    need(std::memcmp(&fresh, &output.fresh, sizeof(fresh)) == 0,
         test.label + ": fresh payload/domain mismatch");
    need(words(output.tail) ==
             reference_philox({query.address.ordinal, query.address.material, query.address.role,
                               static_cast<unsigned>(query.address.domain)},
                              {fresh.tail_key[0], fresh.tail_key[1]}),
         test.label + ": tail domain/address mismatch");
    need(output.uniform_value == double(query.uniform_word >> 8) * 0x1p-24,
         test.label + ": uniform mapping");
    const long double target = query.epsilon + std::abs(static_cast<long double>(query.xyz[0])) +
                               std::abs(static_cast<long double>(query.xyz[1])) +
                               std::abs(static_cast<long double>(query.xyz[2]));
    constexpr long double unit = 0x1p-23L;
    need(output.target.status == test.target_expected,
         test.label + ": explicit target error status");
    if (test.target_expected == r::Status::ok)
        need(std::abs(output.target.value - target) <= 3 * unit * target,
             test.label + ": target arithmetic/support");
    need(output.bounded_status == unsigned(test.bounded_expected),
         test.label + ": bounded target explicit status");
    if (test.bounded_expected == r::Status::ok) {
        long double l1 = 0, flushed_l1 = 0;
        for (const float component : query.xyz) {
            l1 += std::abs(static_cast<long double>(component));
            if (component == 0 || std::isnormal(component))
                flushed_l1 += std::abs(static_cast<long double>(component));
        }
        const long double scale = query.bounded_scale;
        const long double ideal = 1 + .1L * l1 / (scale + l1);
        const long double flushed = 1 + .1L * flushed_l1 / (scale + flushed_l1);
        // Subnormal proxy terms may flush on Metal. Their full/zero-term
        // oracles bracket that declared approximation; normal inputs use one oracle.
        need(std::isfinite(output.bounded_value) && output.bounded_value >= 1 &&
                 output.bounded_value <= 1.1f,
             test.label + ": bounded target global range");
        need(output.bounded_value >= flushed - 8 * unit && output.bounded_value <= ideal + 8 * unit,
             test.label + ": bounded target independent long-double oracle");
        if (l1 == 0)
            need(output.bounded_value == 1, test.label + ": exact black target support");
    }
    const auto final_status =
        test.expected == r::Status::ok ? test.normalized_expected : test.expected;
    need(output.state.status == test.expected && output.normalized.status == final_status,
         test.label + ": explicit error status mismatch");
    if (test.finalize_only) {
        need(output.state.candidate_count == query.initial.candidate_count &&
                 output.state.selected_index == query.initial.selected_index,
             test.label + ": finalization preserves source state");
        return;
    }
    if (test.expected != r::Status::ok)
        return;
    need(output.state.candidate_count == query.candidate_count &&
             output.state.selected_index == test.selected,
         test.label + ": selected index/count");
    long double sum = 0;
    for (unsigned i = 0; i < query.candidate_count; ++i) {
        const auto &step = output.steps[i];
        const auto score =
            static_cast<long double>(query.candidates[i].weight) * query.candidates[i].target;
        need(step.status == unsigned(r::Status::ok) && step.complete && step.words_used <= 9,
             test.label + ": incomplete candidate update");
        if (i) {
            need(step.selected == (i == test.selected), test.label + ": exact candidate decision");
            need(step.words_used > 0 && step.compared_bits > 32 * (step.words_used - 1) &&
                     step.compared_bits <= std::min(32 * step.words_used, step.rare.bit_count),
                 test.label + ": consumed random-word/bit counters");
            if (test.boundary_offset == 0 && i == 1)
                need(step.compared_bits == step.rare.bit_count &&
                         step.words_used == (step.rare.bit_count + 31) / 32,
                     test.label + ": threshold equality consumes the complete dyadic prefix");
            if (test.expected_bit_count && i == 1)
                need(step.rare.bit_count == test.expected_bit_count,
                     test.label + ": full 277-bit exponent gap");
            const auto rare = std::ldexp(static_cast<long double>(step.rare.significand),
                                         -static_cast<int>(step.rare.bit_count));
            const auto ideal = std::min(sum, score) / (sum + score);
            const auto tolerance = (std::pow(1 + unit, static_cast<int>(i + 3)) - 1) * ideal;
            need(rare > 0 && rare <= .5L && std::abs(rare - ideal) <= tolerance,
                 test.label + ": dyadic probability outside conservative Metal bound");
            if (test.exact_probability)
                need(rare == ideal, test.label + ": exact dyadic encoded incorrectly");
        } else
            need(step.words_used == 0 && step.selected,
                 test.label + ": first candidate needs no entropy");
        sum += score;
    }
    const auto ideal_weight =
        sum /
        (query.candidate_count * static_cast<long double>(query.candidates[test.selected].target));
    const auto eta = std::pow(1 + 3 * unit, static_cast<int>(query.candidate_count - 1)) *
                         std::pow(1 + unit, 3) -
                     1;
    need(std::isfinite(output.normalized.value) && output.normalized.value > 0 &&
             std::abs(output.normalized.value - ideal_weight) <= eta * ideal_weight,
         test.label + ": normalized UCW outside conservative Metal bound");
    long double selection = probability(output.steps[test.selected], query, test.selected, true);
    for (unsigned later = test.selected + 1; later < query.candidate_count; ++later)
        selection *= probability(output.steps[later], query, later, false);
    const auto coefficient = selection * output.normalized.value;
    const auto ideal_coefficient =
        static_cast<long double>(query.candidates[test.selected].weight) / query.candidate_count;
    need(std::abs(coefficient - ideal_coefficient) <= eta * ideal_coefficient,
         test.label + ": conditional coefficient outside conservative Metal bound");
}
void masses(const std::vector<Case> &tests, const std::vector<p::Result> &results) {
    std::array<long double, 3> finite{};
    std::array<long double, 2> absorption{};
    long double black = 0;
    for (std::size_t i = 0; i < tests.size(); ++i) {
        const auto &test = tests[i];
        const auto &output = results[i];
        if (test.finite_i >= 0 || test.black_i >= 0) {
            const auto mass = probability(output.steps[1], test.query, 1, test.selected != 0) *
                              output.normalized.value;
            if (test.finite_i >= 0)
                finite[test.selected ? test.finite_j : test.finite_i] += mass / 9;
            else
                black += mass * (test.selected ? test.black_j : test.black_i) * 2 / 4;
        }
        if (test.absorption_arm >= 0) {
            long double selected =
                probability(output.steps[test.selected], test.query, test.selected, true);
            for (unsigned later = test.selected + 1; later < 32; ++later)
                selected *= probability(output.steps[later], test.query, later, false);
            const auto value =
                (test.selected == 0 ? test.absorption_arm == 0 : test.absorption_arm != 0) ? 0 : 1;
            absorption[value] += selected * output.normalized.value / 2;
        }
    }
    constexpr long double unit = 0x1p-23L;
    const auto eta2 = (1 + 3 * unit) * std::pow(1 + unit, 3) - 1;
    for (auto mass : finite)
        need(std::abs(mass - 1.L / 3) <= eta2 / 3, "GPU finite-domain indicator measure");
    need(std::abs(-3 * finite[0] + 5 * finite[1] + 11 * finite[2] - 13.L / 3) <= eta2 * 19.L / 3,
         "GPU arbitrary signed h measure");
    need(std::abs(black - 1) <= eta2, "GPU black candidates K=2 mean");
    const auto eta32 = std::pow(1 + 3 * unit, 31) * std::pow(1 + unit, 3) - 1;
    for (auto mass : absorption)
        need(std::abs(mass - .5L) <= eta32 / 2, "GPU capped absorption indicator measure");
}
std::vector<Case> temporal_cases(const std::vector<Case> &source,
                                 const std::vector<p::Result> &results) {
    constexpr std::array<float, 3> target_b{3, 1, 5};
    std::vector<Case> tests;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const auto &old = source[i];
        if (old.finite_i < 0)
            continue;
        const unsigned old_value = old.selected ? old.finite_j : old.finite_i;
        const auto old_probability =
            probability(results[i].steps[1], old.query, 1, old.selected != 0) / 9;
        for (unsigned fresh = 0; fresh < 3; ++fresh)
            for (unsigned selected = 0; selected < 2; ++selected) {
                auto test = base("Temporal imported UCW target B");
                stream(test, {results[i].normalized.value, 1},
                       {target_b[old_value], target_b[fresh]}, selected);
                test.temporal_value = static_cast<int>(selected ? fresh : old_value);
                test.prior_probability = old_probability / 3;
                tests.push_back(test);
            }
    }
    return tests;
}
void temporal_masses(const std::vector<Case> &tests, const std::vector<p::Result> &results) {
    std::array<long double, 3> mass{};
    for (std::size_t i = 0; i < tests.size(); ++i)
        mass[tests[i].temporal_value] +=
            tests[i].prior_probability *
            probability(results[i].steps[1], tests[i].query, 1, tests[i].selected != 0) *
            results[i].normalized.value;
    constexpr long double unit = 0x1p-23L;
    const auto eta2 = (1 + 3 * unit) * std::pow(1 + unit, 3) - 1;
    const auto bound = eta2 + (1 + eta2) * eta2 / 2;
    for (const auto value : mass)
        need(std::abs(value - 1.L / 3) <= bound / 3, "GPU temporal indicator measure");
    need(std::abs(-3 * mass[0] + 5 * mass[1] + 11 * mass[2] - 13.L / 3) <= bound * 19.L / 3,
         "GPU temporal signed-h measure");
}
std::vector<Case> hdr_boundaries(const std::vector<Case> &tests,
                                 const std::vector<p::Result> &results) {
    std::vector<Case> boundary;
    for (std::size_t i = 0; i < tests.size(); ++i) {
        if (tests[i].label != "HDR target order" && tests[i].label != "HDR weight exponent gap")
            continue;
        if (tests[i].selected != 0)
            continue;
        const auto &step = results[i].steps[1];
        const auto &q = tests[i].query;
        const bool new_small =
            static_cast<long double>(q.candidates[1].weight) * q.candidates[1].target <=
            static_cast<long double>(q.candidates[0].weight) * q.candidates[0].target;
        for (int offset = -1; offset <= 1; ++offset) {
            auto test = tests[i];
            test.label += " GPU dyadic boundary " + std::to_string(offset);
            test.selected = ((offset < 0) == new_small) ? 1 : 0;
            test.boundary_offset = offset;
            long double fraction =
                std::ldexp(static_cast<long double>(step.rare.significand) + offset,
                           -static_cast<int>(step.rare.bit_count));
            for (auto &word : test.query.candidates[1].words) {
                fraction = std::ldexp(fraction, 32);
                const auto digit = std::floor(fraction);
                word = static_cast<unsigned>(digit);
                fraction -= digit;
            }
            boundary.push_back(test);
        }
    }
    return boundary;
}
int run(const char *path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for area math conformance");
    Probe probe(device, path);
    const auto tests = cases();
    const auto results = probe.run(tests);
    for (std::size_t i = 0; i < tests.size(); ++i)
        check(tests[i], results[i]);
    masses(tests, results);
    const auto temporal = temporal_cases(tests, results);
    const auto temporal_results = probe.run(temporal);
    for (std::size_t i = 0; i < temporal.size(); ++i)
        check(temporal[i], temporal_results[i]);
    temporal_masses(temporal, temporal_results);
    const auto boundary = hdr_boundaries(tests, results);
    const auto boundary_results = probe.run(boundary);
    for (std::size_t i = 0; i < boundary.size(); ++i)
        check(boundary[i], boundary_results[i]);
    std::cout << "Area shared math GPU conformance passed: " << tests.size() << " cases, "
              << boundary.size() << " HDR boundary cases, " << temporal.size()
              << " temporal cases\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Usage: area-math-probe LIBRARY.metallib");
            return run(argv[1]);
        } catch (const std::exception &failure) {
            std::cerr << failure.what() << '\n';
            return 1;
        }
    }
}
