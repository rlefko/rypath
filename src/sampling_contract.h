// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#if !defined(__METAL_VERSION__)
#include <cstdint>
#endif

namespace rypath {
namespace sampling_contract {
#if defined(__METAL_VERSION__)
using Word = uint;
#else
using Word = std::uint32_t;
#endif
// Keep all eight v1 transport dimensions unchanged. The dielectric extension
// reserves a disjoint tail, including for smooth and index-matched interfaces.
enum : Word { dielectric_max_depth = 113, dielectric_lobe_begin = 908 };
inline constexpr Word dielectric_lobe_dimension(Word bounce) {
    return dielectric_lobe_begin + bounce;
}
inline constexpr Word dielectric_lobe_block(Word bounce) { return 0x80000000u | bounce; }
// Lens draws occupy the unused Sobol tail and a separate Philox counter block.
// Neither extension advances or renumbers the existing transport dimensions.
enum : Word { lens_x_dimension = 1022, lens_y_dimension = 1023, lens_block = 0xc0000000u };
static_assert(dielectric_lobe_dimension(dielectric_max_depth - 1) < lens_x_dimension,
              "Disjoint lens dimensions");
// Halton's 1000-prime table reserves its own non-overlapping extension tail.
enum : Word {
    halton_dielectric_max_depth = 110,
    halton_dielectric_lobe_begin = 884,
    halton_lens_x_dimension = 998,
    halton_lens_y_dimension = 999
};
static_assert(4 + 8 * halton_dielectric_max_depth == halton_dielectric_lobe_begin,
              "Disjoint Halton dielectric dimensions");
static_assert(halton_dielectric_lobe_begin + halton_dielectric_max_depth <= halton_lens_x_dimension,
              "Disjoint Halton lens dimensions");
// Material-local v1 streams reserve a separate Philox counter domain. The
// pixel and absolute sample index remain in the other counter words; the
// stable selected material ID is mixed into the key, never a host address.
enum : Word { material_stream_v1 = 0x6d617431u };
inline constexpr Word material_local_block(Word bounce) { return 0x40000000u | bounce; }
// Media use a checked monotonic draw-block index, including depth-neutral
// boundary segments. Domain separation preserves all existing surface streams.
enum : Word { medium_stream_v1 = 0x6d656431u, medium_block_limit = 0x20000000u };
inline constexpr Word medium_local_block(Word draw) { return 0x20000000u | draw; }
static_assert(4 + 8 * dielectric_max_depth == dielectric_lobe_begin,
              "Disjoint dielectric dimensions");
static_assert(dielectric_lobe_dimension(dielectric_max_depth - 1) < 1024, "Dielectric table bound");
} // namespace sampling_contract
} // namespace rypath
