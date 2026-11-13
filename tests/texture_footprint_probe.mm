// SPDX-License-Identifier: GPL-3.0-only

#include "metal_execution.hpp"
#include "render_wire.h"
#include "rgb_spectrum.hpp"
#include "texture_probe_wire.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

using namespace rypath;
namespace wire = render_wire;
namespace probe = texture_probe;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void compare(probe::Vector actual, probe::Vector expected, std::string_view name,
             std::uint32_t budget) {
    constexpr float tolerance = 2e-6f;
    require(std::abs(actual.x - expected.x) < tolerance &&
                std::abs(actual.y - expected.y) < tolerance &&
                std::abs(actual.z - expected.z) < tolerance &&
                std::abs(actual.w - expected.w) < tolerance,
            std::string{name} + " mismatch at budget " + std::to_string(budget) + " (got " +
                std::to_string(actual.x) + ", " + std::to_string(actual.y) + ", " +
                std::to_string(actual.z) + ", " + std::to_string(actual.w) + ")");
}

void check(const probe::Result &r, std::uint32_t budget) {
    const auto scale = static_cast<float>(std::max(0.125, 1 / std::sqrt(budget)));
    // Intersect a lerp of +Z and normalized (+X+Z) with z=2. The ratio
    // uses the scaled direction's z and thus distinguishes from lerping hits.
    const float primary = static_cast<float>(2 * scale / ((1 - scale) * std::sqrt(2.) + scale));
    compare(r.primary[0], {primary, 0, 0, 1}, "Primary X differential", budget);
    compare(r.primary[1], {0, primary, 0, scale}, "Primary Y differential and scale", budget);
    compare(r.parallel, {}, "Parallel primary differential falls back", budget);

    // At x=2, the rotated differential rays hit the plane two units along
    // their forward axis. Undoing the rotation cycles x->y, y->z, z->x.
    compare(r.rotated[0], {0, 0.4f * scale, 0.8f * scale, 0}, "Rotated secondary X", budget);
    compare(r.rotated[1], {0, 0, 0.25f * scale, 0}, "Rotated secondary Y", budget);
    // The sheared rays in world space have directions (.25,0,1) and
    // (.125,.25,1). Their intersections with x+z=2 are at t=1.6 and t=16/9.
    compare(r.sheared[0], {0.4f * scale, 0, -0.4f * scale, 0}, "Sheared secondary X", budget);
    compare(r.sheared[1], {2.f / 9 * scale, 4.f / 9 * scale, -2.f / 9 * scale, 0},
            "Sheared secondary Y", budget);
    compare(r.grazing[0], {}, "Undefined secondary X is zero", budget);
    compare(r.grazing[1], {0, 0, -2 * scale, 0}, "Defined secondary Y is preserved", budget);

    compare(r.patch[0], {1, 0.25f, 0, 1}, "Skewed patch UV", budget);
    compare(r.patch[1], {1, 0.5f, 0, 0}, "Skewed patch ds tangent", budget);
    compare(r.patch[2], {0, 1, 0, 0}, "Skewed patch dt tangent", budget);
    compare(r.patch_derivatives, {2, 2, -1, -3.5f}, "Skewed patch derivative solve", budget);
    compare(r.mirrored[0], {0, 1, 0, 1}, "Mirrored patch ds tangent", budget);
    compare(r.mirrored[1], {-1, 0, 0, 0}, "Mirrored patch orientation correction", budget);
    compare(r.degenerate[0], {1, 0, 0, 1}, "Degenerate patch raw u fallback", budget);
    compare(r.degenerate[1], {0, 1, 0, 0}, "Degenerate patch raw v fallback", budget);
    compare(r.sanitized, {0, 0, 1e8f, -1e8f}, "Derivative nonfinite and clamp handling", budget);

    const probe::Vector fine{0.25f, 0.25f, 0.25f, 0};
    compare(r.fine, fine, "Base-level bilinear after vertical flip", budget);
    compare(r.middle, {0.25f, 0.5f, 0.75f, 0}, "Single intermediate mip level", budget);
    compare(r.coarse, {1, 0, 0, 0}, "Coarsest texel outside black wrap", budget);
    compare(r.wrapped, fine, "Negative repeat coordinate", budget);
    compare(r.black, {}, "Black border at base level", budget);

    const auto spectral = reconstruct_srgb({0.25f, 0.5f, 0.75f});
    require(spectral.has_value(), "CPU spectral reconstruction for probe failed");
    compare(r.spectrum,
            {evaluate_srgb(*spectral, 460), evaluate_srgb(*spectral, 550),
             evaluate_srgb(*spectral, 610), 0.25f},
            "Chroma and grayscale spectral reconstruction", budget);
}

void run_probe(id<MTLDevice> device) {
    std::string diagnostic;
    const auto library = metal::load_library(device, {}, diagnostic);
    require(library != nil, diagnostic);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_texture_footprint_probe", diagnostic);
    require(pipeline != nil, diagnostic);

    constexpr std::array<std::uint32_t, 4> budgets{1, 4, 64, 256};
    constexpr std::array<wire::ImageTexture, 2> textures{{
        {0, 3, 0, 0, 1, 1, 0, 0, 1, 0},
        {0, 3, 2, 0, 1, 1, 0, 0, 1, 0},
    }};
    constexpr std::array<wire::TextureLevel, 3> levels{{{4, 4, 0}, {2, 2, 16}, {1, 1, 20}}};
    std::array<wire::Word, 21> texels{};
    texels[0] = 0x00000004u;
    texels[1] = 0x00000400u;
    texels[4] = 0x00040000u;
    texels[16] = 0x00030201u;
    texels[20] = 0x00000004u;
    // Exact synthetic transfer values isolate filtering arithmetic from transfer encoding.
    std::array<float, 256> transfer{};
    for (std::size_t i = 0; i <= 4; ++i) {
        transfer[i] = 0.25f * static_cast<float>(i);
    }
    wire::TextureParameters parameters{};
    parameters.texture_count = static_cast<wire::Word>(textures.size());
    parameters.level_count = static_cast<wire::Word>(levels.size());
    parameters.texel_count = static_cast<wire::Word>(texels.size());

    const auto table = srgb_reconstruction_table();
    const auto table_buffer = [device newBufferWithBytes:table.data()
                                                  length:table.size_bytes()
                                                 options:MTLResourceStorageModeShared];
    constexpr std::size_t result_bytes = budgets.size() * sizeof(probe::Result);
    const auto output = [device newBufferWithLength:result_bytes
                                            options:MTLResourceStorageModeShared];
    require(table_buffer != nil && output != nil, "Texture probe buffer allocation failed");

    const auto queue = [device newCommandQueue];
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(queue != nil && command != nil && encoder != nil, "Texture probe command failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBytes:budgets.data() length:sizeof(budgets) atIndex:0];
    [encoder setBytes:textures.data() length:sizeof(textures) atIndex:1];
    [encoder setBytes:levels.data() length:sizeof(levels) atIndex:2];
    [encoder setBytes:texels.data() length:sizeof(texels) atIndex:3];
    [encoder setBytes:transfer.data() length:sizeof(transfer) atIndex:4];
    [encoder setBuffer:table_buffer offset:0 atIndex:5];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:6];
    [encoder setBuffer:output offset:0 atIndex:7];
    [encoder dispatchThreads:MTLSizeMake(budgets.size(), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(budgets.size(), 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Texture footprint probe");
    require(diagnostic.empty(), diagnostic);
    const std::span<const probe::Result> results{
        static_cast<const probe::Result *>(output.contents), budgets.size()};
    for (std::size_t i = 0; i < budgets.size(); ++i) {
        check(results[i], budgets[i]);
    }
}

} // namespace

int main() {
    @autoreleasepool {
        try {
            const id<MTLDevice> device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP Texture GPU probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Texture GPU probe requires a Metal device");
            run_probe(device);
        } catch (const std::exception &error) {
            std::cerr << "Texture footprint probe: " << error.what() << '\n';
            return 1;
        }
    }
}
