// SPDX-License-Identifier: GPL-3.0-only
#include "geometry_wire.h"
#include "metal_execution.hpp"
#include "triangle_extended_cases.hpp"
#include "triangle_product_guard_cases.hpp"
#include <algorithm>
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
            const auto rounded = test::product_guard::rounded_cases();
            const auto rounded_count = static_cast<std::uint32_t>(rounded.size());
            auto rounded_pipeline = metal::load_pipeline(
                device, library, @"rypath_triangle_rounded_product_probe", error);
            need(rounded_pipeline != nil, error);
            auto rounded_input = [device newBufferWithBytes:rounded.data()
                                                     length:rounded.size() * sizeof(rounded[0])
                                                    options:MTLResourceStorageModeShared];
            auto rounded_output = [device newBufferWithLength:(rounded.size() + 64) * 16
                                                      options:MTLResourceStorageModeShared];
            need(rounded_input && rounded_output, "Rounded product buffers");
            std::memset(rounded_output.contents, 0xa5, rounded_output.length);
            command = [queue commandBuffer];
            encoder = [command computeCommandEncoder];
            need(command && encoder, "Rounded product command");
            [encoder setComputePipelineState:rounded_pipeline];
            [encoder setBuffer:rounded_input offset:0 atIndex:0];
            [encoder setBuffer:rounded_output offset:0 atIndex:1];
            [encoder setBytes:&rounded_count length:sizeof(rounded_count) atIndex:2];
            [encoder dispatchThreadgroups:MTLSizeMake((rounded_count + 63) / 64, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [encoder endEncoding];
            error = metal::complete(command, "Triangle rounded product probe");
            need(error.empty(), error);
            auto *rounded_words = static_cast<const std::uint32_t *>(rounded_output.contents);
            for (std::size_t i = 0; i < rounded.size(); ++i) {
                const auto c = rounded[i];
                const auto *w = rounded_words + i * 4;
                const float a = test::product_guard::value(c[0]);
                const float b = test::product_guard::value(c[1]);
                const float primary = static_cast<float>(double(a) * double(b));
                const auto normal = [](float x) { return x == 0 || std::isnormal(x); };
                const bool supported = normal(a) && normal(b) && normal(primary);
                need(w[2] == test::product_guard::rounded_zero_oracle(c[0], c[1]),
                     "Native rounded-zero predicate differs from exact double oracle");
                need(bool(w[1]) == supported, "Rounded scalar supported state");
                need(w[3] == 0xc001cafe, "Rounded scalar completion guard");
                if (supported)
                    need(w[0] == test::product_guard::bits(primary),
                         "Rounded scalar result, including signed zero");
            }
            for (std::size_t i = rounded.size() * 16; i < rounded_output.length; ++i)
                need(static_cast<const unsigned char *>(rounded_output.contents)[i] == 0xa5,
                     "Rounded scalar padded output");
            const auto symmetry = test::product_guard::symmetry_cases();
            const auto symmetry_count = static_cast<std::uint32_t>(symmetry.size());
            auto symmetry_pipeline = metal::load_pipeline(
                device, library, @"rypath_triangle_pair_symmetry_probe", error);
            need(symmetry_pipeline != nil, error);
            auto symmetry_input = [device newBufferWithBytes:symmetry.data()
                                                      length:symmetry.size() * sizeof(symmetry[0])
                                                     options:MTLResourceStorageModeShared];
            auto symmetry_output = [device newBufferWithLength:(symmetry.size() + 64) * 64
                                                       options:MTLResourceStorageModeShared];
            need(symmetry_input && symmetry_output, "Symmetry buffers");
            std::memset(symmetry_output.contents, 0xa5, symmetry_output.length);
            command = [queue commandBuffer];
            encoder = [command computeCommandEncoder];
            need(command && encoder, "Symmetry command");
            [encoder setComputePipelineState:symmetry_pipeline];
            [encoder setBuffer:symmetry_input offset:0 atIndex:0];
            [encoder setBuffer:symmetry_output offset:0 atIndex:1];
            [encoder setBytes:&symmetry_count length:sizeof(symmetry_count) atIndex:2];
            [encoder dispatchThreads:MTLSizeMake(symmetry_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [encoder endEncoding];
            error = metal::complete(command, "Triangle pair symmetry probe");
            need(error.empty(), error);
            auto *sw = static_cast<const std::uint32_t *>(symmetry_output.contents);
            for (std::size_t i = 0; i < symmetry.size(); ++i) {
                const auto *w = sw + i * 16;
                need(bool(w[6]) == (bool(w[0]) || bool(w[3])),
                     "Canonical multiplication preserves ordered support union");
                need(w[6] == w[9] && w[7] == w[10] && w[8] == w[11],
                     "Canonical multiplication bitwise commutative");
                need(w[6] == w[12] && w[15] == 0,
                     "Sign covariance validity and parent invalid preserved");
                if (w[6]) {
                    need(test::product_guard::value(w[13]) == -test::product_guard::value(w[7]) &&
                             test::product_guard::value(w[14]) == -test::product_guard::value(w[8]),
                         "Whole-operand sign covariance");
                }
                if (i == 8)
                    need(w[6] && w[7] == 1068501259u && w[8] == 3008619499u,
                         "Independent known canonical pair result");
            }
            for (std::size_t i = symmetry.size() * 64; i < symmetry_output.length; ++i)
                need(static_cast<const unsigned char *>(symmetry_output.contents)[i] == 0xa5,
                     "Symmetry padded output");
            const auto &extended_vectors = test::triangle_extended_vectors::vectors;
            std::vector<std::array<std::uint32_t, 8>> extended_inputs;
            for (auto c : extended_vectors)
                extended_inputs.push_back({std::uint32_t(c.a.mantissa), std::uint32_t(c.a.exponent),
                                           c.a.negative, c.a.valid, std::uint32_t(c.b.mantissa),
                                           std::uint32_t(c.b.exponent), c.b.negative, c.b.valid});
            const auto extended_count = static_cast<std::uint32_t>(extended_inputs.size());
            auto extended_pipeline =
                metal::load_pipeline(device, library, @"rypath_triangle_extended_probe", error);
            need(extended_pipeline != nil, error);
            auto extended_input =
                [device newBufferWithBytes:extended_inputs.data()
                                    length:extended_inputs.size() * sizeof(extended_inputs[0])
                                   options:MTLResourceStorageModeShared];
            auto extended_output = [device newBufferWithLength:(extended_inputs.size() + 64) * 64
                                                       options:MTLResourceStorageModeShared];
            need(extended_input && extended_output, "Extended differential buffers");
            std::memset(extended_output.contents, 0xa5, extended_output.length);
            command = [queue commandBuffer];
            encoder = [command computeCommandEncoder];
            need(command && encoder, "Extended differential command");
            [encoder setComputePipelineState:extended_pipeline];
            [encoder setBuffer:extended_input offset:0 atIndex:0];
            [encoder setBuffer:extended_output offset:0 atIndex:1];
            [encoder setBytes:&extended_count length:sizeof(extended_count) atIndex:2];
            [encoder dispatchThreads:MTLSizeMake(extended_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [encoder endEncoding];
            error = metal::complete(command, "Triangle extended differential probe");
            need(error.empty(), error);
            auto *ew = static_cast<const std::uint32_t *>(extended_output.contents);
            for (std::size_t i = 0; i < extended_inputs.size(); ++i)
                for (unsigned op = 0; op < 4; ++op) {
                    auto expected = extended_vectors[i].expected[op];
                    const auto *w = ew + i * 16 + op * 4;
                    need(w[0] == expected.mantissa &&
                             std::bit_cast<std::int32_t>(w[1]) == expected.exponent &&
                             w[2] == expected.negative && w[3] == expected.valid,
                         "Native extended RN matches independent Fraction vector");
                }
            for (std::size_t i = extended_inputs.size() * 64; i < extended_output.length; ++i)
                need(static_cast<const unsigned char *>(extended_output.contents)[i] == 0xa5,
                     "Extended padded output");
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
            // Actual contracted primary ray and primitive from the point-light
            // regression capture. Exact rational geometry gives t=1, u near 1/2,
            // v=47572203/10384593717069655257060992658440192, strictly inside.
            const std::uint32_t axial_vertices[3][3] = {{3238002688u, 3238002688u, 1065353216u},
                                                        {1090519040u, 1090519040u, 1065353216u},
                                                        {1090519040u, 3238002688u, 1065353216u}};
            const std::uint32_t axial_ray[8] = {0u,         0u,          0u, 368197336u,
                                                351475317u, 1065353216u, 0u, 0x7f800000u};
            std::memcpy(tb.contents, axial_vertices, sizeof(axial_vertices));
            std::memcpy(rb.contents, axial_ray, sizeof(axial_ray));
            std::memset(result.contents, 0xa5, 32);
            command = [queue commandBuffer];
            encoder = [command computeCommandEncoder];
            need(command && encoder, "Axial witness command");
            [encoder setComputePipelineState:witness];
            [encoder setBuffer:tb offset:0 atIndex:0];
            [encoder setBuffer:rb offset:0 atIndex:1];
            [encoder setBuffer:result offset:0 atIndex:2];
            [encoder dispatchThreads:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [encoder endEncoding];
            error = metal::complete(command, "Triangle axial witness probe");
            need(error.empty(), error);
            const auto *axial = static_cast<const std::uint32_t *>(result.contents);
            need(axial[0] == 1 && axial[1] == 1, "Captured axial triangle remains valid and hits");
            need(axial[2] == 0x3f800000u && std::isfinite(test::product_guard::value(axial[3])),
                 "Captured axial triangle t=1");
            need(axial[4] == 0x3f000000u && axial[5] == 330660155u,
                 "Captured axial triangle independent rational barycentrics");
            need(axial[6] == 0xc001cafe && axial[7] == 0xa5a5a5a5,
                 "Axial witness completion and guard");
            const std::uint32_t area_vertices[3][3] = {{3253731328u, 3204448256u, 3245342720u},
                                                       {1106247680u, 3204448256u, 1106247680u},
                                                       {1106247680u, 3204448256u, 3245342720u}};
            const std::uint32_t area_ray[8] = {1085276160u, 1090519040u, 3238002688u, 2360150648u,
                                               3205440214u, 1062485707u, 0u,          0x7f800000u};
            std::memcpy(tb.contents, area_vertices, sizeof(area_vertices));
            std::memcpy(rb.contents, area_ray, sizeof(area_ray));
            std::memset(result.contents, 0xa5, 32);
            command = [queue commandBuffer];
            encoder = [command computeCommandEncoder];
            need(command && encoder, "Area witness command");
            [encoder setComputePipelineState:witness];
            [encoder setBuffer:tb offset:0 atIndex:0];
            [encoder setBuffer:rb offset:0 atIndex:1];
            [encoder setBuffer:result offset:0 atIndex:2];
            [encoder dispatchThreads:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [encoder endEncoding];
            error = metal::complete(command, "Captured Area fallback witness");
            need(error.empty(), error);
            const auto *area = static_cast<const std::uint32_t *>(result.contents);
            need(area[0] == 1 && area[1] == 1 && area[6] == 0xc001cafe && area[7] == 0xa5a5a5a5,
                 "Captured Area fallback valid hit and guards");
            need(std::abs(double(test::product_guard::value(area[2])) +
                          test::product_guard::value(area[3]) - 4194304. / 275899.) < 1e-10,
                 "Captured Area independent exact rational t");
            need(std::abs(double(test::product_guard::value(area[4])) - 21634879. / 49661820.) <
                         3e-8 &&
                     std::abs(double(test::product_guard::value(area[5])) - 0.1560225642153268) <
                         3e-8,
                 "Captured Area independent rational barycentrics");
            // Direct helper endpoint checks make no native offering assumption.
            for (unsigned fixture = 0; fixture < 6; ++fixture) {
                std::array<std::uint32_t, 8> input_bits{};
                std::copy(std::begin(axial_ray), std::end(axial_ray), input_bits.begin());
                std::uint32_t corners[3][3];
                std::memcpy(corners, axial_vertices, sizeof(corners));
                bool expected_valid = false, expected_hit = false;
                if (fixture == 0) {
                    input_bits[6] = 0x3f800000u;
                    input_bits[7] = 0x3f800000u;
                    expected_valid = true;
                    expected_hit = true;
                }
                if (fixture == 1)
                    input_bits[7] = 1u;
                if (fixture == 2)
                    input_bits[3] = 1u;
                if (fixture == 3)
                    corners[0][0] = 1u;
                if (fixture == 4 || fixture == 5) {
                    for (auto &corner : corners)
                        corner[2] = fixture == 4 ? 0x41000000u : 0x00800000u;
                    input_bits[3] = 0;
                    input_bits[4] = 0;
                    input_bits[5] = fixture == 4 ? 0x00800000u : 0x41000000u;
                }
                std::memcpy(tb.contents, corners, sizeof(corners));
                std::memcpy(rb.contents, input_bits.data(), sizeof(ray));
                std::memset(result.contents, 0xa5, 32);
                command = [queue commandBuffer];
                encoder = [command computeCommandEncoder];
                need(command && encoder, "Range fixture command");
                [encoder setComputePipelineState:witness];
                [encoder setBuffer:tb offset:0 atIndex:0];
                [encoder setBuffer:rb offset:0 atIndex:1];
                [encoder setBuffer:result offset:0 atIndex:2];
                [encoder dispatchThreads:MTLSizeMake(1, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [encoder endEncoding];
                error = metal::complete(command, "Triangle source/output range fixture");
                need(error.empty(), error);
                const auto *w = static_cast<const std::uint32_t *>(result.contents);
                need(bool(w[0]) == expected_valid && bool(w[1]) == expected_hit,
                     "Source policy and public t range remain visible");
                need(w[6] == 0xc001cafe && w[7] == 0xa5a5a5a5, "Range fixture guards");
            }
            std::cout
                << symmetry.size() << " native symmetry cases; " << extended_inputs.size()
                << " native Fraction RN vectors; captured Area and six range fixtures checked\n";
            std::cout << rounded.size()
                      << " rounded scalar cases and captured axial triangle checked\n";
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
