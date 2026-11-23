// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "layered_schedule_oracles.h"
// Root generates and reviews this f-only overlay using
// scripts/update_layered_schedule_oracles.py. Missing data intentionally prevents
// compiling corrected expectations; stock values must never silently substitute.
#include "layered_schedule_corrected_f.h"
#include <bit>
#include <cstddef>
namespace layered_schedule_corrected {
inline std::array<float, 8> expected(std::size_t index) {
    auto result = layered_schedule_oracles::fixtures.at(index).expected;
    result[0] = std::bit_cast<float>(directional_f_bits.at(index));
    return result;
}
} // namespace layered_schedule_corrected
