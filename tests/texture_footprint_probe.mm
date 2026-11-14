// SPDX-License-Identifier: GPL-3.0-only

#include "metal_execution.hpp"
#include "render_wire.h"
#include "rgb_spectrum.hpp"
#include "texture_probe_wire.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
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

struct PointCheck {
    probe::PointCase input;
    probe::Vector expected;
    std::string_view name;
};

constexpr auto huge = std::numeric_limits<float>::max();
constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
constexpr probe::Vector red{1, 0, 0, 0}, green{0, 1, 0, 0}, blue{0, 0, 1, 0}, white{1, 1, 1, 0};
constexpr probe::Vector mid{0.25f, 0.5f, 0.75f, 0}, bad{nan, nan, nan, 0};
const std::array<PointCheck, probe::point_case_count> point_checks{{
    {{7, -0.25f, 0.875f, 0}, blue, "Negative half-texel tie"},
    {{7, 0.25f, 0.875f, 0}, green, "Positive half-texel tie"},
    {{7, std::nextafter(0.25f, 0.0f), 0.875f, 0}, red, "Below positive tie"},
    {{7, std::nextafter(0.25f, 1.0f), 0.875f, 0}, green, "Above positive tie"},
    {{7, 0, 0.875f, 0}, white, "Repeat at zero"},
    {{7, 1, 0.875f, 0}, red, "Repeat at one"},
    {{7, 0x1p-24f, 0.875f, 0}, red, "Repeat inside zero edge"},
    {{7, std::nextafter(1.0f, 0.0f), 0.875f, 0}, white, "Repeat inside one edge"},
    {{7, 0.25f, 0.75f, 0.3535533905932738f}, mid, "Fractional nonzero LOD selects a texel"},
    {{7, 0x1.fffffep28f, 0.875f, 0}, red, "Scaled repeat just below INT_MAX"},
    {{7, 0x1p29f, 0.875f, 0}, red, "Scaled repeat above INT_MAX"},
    {{7, -0x1.000002p29f, 0.875f, 0}, red, "Scaled repeat below INT_MIN"},
    {{7, huge, 0.875f, 0}, red, "Positive repeat scaling overflow"},
    {{7, -huge, 0.875f, 0}, red, "Negative repeat scaling overflow"},
    {{8, -huge, 0.875f, 0}, red, "Clamp at negative extreme"},
    {{8, huge, 0.875f, 0}, white, "Clamp at positive extreme"},
    {{9, 0, 0.875f, 0}, {}, "Black at zero"},
    {{9, 1, 0.875f, 0}, {}, "Black at one"},
    {{9, 0x1p-24f, 0.875f, 0}, red, "Black just inside zero edge"},
    {{9, std::nextafter(1.0f, 0.0f), 0.875f, 0}, white, "Black just inside one edge"},
    {{10, 0.0f, 0.5f, 0}, {}, "Narrow black singleton at edge"},
    {{10, 0.5f, 0.5f, 0}, red, "Narrow black singleton interior"},
    {{10, 0.0f, 0.5f, 0.5f}, red, "Wide black singleton"},
    {{11, huge, 0.875f, 0}, bad, "Narrow overflow in mapped coordinate"},
    {{11, huge, 0.875f, 0.5f}, red, "Wide footprint returns before mapping overflow"},
    {{12, std::nextafter(1.0f, 2.0f), 0.875f, 0}, white, "UV mapping rounds product before add"},
    {{13, 0.25f, 0.75f, 0.3535533905932738f}, bad, "Point selected mip has invalid extent"},
    {{14, 0.25f, 0.75f, 0.5f}, bad, "Point descriptor has invalid wrap"},
    {{15, 0.25f, 0.75f, 0.5f}, bad, "Point descriptor has invalid encoding"},
    {{16, 0.25f, 0.5f, 0.25f}, red, "Exact higher mip transition"},
    {{16, 0.25f, 0.5f, 0x1.fffffap-3f}, red, "LOD rounds log term before add"},
}};

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
    compare(r.trilinear_base, fine, "Trilinear keeps LOD 0.5 at the base level", budget);
    compare(r.trilinear_integer, {0.25f, 0.5f, 0.75f, 0}, "Integer trilinear LOD", budget);
    compare(r.trilinear_middle, {0.625f, 0.25f, 0.375f, 0}, "Halfway adjacent-mip blend", budget);
    // At (.25,.25), black addressing weights the upper 1x1 red texel by .75 squared.
    compare(r.trilinear_black, {0.40625f, 0.25f, 0.375f, 0}, "Black-wrapped upper mip", budget);
    compare(r.trilinear_coarse, {1, 0, 0, 0}, "Trilinear coarsest texel", budget);
    compare(r.singleton_narrow, {0.5f, 0, 0, 0}, "Narrow singleton black border", budget);
    compare(r.singleton_wide, {1, 0, 0, 0}, "Wide singleton coarsest texel", budget);
    for (const auto invalid : {r.invalid_upper, r.invalid_filter}) {
        require(std::isnan(invalid.x) && std::isnan(invalid.y) && std::isnan(invalid.z),
                "Invalid texture filter or upper mip must fail visibly");
    }
    for (std::size_t i = 0; i < point_checks.size(); ++i) {
        const auto &test = point_checks[i];
        auto expected = test.expected;
        auto actual = r.point[i];
        if (i == probe::point_lod_seam_case) {
            // Use the device's logarithm to isolate mip rounding from differences
            // between CPU and GPU implementations of log.
            constexpr float inv_log2 = 1.4426950408889634f;
            require(std::isfinite(actual.w), "Nonfinite device log at point LOD transition");
            const volatile float log_width = actual.w * inv_log2;
            const bool upper = 6.0f + log_width >= 5.0f;
            expected = upper ? red : green;
            if (budget == 1) {
                const bool fused_upper = std::fma(actual.w, inv_log2, 6.0f) >= 5.0f;
                std::cout << "Point LOD transition mip with separate rounding " << (upper ? 5 : 4)
                          << ", with fused rounding " << (fused_upper ? 5 : 4) << '\n';
            }
            actual.w = 0;
        }
        const bool matches =
            std::isnan(expected.x)
                ? std::isnan(actual.x) && std::isnan(actual.y) && std::isnan(actual.z)
                : actual.x == expected.x && actual.y == expected.y && actual.z == expected.z &&
                      actual.w == expected.w;
        require(matches, std::string{test.name} + " mismatch at budget " + std::to_string(budget));
    }

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
    constexpr std::array<wire::ImageTexture, 17> textures{{
        {0, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::bilinear_texture_filter},
        {0, 3, 2, 0, 1, 1, 0, 0, 1, 0, wire::bilinear_texture_filter},
        {0, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::trilinear_texture_filter},
        {0, 3, 2, 0, 1, 1, 0, 0, 1, 0, wire::trilinear_texture_filter},
        {2, 1, 2, 0, 1, 1, 0, 0, 1, 0, wire::trilinear_texture_filter},
        {1, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::trilinear_texture_filter},
        {0, 3, 0, 0, 1, 1, 0, 0, 1, 0, 99},
        {0, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter},
        {0, 3, 1, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter},
        {0, 3, 2, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter},
        {2, 1, 2, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter},
        {0, 3, 2, 0, 2, 1, 0, 0, 1, 0, wire::point_texture_filter},
        {0, 3, 0, 0, 0x1.fffffep-1f, 1, -1, 0, 1, 0, wire::point_texture_filter},
        {2, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter},
        {0, 3, 99, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter},
        {0, 3, 0, 99, 1, 1, 0, 0, 1, 0, wire::point_texture_filter},
        {4, 7, 0, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter},
    }};
    constexpr std::size_t texel_count = 148;
    constexpr std::array<wire::TextureLevel, 11> levels{{
        {4, 4, 0},
        {2, 2, 16},
        {1, 1, 20},
        {1, 1, texel_count},
        {64, 1, 21},
        {32, 1, 85},
        {16, 1, 117},
        {8, 1, 133},
        {4, 1, 141},
        {2, 1, 145},
        {1, 1, 147},
    }};
    std::array<wire::Word, texel_count> texels{};
    texels[0] = 0x00000004u;
    texels[1] = 0x00000400u;
    texels[2] = 0x00040000u;
    texels[3] = 0x00040404u;
    texels[4] = 0x00040000u;
    texels[16] = 0x00030201u;
    texels[20] = 0x00000004u;
    texels[142] = 0x00000400u;
    texels[145] = 0x00000004u;
    // Exact synthetic transfer values isolate filtering arithmetic from transfer encoding.
    std::array<float, 256> transfer{};
    for (std::size_t i = 0; i <= 4; ++i) {
        transfer[i] = 0.25f * static_cast<float>(i);
    }
    wire::TextureParameters parameters{};
    parameters.texture_count = static_cast<wire::Word>(textures.size());
    parameters.level_count = static_cast<wire::Word>(levels.size());
    parameters.texel_count = static_cast<wire::Word>(texels.size());
    std::array<probe::PointCase, probe::point_case_count> point_cases{};
    std::ranges::transform(point_checks, point_cases.begin(), &PointCheck::input);

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
    [encoder setBytes:point_cases.data() length:sizeof(point_cases) atIndex:8];
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
