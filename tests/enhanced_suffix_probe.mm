// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_suffix_tape_shared.h"
#include "metal_execution.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace rypath;
namespace tape = rypath_enhanced_suffix_tape;
namespace hero = rypath_hero;
using Word = std::uint32_t;
using Query = std::array<Word, 40>;
using Four = std::array<long double, 4>;
constexpr unsigned stride = 112, guard = 256;
void need(bool condition, const std::string &why) {
    if (!condition)
        throw std::runtime_error(why);
}
Word bits(float x) { return std::bit_cast<Word>(x); }
float fp(Word x) { return std::bit_cast<float>(x); }
hero::Density density(hero::Vector values) {
    hero::Density result{};
    for (unsigned k = 0; k < 4; ++k) {
        int exponent = 0;
        const float fraction = std::frexp(values[k], &exponent);
        result.lane[k] = {fraction, exponent};
    }
    return result;
}
long double number(hero::Number x) {
    return std::ldexp(static_cast<long double>(x.fraction), x.exponent);
}
long double sum(Four x) { return (x[0] + x[1]) + (x[2] + x[3]); }
struct Expected {
    Four value{}, beta{}, density{}, mis{};
};
// Independent long-double likelihood/mixture arithmetic with explicit binary32
// beta and terminal product materialization. No portable transport helper is
// called for expectations; tolerances are fixed before GPU execution.
Expected oracle(const tape::Entry &e, const tape::Input &input) {
    Expected result;
    for (unsigned k = 0; k < 4; ++k) {
        result.beta[k] = input.beta[k];
        result.density[k] = number(input.density.lane[k]);
    }
    const auto count = e.flags & tape::operation_count_mask;
    for (unsigned op = 0; op < count; ++op) {
        result.mis = result.density;
        const auto before = sum(result.density);
        for (unsigned k = 0; k < 4; ++k)
            result.density[k] *= e.operations[op].sampled_likelihood[k];
        const auto after = sum(result.density);
        need(after > 0, "Independent suffix oracle sampled support");
        for (unsigned k = 0; k < 4; ++k)
            result.beta[k] =
                static_cast<float>(result.beta[k] * e.operations[op].numerator[k] * before / after);
    }
    const bool direct = (e.flags & tape::nee) != 0;
    for (unsigned k = 0; k < 4; ++k) {
        float value = static_cast<float>(result.beta[k]) * e.factor_a[k];
        if (direct) {
            value = value * e.factor_b[k];
            value = value * e.cosine;
        }
        result.value[k] = value;
    }
    if (std::ranges::all_of(result.value, [](auto x) { return x == 0; }))
        return result; // Original terminal-zero branch precedes MIS/PDF work.
    if (direct)
        result.mis = result.density;
    else if (count == 0)
        for (unsigned k = 0; k < 4; ++k)
            result.mis[k] = number(input.pre_pivot_density.lane[k]);
    long double sampled = 0, other = 0;
    for (unsigned k = 0; k < 4; ++k) {
        if (result.mis[k] == 0)
            continue; // Poisoned unused PDF words are not receipts.
        const float q = !direct && count == 0 ? input.immediate_bsdf_pdf[k] : e.bsdf_pdf[k];
        sampled += result.mis[k] * (direct ? e.light_pdf : q);
        other += result.mis[k] * (direct ? q : e.light_pdf);
    }
    if (other != 0)
        for (auto &value : result.value)
            value *= sampled / (sampled + other);
    return result;
}
struct Case {
    tape::Entry entry{};
    tape::Input input{};
    tape::Status expected = tape::Status::ok;
    bool deferred = false, zero = false;
    std::string label;
};
Case basic(unsigned count, bool direct) {
    Case c;
    auto &e = c.entry;
    auto &x = c.input;
    e.version = tape::version;
    e.flags = count | tape::required_capture_flags | (direct ? tape::nee : 0);
    e.coverage = tape::known_coverage;
    e.pivot_event = x.pivot_event = 1;
    e.selected_slot = x.selected_slot = 2 * (count + 2) + unsigned(direct);
    e.context_low = x.context_low = 0x12345678u;
    e.context_high = x.context_high = 0x9abcdef0u;
    x.beta = {.5f, 1.5f, 2, 3};
    x.density = density({1, 2, 3, 4});
    x.pre_pivot_density = density({4, 3, 2, 1});
    x.immediate_bsdf_pdf = {.125f, .75f, .25f, 1};
    x.immediate_pdf_ready = 1;
    x.immediate_pdf_coverage = 15;
    const hero::Vector numerator[2]{{.5f, 1.25f, 2, 3}, {1.5f, .75f, .25f, 2}};
    const hero::Vector pdf[2]{{.25f, .5f, .75f, 1}, {.5f, 1, .125f, .25f}};
    const hero::Vector a{1, 2, .5f, 3}, b{.25f, 1.5f, 2, .75f}, q{.25f, .5f, .75f, 1};
    for (unsigned k = 0; k < 4; ++k) {
        for (unsigned op = 0; op < 2; ++op) {
            e.operations[op].numerator[k] = numerator[op][k];
            e.operations[op].sampled_likelihood[k] = pdf[op][k];
        }
        e.factor_a[k] = a[k];
        e.factor_b[k] = direct ? b[k] : 0;
        e.bsdf_pdf[k] = q[k];
    }
    e.cosine = direct ? .5f : 0;
    e.light_pdf = .3f;
    e.immediate_direction[2] = 1;
    c.deferred = count == 0 && !direct;
    c.label = std::to_string(count) + " ordinary operations " + (direct ? "N" : "E");
    return c;
}
std::vector<Case> cases() {
    std::vector<Case> result;
    for (unsigned count = 0; count < 3; ++count)
        for (bool direct : {false, true})
            result.push_back(basic(count, direct));
    auto c = basic(0, false);
    c.entry.light_pdf = 0;
    c.label = "immediate E zero competitor";
    result.push_back(c);
    c.input.immediate_bsdf_pdf = {};
    c.label = "immediate E both techniques zero";
    result.push_back(c);
    c = basic(0, false);
    c.input.beta[0] = 0;
    c.input.density.lane[0] = {};
    c.entry.coverage &=
        ~((1u << tape::terminal_prepared_shift) | (1u << tape::terminal_response_shift));
    c.label = "PRE-pivot P-positive lane with current P and beta zero";
    result.push_back(c);
    c = basic(0, false);
    c.input.pre_pivot_density.lane[3] = {};
    c.input.immediate_bsdf_pdf[3] = NAN;
    c.input.immediate_pdf_coverage = 7;
    c.label = "unused live q poison ignored";
    result.push_back(c);
    c = basic(0, false);
    c.input.immediate_pdf_coverage = 7;
    c.expected = tape::Status::cache_miss;
    c.label = "missing immediate PDF coverage";
    result.push_back(c);
    for (float invalid : {-1.f, NAN}) {
        c = basic(0, false);
        c.input.immediate_bsdf_pdf[0] = invalid;
        c.expected = tape::Status::invalid_arithmetic;
        c.label = "invalid consumed live q";
        result.push_back(c);
    }
    c = basic(0, false);
    c.input.immediate_pdf_coverage = 31;
    c.expected = tape::Status::invalid_arithmetic;
    c.label = "unknown immediate coverage bit";
    result.push_back(c);
    c = basic(0, false);
    for (unsigned k = 0; k < 4; ++k) {
        c.entry.factor_a[k] = 0;
        c.input.pre_pivot_density.lane[k].fraction = NAN;
        c.input.immediate_bsdf_pdf[k] = NAN;
    }
    c.entry.light_pdf = NAN;
    c.input.immediate_pdf_ready = 2;
    c.input.immediate_pdf_coverage = ~0u;
    c.deferred = false;
    c.zero = true;
    c.label = "zero terminal skips poisoned MIS and unready receipt";
    result.push_back(c);
    c = basic(1, true);
    c.entry.coverage &= ~(1u << tape::terminal_pdf_shift);
    c.expected = tape::Status::cache_miss;
    c.label = "missing later N PDF coverage";
    result.push_back(c);
    c = basic(0, false);
    c.input.beta = {1, 1, 1, 1};
    c.input.immediate_bsdf_pdf.fill(std::numeric_limits<float>::min());
    c.entry.light_pdf = std::numeric_limits<float>::max();
    for (auto &a : c.entry.factor_a)
        a = std::numeric_limits<float>::max();
    c.label = "bright terminal with sub-FLOAT MIS weight";
    result.push_back(c);
    return result;
}
Query pack(const tape::Input &x) {
    Query q{};
    for (unsigned k = 0; k < 4; ++k) {
        q[k] = bits(x.beta[k]);
        q[4 + 2 * k] = bits(x.density.lane[k].fraction);
        q[5 + 2 * k] = std::bit_cast<Word>(x.density.lane[k].exponent);
        q[12 + 2 * k] = bits(x.pre_pivot_density.lane[k].fraction);
        q[13 + 2 * k] = std::bit_cast<Word>(x.pre_pivot_density.lane[k].exponent);
        q[20 + k] = bits(x.immediate_bsdf_pdf[k]);
    }
    q[24] = x.immediate_pdf_ready;
    q[25] = x.immediate_pdf_coverage;
    q[26] = x.selected_slot;
    q[27] = x.pivot_event;
    q[28] = x.context_low;
    q[29] = x.context_high;
    return q;
}
void close(long double actual, long double expected, const std::string &why) {
    const auto tolerance = 64.L * std::numeric_limits<float>::epsilon() * std::abs(expected) +
                           std::numeric_limits<float>::denorm_min();
    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, why);
}
long double scaled(const Word *words) {
    const float fraction = fp(words[0]);
    const int exponent = std::bit_cast<std::int32_t>(words[1]);
    need((words[0] == 0 && exponent == 0) ||
             (std::isfinite(fraction) && fraction >= .5f && fraction < 1 &&
              exponent >= -int(tape::maximum_density_exponent) &&
              exponent <= int(tape::maximum_density_exponent)),
         "GPU density is a normalized Number");
    return std::ldexp(static_cast<long double>(fraction), exponent);
}
int run(const std::filesystem::path &path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for Enhanced suffix math probe");
    std::string error;
    auto library = metal::load_library(device, path, error);
    need(library != nil, error);
    auto pipeline = metal::load_pipeline(device, library, @"rypath_enhanced_suffix_math", error);
    need(pipeline != nil, error);
    auto queue = [device newCommandQueue];
    need(queue != nil, "Enhanced suffix probe queue");
    const auto fixtures = cases();
    need(fixtures.size() == 17, "Bounded suffix math fixture inventory");
    std::vector<tape::Entry> entries;
    std::vector<Query> queries;
    for (const auto &c : fixtures) {
        entries.push_back(c.entry);
        queries.push_back(pack(c.input));
    }
    const unsigned count = unsigned(fixtures.size());
    auto entry_buffer = [device newBufferWithBytes:entries.data()
                                            length:entries.size() * sizeof(tape::Entry)
                                           options:MTLResourceStorageModeShared];
    auto query_buffer = [device newBufferWithBytes:queries.data()
                                            length:queries.size() * sizeof(Query)
                                           options:MTLResourceStorageModeShared];
    const std::size_t output_bytes = std::size_t(count + 1) * stride * sizeof(Word);
    auto output = [device newBufferWithLength:output_bytes + 2 * guard
                                      options:MTLResourceStorageModeShared];
    need(entry_buffer && query_buffer && output, "Enhanced suffix probe buffers");
    std::memset(output.contents, 0xa5, output.length);
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    need(command && encoder, "Enhanced suffix probe encoder");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:entry_buffer offset:0 atIndex:0];
    [encoder setBuffer:query_buffer offset:0 atIndex:1];
    [encoder setBuffer:output offset:guard atIndex:2];
    [encoder setBytes:&count length:sizeof(count) atIndex:3];
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    error = metal::complete(command, "Enhanced suffix pure helper probe");
    need(error.empty(), error);
    const auto *bytes = static_cast<const unsigned char *>(output.contents);
    const auto *out = reinterpret_cast<const Word *>(bytes + guard);
    unsigned deferred = 0;
    for (unsigned n = 0; n < count; ++n) {
        const auto &c = fixtures[n];
        const auto *row = out + n * stride;
        const auto *ready = row + 4, *pending = row + 36, *resumed = row + 68;
        const std::string label = "Suffix case " + std::to_string(n) + " " + c.label;
        need(row[0] == 0x53554631u && row[1] == 160, label + " wire layout");
        need(row[2] == unsigned(c.deferred), label + " deferred completion request/skip");
        need(ready[0] == Word(c.expected), label + " ready status");
        if (c.deferred) {
            ++deferred;
            need(pending[0] == Word(tape::Status::needs_immediate_pdf), label + " pending status");
            std::string difference;
            for (unsigned word = 0; word < 32; ++word)
                if (ready[word] != resumed[word]) {
                    const std::string field = word == 0                 ? "status"
                                              : word == 1               ? "operations_applied"
                                              : word == 2               ? "early_zero"
                                              : word >= 4 && word < 8   ? "value"
                                              : word >= 8 && word < 12  ? "beta"
                                              : word >= 12 && word < 20 ? "density"
                                              : word >= 20 && word < 28 ? "mis_density"
                                                                        : "reserved";
                    difference = " first_word=" + std::to_string(word) + " field=" + field +
                                 " ready=" + std::to_string(ready[word]) +
                                 " pending=" + std::to_string(pending[word]) +
                                 " resumed=" + std::to_string(resumed[word]);
                    break;
                }
            if (!difference.empty()) {
                // The first mismatch terminates the test below. Dump named
                // serialized words once, without changing any acceptance gate.
                std::cerr << label << " result_words (uint32)\n";
                for (unsigned word = 0; word < 32; ++word)
                    std::cerr << "word=" << word << " ready=" << ready[word]
                              << " pending=" << pending[word] << " resumed=" << resumed[word]
                              << '\n';
            }
            need(std::memcmp(ready, resumed, 32 * sizeof(Word)) == 0,
                 label + " exact ready/deferred result including all state bits" + difference);
            need(std::memcmp(pending + 8, resumed + 8, 20 * sizeof(Word)) == 0,
                 label + " completion retains beta/current P/MIS P bits");
            for (unsigned k = 0; k < 4; ++k) {
                const float unweighted = c.input.beta[k] * c.entry.factor_a[k];
                need(pending[4 + k] == bits(unweighted),
                     label + " pending receipt retains unweighted beta*Le bits");
            }
        } else {
            need(std::memcmp(ready, pending, 32 * sizeof(Word)) == 0 &&
                     std::memcmp(pending, resumed, 32 * sizeof(Word)) == 0,
                 label + " nonimmediate or zero result needs no live receipt");
        }
        if (c.expected != tape::Status::ok)
            continue;
        const auto expected = oracle(c.entry, c.input);
        need(ready[1] == (c.entry.flags & tape::operation_count_mask) &&
                 ready[2] == unsigned(c.zero),
             label + " operation/zero accounting");
        for (unsigned k = 0; k < 4; ++k) {
            close(fp(ready[4 + k]), expected.value[k], label + " independent terminal MIS");
            close(fp(ready[8 + k]), expected.beta[k], label + " independent sequential beta");
            close(scaled(ready + 12 + 2 * k), expected.density[k],
                  label + " independent current P");
            close(scaled(ready + 20 + 2 * k), expected.mis[k], label + " independent MIS P");
        }
    }
    need(deferred == 10, "Immediate-E deferred fixture inventory");
    for (unsigned k = 0; k < guard; ++k)
        need(bytes[k] == 0xa5 && bytes[guard + output_bytes + k] == 0xa5,
             "Enhanced suffix output outer guard");
    for (unsigned k = 0; k < stride; ++k)
        need(out[count * stride + k] == 0xa5a5a5a5u, "Enhanced suffix padded invocation guard");
    need(std::memcmp(entry_buffer.contents, entries.data(), entry_buffer.length) == 0 &&
             std::memcmp(query_buffer.contents, queries.data(), query_buffer.length) == 0,
         "Enhanced suffix entry and live inputs remain unchanged");
    std::cout << count << " Enhanced suffix pure helper cases, " << deferred
              << " deferred receipts; independent state/MIS oracle and exact resumed controls; "
                 "no live BSDF query, AS or full transport coverage\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Expected Enhanced suffix probe metallib");
            return run(argv[1]);
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
