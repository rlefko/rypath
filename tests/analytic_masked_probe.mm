// SPDX-License-Identifier: GPL-3.0-only
#include "analytic.hpp"
#include "masked_probe_wire.h"
#include "metal_execution.hpp"
#include "metal_geometry_access.hpp"
#include <iostream>
#include <stdexcept>
namespace {
using namespace rypath;
namespace probe = masked_probe;
void require(bool test, const std::string &message) {
    if (!test)
        throw std::runtime_error(message);
}
int run(const std::filesystem::path &path) {
    GeometryData data;
    for (auto kind : {AnalyticKind::sphere, AnalyticKind::disk, AnalyticKind::cylinder}) {
        GeometryMesh mesh;
        GeometryAnalytic shape;
        shape.kind = kind;
        shape.radius = .25f;
        shape.z_min = -.25f;
        shape.z_max = .25f;
        mesh.analytics.push_back(shape);
        data.meshes.push_back(mesh);
        GeometryInstance instance;
        instance.mesh_index = static_cast<std::uint32_t>(data.instances.size());
        instance.transform.values[7] = float(data.instances.size()) * 4;
        data.instances.push_back(instance);
    }
    // Same source mesh expands to triangle, patch, and analytic native instances.
    GeometryMesh mixed = data.meshes[0];
    mixed.positions = {{.1f, -1, -1}, {.1f, 1, -1}, {.1f, -1, 1}, {.1f, 1, 1},
                       {.2f, -1, -1}, {.2f, 1, -1}, {.2f, -1, 1}};
    mixed.patches = {{0, 1, 2, 3}};
    mixed.triangles = {{4, 5, 6}};
    data.meshes.push_back(mixed);
    data.instances.push_back({3, {{1, 0, 0, 0, 0, 1, 0, 12, 0, 0, 1, 0}}, true});
    auto built = build_metal_geometry(data, path);
    if (built.status == GeometryStatus::no_device) {
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
        std::cerr << built.diagnostic << '\n';
        return 77;
#endif
    }
    if (built.status == GeometryStatus::no_ray_tracing) {
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
        std::cerr << built.diagnostic << '\n';
        return 77;
#endif
    }
    require(built.status == GeometryStatus::success, built.diagnostic);
    auto &scene = *built.scene;
    const auto device = MetalGeometryAccess::device(scene);
    std::string diagnostic;
    const auto library = metal::load_library(device, path, diagnostic);
    require(library != nil, diagnostic);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_analytic_masked_probe", diagnostic);
    require(pipeline != nil, diagnostic);
    std::vector<probe::Case> cases;
    for (unsigned kind = 0; kind < 3; ++kind) {
        for (auto rule : {probe::accept, probe::reject_near, probe::reject, probe::invalid}) {
            for (unsigned any = 0; any < 2; ++any) {
                probe::Case item{};
                item.expected_triangle_primitives[0] = 0;
                item.expected_triangle_primitives[1] = probe::absent_primitive;
                item.ray = {{-.5f, 4.f * static_cast<float>(kind), 0}, {1, 0, 0}, 0, 1};
                if (kind == 1)
                    item.ray = {{.125f, 4.f, .25f}, {0, 0, -1}, 0, 1};
                item.patch_rule = rule;
                item.any_hit = any;
                cases.push_back(item);
            }
        }
    }
    probe::Case blocked{};
    blocked.expected_triangle_primitives[0] = 0;
    blocked.expected_triangle_primitives[1] = probe::absent_primitive;
    blocked.ray = {{-.5f, 12, 0}, {1, 0, 0}, 0, 1};
    blocked.patch_rule = probe::reject_near;
    cases.push_back(blocked);
    const auto input = [device newBufferWithBytes:cases.data()
                                           length:cases.size() * sizeof(probe::Case)
                                          options:MTLResourceStorageModeShared];
    const auto output = [device newBufferWithLength:cases.size() * sizeof(probe::Result)
                                            options:MTLResourceStorageModeShared];
    require(input && output, "probe buffers");
    const auto command = [MetalGeometryAccess::queue(scene) commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(command && encoder, "probe command");
    [encoder setComputePipelineState:pipeline];
    MetalGeometryAccess::bind(scene, encoder, 0);
    [encoder setBuffer:input offset:0 atIndex:1];
    [encoder setBuffer:output offset:0 atIndex:2];
    const auto count = static_cast<std::uint32_t>(cases.size());
    [encoder setBytes:&count length:sizeof(count) atIndex:3];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Analytic alpha probe");
    require(diagnostic.empty(), diagnostic);
    const auto *results = static_cast<const probe::Result *>(output.contents);
    require(results != nullptr, "analytic masked output contents");
    for (unsigned i = 0; i < 24; ++i) {
        const unsigned shape = i / 8, rule = (i % 8) / 2;
        const auto &result = results[i];
        const auto expected = rule == 3                                ? geometry_wire::invalid_hit
                              : rule == 2 || (shape == 1 && rule == 1) ? geometry_wire::miss
                                                                       : geometry_wire::analytic;
        require(result.hit.kind == expected, "masked analytic type case " + std::to_string(i));
        if (expected == geometry_wire::analytic) {
            const float expected_t = rule == 1 ? .75f : .25f;
            require(std::abs(result.hit.t - expected_t) < 1e-6f, "masked analytic root");
            require(MetalGeometryAccess::source_instance(scene, result.hit.instance) == shape,
                    "masked source ordinal");
            if (rule == 1)
                require(result.patch_calls == 2, "both roots tested after alpha rejection");
        }
    }
    require(results[24].hit.kind == geometry_wire::bilinear_patch, "mixed source family routing");
    require(std::abs(results[24].hit.t - .6f) < 1e-6f,
            "blocker before rejected-near analytic farther root");
    require(MetalGeometryAccess::source_instance(scene, results[24].hit.instance) == 3,
            "mixed source mapping");
    std::cout << "Analytic masked/native family checks passed\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    try {
        return run(argc > 1 ? argv[1] : "");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
