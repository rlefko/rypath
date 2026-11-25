// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "packet_wire.h"
#include "render.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rypath::packet_dispatch {
inline constexpr std::size_t guard_bytes = 64;
inline constexpr std::size_t maximum_payload_bytes = std::size_t{1024} * 1024 * 1024;
inline std::size_t lane_bytes(const RenderScene &scene) {
    // Hero PT uses the same rolling state in both execution modes and emits no
    // splats. Its offline output does not need legacy camera/light path arrays.
    return scene.spectral_transport == SpectralTransport::hero
               ? sizeof(packet_wire::Result)
               : packet_wire::lane_bytes(scene.max_depth);
}

// The estimate already charges three copies of the baseline payload and guard.
// This extends that accounting; it is not a whole-process peak-memory bound.
inline constexpr std::size_t payload_bytes(std::optional<std::size_t> estimate,
                                           std::optional<std::size_t> finite_limit,
                                           std::size_t recommended_working_set,
                                           std::size_t max_buffer_length) {
    constexpr std::size_t baseline = packet_wire::workspace_bytes;
    if (!estimate || recommended_working_set == 0 || *estimate >= recommended_working_set ||
        max_buffer_length <= guard_bytes)
        return baseline;
    auto headroom = recommended_working_set - *estimate;
    if (finite_limit) {
        if (*estimate >= *finite_limit)
            return baseline;
        headroom = std::min(headroom, *finite_limit - *estimate);
    }
    // Bound the delta before addition, so even SIZE_MAX headroom cannot overflow.
    const auto extra = std::min(headroom / 3, maximum_payload_bytes - baseline);
    const auto candidate = std::min(baseline + extra, max_buffer_length - guard_bytes);
    return std::max(baseline, candidate);
}

inline constexpr std::uint32_t lane_capacity(std::size_t payload, std::size_t lane_bytes,
                                             std::size_t max_buffer_length) {
    if (lane_bytes == 0 || max_buffer_length <= guard_bytes)
        return 0;
    const auto capacity = std::min({std::size_t{render_tile_capacity}, payload / lane_bytes,
                                    (max_buffer_length - guard_bytes) / lane_bytes});
    return static_cast<std::uint32_t>(capacity);
}
} // namespace rypath::packet_dispatch
