// SPDX-License-Identifier: GPL-3.0-only
#include "layered_schedule_corrected_oracles.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <iostream>
namespace {
void metal_error(const char *operation, NSError *error) {
    std::cerr << operation << ": "
              << (error ? error.description.UTF8String : "Metal returned no NSError") << '\n';
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
        if (!coating_schedule(device, library) || !coating_moments(device, library))
            return 1;
        std::cout << "1024 packet GPU cases passed, max errors " << worst[0] << ' ' << worst[1]
                  << ' ' << worst[2] << ' ' << worst[3] << ' ' << worst[4] << ' ' << worst[5] << ' '
                  << worst[6] << ' ' << worst[7] << ' ' << worst[8] << ' ' << worst[9] << ' '
                  << worst[10] << ' ' << worst[11] << ' ' << worst[12] << ' ' << worst[13] << '\n';
    }
}
