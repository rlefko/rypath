// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render.hpp"
#include <algorithm>

namespace rypath {
// Conservative proof over every translated material, including unused/mix children.
// Rejection chooses generic execution; it never changes scene support or materials.
inline bool scene_proves_diffuse_events(const RenderScene &s) {
    if (s.materials.empty() || s.volume_transport || !s.media.empty() ||
        s.camera_medium != no_medium || !s.instance_media.empty() || !s.image_textures.empty() ||
        !s.texture_graph.nodes.empty() || s.environment_image != UINT32_MAX ||
        std::ranges::any_of(s.instance_alpha_roots, [](auto i) { return i != UINT32_MAX; }))
        return false;
    return std::ranges::all_of(s.materials, [](const auto &m) {
        const bool representation = m.kind == SpectralMaterialKind::constant ||
                                    m.kind == SpectralMaterialKind::rgb_sigmoid ||
                                    m.kind == SpectralMaterialKind::sampled;
        return m.scattering == ScatteringKind::diffuse && representation &&
               m.texture_root == UINT32_MAX && m.bump_root == UINT32_MAX &&
               m.normal_map == UINT32_MAX && !m.table;
    });
}
} // namespace rypath
