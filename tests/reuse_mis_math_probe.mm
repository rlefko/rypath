// SPDX-License-Identifier: GPL-3.0-only
#include "reuse_mis_math_probe_wire.h"
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
namespace p = reuse_mis_probe;
namespace r = rypath_reuse_mis;
namespace m = rypath_reservoir;
namespace mh = rypath_mutation;
constexpr long double unit = 0x1p-23L;
constexpr float low = std::numeric_limits<float>::min(), high = std::numeric_limits<float>::max();
void need(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
float fp(unsigned bits) { return std::bit_cast<float>(bits); }
std::string why(NSError *error) {
    return error ? error.localizedDescription.UTF8String : "No Metal diagnostic";
}
struct Case {
    p::Query q{};
    std::string label = "scaled arithmetic";
    bool accept = true, preflight_failure = false;
    int boundary = 2;
};
Case make(float Z = 7, float current = 2, float previous = 1, bool accept = true) {
    Case c;
    c.q = {3, -7, 11, 1, Z, current, previous, -3, (Z < 0x1p-100f || Z > 0x1p100f) ? 1.f : Z,
           9, {}};
    c.accept = accept;
    std::fill(std::begin(c.q.words), std::end(c.q.words), accept ? 0u : ~0u);
    return c;
}
std::vector<Case> cases() {
    std::vector<Case> out;
    for (const auto &a : {std::array<float, 4>{7, 2, 1, -3},
                          {7, 1, 3, 5},
                          {fp(0x3f800001), 3, 3, -0.f},
                          {low, low, high, -high},
                          {high, high, low, low},
                          {high, high, high, low},
                          {1, low, low, fp(1)},
                          {high, low, low, low},
                          {low, 1, 1, .5f},
                          {high, 1, 1, 2}}) {
        auto c = make(a[0], a[1], a[2]);
        c.q.component = a[3];
        out.push_back(c);
        if (a[1] > a[2]) {
            c.accept = false;
            std::fill(std::begin(c.q.words), std::end(c.q.words), ~0u);
            out.push_back(c);
        }
    }
    auto c = make();
    c.q.x = c.q.y = c.q.z = fp(0x007fffff);
    c.q.target_scale = low;
    c.label = "subnormal XYZ bit normalization";
    out.push_back(c);
    c = make();
    c.q.x = high;
    c.q.y = -high;
    c.q.z = high;
    c.q.target_scale = high;
    c.label = "HDR quarter target";
    out.push_back(c);
    c = make();
    c.q.x = c.q.y = c.q.z = 0;
    c.q.target_scale = low;
    out.push_back(c);
    c = make();
    c.q.current_target = 3;
    c.q.previous_target = 1;
    c.q.word_count = 0;
    out.push_back(c);
    for (float invalid : {0.f, -1.f, fp(1), INFINITY, NAN}) {
        c = make();
        c.q.Z = invalid;
        c.q.normalizer = invalid;
        out.push_back(c);
        c = make();
        c.q.current_target = invalid;
        out.push_back(c);
        c = make();
        c.q.previous_target = invalid;
        out.push_back(c);
        c = make();
        c.q.target_scale = invalid;
        out.push_back(c);
    }
    for (float invalid : {INFINITY, NAN}) {
        c = make();
        c.q.x = invalid;
        out.push_back(c);
        c = make();
        c.q.component = invalid;
        out.push_back(c);
    }
    // Unequal real exponents allow sf>lf. These deterministic cases exercise
    // nonunit represented-probability corrections, without tuning a tolerance.
    for (unsigned i = 1; i <= 64; ++i) {
        const float a = fp(0x3f000000u | ((i * 2654435761u) & 0x007fffffu));
        const float b = fp(0x3f800000u | ((i * 2246822519u) & 0x007fffffu));
        out.push_back(make(1, a, b));
        out.push_back(make(1, b, a));
    }
    return out;
}
class Probe {
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLComputePipelineState> pipeline_;

  public:
    Probe(id<MTLDevice> device, const char *path) : device_(device) {
        NSString *name = [NSString stringWithUTF8String:path];
        need(name != nil, "Non-UTF8 library path");
        NSError *error = nil;
        auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:name] error:&error];
        need(library != nil, "Reuse math library: " + why(error));
        auto function = [library newFunctionWithName:@"rypath_reuse_mis_math_probe"];
        need(function != nil, "Missing reuse math kernel");
        pipeline_ = [device newComputePipelineStateWithFunction:function error:&error];
        queue_ = [device newCommandQueue];
        need(pipeline_ != nil && queue_ != nil, "Reuse math pipeline: " + why(error));
    }
    std::vector<p::Result> run(const std::vector<Case> &tests) const {
        need(!tests.empty(), "No arithmetic cases");
        std::vector<p::Query> queries;
        for (const auto &c : tests)
            queries.push_back(c.q);
        auto input = [device_ newBufferWithBytes:queries.data()
                                          length:queries.size() * sizeof(p::Query)
                                         options:MTLResourceStorageModeShared];
        auto output = [device_ newBufferWithLength:(queries.size() + 1) * sizeof(p::Result)
                                           options:MTLResourceStorageModeShared];
        need(input != nil && output != nil, "Reuse math buffers");
        std::memset(output.contents, 0xa5, output.length);
        auto command = [queue_ commandBuffer];
        auto encoder = [command computeCommandEncoder];
        need(command != nil && encoder != nil, "Reuse math encoding");
        [encoder setComputePipelineState:pipeline_];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        const auto count = unsigned(queries.size());
        [encoder setBytes:&count length:sizeof(count) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(
                                                  32, pipeline_.maxTotalThreadsPerThreadgroup),
                                              1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        need(command.status == MTLCommandBufferStatusCompleted,
             "Reuse math command: " + why(command.error));
        const auto *bytes = static_cast<const unsigned char *>(output.contents);
        need(std::all_of(bytes + queries.size() * sizeof(p::Result), bytes + output.length,
                         [](unsigned char x) { return x == 0xa5; }),
             "Reuse math guard overwritten");
        std::vector<p::Result> result(queries.size());
        std::memcpy(result.data(), output.contents, result.size() * sizeof(p::Result));
        return result;
    }
};
void scalar(const m::ScalarResult &gpu, const m::ScalarResult &cpu, long double expected,
            const std::string &label) {
    need(gpu.status == cpu.status, label + ": explicit status");
    if (cpu.status != m::Status::ok)
        return;
    if (expected == 0) {
        need(m::bits(gpu.value) == m::bits(cpu.value), label + ": exact signed zero");
        return;
    }
    // <=32u covers the fixed scaled add/multiply/divide sequence and independent
    // scalar formula rounding, including backend correctly-rounded division
    // allowance. No absolute epsilon masks tiny normal outputs or HDR errors.
    const long double tolerance =
        (32 * unit + 64 * std::numeric_limits<long double>::epsilon()) * std::abs(expected);
    need(std::isnormal(gpu.value) && std::abs((long double)gpu.value - expected) <= tolerance,
         label + ": independent scalar formula");
}
void check(const Case &c, const p::Result &v) {
    const auto &q = c.q;
    const std::string &label = c.label;
    const long double x = q.x, y = q.y, z = q.z, Z = q.Z, t = q.current_target,
                      s = q.previous_target;
    const long double target = std::max((long double)low, 1e-6L * (long double)q.target_scale) +
                               ((std::abs(x) / 4 + std::abs(y) / 4) + std::abs(z) / 4);
    scalar(v.target, r::importance_target(q.x, q.y, q.z, q.target_scale), target,
           label + " target");
    scalar(v.fresh, r::temporal_fresh_score(q.current_target, q.previous_target),
           t * t / (t + 20 * s), label + " fresh");
    scalar(v.history, r::temporal_history_score(q.Z, q.current_target, q.previous_target),
           Z * 20 * t / (t + 20 * s), label + " history");
    scalar(v.pair, r::spatial_pair_score(q.Z, q.current_target, q.previous_target),
           Z * t / (4 * (t + s)), label + " spatial");
    scalar(v.contribution, r::signed_contribution(q.Z, q.component, q.current_target),
           Z * (long double)q.component / t, label + " signed");
    const auto prepared =
        r::prepare_normalizer_acceptance(q.normalizer, q.current_target, q.previous_target);
    const auto status = c.preflight_failure ? mh::Status::arithmetic_failure : prepared.status;
    need(v.prepared_status == unsigned(status), label + ": MH preflight status");
    if (status != mh::Status::ok) {
        need(v.final_status == unsigned(status) && v.words_used == 0 && v.compared_bits == 0,
             label + ": failed preflight must consume no entropy");
        return;
    }
    if (q.current_target == q.previous_target) {
        need(v.final_status == unsigned(mh::Status::ok) && v.accepted && v.selected && v.complete &&
                 v.words_used == 0 && v.compared_bits == 0 &&
                 m::bits(v.final_normalizer) == m::bits(q.normalizer) &&
                 m::bits(v.prepared_normalizer) == m::bits(q.normalizer),
             label + ": exact equal-target contract");
        return;
    }
    need(v.ratio.significand >= 0x00800000u && v.ratio.significand <= 0x00ffffffu &&
             v.ratio.bit_count >= 24 && v.ratio.bit_count <= 277,
         label + ": dyadic representation bounds");
    const long double represented =
        std::ldexp((long double)v.ratio.significand, -int(v.ratio.bit_count));
    const long double ideal = std::min(t, s) / std::max(t, s);
    need(represented > 0 && represented <= 1 && std::abs(represented - ideal) <= unit * ideal,
         label + ": actual GPU ratio");
    if ((q.current_target == low && q.previous_target == high) ||
        (q.current_target == high && q.previous_target == low))
        need(v.ratio.bit_count == 277, "Extreme dyadic support277 bits");
    const long double correction = (s > t ? represented : 1 / represented) * s / t;
    const long double expected = (long double)q.normalizer * correction;
    need(std::isnormal(v.prepared_normalizer) &&
             std::abs((long double)v.prepared_normalizer - expected) <=
                 (4 * unit + 64 * std::numeric_limits<long double>::epsilon()) * expected,
         label + ": normalizer correction from ACTUAL GPU dyadic probability");
    if (q.word_count == 0 && s < t) {
        need(v.final_status == unsigned(mh::Status::incomplete) && !v.complete && v.words_used == 0,
             label + ": incomplete decision");
        return;
    }
    need(v.final_status == unsigned(mh::Status::ok) && bool(v.accepted) == c.accept &&
             bool(v.selected) == c.accept && v.complete,
         label + ": actual decision");
    need(m::bits(v.final_normalizer) == m::bits(c.accept ? v.prepared_normalizer : q.normalizer),
         label + ": selected/rejected copy retains exact bits");
    if (s > t)
        need(v.words_used == 0 && v.compared_bits == 0,
             "Certain MH acceptance consumes no entropy");
    else {
        need(v.words_used > 0 && v.words_used <= 9 && v.compared_bits > 32 * (v.words_used - 1) &&
                 v.compared_bits <= std::min(32 * v.words_used, v.ratio.bit_count),
             label + ": dyadic cursor bounds");
        if (c.boundary == 0)
            need(v.compared_bits == v.ratio.bit_count &&
                     v.words_used == (v.ratio.bit_count + 31) / 32,
                 "Exact GPU dyadic threshold prefix");
    }
}
std::vector<Case> boundaries(const std::vector<Case> &tests, const std::vector<p::Result> &values,
                             unsigned &nonnormal) {
    std::vector<Case> out;
    nonnormal = 0;
    for (std::size_t i = 0; i < tests.size(); ++i) {
        const auto &v = values[i];
        const auto &q = tests[i].q;
        if (v.final_status != unsigned(mh::Status::ok) || !tests[i].accept ||
            q.current_target == q.previous_target)
            continue;
        if (q.current_target > q.previous_target)
            for (int offset = -1; offset <= 1; ++offset) {
                auto c = tests[i];
                c.boundary = offset;
                c.accept = offset < 0;
                c.q.word_count = 9;
                c.label += " GPU threshold";
                long double threshold =
                    std::ldexp((long double)v.ratio.significand + offset, -int(v.ratio.bit_count));
                if (threshold >= 1)
                    std::fill(std::begin(c.q.words), std::end(c.q.words), ~0u);
                else
                    for (auto &word : c.q.words) {
                        threshold = std::ldexp(threshold, 32);
                        const auto digit = std::floor(threshold);
                        word = unsigned(digit);
                        threshold -= digit;
                    }
                out.push_back(c);
            }
        // With Z=1, the prepared value is exactly the GPU's represented C.
        // Reuse unchanged targets, choosing a boundary only when this observed
        // correction forces a nonnormal scaled Z; never infer C from CPU bits.
        if (q.normalizer == 1 && v.prepared_normalizer != 1) {
            auto c = tests[i];
            c.q.normalizer = v.prepared_normalizer < 1 ? low : high;
            c.preflight_failure = true;
            c.label = "GPU-derived nonnormalZ preflight";
            out.push_back(c);
            ++nonnormal;
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
    need(device != nil, "No Metal device for reuse MIS arithmetic probe");
    const Probe probe(device, path);
    const auto tests = cases();
    const auto values = probe.run(tests);
    for (std::size_t i = 0; i < tests.size(); ++i)
        check(tests[i], values[i]);
    unsigned nonnormal = 0;
    const auto edge = boundaries(tests, values, nonnormal);
    need(nonnormal > 0, "No actual GPU normalizer-correction boundary witness");
    const auto results = probe.run(edge);
    for (std::size_t i = 0; i < edge.size(); ++i)
        check(edge[i], results[i]);
    std::cout << "Reuse MIS arithmetic GPU conformance passed: " << tests.size() << " base, "
              << edge.size() << " GPU-derived boundaries, " << nonnormal
              << " nonnormalZ failures\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Usage: reuse-mis-math-probe LIBRARY");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
