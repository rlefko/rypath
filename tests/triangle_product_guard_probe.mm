// SPDX-License-Identifier: GPL-3.0-only
#include "geometry_wire.h"
#include "metal_execution.hpp"
#include "triangle_product_guard_cases.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace rypath;
namespace {
void need(bool value, std::string_view reason) {
    if (!value)
        throw std::runtime_error(std::string(reason));
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            bool allow_no_device = false;
            for (int i = 1; i < argc; ++i) {
                std::string_view flag(argv[i]);
                need(flag == "--allow-no-device" || flag == "--allow-no-ray-tracing",
                     "Unexpected guard probe argument");
                allow_no_device |= flag == "--allow-no-device";
            }
            auto device = MTLCreateSystemDefaultDevice();
            if (device == nil && allow_no_device) {
                std::cout << "SKIP Triangle product guard probe: no Metal device available\n";
                return 77;
            }
            need(device != nil, "Triangle product guard probe requires Metal device");
            std::string error;
            auto library = metal::load_library(device, {}, error);
            need(library != nil, error);
            auto pipeline = metal::load_pipeline(device, library,
                                                 @"rypath_triangle_product_guard_probe", error);
            need(pipeline != nil, error);
            auto cases = test::product_guard::cases();
            const auto count = static_cast<std::uint32_t>(cases.size());
            static_assert(sizeof(test::product_guard::Case) == 16);
            auto input = [device newBufferWithBytes:cases.data()
                                             length:cases.size() * sizeof(cases[0])
                                            options:MTLResourceStorageModeShared];
            auto output = [device newBufferWithLength:(cases.size() + 64) * 32
                                              options:MTLResourceStorageModeShared];
            need(input && output, "Guard probe buffers");
            std::memset(output.contents, 0xa5, output.length);
            auto queue = [device newCommandQueue];
            auto command = [queue commandBuffer];
            auto encoder = [command computeCommandEncoder];
            need(queue && command && encoder, "Guard probe command");
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:input offset:0 atIndex:0];
            [encoder setBuffer:output offset:0 atIndex:1];
            [encoder setBytes:&count length:sizeof(count) atIndex:2];
            [encoder dispatchThreadgroups:MTLSizeMake((count + 63) / 64, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [encoder endEncoding];
            error = metal::complete(command, "Triangle product guard probe");
            need(error.empty(), error);
            auto words = static_cast<const std::uint32_t *>(output.contents);
            std::size_t unsupported = 0;
            for (std::size_t i = 0; i < cases.size(); ++i) {
                auto c = cases[i];
                const auto *w = words + i * 8;
                auto expected = test::product_guard::oracle(c.high_a, c.high_b);
                need(w[0] == expected,
                     "Native range classifier differs from independent double oracle");
                const float a = test::product_guard::value(c.high_a),
                            b = test::product_guard::value(c.high_b), primary = a * b;
                if (expected != triangle_numeric::unsupported) {
                    need(test::product_guard::value(w[1]) == primary,
                         "Returned primary differs from CPU product");
                    const float residual =
                        static_cast<float>(double(a) * double(b) - double(primary));
                    need(test::product_guard::value(w[2]) == residual,
                         "Returned FMA residual differs from independent exact double oracle");
                } else
                    ++unsupported;
                if (i == 3) {
                    need(w[3] == 1 && w[6] == 1 && w[7] == 1,
                         "Discarded subnormal cross residual must not invalidate normal rounded "
                         "result");
                    need(w[4] == 0x1f800001u && w[5] == 0x12800002u,
                         "Returned cross-term pair preserves declared rounded operations");
                } else {
                    need(bool(w[3]) == (expected != triangle_numeric::unsupported),
                         "Retained error unsupported state must remain visible");
                    if (w[3]) {
                        need(test::product_guard::value(w[4]) == primary, "Returned pair high");
                        need(test::product_guard::value(w[5]) ==
                                 static_cast<float>(double(a) * double(b) - double(primary)),
                             "Returned pair low");
                    }
                }
            }
            for (std::size_t i = cases.size() * 32; i < output.length; ++i)
                need(static_cast<const unsigned char *>(output.contents)[i] == 0xa5,
                     "Guard probe padded output");
            // Direct helper check of the captured native candidate. This deliberately
            // avoids assuming native traversal will offer an exact t=0 endpoint again.
            // Three-vertex excerpt from mesh_00056.ply in pinned pbrt-v4-scenes
            // 30cf4a0346ae. Geometry: Hamza Cheggour, CC-BY-2.0; full source
            // attribution and asset hash are in tests/corpus/pbrt-pavilion.json.
            const std::uint32_t vertices[3][3] = {{1083894315u, 1069847992u, 1087639438u},
                                                  {1083894315u, 1083048660u, 1087639438u},
                                                  {1083894315u, 1083048660u, 1087578866u}};
            geometry_wire::Triangle triangle{};
            std::memcpy(triangle.corners, vertices, sizeof(vertices));
            const std::uint32_t ray_bits[8] = {1083894315u, 1077860853u, 1087639424u, 1028619428u,
                                               1027843071u, 3212796152u, 0u,          0x7f800000u};
            geometry_wire::Ray ray{};
            std::memcpy(&ray, ray_bits, sizeof(ray));
            auto tb = [device newBufferWithBytes:&triangle
                                          length:sizeof(triangle)
                                         options:MTLResourceStorageModeShared],
                 rb = [device newBufferWithBytes:&ray
                                          length:sizeof(ray)
                                         options:MTLResourceStorageModeShared],
                 result = [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
            need(tb && rb && result, "Witness buffers");
            auto witness =
                metal::load_pipeline(device, library, @"rypath_triangle_witness_probe", error);
            need(witness != nil, error);
            for (int delta : {0, -1, 1}) {
                auto input_bits = std::array<std::uint32_t, 8>{
                    1083894315u, 1077860853u, 1087639424u, 1028619428u,
                    1027843071u, 3212796152u, 0u,          0x7f800000u};
                input_bits[0] = static_cast<std::uint32_t>(static_cast<int>(input_bits[0]) + delta);
                std::memcpy(rb.contents, input_bits.data(), sizeof(ray));
                std::memset(result.contents, 0xa5, 32);
                command = [queue commandBuffer];
                encoder = [command computeCommandEncoder];
                need(command && encoder, "Witness command");
                [encoder setComputePipelineState:witness];
                [encoder setBuffer:tb offset:0 atIndex:0];
                [encoder setBuffer:rb offset:0 atIndex:1];
                [encoder setBuffer:result offset:0 atIndex:2];
                [encoder dispatchThreads:MTLSizeMake(1, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [encoder endEncoding];
                error = metal::complete(command, "Triangle captured witness probe");
                need(error.empty(), error);
                auto *w = static_cast<const std::uint32_t *>(result.contents);
                need(w[0] == 1, "Captured witness and neighbors retain valid arithmetic");
                need(w[6] == 0xc001cafe && w[7] == 0xa5a5a5a5, "Witness completion and guard");
                if (delta == 0) {
                    // Exact t=0 is a finite-pair numerical boundary, distinct
                    // from the native candidate endpoint inclusion limitation.
                    // Exact scalar reconstruction gives a tiny negative computed
                    // t; characterize hit/miss without widening or clamping it.
                    std::cout << "Captured exact t=0 boundary: valid=" << w[0] << ", hit=" << w[1]
                              << ", t bits=" << w[2] << ',' << w[3] << '\n';
                } else if (delta < 0) {
                    // Independent exact rational oracle: t=32/3399465,
                    // u=207943247394224913/417726923546113400,
                    // v=57269263/102956196990, all barycentrics positive.
                    need(w[1] == 1 &&
                             test::product_guard::value(w[2]) == static_cast<float>(32. / 3399465.),
                         "Strict positive captured neighbor hit and correctly rounded t");
                } else {
                    // Exact t=-32/3399465, v=-9676753/102956196990: strict miss.
                    need(w[1] == 0, "Strict negative/outside captured neighbor miss");
                }
                if (w[1]) {
                    const float high = test::product_guard::value(w[2]);
                    const float low = test::product_guard::value(w[3]);
                    const float u = test::product_guard::value(w[4]);
                    const float v = test::product_guard::value(w[5]);
                    need(std::isfinite(high) && std::isfinite(low) && std::isfinite(u) &&
                             std::isfinite(v) && double(high) + low >= 0 && u >= 0 && v >= 0 &&
                             u + v <= 1,
                         "Returned captured hit is finite and barycentrically consistent");
                    const double expected_u = delta == 0
                                                  ? 18362800641. / 36864058628.
                                                  : 207943247394224913. / 417726923546113400.;
                    const double expected_v = delta == 0 ? 7. / 30286. : 57269263. / 102956196990.;
                    need(std::abs(double(u) - expected_u) <= 3e-8 &&
                             std::abs(double(v) - expected_v) <= 3e-8,
                         "Captured witness/neighbor independent barycentrics");
                }
            }
            std::cout << cases.size() << " CPU/native product error cases; " << unsupported
                      << " genuinely unsupported primaries/residuals retained; exact t=0 "
                         "boundary characterized and two strict neighbors checked\n";
            return 0;
        } catch (const std::exception &e) {
            std::cerr << "Triangle product guard probe failed: " << e.what() << '\n';
            return 1;
        }
    }
}
