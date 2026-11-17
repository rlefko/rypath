// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"

namespace rypath {
namespace packet_wire {
using Word = render_wire::Word;
// Opaque per-vertex storage is interpreted only by the Metal implementation.
// A compile-time device assertion checks the capacity and alignment.
enum : Word { vertex_bytes = 2048 };
enum : Word { workspace_bytes = 256u * 1024u * 1024u };
enum : Word { max_depth = 64 };
struct Result {
    Word status, pixel, sample_low, sample_high, splat_count;
    float wavelengths[4], wavelength_pdfs[4], radiance[4];
    float filter_weight;
    Word reserved[2];
};
struct Splat {
    float raster[2], radiance[4];
    Word strategy, reserved;
};
inline constexpr Word vertex_offset(Word depth) { return sizeof(Result) + depth * sizeof(Splat); }
inline constexpr Word lane_bytes(Word depth) {
    return vertex_offset(depth) + (2 * depth + 3) * vertex_bytes;
}
static_assert(sizeof(Result) == 80 && sizeof(Splat) == 32, "Packet output ABI");
static_assert(vertex_offset(max_depth) % 16 == 0 && vertex_bytes % 16 == 0,
              "Packet vertex alignment");
} // namespace packet_wire
} // namespace rypath
