// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "geometry.hpp"
#include <cstddef>
#include <limits>

namespace rypath {
struct NativeHintBudget {
    bool valid;
    std::size_t additional;
    std::size_t combined;
};
// Match the existing preparation estimator's conservative three-copy policy.
// Native AS storage, driver scratch and pipelines remain separately excluded.
inline NativeHintBudget native_hint_budget(const GeometryData &geometry, std::size_t base,
                                           std::size_t limit) {
    if (base > limit)
        return {false, 0, base};
    std::size_t used = base;
    const auto charge = [&](std::size_t count, std::size_t stride) {
        if (stride > std::numeric_limits<std::size_t>::max() / 3)
            return false;
        stride *= 3;
        if (count > (limit - used) / stride)
            return false;
        used += count * stride;
        return true;
    };
    for (const auto &mesh : geometry.meshes) {
        if (mesh.triangles.empty())
            continue;
        if (!charge(mesh.positions.size(), 12) || !charge(mesh.triangles.size(), 12))
            return {false, 0, used};
    }
    // Hint child-index vector and conservative pointer-slot staging for the new
    // child array/map; original geometry data is shared, not copied a second time.
    if (!charge(geometry.meshes.size(), sizeof(std::uint32_t) + 4 * sizeof(void *)))
        return {false, 0, used};
    for (const auto &instance : geometry.instances) {
        const auto &mesh = geometry.meshes[instance.mesh_index];
        const std::size_t slots = std::size_t(!mesh.triangles.empty()) +
                                  std::size_t(!mesh.patches.empty()) +
                                  std::size_t(!mesh.analytics.empty());
        if (!charge(slots, 64))
            return {false, 0, used};
    }
    return {true, used - base, used};
}
} // namespace rypath
