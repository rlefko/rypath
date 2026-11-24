// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "metal_geometry.hpp"
#include "render.hpp"

namespace rypath {
enum class ShadowTraversal { query, opaque_auto };
struct ShadowTraversalSelection {
    ShadowTraversal requested = ShadowTraversal::query;
    ShadowTraversal effective = ShadowTraversal::query;
    bool eligible = false;
    std::string reason;
};
// Inputs must already pass scene/geometry validation. Inspect the same prepared
// geometry supplied to the primary AS builder, including baked emitter meshes.
ShadowTraversalSelection select_shadow_traversal(const RenderScene &, const GeometryData &prepared,
                                                 IntersectionProfile, ShadowTraversal);
} // namespace rypath
