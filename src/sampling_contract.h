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
static_assert(4 + 8 * dielectric_max_depth == dielectric_lobe_begin,
              "Disjoint dielectric dimensions");
static_assert(dielectric_lobe_dimension(dielectric_max_depth - 1) < 1024, "Dielectric table bound");
} // namespace sampling_contract
} // namespace rypath
