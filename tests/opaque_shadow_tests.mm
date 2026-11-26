// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"
#include "geometry_reference.hpp"
#include "geometry_wire.h"
#include "metal_execution.hpp"
#include "metal_geometry_access.hpp"
#include "opaque_shadow_capability.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace rypath;
namespace wire = geometry_wire;
void require(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
void available(GeometryStatus status, const std::string &why) {
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (status == GeometryStatus::no_device)
        throw 77;
#endif
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
    if (status == GeometryStatus::no_ray_tracing)
        throw 77;
#endif
    require(status == GeometryStatus::success, why);
}
void capabilities(const std::filesystem::path &current, const std::filesystem::path &legacy) {
    auto device = MTLCreateSystemDefaultDevice();
    if (device == nil)
        available(GeometryStatus::no_device, "Opaque capability test requires Metal");
    std::string why;
    auto library = metal::load_library(device, current, why);
    require(library != nil, why);
    for (NSString *entry in @[
             @"rypath_render_paths", @"rypath_render_packets", @"rypath_wavefront_shade_scalar",
             @"rypath_wavefront_shade_packet"
         ])
        require(metal::verify_opaque_shadow_capability(library, entry, why), why);
    auto fixture = metal::load_library(device, legacy, why);
    require(fixture != nil, why);
    // Execute its unchanged native handshake: native compatibility alone cannot
    // establish that a render/shade entry understands the opaque specialization.
    const bool native = true;
    auto constants = [[MTLFunctionConstantValues alloc] init];
    [constants setConstantValue:&native type:MTLDataTypeBool atIndex:21];
    auto pipeline = metal::load_pipeline(device, fixture, @"rypath_native_intersection_contract",
                                         why, nil, constants);
    require(pipeline != nil, why);
    auto output = [device newBufferWithLength:4 * sizeof(std::uint32_t)
                                      options:MTLResourceStorageModeShared];
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    require(output && command && encoder, "Legacy native handshake resources");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    require(metal::complete(command, "legacy native capability").empty(),
            "Legacy native handshake");
    const auto words = static_cast<const std::uint32_t *>(output.contents);
    require(words[0] == 1 && words[1] == 1 && words[2] == 0 && words[3] == 0,
            "Fixture passes native capability without opaque support");
    require(metal::verify_opaque_shadow_capability(fixture, @"rypath_render_paths", why),
            "Mixed fixture main exposes opaque capability");
    for (NSString *entry in @[
             @"rypath_render_packets", @"rypath_wavefront_shade_scalar",
             @"rypath_wavefront_shade_packet"
         ])
        require(!metal::verify_opaque_shadow_capability(fixture, entry, why) &&
                    why.find("bool function constant 22") != std::string::npos,
                "Native-capable legacy/mixed entry rejects unknown opaque constant");
}
GeometryPoint transform(GeometryPoint p, const GeometryTransform &m, bool position) {
    const auto &v = m.values;
    return {v[0] * p.x + v[1] * p.y + v[2] * p.z + (position ? v[3] : 0),
            v[4] * p.x + v[5] * p.y + v[6] * p.z + (position ? v[7] : 0),
            v[8] * p.x + v[9] * p.y + v[10] * p.z + (position ? v[11] : 0)};
}
GeometryMesh square(float z) {
    GeometryMesh mesh;
    mesh.positions = {{-1, -1, z}, {1, -1, z}, {1, 1, z}, {-1, 1, z}};
    mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    return mesh;
}
void rays(const std::filesystem::path &library) {
    const std::array<GeometryTransform, 4> transforms{
        {{},
         {{-1, 0, 0, 8, 0, 2, 0, -3, 0, 0, .5f, 2}},
         {{1, .25f, 0, 0, 0, 1, .125f, 0, .25f, 0, 1, 0}},
         {{0, 0, 2, 1, 1, 0, 0, 2, 0, .5f, 0, 3}}}};
    for (const auto &matrix : transforms) {
        GeometryData g{{square(0)}, {{0, matrix}}};
        std::vector<GeometryRay> input{
            {{0, 0, 3}, {0, 0, -2}},          // shared edge
            {{1, 1, 3}, {0, 0, -2}},          // shared vertex
            {{.25f, -.25f, -3}, {0, 0, 2}},   // reverse winding, no cull
            {{0, 0, 3}, {0, 0, -2}, 0, 1.5f}, // inclusive endpoint
            {{0, 0, 3}, {0, 0, -2}, 0, std::nextafter(1.5f, 0.f)},
            {{0, 0, 3}, {0, 0, -2}, 1.5f, 2},
            {{0, 0, 3}, {0, 0, -2}, std::nextafter(1.5f, 2.f), 2},
            {{2, 2, 3}, {0, 0, -2}},                   // miss
            {{0, 0, 3}, {1, 0, 0}},                    // parallel
            {{-.75f, .125f, .001f}, {.5f, 0, -.001f}}, // grazing interior
            {{-.75f, -.25f, .001f}, {.5f, 0, -.001f}}, // grazing shared-edge witness
        };
        for (auto &r : input) {
            r.origin = transform(r.origin, matrix, true);
            r.direction = transform(r.direction, matrix, false);
        }
        auto built = build_metal_geometry(g, library, {IntersectionProfile::native});
        available(built.status, built.diagnostic);
        const auto query = built.scene->occlusion(input);
        available(query.status, query.diagnostic);
        require(query.guard_count > 0, "Query output guards");
        std::vector<wire::Ray> packed;
        for (const auto &r : input)
            packed.push_back({{r.origin.x, r.origin.y, r.origin.z},
                              {r.direction.x, r.direction.y, r.direction.z},
                              r.t_min,
                              r.t_max});
        const auto invalid_begin = packed.size();
        for (unsigned invalid = 0; invalid < 5; ++invalid) {
            auto r = packed[0];
            if (invalid == 0)
                r.origin[0] = NAN;
            else if (invalid == 1)
                r.direction[0] = r.direction[1] = r.direction[2] = 0;
            else if (invalid == 2)
                r.t_min = -1;
            else if (invalid == 3)
                r.t_max = NAN;
            else
                r.direction[0] = std::numeric_limits<float>::denorm_min();
            packed.push_back(r);
        }
        auto &scene = *built.scene;
        auto device = MetalGeometryAccess::device(scene);
        std::string why;
        auto lib = metal::load_library(device, library, why);
        require(lib != nil, why);
        auto pipeline = metal::load_pipeline(device, lib, @"rypath_opaque_shadow_probe", why);
        require(pipeline != nil, why);
        auto source = [device newBufferWithBytes:packed.data()
                                          length:packed.size() * sizeof(wire::Ray)
                                         options:MTLResourceStorageModeShared];
        const auto count = static_cast<std::uint32_t>(packed.size());
        auto output = [device newBufferWithLength:(count + 4) * sizeof(std::uint32_t)
                                          options:MTLResourceStorageModeShared];
        require(source && output, "Opaque shadow probe buffers");
        std::memset(output.contents, 0xa5, output.length);
        auto command = [MetalGeometryAccess::queue(scene) commandBuffer];
        auto encoder = [command computeCommandEncoder];
        require(command && encoder, "Opaque shadow probe command");
        [encoder setComputePipelineState:pipeline];
        MetalGeometryAccess::bind(scene, encoder, 0);
        [encoder setBuffer:source offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&count length:sizeof(count) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(count + 4, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [encoder endEncoding];
        require(metal::complete(command, "opaque shadow probe").empty(), "Opaque probe execution");
        const auto result = static_cast<const std::uint32_t *>(output.contents);
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto expected = !test::reference_closest(g, input[i]).empty();
            require(result[i] <= 1 && bool(result[i]) == bool(query.occluded[i]),
                    "Opaque/query Boolean shadow agreement");
            // Exact endpoint and shared-edge cases above are independently scanned
            // in double precision; no primitive identity or tie order is required.
            if (i == 10) {
                // This near-parallel shared-edge ray is a captured native-profile
                // miss on M4 Max even in the existing query path. Retain exact
                // query agreement and report the independent result separately.
                std::cout << "CHARACTERIZE grazing shared-edge transform="
                          << (&matrix - transforms.data()) << " native=" << result[i]
                          << " independent=" << expected << '\n';
                continue;
            }
            require(bool(result[i]) == expected,
                    "Independent brute-force occlusion oracle transform=" +
                        std::to_string(&matrix - transforms.data()) + " ray=" + std::to_string(i) +
                        " native=" + std::to_string(result[i]) +
                        " expected=" + std::to_string(expected));
        }
        for (std::size_t i = invalid_begin; i < count; ++i)
            require(result[i] == wire::invalid_hit, "Invalid shadow ray remains visible failure");
        for (std::size_t i = count; i < count + 4; ++i)
            require(result[i] == 0xa5a5a5a5u, "Opaque probe output guards");
    }
}
// Optional frozen native library supplied by local qualification, never Git.
void stale_native_library(const std::filesystem::path &library) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{{}, {-.3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
    scene.environment.fill(.1f);
    scene.max_depth = 1;
    scene.materials.resize(1);
    scene.materials[0].kind = SpectralMaterialKind::constant;
    scene.geometry = {{square(3)}, {{0, {}}}};
    scene.instance_materials = {0};
    FrameConfig config{.width = 1, .height = 1};
    config.execution = FrameExecution::megakernel;
    auto query = build_frame_session(scene, config, library);
    require(query.status == GeometryStatus::failure && !query.session &&
                query.diagnostic.find("rypath_native_intersection_contract_v2") !=
                    std::string::npos,
            "Old native metadata library cannot publish a query frame session");
    config.shadow_traversal = ShadowTraversal::opaque_auto;
    auto opaque = build_frame_session(scene, config, library);
    require(opaque.status == GeometryStatus::failure && !opaque.session &&
                opaque.shadow_traversal.effective == ShadowTraversal::query &&
                opaque.diagnostic.find("rypath_native_intersection_contract_v2") !=
                    std::string::npos,
            "Old native metadata library cannot publish opaque execution");
}
void render(const std::filesystem::path &library) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{{}, {-.6f, -.6f, 1}, {1.2f, 0, 0}, {0, 1.2f, 0}};
    scene.environment.fill(.2f);
    scene.max_depth = 3;
    scene.materials.resize(1);
    scene.materials[0].kind = SpectralMaterialKind::constant;
    scene.materials[0].constant = .6f;
    scene.geometry = {{square(3), square(1.5f)}, {{0, {}}, {1, {}}}};
    // Shift a small occluder to one side; camera and direct-light rays see both
    // blocked and clear segments. Both modes retain the same closest-hit path.
    scene.geometry.instances[1].transform.values = {.25f, 0, 0, .5f, 0, .25f, 0, 0, 0, 0, 1, 0};
    scene.instance_materials = {0, 0};
    for (auto transport : {SpectralTransport::scalar, SpectralTransport::packet}) {
        scene.spectral_transport = transport;
        for (auto execution : {FrameExecution::megakernel, FrameExecution::wavefront}) {
            FrameConfig config{.width = 17, .height = 9};
            config.execution = execution;
            auto control = build_frame_session(scene, config, library);
            available(control.status, control.diagnostic);
            config.shadow_traversal = ShadowTraversal::opaque_auto;
            auto candidate = build_frame_session(scene, config, library);
            available(candidate.status, candidate.diagnostic);
            require(candidate.shadow_traversal.eligible &&
                        candidate.shadow_traversal.effective == ShadowTraversal::opaque_auto &&
                        control.shadow_traversal.effective == ShadowTraversal::query,
                    "Actual scalar/packet frame pipeline specialization");
            bool nonzero = false;
            for (std::uint64_t index : {0ull, 1ull, (1ull << 32) + 7}) {
                auto a = control.session->submit({.sample_index = index});
                auto b = candidate.session->submit({.sample_index = index});
                available(a.status, a.diagnostic);
                available(b.status, b.diagnostic);
                require(a.frame && b.frame, "Raw differential frames");
                require(a.frame->wait().status == FrameStatus::complete &&
                            b.frame->wait().status == FrameStatus::complete,
                        "Raw differential frame completion");
                const auto raw_a = a.frame->capture(), raw_b = b.frame->capture();
                available(raw_a.status, raw_a.diagnostic);
                available(raw_b.status, raw_b.diagnostic);
                require(raw_a.raw_xyz == raw_b.raw_xyz,
                        "Query/opaque raw XYZ exact differential control");
                for (const auto &p : raw_a.raw_xyz)
                    for (const auto value : p) {
                        require(std::isfinite(value), "Finite raw differential output");
                        nonzero |= value != 0;
                    }
            }
            require(nonzero, "Nontrivial scalar/packet raw output witness");
        }
    }
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            const std::filesystem::path library = argc > 1 ? argv[1] : "";
            if (argc > 2)
                capabilities(library, argv[2]);
            if (argc > 3)
                stale_native_library(argv[3]);
            rays(library);
            render(library);
            std::cout << "PASS opaque Boolean shadows and raw frame controls\n";
            return 0;
        } catch (int code) {
            return code;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
