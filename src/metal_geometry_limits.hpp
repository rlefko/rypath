// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>

namespace rypath::metal {

// Standard acceleration structures use the limits documented by Apple for
// MTLAccelerationStructureUsageNone.
constexpr bool fits_primitive_limit(std::size_t count) { return count <= (std::size_t{1} << 28); }

constexpr bool fits_instance_limit(std::size_t count) { return count <= (std::size_t{1} << 24); }

} // namespace rypath::metal
