// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <metal_stdlib>

namespace rypath_packet_event {
// Match the original event's trace preconditions, including the zero-throughput
// exit before finite/nonnegative validation. This is scheduling, not roulette.
inline bool needs_hit(bool complete, unsigned count, unsigned capacity, metal::float4 beta,
                      metal::float3 origin, metal::float3 direction) {
    return !complete && count < capacity && metal::any(beta > 0) &&
           metal::all(metal::isfinite(origin)) && metal::all(metal::isfinite(direction)) &&
           metal::all(metal::isfinite(beta)) && metal::all(beta >= 0);
}
inline bool ready_valid(bool needs_hit, unsigned ready) {
    return ready <= 1 && bool(ready) == needs_hit;
}
} // namespace rypath_packet_event
