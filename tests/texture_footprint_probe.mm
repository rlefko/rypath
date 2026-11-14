// SPDX-License-Identifier: GPL-3.0-only

#include "ewa_reference.hpp"
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
#include <vector>

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
    {{15, 0.25f, 0.75f, 0.5f}, bad, "Point descriptor has invalid texel format"},
    {{16, 0.25f, 0.5f, 0.25f}, red, "Exact higher mip transition"},
    {{16, 0.25f, 0.5f, 0x1.fffffap-3f}, red, "LOD rounds log term before add"},
}};

struct EwaCheck {
    probe::EwaCase input;
    std::string_view name;
    bool invalid = false;
};

enum EwaTexture : unsigned {
    ewa_repeat = 17,
    ewa_cap_one,
    ewa_cap_four,
    ewa_cap_sixteen,
    ewa_clamp,
    ewa_black,
    ewa_mapped,
    ewa_overflow,
    ewa_cap_low,
    ewa_cap_high,
    ewa_cap_nan,
    ewa_wrap_invalid,
    ewa_format_invalid,
    ewa_upper_invalid,
    ewa_top_invalid,
    ewa_subnormal_scale,
    ewa_subnormal_center,
};

constexpr unsigned half_texture_begin = ewa_subnormal_center + 1;
constexpr unsigned filter_count = wire::ewa_texture_filter + 1;
constexpr unsigned half_invalid_begin = half_texture_begin + 3 * filter_count;
constexpr unsigned half_truncated_begin = half_invalid_begin + filter_count;
constexpr unsigned half_upper_begin = half_truncated_begin + filter_count;
constexpr unsigned half_level_begin = 19;
constexpr unsigned half_upper_level_begin = half_level_begin + 3;
constexpr unsigned half_truncated_level = half_upper_level_begin + 2;

constexpr unsigned half_texture(unsigned filter, unsigned wrap) {
    return half_texture_begin + 3 * filter + wrap;
}

enum class HalfComparison { exact, tolerance, invalid };

struct HalfCheck {
    probe::EwaCase input;
    std::string_view name;
    probe::Vector expected = {};
    HalfComparison comparison = HalfComparison::exact;
};

std::vector<HalfCheck> half_checks() {
    constexpr probe::Vector first{0x1p-24f, 0x1.ff8p-15f, 1.25f, 0};
    constexpr probe::Vector last{0.5f, 0.25f, 0.125f, 0};
    constexpr probe::Vector coarse{0.75f, 0.5f, 0.25f, 0};
    constexpr std::array<probe::Vector, 3> wrapped{last, first, {}};
    constexpr auto invalid = HalfComparison::invalid;
    constexpr auto tolerance = HalfComparison::tolerance;
    std::vector<HalfCheck> checks;
    for (unsigned filter = 0; filter < filter_count; ++filter) {
        for (unsigned wrap = 0; wrap < wrapped.size(); ++wrap) {
            const unsigned texture = half_texture(filter, wrap);
            checks.push_back({{texture, -0.125f, 0.5f, 0, 0, 0, 0}, "Half wrap", wrapped[wrap]});
            checks.push_back({{texture, -2, 0.5f, 32, 0, 0, 1}, "Half wide", coarse});
        }
        checks.push_back({{half_invalid_begin + filter, 0.125f, 0.5f, 0, 0, 0, 0},
                          "Invalid texel format",
                          {},
                          invalid});
        checks.push_back({{half_truncated_begin + filter, 0.5f, 0.5f, 0, 0, 0, 0},
                          "Missing half word",
                          {},
                          invalid});
    }
    constexpr float standard_width = 0.3535533905932738f, ewa_width = 0.7071067811865476f;
    constexpr auto trilinear_clamp = half_texture(wire::trilinear_texture_filter, 1);
    constexpr auto elliptical_clamp = half_texture(wire::ewa_texture_filter, 1);
    constexpr auto truncated_bilinear = half_truncated_begin + wire::bilinear_texture_filter;
    constexpr auto truncated_ewa = half_truncated_begin + wire::ewa_texture_filter;
    constexpr probe::Vector blend{0.5f, 0.375f, 0.25f, 0};
    checks.insert(
        checks.end(),
        {
            {{trilinear_clamp, 0.5f, 0.5f, standard_width, 0, 0, 0},
             "Half trilinear blend",
             blend,
             tolerance},
            {{elliptical_clamp, 0.5f, 0.5f, ewa_width, 0, 0, ewa_width},
             "Half EWA blend",
             blend,
             tolerance},
            {{half_upper_begin, 0.5f, 0.5f, standard_width, 0, 0, 0},
             "Missing trilinear upper word",
             {},
             invalid},
            {{half_upper_begin + 1, 0.5f, 0.5f, ewa_width, 0, 0, ewa_width},
             "Missing EWA upper word",
             {},
             invalid},
            {{truncated_bilinear, -2, 0.5f, 32, 0, 0, 1},
             "Missing standard wide word",
             {},
             invalid},
            {{truncated_ewa, -2, 0.5f, 32, 0, 0, 1}, "Missing EWA wide word", {}, invalid},
            {{truncated_ewa, 0.5f, 0.5f, 1.5f, 0, 0, 1.5f}, "Missing EWA top word", {}, invalid},
        });
    require(checks.size() <= probe::ewa_case_count,
            "Half cases exceed texture lookup probe capacity");
    return checks;
}

constexpr std::array<EwaCheck, probe::ewa_case_count> ewa_checks{{
    {{ewa_repeat, 0.443f, 0.591f, 0.249f, 0.112f, -0.064f, 0.093f}, "Rotated rectangular EWA"},
    {{ewa_cap_one, 0.443f, 0.591f, 0.318f, 0.183f, -0.008f, 0.012f}, "EWA cap one"},
    {{ewa_cap_four, 0.443f, 0.591f, 0.318f, 0.183f, -0.008f, 0.012f}, "EWA cap four"},
    {{ewa_repeat, 0.443f, 0.591f, 0.318f, 0.183f, -0.008f, 0.012f}, "EWA cap eight"},
    {{ewa_cap_sixteen, 0.443f, 0.591f, 0.318f, 0.183f, -0.008f, 0.012f}, "EWA cap sixteen"},
    {{ewa_mapped, 0.223f, 0.553f, 0.128f, 0.202f, 0.033f, -0.085f}, "Signed UV mapping"},
    {{ewa_repeat, 0.443f, 0.591f, 0, 0, 0.249f, 0.112f}, "Zero vector uses base bilerp"},
    {{ewa_repeat, 0.443f, 0.591f, 0.25f, 0.125f, 0.0625f, 0.03125f}, "Nonzero collinear vectors"},
    {{ewa_repeat, 0.443f, 0.591f, 0.5f, 0.25f, 0.125f, 0.0625f}, "Collinear fractional mip"},
    {{ewa_repeat, 0.443f, 0.591f, 0.192f, 0.072f, -0.046f, 0.073f}, "EWA fractional base mip"},
    {{ewa_black, 0.04f, 0.92f, 1.51f, 0.6f, -0.33f, 1.18f}, "Black actual top mip blend"},
    {{ewa_black, -2, 2, 1.51f, 0.6f, -0.33f, 1.18f}, "Black disjoint top blend"},
    {{ewa_black, -2, 2, 8, 0, 0, 2}, "Black direct wide top"},
    {{ewa_black, -0.08f, 1.07f, 0.43f, 0.12f, -0.022f, 0.114f}, "Black partial overlap"},
    {{ewa_clamp, -0.08f, 1.07f, 0.43f, 0.12f, -0.022f, 0.114f}, "Clamp partial overlap"},
    {{ewa_repeat, -0.08f, 1.07f, 0.43f, 0.12f, -0.022f, 0.114f}, "Repeat partial overlap"},
    {{ewa_black, -5, 1.07f, 0.43f, 0.12f, -0.022f, 0.114f}, "Black bounds wholly outside"},
    {{ewa_repeat, -64.125f, 65.375f, 0.43f, 0.12f, -0.022f, 0.114f}, "Signed distant repeat phase"},
    {{ewa_repeat, -huge, huge, 0.43f, 0.12f, -0.022f, 0.114f}, "Huge repeat center"},
    {{ewa_clamp, -huge, huge, 0.43f, 0.12f, -0.022f, 0.114f}, "Huge low clamp center"},
    {{ewa_clamp, huge, -huge, 0.43f, 0.12f, -0.022f, 0.114f}, "Huge high clamp center"},
    {{ewa_black, huge, -huge, 0.43f, 0.12f, -0.022f, 0.114f}, "Huge black center"},
    {{ewa_cap_sixteen, 0.35f, 0.62f, 22.0f, 21.0f, 0.114f, 0.125f}, "Broad diagonal ellipse"},
    {{ewa_repeat, 0.38f, 0.67f, huge, huge, 0.14f, 0.09f}, "Overflow-resistant major norm"},
    {{ewa_repeat, 0.38f, 0.67f, huge, huge, 0, 0}, "Zero minor precedes wide shortcut"},
    {{ewa_overflow, huge, 0.61f, 0.16f, 0.11f, -0.09f, 0.12f}, "Mapped center overflows", true},
    {{ewa_overflow, huge, 0.61f, 16, 0, -0.09f, 0.12f}, "Wide before center mapping"},
    {{ewa_overflow, 0.52f, 0.61f, huge, 0, -0.09f, 0.12f}, "Mapped derivative overflows", true},
    {{ewa_cap_low, 0.52f, 0.61f, 16, 0, -0.09f, 0.12f}, "EWA cap below one", true},
    {{ewa_cap_high, 0.52f, 0.61f, 16, 0, -0.09f, 0.12f}, "EWA cap above sixteen", true},
    {{ewa_cap_nan, 0.52f, 0.61f, 16, 0, -0.09f, 0.12f}, "Nonfinite EWA cap", true},
    {{ewa_wrap_invalid, 0.52f, 0.61f, 16, 0, -0.09f, 0.12f}, "Invalid EWA wrap", true},
    {{ewa_format_invalid, 0.52f, 0.61f, 16, 0, -0.09f, 0.12f}, "Invalid EWA encoding", true},
    {{ewa_upper_invalid, 0.52f, 0.61f, 0.62f, 0.22f, -0.27f, 0.21f}, "Invalid upper EWA mip", true},
    {{ewa_top_invalid, 0.52f, 0.61f, 16, 0, -0.09f, 0.12f}, "Invalid wide EWA top mip", true},
    {{ewa_repeat, 0.443f, 0.591f, -0.064f, 0.093f, 0.249f, 0.112f}, "Swapped rotated vectors"},
    {{ewa_repeat, 0.443f, 0.591f, 0.3f, 0.12f, 1e-39f, -1e-39f}, "Nonzero subnormal minor"},
    {{ewa_subnormal_scale, 0.443f, 0.591f, 1.2f, 0.12f, 1e-38f, 0},
     "UV mapping produces a nonzero subnormal minor"},
    {{ewa_subnormal_center, 3e38f, 0.61f, 0.318f, 0.183f, -0.008f, 0.142f},
     "Subnormal UV scale contributes to mapped center"},
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

void check_ewa(const probe::Result &result, std::span<const probe::Vector> expected,
               std::uint32_t budget) {
    for (std::size_t i = 0; i < ewa_checks.size(); ++i) {
        const auto actual = result.ewa[i];
        const auto &test = ewa_checks[i];
        if (test.invalid) {
            require(std::isnan(actual.x) && std::isnan(actual.y) && std::isnan(actual.z),
                    std::string{test.name} + " must fail at budget " + std::to_string(budget));
        } else {
            compare(actual, expected[i], test.name, budget);
        }
    }
}

wire::ImageTexture ewa_descriptor() {
    return {11, 4, 0, 1, 1, 1, 0, 0, 1, 0, wire::ewa_texture_filter, 8.0f};
}

auto ewa_expectations(std::span<const wire::ImageTexture> textures,
                      std::span<const wire::TextureLevel> levels,
                      std::span<const wire::Word> texels) {
    std::array<probe::Vector, probe::ewa_case_count> expected{};
    for (std::size_t i = 0; i < ewa_checks.size(); ++i) {
        const auto &[test, name, invalid] = ewa_checks[i];
        if (invalid) {
            continue;
        }
        const auto &texture = textures[test.texture];
        ImagePyramid image{.encoding = ImageEncoding::linear,
                           .wrap = static_cast<ImageWrap>(texture.wrap),
                           .levels = {},
                           .texels = {texels.begin(), texels.end()}};
        for (const auto &level : levels.subspan(texture.level_begin, texture.level_count)) {
            image.levels.push_back({level.width, level.height, level.offset});
        }
        const auto st = [](float uv, float scale, float offset) {
#pragma clang fp contract(off)
            const float product = uv * scale;
            return product + offset;
        };
        const float s = st(test.u, texture.uscale, texture.udelta);
        const float t = 1.0f - st(test.v, texture.vscale, texture.vdelta);
        const auto rgb = test::ewa_reference(image, s, t, test.dsdx * texture.uscale,
                                             test.dtdx * texture.vscale, test.dsdy * texture.uscale,
                                             test.dtdy * texture.vscale, texture.max_anisotropy);
        expected[i] = {static_cast<float>(rgb[0]), static_cast<float>(rgb[1]),
                       static_cast<float>(rgb[2]), 0};
    }
    return expected;
}

void run_probe(id<MTLDevice> device) {
    std::string diagnostic;
    const auto library = metal::load_library(device, {}, diagnostic);
    require(library != nil, diagnostic);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_texture_footprint_probe", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto lookup_pipeline =
        metal::load_pipeline(device, library, @"rypath_texture_lookup_probe", diagnostic);
    require(lookup_pipeline != nil, diagnostic);

    constexpr std::array<std::uint32_t, 4> budgets{1, 4, 64, 256};
    std::array<wire::ImageTexture, half_upper_begin + 2> textures{{
        {0, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::bilinear_texture_filter, 8},
        {0, 3, 2, 0, 1, 1, 0, 0, 1, 0, wire::bilinear_texture_filter, 8},
        {0, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::trilinear_texture_filter, 8},
        {0, 3, 2, 0, 1, 1, 0, 0, 1, 0, wire::trilinear_texture_filter, 8},
        {2, 1, 2, 0, 1, 1, 0, 0, 1, 0, wire::trilinear_texture_filter, 8},
        {1, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::trilinear_texture_filter, 8},
        {0, 3, 0, 0, 1, 1, 0, 0, 1, 0, 99, 8},
        {0, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter, 8},
        {0, 3, 1, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter, 8},
        {0, 3, 2, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter, 8},
        {2, 1, 2, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter, 8},
        {0, 3, 2, 0, 2, 1, 0, 0, 1, 0, wire::point_texture_filter, 8},
        {0, 3, 0, 0, 0x1.fffffep-1f, 1, -1, 0, 1, 0, wire::point_texture_filter, 8},
        {2, 3, 0, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter, 8},
        {0, 3, 99, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter, 8},
        {0, 3, 0, 99, 1, 1, 0, 0, 1, 0, wire::point_texture_filter, 8},
        {4, 7, 0, 0, 1, 1, 0, 0, 1, 0, wire::point_texture_filter, 8},
    }};
    for (unsigned index = ewa_repeat; index <= ewa_subnormal_center; ++index) {
        textures[index] = ewa_descriptor();
    }
    textures[ewa_cap_one].max_anisotropy = 1;
    textures[ewa_cap_four].max_anisotropy = 4;
    textures[ewa_cap_sixteen].max_anisotropy = 16;
    textures[ewa_clamp].wrap = 1;
    textures[ewa_black].wrap = 2;
    textures[ewa_mapped].uscale = -1.3f;
    textures[ewa_mapped].vscale = 0.6f;
    textures[ewa_mapped].udelta = 0.79f;
    textures[ewa_mapped].vdelta = 0.24f;
    textures[ewa_overflow].uscale = 2.0f;
    textures[ewa_cap_low].max_anisotropy = 0.99f;
    textures[ewa_cap_high].max_anisotropy = 16.1f;
    textures[ewa_cap_nan].max_anisotropy = nan;
    textures[ewa_wrap_invalid].wrap = 99;
    textures[ewa_format_invalid].texel_format = 99;
    textures[ewa_upper_invalid].level_begin = 15;
    textures[ewa_top_invalid].level_begin = 15;
    textures[ewa_top_invalid].level_count = 3;
    textures[ewa_subnormal_scale].uscale = 0.25f;
    textures[ewa_subnormal_scale].udelta = 0.31f;
    textures[ewa_subnormal_center].uscale = 1e-39f;
    textures[ewa_subnormal_center].udelta = 0.05f;
    for (unsigned filter = 0; filter < filter_count; ++filter) {
        wire::ImageTexture texture{
            half_level_begin, 3, 0, wire::linear_half_texel_format, 1, 1, 0, 0, 1, 0, filter, 8};
        for (unsigned wrap = 0; wrap < 3; ++wrap) {
            texture.wrap = wrap;
            textures[half_texture(filter, wrap)] = texture;
        }
        texture.wrap = 1;
        texture.texel_format = 99;
        textures[half_invalid_begin + filter] = texture;
        texture.texel_format = wire::linear_half_texel_format;
        texture.level_begin = half_truncated_level;
        texture.level_count = 1;
        textures[half_truncated_begin + filter] = texture;
    }
    textures[half_upper_begin] = textures[half_texture(wire::trilinear_texture_filter, 1)];
    textures[half_upper_begin].level_begin = half_upper_level_begin;
    textures[half_upper_begin + 1] = textures[half_texture(wire::ewa_texture_filter, 1)];
    textures[half_upper_begin + 1].level_begin = half_upper_level_begin;
    constexpr std::size_t legacy_words = 191, texel_word_count = legacy_words + 15;
    constexpr std::array<wire::TextureLevel, half_truncated_level + 1> levels{{
        {4, 4, 0},
        {2, 2, 16},
        {1, 1, 20},
        {1, 1, texel_word_count},
        {64, 1, 21},
        {32, 1, 85},
        {16, 1, 117},
        {8, 1, 133},
        {4, 1, 141},
        {2, 1, 145},
        {1, 1, 147},
        {8, 4, 148},
        {4, 2, 180},
        {2, 1, 188},
        {1, 1, 190},
        {8, 4, 148},
        {4, 2, 180},
        {2, 1, texel_word_count},
        {1, 1, 190},
        {4, 1, legacy_words},
        {2, 1, legacy_words + 8},
        {1, 1, legacy_words + 12},
        {4, 1, legacy_words},
        {2, 1, legacy_words + 8},
        {1, 1, texel_word_count - 1},
    }};
    std::array<wire::Word, texel_word_count> texels{};
    texels[0] = 0x00000004u;
    texels[1] = 0x00000400u;
    texels[2] = 0x00040000u;
    texels[3] = 0x00040404u;
    texels[4] = 0x00040000u;
    texels[16] = 0x00030201u;
    texels[20] = 0x00000004u;
    texels[142] = 0x00000400u;
    texels[145] = 0x00000004u;
    for (unsigned i = 148; i < legacy_words; ++i) {
        // Asymmetric color pattern and distinct mip contents expose axis and LOD errors.
        texels[i] = ((i * 67u + 1u) & 255u) | (((i * 107u + 79u) & 255u) << 8u) |
                    (((i * 137u + 23u) & 255u) << 16u);
    }
    constexpr std::array<wire::Word, 15> half_words{
        0x03ff0001u, 0x3d00u, 0,           0,       0, 0, 0x34003800u, 0x3000u, // 4x1 mip
        0x34003400u, 0x3400u, 0x34003400u, 0x3400u,                             // 2x1 mip
        0x38003a00u, 0x3400u, 0x38003a00u, // 1x1 and truncated data
    };
    std::ranges::copy(half_words, texels.begin() + legacy_words);
    // Exact synthetic transfer values isolate filtering arithmetic from transfer encoding.
    std::array<float, 512> transfer{};
    for (std::size_t i = 0; i <= 4; ++i) {
        transfer[i] = 0.25f * static_cast<float>(i);
    }
    for (std::size_t i = 0; i < 256; ++i) {
        transfer[256 + i] = static_cast<float>(i) / 255.0f;
    }
    wire::TextureParameters parameters{};
    parameters.texture_count = static_cast<wire::Word>(textures.size());
    parameters.level_count = static_cast<wire::Word>(levels.size());
    parameters.texel_word_count = static_cast<wire::Word>(texels.size());
    std::array<probe::PointCase, probe::point_case_count> point_cases{};
    std::ranges::transform(point_checks, point_cases.begin(), &PointCheck::input);
    std::array<probe::EwaCase, probe::ewa_case_count> ewa_cases{};
    std::ranges::transform(ewa_checks, ewa_cases.begin(), &EwaCheck::input);
    const auto expected_ewa = ewa_expectations(textures, levels, texels);

    const auto table = srgb_reconstruction_table();
    const auto table_buffer = [device newBufferWithBytes:table.data()
                                                  length:table.size_bytes()
                                                 options:MTLResourceStorageModeShared];
    constexpr std::size_t result_bytes = budgets.size() * sizeof(probe::Result);
    const auto output = [device newBufferWithLength:result_bytes
                                            options:MTLResourceStorageModeShared];
    const auto half_output = [device newBufferWithLength:result_bytes
                                                 options:MTLResourceStorageModeShared];
    require(table_buffer != nil && output != nil && half_output != nil,
            "Texture probe buffer allocation failed");

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
    [encoder setComputePipelineState:lookup_pipeline];
    [encoder setBytes:ewa_cases.data() length:sizeof(ewa_cases) atIndex:9];
    [encoder dispatchThreads:MTLSizeMake(ewa_cases.size(), budgets.size(), 1)
        threadsPerThreadgroup:MTLSizeMake(lookup_pipeline.threadExecutionWidth, 1, 1)];
    const auto checks = half_checks();
    std::vector<probe::EwaCase> half_cases(checks.size());
    std::ranges::transform(checks, half_cases.begin(), &HalfCheck::input);
    [encoder setBytes:half_cases.data()
               length:half_cases.size() * sizeof(probe::EwaCase)
              atIndex:9];
    [encoder setBuffer:half_output offset:0 atIndex:7];
    [encoder dispatchThreads:MTLSizeMake(half_cases.size(), budgets.size(), 1)
        threadsPerThreadgroup:MTLSizeMake(lookup_pipeline.threadExecutionWidth, 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Texture footprint probe");
    require(diagnostic.empty(), diagnostic);
    const std::span<const probe::Result> results{
        static_cast<const probe::Result *>(output.contents), budgets.size()};
    const std::span<const probe::Result> half_results{
        static_cast<const probe::Result *>(half_output.contents), budgets.size()};
    for (std::size_t i = 0; i < budgets.size(); ++i) {
        check(results[i], budgets[i]);
        check_ewa(results[i], expected_ewa, budgets[i]);
        for (std::size_t j = 0; j < checks.size(); ++j) {
            const auto &actual = half_results[i].ewa[j];
            const auto &check = checks[j];
            const auto &expected = check.expected;
            if (check.comparison == HalfComparison::invalid) {
                require(std::isnan(actual.x) && std::isnan(actual.y) && std::isnan(actual.z),
                        std::string{check.name} + " was accepted");
            } else if (check.comparison == HalfComparison::exact) {
                require(actual.x == expected.x && actual.y == expected.y && actual.z == expected.z,
                        std::string{check.name} + " changed a decoded channel");
            } else {
                compare(actual, expected, check.name, budgets[i]);
            }
        }
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
