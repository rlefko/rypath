// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifndef __METAL_VERSION__
#include <cstdint>
#endif
namespace hint_probe {
#ifdef __METAL_VERSION__
using Word = uint;
#else
using Word = std::uint32_t;
#endif
struct Observation {
    Word domain_supported, enabled, queried, valid, available;
    Word kind, instance, geometry, primitive;
    Word validated_t_bits, validated_u_bits, validated_v_bits;
};
static_assert(sizeof(Observation) == 48, "hint observation layout");
} // namespace hint_probe
