// SPDX-License-Identifier: GPL-3.0-only
#include "native_hint_budget.hpp"
#ifdef RYPATH_TEST_METAL_STUB
#include "metal_geometry.hpp"
#endif

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
int main() {
    try {
        using namespace rypath;
        GeometryData geometry;
#ifdef RYPATH_TEST_METAL_STUB
        const auto unavailable = build_metal_geometry_with_hints(geometry, {}, true);
        require(unavailable.status == GeometryStatus::not_built && !unavailable.scene &&
                    !unavailable.diagnostic.empty(),
                "The supplementary builder preserves the portable unavailable contract");
#endif
        const auto empty = native_hint_budget(geometry, 100, 100);
        require(empty.valid && empty.additional == 0 && empty.combined == 100,
                "Empty hint preparation has no extra allocation");
        require(!native_hint_budget(geometry, 101, 100).valid,
                "An already exceeded preparation budget remains invalid");
        GeometryMesh mesh;
        mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
        mesh.triangles = {{0, 1, 2}, {1, 3, 2}};
        mesh.patches = {{0, 1, 2, 3}};
        mesh.analytics.push_back(
            {.kind = AnalyticKind::sphere, .radius = 1, .z_min = -1, .z_max = 1});
        geometry.meshes.push_back(mesh);
        geometry.instances = {{.mesh_index = 0, .transform = {}},
                              {.mesh_index = 0, .transform = {}, .non_opaque = true}};
        // Existing policy charges three copies of uploads/staging, including
        // masked placements and all three expanded primitive-kind slots.
        const std::size_t bytes = 3 * (std::size_t{4 + 2} * 12 + sizeof(std::uint32_t) +
                                       4 * sizeof(void *) + std::size_t{6} * 64);
        const auto exact = native_hint_budget(geometry, 100, 100 + bytes);
        require(exact.valid && exact.additional == bytes && exact.combined == 100 + bytes,
                "Shared mesh and split-instance staging match the conservative byte charge");
        require(!native_hint_budget(geometry, 100, 99 + bytes).valid,
                "One byte below the required preparation bound is rejected");
        const auto maximum = std::numeric_limits<std::size_t>::max();
        require(!native_hint_budget(geometry, maximum - 1, maximum).valid,
                "A near-limit base cannot wrap the incremental charge");
        const auto wide = native_hint_budget(geometry, 100, maximum);
        require(wide.valid && wide.additional == bytes && wide.combined == 100 + bytes,
                "A permissive limit leaves the same deterministic charge");
        std::cout << "Native hint preparation budget checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
