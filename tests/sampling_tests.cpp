// SPDX-License-Identifier: GPL-3.0-only

#include "render.hpp"
#include "sampling.hpp"
#include "sobol_shared.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using namespace rypath;
using sobol::Index;
using sobol::Word;

bool check(bool pass, std::string_view description) {
    if (!pass) {
        std::cerr << description << '\n';
    }
    return pass;
}

bool known_answers() {
    struct KnownAnswer {
        Word m, x, y;
        Index sample;
        Word dimension;
        std::int32_t seed;
        Index index;
        Word hash;
        std::array<Word, 4> words;
        std::array<float, 2> camera;
    };
    // Generated with PBRT v4 at b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f in Float mode.
    // SobolSample's randomizer callback recorded its integer word. GetPixel2D supplied the
    // camera values except for the m=0 sample above INT_MAX, where remapping is the identity.
    constexpr std::array answers{
        KnownAnswer{0,
                    0,
                    0,
                    0,
                    3,
                    0,
                    0,
                    0x6e97a272u,
                    {0x00000000u, 0x6e97a272u, 0x410c32c4u, 0x4d50aaacu},
                    {0, 0}},
        KnownAnswer{0,
                    0,
                    0,
                    4503599627370495ull,
                    1023,
                    std::numeric_limits<std::int32_t>::min(),
                    4503599627370495ull,
                    0xae830afdu,
                    {0x0c76950bu, 0xa2f59ff6u, 0x95cfa4e2u, 0xd2c575a4u},
                    {0x1.fffffep-1f, 0x1.0011p-4f}},
        KnownAnswer{1,
                    1,
                    0,
                    15,
                    11,
                    -1,
                    63,
                    0x524afd16u,
                    {0x24000000u, 0x764afd16u, 0x5851b677u, 0x7fa0ffcdu},
                    {0x1.fp-1f, 0x1.5p-1f}},
        KnownAnswer{3,
                    4,
                    1,
                    9,
                    46,
                    17,
                    609,
                    0x60d98904u,
                    {0xcd400000u, 0xad998904u, 0xcae056c4u, 0xc1be889eu},
                    {0x1.9p-3f, 0x1.bp-3f}},
        KnownAnswer{6,
                    35,
                    0,
                    111,
                    16,
                    -19,
                    456689,
                    0x9a34955cu,
                    {0x40d1a000u, 0xdae5355cu, 0x7dd112e0u, 0x0ecc6e63u},
                    {0x1.f7bp-1f, 0x1.e2cp-3f}},
        KnownAnswer{12,
                    2244,
                    33,
                    65535,
                    7,
                    889,
                    1099506831921ull,
                    0x70c9554bu,
                    {0x6e2aedbdu, 0x1ee3b8f6u, 0xf3fb074au, 0xfaae0197u},
                    {0x1.6dcp-1f, 0x1.9b4p-1f}},
        KnownAnswer{19,
                    254427,
                    4,
                    16383,
                    984,
                    std::numeric_limits<std::int32_t>::max(),
                    4503458496306238ull,
                    0xd776bc96u,
                    {0xabaa0426u, 0x7cdcb8b0u, 0xb09e7b61u, 0xc1efcb3fu},
                    {0x1.8p-1f, 0x1.58cp-1f}},
        KnownAnswer{25,
                    33554431,
                    33554430,
                    3,
                    1021,
                    -79,
                    3956104878882815ull,
                    0x21aec8acu,
                    {0x5c681095u, 0x7dc6d839u, 0x5a6fc5d2u, 0x300114fdu},
                    {0, 0}},
    };
    bool pass = true;
    for (const auto &answer : answers) {
        const auto &tables = sobol_tables();
        const Index actual = sobol::index(tables, answer.m, answer.x, answer.y, answer.sample);
        const auto seed_bits = static_cast<Word>(answer.seed);
        pass &= check(actual == answer.index, "PBRT Sobol interval index");
        pass &= check(sobol::dimension_hash(answer.dimension, seed_bits) == answer.hash,
                      "PBRT signed-seed dimension hash");
        pass &= check(sobol_signed_seed(static_cast<Index>(answer.seed)) == answer.seed,
                      "Canonical signed seed decodes exactly");
        for (Word mode = 0; mode < answer.words.size(); ++mode) {
            pass &= check(sobol::word(tables, actual, answer.dimension, mode, seed_bits) ==
                              answer.words[mode],
                          "PBRT Sobol word for randomization mode");
        }
        const std::array pixel{answer.x, answer.y};
        for (Word dim = 0; dim < pixel.size(); ++dim) {
            const Word global =
                sobol::word(tables, actual, dim, sobol::no_randomization, seed_bits);
            pass &= check(sobol::camera_uniform(global, answer.m, pixel[dim]) == answer.camera[dim],
                          "PBRT binary32 pixel remap");
        }
    }
    pass &= check(sobol::uniform(0xffffffffu) == 0x1.fffffep-1f && sobol::uniform(0) == 0.0f &&
                      sobol::uniform(0x80000081u) == 0x1.000002p-1f,
                  "PBRT binary32 conversion rounds and clamps");
    pass &= check(sobol::camera_uniform(0x7fffffffu, 1, 0) == 0x1.fffffep-1f,
                  "PBRT pixel remap clamps a rounded upper endpoint");
    return pass;
}

bool interval_properties() {
    bool pass = true;
    const auto &tables = sobol_tables();
    for (Word m = 1; m <= sobol::max_scale_exponent; ++m) {
        const Word scale = Word{1} << m;
        const Index last = (Index{1} << (sobol::matrix_size - 2 * m)) - 1;
        for (Word x : {0u, scale / 2, scale - 1}) {
            for (Word y : {0u, scale / 2, scale - 1}) {
                for (Index frame : {Index{0}, last / 2, last}) {
                    const Index i = sobol::index(tables, m, x, y, frame);
                    const auto x_word = sobol::word(tables, i, 0, sobol::no_randomization, 0);
                    const auto y_word = sobol::word(tables, i, 1, sobol::no_randomization, 0);
                    pass &= check(i >> (2 * m) == frame, "Interval index preserves absolute frame");
                    pass &= check(x_word >> (32 - m) == x && y_word >> (32 - m) == y,
                                  "Integer Sobol camera coordinates lie in the requested pixel");
                }
            }
        }
    }

    // A power-of-two prefix visits each leading-bit bin exactly once in one dimension,
    // including after all four randomizations. Count word bins before Float rounding.
    constexpr Word bin_bits = 8;
    for (Word mode = sobol::no_randomization; mode <= sobol::owen; ++mode) {
        for (Word dimension : {2u, 163u, 1023u}) {
            std::array<unsigned, 1u << bin_bits> bins{};
            for (Index i = 0; i < bins.size(); ++i) {
                const Word sample = sobol::word(tables, i, dimension, mode, 0xffffffffu);
                ++bins[sample >> (32 - bin_bits)];
            }
            for (const unsigned count : bins) {
                pass &= check(count == 1, "Power-of-two Sobol prefix visits every bin");
            }
        }
    }
    return pass;
}

bool validation() {
    const RenderSampler sampler{SamplerKind::sobol, SobolRandomization::fastowen};
    const auto accepts = [&](Index seed, Word width, Word height, Index begin, Index count) {
        return validate_sampler_range(sampler, seed, width, height, begin, count).empty();
    };
    constexpr Index max_index = std::numeric_limits<Index>::max();
    constexpr Index m0_limit = Index{1} << 52;
    constexpr Index m3_limit = Index{1} << 46;
    constexpr Index min_seed = static_cast<Index>(std::numeric_limits<std::int32_t>::min());
    constexpr Word max_extent = Word{1} << 25;
    bool pass = true;
    pass &= check(sobol_scale_exponent(1, 1) == 0 && sobol_scale_exponent(5, 2) == 3 &&
                      sobol_scale_exponent(2, 5) == 3 && sobol_scale_exponent(8, 2) == 3 &&
                      sobol_scale_exponent(9, 2) == 4 && sobol_scale_exponent(max_extent, 1) == 25,
                  "Sobol scale rounds up the longer film extent");
    pass &= check(accepts(0, 1, 1, 0, m0_limit) && !accepts(0, 1, 1, 0, m0_limit + 1) &&
                      accepts(0, 1, 1, m0_limit - 1, 1) && !accepts(0, 1, 1, m0_limit, 1),
                  "Scale-one index bound");
    pass &= check(accepts(0, 5, 2, m3_limit - 2, 2) && !accepts(0, 2, 5, m3_limit - 2, 3),
                  "Rectangle uses full film extent and absolute range");
    pass &= check(accepts(0, max_extent, 1, 0, 4) && !accepts(0, max_extent, 1, 0, 5) &&
                      !accepts(0, max_extent + 1, 1, 0, 1) && !accepts(0, 0, 1, 0, 1),
                  "Maximum Sobol scale and its four samples");
    pass &= check(!accepts(0, 1, 1, max_index, 2) && !accepts(0, 1, 1, 0, 0),
                  "Reject overflowing and empty absolute ranges");
    pass &= check(accepts(2147483647u, 1, 1, 0, 1) && accepts(min_seed, 1, 1, 0, 1) &&
                      accepts(max_index, 1, 1, 0, 1) && !accepts(min_seed - 1, 1, 1, 0, 1) &&
                      !accepts(2147483648u, 1, 1, 0, 1) && !accepts(0xffffffffu, 1, 1, 0, 1) &&
                      !accepts(Index{1} << 32, 1, 1, 0, 1),
                  "Only canonical signed 32-bit seeds are accepted");
    pass &= check(validate_sampler_range({}, max_index, 1, 1, max_index, 1).empty(),
                  "Independent sampler accepts the full 64-bit seed and index");
    // Fixed underlying types permit these values so public API validation can reject them.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto invalid_kind = static_cast<SamplerKind>(2);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto invalid_mode = static_cast<SobolRandomization>(4);
    pass &= check(
        !validate_sampler_range({invalid_kind, {}}, 0, 1, 1, 0, 1).empty() &&
            !validate_sampler_range({SamplerKind::sobol, invalid_mode}, 0, 1, 1, 0, 1).empty(),
        "Reject unknown sampler and Sobol randomization values");

    RenderSettings settings;
    settings.sampler = sampler;
    settings.seed = 0xffffffffu;
    pass &=
        check(!validate_render_settings(settings).empty(), "Render validation uses Sobol bounds");
    settings.seed = max_index;
    pass &= check(validate_render_settings(settings).empty(), "Render accepts a signed Sobol seed");
    return pass;
}

} // namespace

int main() {
    const bool known = known_answers();
    const bool interval = interval_properties();
    const bool valid = validation();
    return known && interval && valid ? 0 : 1;
}
