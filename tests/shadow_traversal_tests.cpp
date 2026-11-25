// SPDX-License-Identifier: GPL-3.0-only
#include "shadow_traversal.hpp"
#include <iostream>
#include <stdexcept>
using namespace rypath;
namespace {
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
RenderScene base() {
    RenderScene scene;
    GeometryMesh mesh;
    mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.triangles = {{0, 1, 2}};
    scene.geometry = {{mesh}, {{0, {}}}};
    scene.materials.resize(1);
    return scene;
}
void fallback(const RenderScene &scene, const char *reason) {
    const auto selection = select_shadow_traversal(
        scene, scene.geometry, IntersectionProfile::native, ShadowTraversal::opaque_auto);
    require(!selection.eligible && selection.effective == ShadowTraversal::query &&
                selection.reason == reason,
            "Conservative fallback reason");
}
} // namespace
int main() {
    try {
        auto scene = base();
        for (auto requested : {ShadowTraversal::query, ShadowTraversal::opaque_auto}) {
            const auto selection = select_shadow_traversal(scene, scene.geometry,
                                                           IntersectionProfile::native, requested);
            require(selection.eligible && selection.effective == requested &&
                        selection.reason.empty(),
                    "Eligible selection respects requested mode");
        }
        require(select_shadow_traversal(scene, scene.geometry, IntersectionProfile::compensated,
                                        ShadowTraversal::opaque_auto)
                        .reason == "native profile required",
                "Offline compensated profile retains query");
        scene.geometry.instances[0].non_opaque = true;
        fallback(scene, "alpha or nonopaque instance");
        scene = base();
        scene.instance_alpha_roots = {0};
        fallback(scene, "alpha or nonopaque instance");
        for (auto family : {ScatteringKind::mix, ScatteringKind::gltf}) {
            scene = base();
            scene.materials[0].scattering = family;
            fallback(scene, "mix or glTF visibility");
        }
        scene = base();
        scene.materials[0].scattering = ScatteringKind::null_interface;
        fallback(scene, "null or invisible surface");
        scene = base();
        scene.instance_invisible_emitters = {1};
        fallback(scene, "null or invisible surface");
        scene = base();
        scene.volume_transport = true;
        fallback(scene, "medium traversal or interface");
        scene = base();
        scene.camera_medium = 0;
        fallback(scene, "medium traversal or interface");
        scene = base();
        scene.instance_media = {{0, 0}};
        fallback(scene, "medium traversal or interface");
        scene = base();
        scene.media.resize(1);
        fallback(scene, "medium traversal or interface");
        scene = base();
        scene.geometry.meshes[0].patches = {{0, 1, 2, 0}};
        fallback(scene, "procedural geometry");
        scene = base();
        scene.geometry.meshes[0].analytics.resize(1);
        fallback(scene, "procedural geometry");
        scene = base();
        scene.geometry.meshes[0].triangles.push_back({0, 0, 0});
        fallback(scene, "zero-area triangle slot");
        scene = base();
        auto prepared = scene.geometry;
        scene.geometry.meshes[0].triangles.push_back({0, 0, 0});
        require(select_shadow_traversal(scene, prepared, IntersectionProfile::native,
                                        ShadowTraversal::opaque_auto)
                    .eligible,
                "Eligibility inspects actual prepared AS geometry");
        prepared.meshes[0].triangles.push_back({0, 0, 0});
        scene = base();
        require(!select_shadow_traversal(scene, prepared, IntersectionProfile::native,
                                         ShadowTraversal::opaque_auto)
                     .eligible,
                "Prepared miss-tag slot disables specialization");
        scene.geometry.instances.clear();
        fallback(scene, "no triangle instances");
        std::cout << "PASS opaque shadow host selection\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
