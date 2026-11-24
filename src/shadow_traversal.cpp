// SPDX-License-Identifier: GPL-3.0-only
#include "shadow_traversal.hpp"
#include <algorithm>

namespace rypath {
ShadowTraversalSelection select_shadow_traversal(const RenderScene &scene,
                                                 const GeometryData &prepared,
                                                 IntersectionProfile profile,
                                                 ShadowTraversal requested) {
    ShadowTraversalSelection selected;
    selected.requested = requested;
    const auto reason = [&]() -> std::string {
        if (profile != IntersectionProfile::native)
            return "native profile required";
        if (scene.volume_transport || scene.camera_medium != no_medium || !scene.media.empty() ||
            std::ranges::any_of(scene.instance_media, [](const auto &m) {
                return m.inside != no_medium || m.outside != no_medium;
            }))
            return "medium traversal or interface";
        if (std::ranges::any_of(
                scene.materials,
                [](const auto &m) { return m.scattering == ScatteringKind::null_interface; }) ||
            std::ranges::any_of(scene.instance_invisible_emitters,
                                [](auto invisible) { return invisible != 0; }))
            return "null or invisible surface";
        if (std::ranges::any_of(scene.materials, [](const auto &m) {
                return m.scattering == ScatteringKind::mix || m.scattering == ScatteringKind::gltf;
            }))
            return "mix or glTF visibility";
        if (std::ranges::any_of(scene.instance_alpha_roots,
                                [](auto root) { return root != UINT32_MAX; }) ||
            std::ranges::any_of(prepared.instances, [](const auto &i) { return i.non_opaque; }))
            return "alpha or nonopaque instance";
        bool triangles = false;
        for (const auto &mesh : prepared.meshes) {
            if (!mesh.patches.empty() || !mesh.analytics.empty())
                return "procedural geometry";
            for (const auto &t : mesh.triangles) {
                if (!triangle_has_area(mesh.positions[t[0]], mesh.positions[t[1]],
                                       mesh.positions[t[2]]))
                    return "zero-area triangle slot";
                triangles = true;
            }
        }
        if (!triangles || prepared.instances.empty())
            return "no triangle instances";
        return {};
    };
    selected.reason = reason();
    selected.eligible = selected.reason.empty();
    if (requested == ShadowTraversal::opaque_auto && selected.eligible)
        selected.effective = ShadowTraversal::opaque_auto;
    return selected;
}
} // namespace rypath
