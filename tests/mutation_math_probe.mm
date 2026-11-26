// SPDX-License-Identifier: GPL-3.0-only
#include "mutation_math_probe_wire.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
namespace mp = mutation_probe;
namespace mh = rypath_mutation;
namespace rr = rypath_replay;
void need(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
using Words = std::array<unsigned, 4>;
Words philox(Words x, std::array<unsigned, 2> key) {
    for (unsigned round = 0; round < 10; ++round) {
        const std::uint64_t a = std::uint64_t(x[0]) * 0xd2511f53u,
                            b = std::uint64_t(x[2]) * 0xcd9e8d57u;
        x = {unsigned(b >> 32) ^ x[1] ^ key[0], unsigned(b), unsigned(a >> 32) ^ x[3] ^ key[1],
             unsigned(a)};
        key[0] += 0x9e3779b9u;
        key[1] += 0xbb67ae85u;
    }
    return x;
}
Words words(rr::Words4 x) { return {x.x, x.y, x.z, x.w}; }
struct Case {
    mp::Query q{};
    std::string label;
    mh::Status proposal_status = mh::Status::ok, acceptance_status = mh::Status::ok;
    bool accept = true;
    int boundary = 2;
    unsigned expected_bits = 0;
};
struct TargetPair {
    float source, proposal;
};
Case make(float weight, TargetPair targets, bool accept) {
    Case t;
    t.label = "MH arithmetic";
    t.q.source = {{~0u, 0}, {~0u, 0x80000000u}, {29, ~0u}, 0x12345678u, 1};
    t.q.entropy = {0, 3, ~0u, 17};
    t.q.identity = {0x80000001u, 29, 0x80000001u, 0xabcdef01u, 0x98765432u};
    t.q.scale = {16, 8};
    t.q.weight = weight;
    t.q.source_target = targets.source;
    t.q.proposal_target = targets.proposal;
    t.q.word_count = 9;
    t.q.requested_steps = 1;
    t.accept = accept;
    std::fill(std::begin(t.q.words), std::end(t.q.words), accept ? 0u : ~0u);
    return t;
}
std::vector<Case> cases() {
    std::vector<Case> out;
    for (const auto a : {std::array<float, 3>{1, 2, 1},
                         {1.3f, 1.2f, .7f},
                         {0x1.000002p0f, 1, 1.1f},
                         {std::numeric_limits<float>::min(), std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::min()},
                         {std::numeric_limits<float>::max(), std::numeric_limits<float>::min(),
                          std::numeric_limits<float>::max()},
                         {3, 7, 7}}) {
        auto t = make(a[0], {a[1], a[2]}, true);
        if (a[1] == std::numeric_limits<float>::max() || a[2] == std::numeric_limits<float>::max())
            t.expected_bits = 277;
        out.push_back(t);
        if (a[2] < a[1])
            out.push_back(make(a[0], {a[1], a[2]}, false));
    }
    for (unsigned block = 0; block < 4; ++block)
        for (unsigned signs = 0; signs < 4; ++signs) {
            auto t = make(1, {1, 1}, true);
            t.label = "Proposal modular/domain oracle";
            t.q.entropy = {block, signs, ~0u, 0};
            t.q.step = block;
            t.q.requested_steps = block == 3 ? 4 : block;
            out.push_back(t);
        }
    auto witness =
        make(std::bit_cast<float>(0x00813880u), {1, std::bit_cast<float>(0x3f813881u)}, true);
    witness.label = "Normal-boundary double-round regression";
    witness.acceptance_status = mh::Status::arithmetic_failure;
    out.push_back(witness);
    for (unsigned bits : {0x00813882u, 0x00813883u})
        out.push_back(
            make(std::bit_cast<float>(bits), {1, std::bit_cast<float>(0x3f813881u)}, true));
    out.push_back(make(std::bit_cast<float>(0x7effffffu), {2, 1}, true));
    auto overflow = make(0x1p127f, {2, 1}, true);
    overflow.acceptance_status = mh::Status::arithmetic_failure;
    out.push_back(overflow);
    for (float invalid :
         {0.f, -1.f, std::numeric_limits<float>::denorm_min(),
          std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        auto weight = make(invalid, {1, 1}, true);
        weight.acceptance_status = mh::Status::invalid_weight;
        out.push_back(weight);
        auto target = make(1, {invalid, 1}, true);
        target.acceptance_status = mh::Status::invalid_target;
        out.push_back(target);
        target = make(1, {1, invalid}, true);
        target.acceptance_status = mh::Status::invalid_target;
        out.push_back(target);
    }
    auto bad = make(1, {1, 1}, true);
    bad.q.source.version = 0;
    bad.proposal_status = mh::Status::invalid_payload;
    out.push_back(bad);
    bad = make(1, {1, 1}, true);
    bad.q.scale = {31, 1};
    bad.proposal_status = mh::Status::invalid_scale;
    out.push_back(bad);
    bad = make(1, {1, 1}, true);
    bad.q.step = 4;
    out.push_back(bad);
    bad = make(1, {1, 1}, true);
    bad.q.acceptance_ordinal = 9;
    out.push_back(bad);
    bad = make(1, {2, 1}, false);
    bad.q.word_count = 0;
    bad.acceptance_status = mh::Status::incomplete;
    out.push_back(bad);
    for (unsigned n : {0u, 1u, 2u, 3u, 4u, 5u}) {
        auto t = make(1, {1, 1}, true);
        t.q.requested_steps = n;
        out.push_back(t);
    }
    for (unsigned block = 0; block < 4; ++block)
        for (unsigned global = 0; global < 2; ++global)
            for (unsigned word : {0u, 0xffffffffu, 0x12345678u}) {
                auto t = make(1, {1, 1}, true);
                t.label = "Spectral refresh full-word and self oracle";
                t.q.policy = unsigned(mh::Policy::spectral_refresh);
                t.q.entropy = {block | (global ? 0x80000000u : 0), 3, word, 91};
                out.push_back(t);
            }
    return out;
}
std::string why(NSError *e) {
    return e ? e.localizedDescription.UTF8String : "No Metal diagnostic";
}
class Probe {
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLComputePipelineState> pipeline_;

  public:
    Probe(id<MTLDevice> device, const char *path) : device_(device) {
        NSString *name = [NSString stringWithUTF8String:path];
        if (!name)
            throw std::runtime_error("Probe path is not UTF8");
        NSError *error = nil;
        auto lib = [device newLibraryWithURL:[NSURL fileURLWithPath:name] error:&error];
        need(lib != nil, "Mutation math library: " + why(error));
        auto fn = [lib newFunctionWithName:@"rypath_mutation_math_probe"];
        need(fn != nil, "Mutation math kernel");
        pipeline_ = [device newComputePipelineStateWithFunction:fn error:&error];
        queue_ = [device newCommandQueue];
        need(pipeline_ != nil && queue_ != nil, "Mutation pipeline: " + why(error));
    }
    std::vector<mp::Result> run(const std::vector<Case> &tests) const {
        std::vector<mp::Query> q;
        for (const auto &t : tests)
            q.push_back(t.q);
        auto input = [device_ newBufferWithBytes:q.data()
                                          length:q.size() * sizeof(mp::Query)
                                         options:MTLResourceStorageModeShared];
        auto output = [device_ newBufferWithLength:(q.size() + 1) * sizeof(mp::Result)
                                           options:MTLResourceStorageModeShared];
        need(input != nil && output != nil, "Mutation probe buffers");
        std::memset(output.contents, 0xa5, output.length);
        auto command = [queue_ commandBuffer];
        auto encoder = [command computeCommandEncoder];
        need(command != nil && encoder != nil, "Mutation probe encoding");
        [encoder setComputePipelineState:pipeline_];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        const auto count = unsigned(q.size());
        [encoder setBytes:&count length:sizeof(count) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(
                                                  32, pipeline_.maxTotalThreadsPerThreadgroup),
                                              1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        need(command.status == MTLCommandBufferStatusCompleted,
             "Mutation probe execution: " + why(command.error));
        const auto *bytes = static_cast<const unsigned char *>(output.contents);
        need(std::all_of(bytes + q.size() * sizeof(mp::Result), bytes + output.length,
                         [](unsigned char x) { return x == 0xa5; }),
             "Mutation probe guard");
        std::vector<mp::Result> out(q.size());
        std::memcpy(out.data(), output.contents, out.size() * sizeof(mp::Result));
        return out;
    }
};
rr::ReplayState proposal(const mp::Query &q) {
    auto expected = q.source;
    if (q.policy == unsigned(mh::Policy::spectral_refresh) && (q.entropy.x & 0x80000000u)) {
        expected.wavelength = q.entropy.z;
        return expected;
    }
    const auto mask = (1u << q.scale.magnitude_bits) - 1;
    const unsigned m0 = (1 + (q.entropy.z & mask)) << q.scale.shift, m1 = (1 + (q.entropy.w & mask))
                                                                          << q.scale.shift;
    const unsigned d0 = (q.entropy.y & 1) ? 0u - m0 : m0, d1 = (q.entropy.y & 2) ? 0u - m1 : m1;
    switch (q.entropy.x & 3) {
    case 0:
        expected.film[0] += d0;
        expected.film[1] += d1;
        break;
    case 1:
        expected.lens[0] += d0;
        expected.lens[1] += d1;
        break;
    case 2:
        expected.wavelength += d0;
        break;
    default:
        expected.tail_key[0] += d0;
        expected.tail_key[1] += d1;
        break;
    }
    return expected;
}
long double ratio(const mp::Result &r) {
    return std::ldexp(static_cast<long double>(r.ratio.significand), -int(r.ratio.bit_count));
}
void check(const Case &t, const mp::Result &r) {
    const auto &q = t.q;
    need(r.proposal.status == t.proposal_status, t.label + ": proposal explicit status");
    if (t.proposal_status == mh::Status::ok) {
        const auto expected = proposal(q);
        need(std::memcmp(&expected, &r.proposal.payload, sizeof(expected)) == 0 &&
                 unsigned(r.proposal.block) ==
                     ((q.policy == unsigned(mh::Policy::spectral_refresh) &&
                       (q.entropy.x & 0x80000000u))
                          ? unsigned(mh::Block::wavelength)
                          : (q.entropy.x & 3)),
             t.label + ": independent modular proposal payload");
    }
    const bool step = q.step < 4;
    need(r.counter_status == unsigned(step ? mh::Status::ok : mh::Status::invalid_step),
         "Decision counter step status");
    if (step) {
        Words counter{q.identity.pixel, q.identity.serial_low, q.identity.serial_high,
                      0x6d750000u + q.step * 256 + 16};
        need(words(r.counter) == counter, "Independent proposal domain address");
        need(r.proposal_words_status == unsigned(mh::Status::ok) &&
                 words(r.proposal_words) ==
                     philox(counter, {q.identity.seed_low, q.identity.seed_high}),
             "Independent proposal entropy");
    }
    for (unsigned i = 0; i < 9; ++i) {
        need(r.acceptance_status[i] == unsigned(step ? mh::Status::ok : mh::Status::invalid_step),
             "Acceptance entropy step status");
        if (step) {
            const auto block =
                philox({q.identity.pixel, q.identity.serial_low, q.identity.serial_high,
                        0x6d750000u + q.step * 256 + 32 + i / 4},
                       {q.identity.seed_low, q.identity.seed_high});
            need(r.acceptance_words[i] == block[i % 4],
                 "Independent acceptance entropy domain/ordinal");
        }
    }
    const auto ordinal_status = q.acceptance_ordinal >= 9 ? mh::Status::invalid_domain
                                : step                    ? mh::Status::ok
                                                          : mh::Status::invalid_step;
    need(r.ordinal_status == unsigned(ordinal_status), "Acceptance ordinal bounds");
    if (ordinal_status == mh::Status::ok)
        need(r.ordinal_value == r.acceptance_words[q.acceptance_ordinal],
             "Acceptance ordinal word");
    need(bool(r.valid_step_count) == (q.requested_steps == 0 || q.requested_steps == 1 ||
                                      q.requested_steps == 2 || q.requested_steps == 4),
         "Legal step-count helper");
    need(r.final_status == unsigned(t.acceptance_status), t.label + ": final acceptance status");
    const auto expected_prepare =
        t.acceptance_status == mh::Status::incomplete ? mh::Status::ok : t.acceptance_status;
    need(r.prepared_status == unsigned(expected_prepare),
         t.label + ": acceptance preflight status");
    if (expected_prepare != mh::Status::ok)
        need(r.words_used == 0 && r.compared_bits == 0,
             t.label + ": failed accepted-outcome preflight consumes no entropy");
    if (t.acceptance_status != mh::Status::ok)
        return;
    need(r.prepared_status == unsigned(mh::Status::ok) && bool(r.accepted) == t.accept &&
             bool(r.selected) == t.accept && r.complete,
         "MH decision metadata");
    constexpr long double unit = 0x1p-23L;
    const auto represented = ratio(r),
               ideal = std::min(static_cast<long double>(q.source_target),
                                static_cast<long double>(q.proposal_target)) /
                       std::max(static_cast<long double>(q.source_target),
                                static_cast<long double>(q.proposal_target));
    need(represented > 0 && represented <= 1 && std::abs(represented - ideal) <= unit * ideal,
         "Represented MH ratio versus independent high precision");
    const auto correction = q.proposal_target >= q.source_target ? represented : 1 / represented;
    const auto expected = t.accept ? static_cast<long double>(q.weight) * correction : q.weight;
    need(std::isnormal(r.final_weight) &&
             std::abs(r.final_weight - expected) <=
                 unit * expected + 64 * std::numeric_limits<long double>::epsilon() * expected,
         "Actual reverse/forward MH correction one-operation bound");
    if (!t.accept)
        need(r.final_weight == q.weight, "Rejected MH move preserves UCW exactly");
    if (q.source_target == q.proposal_target)
        need(r.final_weight == q.weight && r.prepared_weight == q.weight && r.accepted,
             "Equal targets preserve UCW exactly in preparation and finalization");
    if (q.proposal_target >= q.source_target)
        need(r.words_used == 0 && r.compared_bits == 0, "Certain MH acceptance uses no entropy");
    else {
        need(r.words_used > 0 && r.words_used <= 9 && r.compared_bits > 32 * (r.words_used - 1) &&
                 r.compared_bits <= std::min(32 * r.words_used, r.ratio.bit_count),
             "MH random-word counters");
        if (t.boundary == 0)
            need(r.compared_bits == r.ratio.bit_count &&
                     r.words_used == (r.ratio.bit_count + 31) / 32,
                 "Equal MH threshold consumes full dyadic prefix");
    }
    if (t.expected_bits)
        need(r.ratio.bit_count == t.expected_bits, "MH nine-word extreme support");
}
std::vector<Case> boundaries(const std::vector<Case> &tests,
                             const std::vector<mp::Result> &results) {
    std::vector<Case> out;
    for (std::size_t i = 0; i < tests.size(); ++i) {
        if (tests[i].acceptance_status != mh::Status::ok ||
            tests[i].q.proposal_target >= tests[i].q.source_target || !tests[i].accept)
            continue;
        for (int offset = -1; offset <= 1; ++offset) {
            auto t = tests[i];
            t.label += " GPU ratio boundary";
            t.accept = offset < 0;
            t.boundary = offset;
            long double fraction =
                std::ldexp(static_cast<long double>(results[i].ratio.significand) + offset,
                           -int(results[i].ratio.bit_count));
            if (fraction >= 1) {
                std::fill(std::begin(t.q.words), std::end(t.q.words), 0xffffffffu);
                out.push_back(t);
                continue;
            }
            for (auto &word : t.q.words) {
                fraction = std::ldexp(fraction, 32);
                const auto digit = std::floor(fraction);
                word = unsigned(digit);
                fraction -= digit;
            }
            out.push_back(t);
        }
    }
    return out;
}
int run(const char *path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for mutation math probe");
    need(philox({0, 0, 0, 0}, {0, 0}) == Words{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u},
         "Independent Random123 KAT");
    const Probe probe(device, path);
    const auto tests = cases();
    const auto results = probe.run(tests);
    for (std::size_t i = 0; i < tests.size(); ++i)
        check(tests[i], results[i]);
    const auto edge = boundaries(tests, results);
    const auto values = probe.run(edge);
    for (std::size_t i = 0; i < edge.size(); ++i)
        check(edge[i], values[i]);
    std::cout << "Mutation math GPU conformance passed: " << tests.size() << " cases, "
              << edge.size() << " GPU-dyadic boundary cases\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Usage: mutation-math-probe LIBRARY");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
