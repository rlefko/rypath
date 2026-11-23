// SPDX-License-Identifier: GPL-3.0-only
#include "layered_schedule_corrected_oracles.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
namespace {
void metal_error(const char *operation, NSError *error) {
    std::cerr << operation << ": "
              << (error ? error.description.UTF8String : "Metal returned no NSError") << '\n';
}
bool outer_roulette(id<MTLDevice> device, id<MTLLibrary> library, id<MTLBuffer> table) {
    constexpr unsigned count = 48, words = 16;
    std::array<float, count * words + 1> initial{};
    initial.fill(std::numeric_limits<float>::quiet_NaN());
    initial.back() = 12345;
    NSError *error = nil;
    auto function = [library newFunctionWithName:@"packet_outer_roulette_probe"];
    auto pipeline =
        function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    auto output = [device newBufferWithBytes:initial.data()
                                      length:sizeof(initial)
                                     options:MTLResourceStorageModeShared];
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    if (!pipeline || !output || !queue || !command || !encoder) {
        metal_error("Packet roulette probe setup", error);
        return false;
    }
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder setBuffer:table offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(
                                  std::min<NSUInteger>(64, pipeline.maxTotalThreadsPerThreadgroup),
                                  1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        metal_error("Packet roulette probe execution", command.error);
        return false;
    }
    const auto *values = static_cast<const float *>(output.contents);
    if (!values || values[count * words] != 12345) {
        std::cerr << "Packet roulette probe guard\n";
        return false;
    }
    for (unsigned n = 0; n < 16; ++n) {
        std::array<float, 4> expected{1, .5f, 0, 0};
        bool eligible = true, enabled = true, alive = true;
        float scale = 1;
        if (n == 0) {
            expected = {.25f, .125f, 0, 0};
            alive = false;
        }
        if (n == 2) {
            expected = {.25f, .125f, 0, 0};
            eligible = false;
        }
        if (n == 3) {
            expected = {0x1p-26f, 0, 0, 0};
            eligible = false;
        }
        if (n == 4 || n == 7 || n == 15) {
            expected = {.25f, .125f, 0, 0};
            eligible = false;
            enabled = false;
        }
        if (n == 5) {
            expected = {.25f, .25f, .25f, .25f};
            scale = 4;
            eligible = false;
        }
        if (n == 9)
            expected = {0, 1, 0, .5f};
        if (n == 10) {
            expected = {1, .5f, 0, 0};
            eligible = false;
        }
        if (n == 11) {
            expected = {0, 0, 0, 0};
            eligible = false;
        }
        if (n == 13) {
            expected = {2, 1, 0, 0};
            scale = std::numeric_limits<float>::max();
            eligible = false;
        }
        if (n == 14) {
            expected = {0x1p-126f, 0, 0, 0};
            scale = 0x1p-10f;
            eligible = false;
        }
        if (n == 15)
            scale = 0x1p120f;
        const float *q = values + n * words;
        if (!std::equal(expected.begin(), expected.end(), q) || q[4] != (eligible ? .75f : 0) ||
            q[5] != float(eligible) || q[6] != scale || q[7] != float(enabled) ||
            q[8] != float(alive) || q[12] != 1 || q[13] != 1 || q[14] != 0 || q[15] != 0) {
            std::cerr << "Packet roulette branch case " << n << '\n';
            return false;
        }
    }
    for (unsigned n = 16; n < count; ++n) {
        const unsigned which = (n - 16) % 16;
        const float *q = values + n * words;
        // Rejected extreme arithmetic is permitted, but both old/new scalar APIs
        // must agree and every returned packet eta must be the producer's actual ep.
        if (q[7] != 1 || (which != 6 && (q[2] != 1 || q[3] != 1))) {
            std::cerr << "Packet sampled eta validity case " << n << '\n';
            return false;
        }
        if ((which <= 6 || which == 13) && q[0] != q[1]) {
            std::cerr << "Packet eta differs from sampled dielectric optics " << n << '\n';
            return false;
        }
        if (which != 6) {
            const float expected = which == 0 || which == 4 || which == 13 || which == 14 ? 1.5f
                                   : which == 1 || which == 5 || which == 15 ? 1.f / 1.5f
                                                                             : 1.f;
            if (q[0] != expected || (which == 13 && (q[10] != 1 || q[11] != 0))) {
                std::cerr << "Packet event eta convention case " << n << '\n';
                return false;
            }
        }
    }
    return true;
}
bool diffuse_map(id<MTLDevice> device, id<MTLLibrary> library) {
    constexpr float near_one = 0x1.fffffep-1f;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // Axis/diagonal values are closed-form geometry, not a second disk mapper.
    const double h = std::sqrt(.5), k = std::sqrt(.125);
    const double a = 1 - 0x1p-23;
    const double edge_cos = std::cos(a * 3.14159265358979323846 / 4);
    const double edge_sin = std::sin(a * 3.14159265358979323846 / 4);
    struct Case {
        std::array<float, 4> query;
        std::array<double, 3> expected;
        bool valid;
    };
    constexpr unsigned count = 27;
    const std::array<Case, count> cases{
        {Case{{.5f, .5f, .25f, 0}, {0, 0, 1}, true},
         Case{{.75f, .5f, .25f, 0}, {.5, 0, .75}, true},
         Case{{.25f, .5f, .25f, 0}, {-.5, 0, .75}, true},
         Case{{.5f, .75f, .25f, 0}, {0, .5, .75}, true},
         Case{{.5f, .25f, .25f, 0}, {0, -.5, .75}, true},
         Case{{.75f, .75f, .25f, 0}, {k, k, .75}, true},
         Case{{.25f, .75f, .25f, 0}, {-k, k, .75}, true},
         Case{{.25f, .25f, .25f, 0}, {-k, -k, .75}, true},
         Case{{.75f, .25f, .25f, 0}, {k, -k, .75}, true},
         Case{{0, .5f, .25f, 0}, {-1, 0, 0}, true},
         Case{{.5f, 0, .25f, 0}, {0, -1, 0}, true},
         Case{{0, 0, .25f, 0}, {-h, -h, 0}, true},
         Case{{near_one, near_one, .25f, 0}, {a * h, a * h, 1 - a * a}, true},
         Case{{near_one, .5f, .25f, 0}, {a, 0, 1 - a * a}, true},
         Case{{0, near_one, .25f, 0}, {-edge_cos, edge_sin, 0}, true},
         Case{{near_one, 0, .25f, 0}, {edge_sin, -edge_cos, 0}, true},
         Case{{nan, .5f, .25f, 0}, {}, false},
         Case{{.5f, nan, .25f, 0}, {}, false},
         Case{{-.125f, .5f, .25f, 0}, {}, false},
         Case{{.5f, 1, .25f, 0}, {}, false},
         Case{{.5f, .5f, nan, 0}, {}, false},
         Case{{.5f, .5f, -.125f, 0}, {}, false},
         Case{{.5f, .5f, 1, 0}, {}, false},
         Case{{1, .5f, .25f, 0}, {}, false},
         Case{{inf, .5f, .25f, 0}, {}, false},
         Case{{.5f, inf, .25f, 0}, {}, false},
         Case{{.5f, .5f, inf, 0}, {}, false}}};
    std::array<std::array<float, 4>, count> queries{};
    for (std::size_t n = 0; n < cases.size(); ++n)
        queries[n] = cases[n].query;
    constexpr std::size_t words = 24, variants = 8;
    std::array<float, (count * variants + 1) * words> initial{};
    initial.fill(12345);
    NSError *error = nil;
    auto function = [library newFunctionWithName:@"packet_diffuse_map_probe"];
    id<MTLComputePipelineState> pipeline =
        function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!pipeline) {
        metal_error("Packet diffuse map pipeline", error);
        return false;
    }
    auto input = [device newBufferWithBytes:queries.data()
                                     length:sizeof(queries)
                                    options:MTLResourceStorageModeShared];
    auto output = [device newBufferWithBytes:initial.data()
                                      length:sizeof(initial)
                                     options:MTLResourceStorageModeShared];
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    if (!input || !output || !output.contents || !queue || !command || !encoder)
        return false;
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(cases.size() * variants + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(
                                  std::min(NSUInteger{8}, pipeline.maxTotalThreadsPerThreadgroup),
                                  1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        metal_error("Packet diffuse map command", command.error);
        return false;
    }
    const auto *data = static_cast<const float *>(output.contents);
    for (std::size_t n = cases.size() * variants * words; n < initial.size(); ++n)
        if (data[n] != 12345)
            return false;
    const auto close = [](double x, double y) {
        return std::isfinite(x) && std::abs(x - y) <= 2e-7;
    };
    for (std::size_t variant = 0; variant < variants; ++variant)
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const auto *r = data + (variant * cases.size() + n) * words;
            const auto &c = cases[n];
            const auto fail = [&] {
                std::cerr << "Packet diffuse map case " << n << " variant " << variant << '\n';
                return false;
            };
            if (r[9] != (c.valid ? 1 : 0) || r[11] != 0 || r[15] != 0)
                return fail();
            if (!c.valid) {
                if (r[10] != 0 || r[3] != 0)
                    return fail();
            }
            if (!r[10]) {
                if (r[8] != 0 || r[14] != 0)
                    return fail();
                for (unsigned lane = 0; lane < 4; ++lane)
                    if (r[4 + lane] != 0)
                        return fail();
            }
            if (c.valid) {
                const double sign = (variant & 1) ? -1 : 1;
                if (!close(r[0], c.expected[0]) || !close(r[1], c.expected[1]) ||
                    !close(double(r[2]) * r[2], c.expected[2]) || sign * r[2] < 0 ||
                    !close(r[3], std::abs(r[2]) / 3.14159265358979323846) ||
                    r[10] != (r[3] > 0 ? 1 : 0))
                    return fail();
                if (r[10]) {
                    // Unit wo gives reverse density1/pi; sampled forward density uses actual z.
                    if (!close(r[8], 1 / 3.14159265358979323846) || !close(r[12], r[3]) ||
                        !close(r[13], r[8]) || r[14] != 1)
                        return fail();
                    for (unsigned lane = 0; lane < 4; ++lane) {
                        const bool active = !(variant & 4) || lane == 1 || lane == 3;
                        if (!close(r[4 + lane], active ? (lane + 1) * .125 : 0))
                            return fail();
                    }
                }
            }
            for (unsigned offset : {16u, 20u})
                if (!close(r[offset], -h) || !close(r[offset + 1], 0) || !close(r[offset + 2], h) ||
                    r[offset + 3] != 1)
                    return fail();
        }
    return true;
}
bool directional_pdf_validity(id<MTLDevice> device, id<MTLLibrary> library) {
    constexpr std::size_t cases = 7, words = 32;
    constexpr std::uint32_t sentinel = 0xcdfacadeu;
    std::array<std::uint32_t, (cases + 1) * words> initial{};
    initial.fill(sentinel);
    NSError *error = nil;
    auto function = [library newFunctionWithName:@"packet_pdf_validity_probe"];
    id<MTLComputePipelineState> pipeline =
        function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!pipeline) {
        metal_error("Packet PDF validity pipeline", error);
        return false;
    }
    auto output = [device newBufferWithBytes:initial.data()
                                      length:sizeof(initial)
                                     options:MTLResourceStorageModeShared];
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    if (!output || !output.contents || !queue || !command || !encoder)
        return false;
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(cases + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(
                                  std::min(NSUInteger{8}, pipeline.maxTotalThreadsPerThreadgroup),
                                  1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        metal_error("Packet PDF validity command", command.error);
        return false;
    }
    const auto *data = static_cast<const std::uint32_t *>(output.contents);
    for (std::size_t i = cases * words; i < initial.size(); ++i)
        if (data[i] != sentinel)
            return false;
    for (std::size_t i = 0; i < cases; ++i) {
        const auto *r = data + i * words;
        const auto value = [r](unsigned slot) { return std::bit_cast<float>(r[slot]); };
        bool valid = r[0] == 0x50444631u && r[1] == i && r[2] == 1 && r[3] == 1 && r[12] == 1 &&
                     r[13] == 1 && r[28] == 1;
        if (i == 1) {
            // Finite preparation succeeds, but eta squared overflows binary32.
            // Invalid leaf arithmetic must not masquerade as legal support zero.
            valid = valid && r[4] == 0 && value(5) == 0 && std::isnan(value(6)) &&
                    std::isnan(value(7)) && r[8] == 0 && r[9] == 0 && r[10] == 0 &&
                    std::isnan(value(24)) && std::isnan(value(25));
        } else {
            valid = valid && r[4] == 1 && r[8] == 1 && r[9] == 1 && r[26] == 1 && r[29] == 1 &&
                    r[5] == r[6] && r[27] == r[7] && r[24] == r[6] && r[25] == r[7];
            if (i == 0 || i == 2 || i == 3 || i == 5)
                valid = valid && std::isfinite(value(6)) && value(6) > 0;
            if (i == 0)
                valid = valid && r[10] == 1 && r[14] == 1; // Nonvacuous joint Sample_f check.
            if (i == 4)
                valid = valid && value(6) == 0; // Non-layered support zero is legal.
            if (i == 5)
                valid = valid && r[10] == 0; // Valid layered tangent rejection.
            if (i == 6)
                valid = valid && r[10] == 1 && r[11] == 1; // Delta sample stays accepted.
        }
        if (!valid) {
            std::cerr << "Packet PDF validity case " << i << " leaf/evaluation/sample flags "
                      << r[4] << '/' << r[8] << '/' << r[9] << " queried PDF words " << r[6] << '/'
                      << r[7] << '\n';
            return false;
        }
    }
    return true;
}

bool coating_schedule(id<MTLDevice> device, id<MTLLibrary> library) {
    using namespace layered_schedule_oracles;
    using Q = rypath::layered_probe::Query;
    using R = rypath::layered_probe::Result;
    std::array<Q, fixtures.size()> queries{};
    for (std::size_t n = 0; n < queries.size(); ++n)
        queries[n] = fixtures[n].query;
    NSError *error = nil;
    auto function = [library newFunctionWithName:@"coating_schedule_probe"];
    if (!function)
        return false;
    auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) {
        metal_error("Coating schedule pipeline", error);
        return false;
    }
    auto input = [device newBufferWithBytes:queries.data()
                                     length:sizeof(queries)
                                    options:MTLResourceStorageModeShared];
    auto output = [device newBufferWithLength:(queries.size() + 1) * sizeof(R)
                                      options:MTLResourceStorageModeShared];
    if (!input || !output || !output.contents)
        return false;
    auto data = static_cast<R *>(output.contents);
    data[queries.size()].valid = 12345;
    const unsigned count = static_cast<unsigned>(queries.size());
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    if (!queue || !command || !encoder)
        return false;
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(
                                  std::min(NSUInteger{32}, pipeline.maxTotalThreadsPerThreadgroup),
                                  1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted || data[queries.size()].valid != 12345)
        return false;
    for (std::size_t n = 0; n < queries.size(); ++n) {
        const auto &r = data[n];
        const auto e = layered_schedule_corrected::expected(n);
        auto close = [&](float actual, float expected, float tolerance, const char *field) {
            if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
                std::cerr << "PBRT schedule " << fixtures[n].label << ' ' << field << ": " << actual
                          << " expected " << expected << '\n';
                return false;
            }
            return true;
        };
        // The fixed inputs and RNG schedule match. Permit only CPU/Metal
        // transcendental/GGX arithmetic differences, not independent streams.
        if (!r.valid || !r.evaluation_valid ||
            !close(r.f, e[0], 5e-5f * std::max(1.f, std::abs(e[0])), "f") ||
            !close(r.pdf, e[1], 5e-5f * std::max(1.f, std::abs(e[1])), "PDF"))
            return false;
        if (queries[n].operation == 4) {
            // At disk corners a single sin/cos ULP can round z to zero.
            // Check the radicand (z squared), whose bound remains conditioned.
            if (bool(r.accepted) != (std::abs(r.iz) > 0)) {
                std::cerr << "PBRT base map " << fixtures[n].label << " sample acceptance\n";
                return false;
            }
            if (!close(r.sample_pdf, std::abs(r.iz) / 3.14159265358979323846f, 2e-7f,
                       "sampled base PDF") ||
                !close(r.ix, e[4], 2e-7f, "disk x") || !close(r.iy, e[5], 2e-7f, "disk y") ||
                !close(r.iz * r.iz, e[6] * e[6], 2e-7f, "disk z squared") ||
                (r.accepted && !close(r.weight, e[2], 2e-7f, "base weight")))
                return false;
        } else {
            if (bool(r.accepted) != (e[3] > 0)) {
                std::cerr << "PBRT schedule " << fixtures[n].label << " walk acceptance\n";
                return false;
            }
            if (r.accepted &&
                (!close(r.weight, e[2], 5e-5f, "joint weight") ||
                 !close(r.ix, e[4], 5e-5f, "wi.x") || !close(r.iy, e[5], 5e-5f, "wi.y") ||
                 !close(r.iz, e[6], 5e-5f, "wi.z")))
                return false;
        }
    }
    std::cout << "PASS " << count << " fixed modified-PBRT packet schedule/base-map fixtures\n";
    return true;
}
bool coating_moments(id<MTLDevice> device, id<MTLLibrary> library) {
    constexpr unsigned count = 131072, routes = 12;
    // PBRT b0bdf461 with the two corrected coating MIS proposal calls.
    // scripts/pbrt_layered_moments_probe.cpp; fixed PCG(17,29), 131072 samples.
    // Only f/weighted-f columns changed; all 72 joint/PDF targets stay exact.
    // Authenticated proposal SHA256:
    // da53b157642728af8f48f971572309ae189e7633a930a432bb6ed5ee6c4ee7a5
    // Mean and raw second moment for joint weight, weighted joint weight,
    // f*cos, weighted f*cos, and directional PDF (the last three integrate
    // a uniform upper hemisphere). These finite approximations are distinct.
    constexpr double expected[routes][10] = {
        {0.321860736847, 0.244908528094, 0.407147824532, 0.405096276358, 0.320757308162,
         0.163443373771, 0.405552455696, 0.289120055581, 2.20646792897, 7.12102924611},
        {0.321446524062, 0.205473994699, 0.406551870989, 0.340634568985, 0.320956115609,
         0.159489086317, 0.405786658962, 0.282080747154, 2.20646792897, 7.12102924611},
        {0.484329702855, 0.433300984926, 0.630784069651, 0.752638483952, 0.482015898476,
         0.640948999356, 0.627584416425, 1.20065614432, 2.4452658914, 14.9651038158},
        {0.484686496905, 0.392422026037, 0.631253374529, 0.687755132873, 0.481721173499,
         0.630134518901, 0.627140564912, 1.18267713271, 2.4452658914, 14.9651038158},
        {0.283016152033, 0.182684051477, 0.361501551437, 0.311699208666, 0.296292851015,
         0.144692353284, 0.377509464645, 0.263504923981, 2.20513397933, 7.21007559881},
        {0.283016170377, 0.182684073326, 0.36150157458, 0.31169924462, 0.296292852387,
         0.144692354799, 0.377509466375, 0.263504926663, 2.20513397933, 7.21007559881},
        {0.448214977921, 0.373131053162, 0.60805408148, 0.706060468516, 0.458677216919,
         0.774808793701, 0.620066415188, 1.59566827129, 2.43945688566, 16.706759065},
        {0.448215008686, 0.373131103545, 0.60805412313, 0.706060563467, 0.458677220075,
         0.774808805703, 0.620066419455, 1.59566829531, 2.43945688566, 16.706759065},
        {0.412836457244, 0.339137574552, 0.433530314036, 0.383802289774, 0.412087849204,
         1.87050507127, 0.432995330654, 1.26492707551, 2.24634533419, 13.9130076048},
        {0.412369845167, 0.308171673618, 0.432956066904, 0.333745456252, 0.412312180624,
         1.86991127854, 0.433275734396, 1.26392573447, 2.24634533419, 13.9130076048},
        {0.4168974057, 0.372479815178, 0.424195581935, 0.41069608421, 0.416411038133, 1.99729434306,
         0.424240680322, 1.35398547756, 1.80558776196, 17.6628605023},
        {0.417007112376, 0.326810833692, 0.42453415955, 0.339569070885, 0.416724079962,
         1.99402974418, 0.424661799749, 1.34965528712, 1.80558776196, 17.6628605023},
    };
    NSError *error = nil;
    auto function = [library newFunctionWithName:@"coating_moments_probe"];
    if (!function) {
        std::cerr << "Missing coating_moments_probe\n";
        return false;
    }
    auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) {
        metal_error("Coating moment pipeline", error);
        return false;
    }
    const std::size_t values = std::size_t{count} * routes * 8;
    auto output = [device newBufferWithLength:(values + 4) * sizeof(float)
                                      options:MTLResourceStorageModeShared];
    if (!output || !output.contents) {
        std::cerr << "Coating moment output allocation failed\n";
        return false;
    }
    auto data = static_cast<float *>(output.contents);
    data[values] = 12345;
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    if (!queue || !command || !encoder)
        return false;
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(count * routes + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(
                                  std::min(NSUInteger{32}, pipeline.maxTotalThreadsPerThreadgroup),
                                  1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted || data[values] != 12345) {
        metal_error("Coating moment dispatch", command.error);
        return false;
    }
    for (unsigned route = 0; route < routes; ++route) {
        double sum[5] = {}, square[5] = {};
        for (unsigned n = 0; n < count; ++n) {
            const auto q = data + (std::size_t{route} * count + n) * 8;
            if (q[5] != 1)
                return false;
            for (unsigned i = 0; i < 5; ++i) {
                if (!std::isfinite(q[i]))
                    return false;
                sum[i] += q[i];
                square[i] += double(q[i]) * q[i];
            }
        }
        for (unsigned i = 0; i < 5; ++i) {
            const double mean = sum[i] / count, target = expected[route][std::size_t{2} * i];
            const double variance = std::max(0., square[i] / count - mean * mean);
            const double reference_variance =
                std::max(0., expected[route][std::size_t{2} * i + 1] - target * target);
            // Selected before GPU results: six combined standard errors plus
            // 0.1% absolute/relative numerical allowance. Hash discontinuities
            // preclude pointwise equality after floating frame arithmetic.
            const double tolerance = 6 * std::sqrt((variance + reference_variance) / count) +
                                     .001 * std::max(1., std::abs(target));
            std::cout << "Coating route " << route << " moment " << i << ": " << mean << " PBRT "
                      << target << " tolerance " << tolerance << '\n';
            if (std::abs(mean - target) > tolerance)
                return false;
        }
    }
    return true;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) {
            std::cerr << "Expected one packet scattering metallib argument; received " << argc - 1
                      << '\n';
            return 2;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::cerr << "No Metal device\n";
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            return 77;
#else
            return 1;
#endif
        }
        const auto maximum_group = device.maxThreadsPerThreadgroup;
        std::cout << "Packet scattering device: " << device.name.UTF8String << "; maximum group "
                  << maximum_group.width << 'x' << maximum_group.height << 'x'
                  << maximum_group.depth << "; library: " << argv[1] << std::endl;
        auto path = [NSString stringWithUTF8String:argv[1]];
        if (!path) {
            std::cerr << "Packet scattering metallib path is not valid UTF-8\n";
            return 2;
        }
        NSError *error = nil;
        auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
        if (!library) {
            metal_error("Packet scattering library load failed", error);
            return 2;
        }
        auto function = [library newFunctionWithName:@"packet_scattering_probe"];
        if (!function) {
            std::cerr << "Metal library has no packet_scattering_probe function\n";
            return 2;
        }
        error = nil;
        auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            metal_error("Packet scattering pipeline creation failed", error);
            return 2;
        }
        const auto maximum_threads = pipeline.maxTotalThreadsPerThreadgroup;
        const auto group_width = std::min(NSUInteger{32}, maximum_threads);
        std::cout << "Packet scattering pipeline: execution width " << pipeline.threadExecutionWidth
                  << ", maximum threads " << maximum_threads
                  << "; dispatch 1025 threads, group width " << group_width << std::endl;
        if (group_width == 0) {
            std::cerr << "Packet scattering pipeline reports zero maximum threads\n";
            return 3;
        }
        constexpr std::size_t output_values = std::size_t{1024} * 14 * 4;
        auto output = [device newBufferWithLength:(output_values + 4) * sizeof(float)
                                          options:MTLResourceStorageModeShared];
        if (!output) {
            std::cerr << "Packet scattering output allocation failed: requested "
                      << (output_values + 4) * sizeof(float) << " bytes; device maximum "
                      << device.maxBufferLength << '\n';
            return 3;
        }
        auto output_data = static_cast<float *>(output.contents);
        if (!output_data) {
            std::cerr << "Packet scattering shared output buffer has no CPU mapping\n";
            return 3;
        }
        auto guard = output_data + output_values;
        guard[0] = 12345;
        std::array<unsigned, 72> table{};
        table[0] = 1;
        table[1] = 3;
        table[2] = 1;
        table[3] = 1;
        table[4] = std::bit_cast<unsigned>(1.5f);
        table[5] = 32;
        table[6] = 35;
        table[7] = 44;
        table[8] = 62;
        for (unsigned i = 0; i < 3; ++i)
            table[32 + i] = std::bit_cast<unsigned>(float(i) - 1);
        for (unsigned o = 0; o < 3; ++o)
            for (unsigned i = 0; i < 3; ++i) {
                table[35 + o * 3 + i] = std::bit_cast<unsigned>(float(i));
                table[44 + 2 * (o * 3 + i)] = 0;
                table[45 + 2 * (o * 3 + i)] = 1;
            }
        table[62] = std::bit_cast<unsigned>(1.f);
        auto table_buffer = [device newBufferWithBytes:table.data()
                                                length:sizeof(table)
                                               options:MTLResourceStorageModeShared];
        if (!table_buffer) {
            std::cerr << "Packet scattering table allocation failed: requested " << sizeof(table)
                      << " bytes; device maximum " << device.maxBufferLength << '\n';
            return 3;
        }
        auto queue = [device newCommandQueue];
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (!queue || !command || !encoder) {
            std::cerr << "Packet scattering command allocation failed: queue " << (queue != nil)
                      << ", command " << (command != nil) << ", encoder " << (encoder != nil)
                      << '\n';
            return 3;
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBuffer:table_buffer offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(1025, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            std::cerr << "Packet scattering command status "
                      << static_cast<unsigned long>(command.status) << '\n';
            metal_error("Packet scattering GPU execution failed", command.error);
            return 3;
        }
        if (guard[0] != 12345) {
            std::cerr << "Packet scattering guard overwritten at byte offset "
                      << output_values * sizeof(float) << ": expected 12345, observed " << guard[0]
                      << '\n';
            return 3;
        }
        const float *p = static_cast<const float *>(output.contents);
        if (!p) {
            std::cerr << "Packet scattering output has no CPU mapping after GPU completion\n";
            return 3;
        }
        float worst[14] = {};
        for (unsigned n = 0; n < 1024; ++n)
            for (unsigned c = 0; c < 14; ++c) {
                auto q = p + (std::size_t{n} * 14 + c) * 4;
                worst[c] = std::max(worst[c], q[0]);
                if (c == 11 && !(q[3] > 1e-7f)) {
                    std::cerr
                        << "Coating permutation fixture did not exercise old hero-only density\n";
                    return 1;
                }
                if (!std::isfinite(q[0]) || q[0] > 2e-5f || q[1] != 1 || q[2] != 1) {
                    std::cerr << "Failure " << n << " case " << c << " error " << q[0] << " flags "
                              << q[1] << ',' << q[2] << '\n';
                    return 1;
                }
            }
        if (!outer_roulette(device, library, table_buffer) || !diffuse_map(device, library) ||
            !directional_pdf_validity(device, library) || !coating_schedule(device, library) ||
            !coating_moments(device, library))
            return 1;
        std::cout << "1024 packet GPU cases passed, max errors " << worst[0] << ' ' << worst[1]
                  << ' ' << worst[2] << ' ' << worst[3] << ' ' << worst[4] << ' ' << worst[5] << ' '
                  << worst[6] << ' ' << worst[7] << ' ' << worst[8] << ' ' << worst[9] << ' '
                  << worst[10] << ' ' << worst[11] << ' ' << worst[12] << ' ' << worst[13] << '\n';
    }
}
