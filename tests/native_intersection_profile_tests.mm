// SPDX-License-Identifier: GPL-3.0-only
#include "geometry_reference.hpp"
#include "masked_probe_wire.h"
#include "metal_execution.hpp"
#include "metal_geometry_access.hpp"
#include "metal_render.hpp"
#include "triangle_candidate_fixture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace rypath;
namespace wire = geometry_wire;
namespace probe = masked_probe;
constexpr auto profile = IntersectionProfile::native;
void require(bool ok, const std::string &why) {
    if (!ok)
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
GeometryPoint transform(GeometryPoint p, const GeometryTransform &m, bool position) {
    const auto &v = m.values;
    return {v[0] * p.x + v[1] * p.y + v[2] * p.z + (position ? v[3] : 0),
            v[4] * p.x + v[5] * p.y + v[6] * p.z + (position ? v[7] : 0),
            v[8] * p.x + v[9] * p.y + v[10] * p.z + (position ? v[11] : 0)};
}
GeometryMesh triangle(float z) {
    GeometryMesh mesh;
    mesh.positions = {{-2, -2, z}, {6, -2, z}, {-2, 6, z}};
    // Preserve a zero-area slot and original corner order.
    mesh.triangles = {{0, 0, 0}, {0, 1, 2}};
    return mesh;
}
void ordinary(const std::filesystem::path &library) {
    const std::array<GeometryTransform, 4> transforms{
        {{},
         {{-1, 0, 0, 8, 0, 2, 0, -3, 0, 0, .5f, 2}},
         {{1, .25f, 0, 0, 0, 1, .125f, 0, .25f, 0, 1, 0}},
         {{0, 0, 2, 1, 1, 0, 0, 2, 0, .5f, 0, 3}}}};
    for (const auto &matrix : transforms)
        for (bool reverse : {false, true})
            for (bool non_opaque : {false, true}) {
                GeometryData g{{{}, triangle(0)}, {{0, {}}, {1, matrix, non_opaque}}};
                if (reverse)
                    std::swap(g.meshes[1].triangles[1][1], g.meshes[1].triangles[1][2]);
                std::array<GeometryRay, 7> rays{{{{0, 0, 3}, {0, 0, -2}},
                                                 {{0, 0, -3}, {0, 0, 2}},
                                                 {{0, 0, 3}, {0, 0, -2}, 0, 1.49f},
                                                 {{0, 0, 3}, {0, 0, -2}, 0, 1.51f},
                                                 {{0, 0, 3}, {0, 0, -2}, 1.51f, 2},
                                                 {{10, 10, 3}, {0, 0, -2}},
                                                 {{0, 0, 3}, {1, 0, 0}}}};
                for (auto &r : rays) {
                    r.origin = transform(r.origin, matrix, true);
                    r.direction = transform(r.direction, matrix, false);
                }
                auto built = build_metal_geometry(g, library, {profile});
                available(built.status, built.diagnostic);
                auto closest = built.scene->closest_hits(rays);
                auto shadow = built.scene->occlusion(rays);
                available(closest.status, closest.diagnostic);
                available(shadow.status, shadow.diagnostic);
                require(closest.guard_count && shadow.guard_count, "native output guards");
                for (std::size_t i = 0; i < rays.size(); ++i) {
                    const auto expected = test::reference_closest(g, rays[i]);
                    test::GeometryErrors errors;
                    require(test::compare_closest(g, rays[i], expected, closest.hits[i], errors)
                                .empty(),
                            "native ordinary affine/winding/source/primitive oracle");
                    require(bool(shadow.occluded[i]) == !expected.empty(),
                            "native occlusion oracle");
                }
            }
}
GeometryData composition() {
    auto mesh = triangle(.25f);
    mesh.positions.insert(mesh.positions.end(),
                          {{-1, -1, .75f}, {1, -1, .75f}, {-1, 1, .75f}, {1, 1, .75f}});
    mesh.patches = {{3, 4, 5, 6}};
    return {{mesh}, {{0, {}, true}}};
}
void callbacks(const std::filesystem::path &library) {
    auto built = build_metal_geometry(composition(), library, {profile});
    available(built.status, built.diagnostic);
    auto &scene = *built.scene;
    auto device = MetalGeometryAccess::device(scene);
    std::string why;
    auto lib = metal::load_library(device, library, why);
    require(lib != nil, why);
    auto constants = [[MTLFunctionConstantValues alloc] init];
    const bool native = true;
    [constants setConstantValue:&native type:MTLDataTypeBool atIndex:21];
    auto pipeline =
        metal::load_pipeline(device, lib, @"rypath_masked_intersection_probe", why, nil, constants);
    require(pipeline != nil, why);
    const wire::Ray ray{{0, 0, 0}, {0, 0, 1}, 0, 1};
    const auto absent = probe::absent_primitive;
    std::array<probe::Case, 6> cases{{{ray, probe::accept, probe::accept, absent, 0, {1, absent}},
                                      {ray, probe::reject, probe::accept, absent, 0, {1, absent}},
                                      {ray, probe::reject, probe::reject, absent, 0, {1, absent}},
                                      {ray, probe::invalid, probe::accept, absent, 0, {1, absent}},
                                      {ray, probe::reject, probe::accept, absent, 1, {1, absent}},
                                      {ray, probe::accept, probe::reject, absent, 1, {1, absent}}}};
    auto input = [device newBufferWithBytes:cases.data()
                                     length:sizeof(cases)
                                    options:MTLResourceStorageModeShared];
    auto output = [device newBufferWithLength:cases.size() * sizeof(probe::Result)
                                      options:MTLResourceStorageModeShared];
    require(input && output, "native callback buffers");
    auto command = [MetalGeometryAccess::queue(scene) commandBuffer];
    auto encoder = [command computeCommandEncoder];
    require(command && encoder, "native callback command");
    [encoder setComputePipelineState:pipeline];
    MetalGeometryAccess::bind(scene, encoder, 0);
    [encoder setBuffer:input offset:0 atIndex:1];
    [encoder setBuffer:output offset:0 atIndex:2];
    const auto count = static_cast<wire::Word>(cases.size());
    [encoder setBytes:&count length:sizeof(count) atIndex:3];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    require(metal::complete(command, "native alpha/procedural composition").empty(),
            "native callback dispatch");
    std::array<probe::Result, cases.size()> result{};
    std::memcpy(result.data(), output.contents, sizeof(result));
    const std::array<wire::Word, 6> kinds{wire::triangle,    wire::bilinear_patch, wire::miss,
                                          wire::invalid_hit, wire::bilinear_patch, wire::triangle};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto &r = result[i];
        require(r.hit.kind == kinds[i],
                "native alpha accept/reject/invalid and procedural ordering");
        if (cases[i].any_hit == 0 || r.hit.kind == wire::triangle)
            require(r.triangle_calls > 0, "nonopaque native triangle must reach alpha");
        if (r.hit.kind == wire::triangle) {
            require(r.hit.instance == 0 && r.hit.primitive == 1 && r.hit.t == .25f &&
                        r.hit.u == .25f && r.hit.v == .25f,
                    "alpha sees native barycentrics/source ordinals");
        } else if (r.hit.kind == wire::bilinear_patch) {
            require(r.hit.instance == 1 && r.hit.t == .75f && r.hit.u == .5f && r.hit.v == .5f,
                    "rejected native triangle exposes procedural patch");
        }
    }
    // Analytic surfaces share the primary TLAS too; the nearer primitive wins.
    auto g = composition();
    g.meshes[0].analytics = {{AnalyticKind::disk, 1, -1, 1, 6.283185307179586f, 0, .125f}};
    built = build_metal_geometry(g, library, {profile});
    available(built.status, built.diagnostic);
    const std::array<GeometryRay, 2> rays{
        {{{.25f, 0, 0}, {0, 0, 1}}, {{.25f, 0, 0}, {0, 0, 1}, .2f, 1}}};
    auto hit = built.scene->closest_hits(rays);
    available(hit.status, hit.diagnostic);
    require(hit.hits[0].kind == PrimitiveKind::analytic && hit.hits[0].t == .125f &&
                hit.hits[1].kind == PrimitiveKind::triangle && hit.hits[1].t == .25f,
            "native triangle/analytic/patch composition and original lower bound");
}
void render_contracts(const std::filesystem::path &library) {
    const auto base = [] {
        RenderScene scene;
        scene.model = RenderModel::pbrt;
        scene.raster_camera = RasterCamera{{}, {-1e-7f, -1e-7f, 1}, {2e-7f, 0, 0}, {0, 2e-7f, 0}};
        scene.environment.fill(1);
        scene.max_depth = 0;
        SpectralMaterial black;
        black.kind = SpectralMaterialKind::constant;
        scene.materials.push_back(black);
        scene.geometry = {{triangle(1)}, {{0, {}}}};
        scene.instance_materials = {0};
        return scene;
    };
    const auto check = [&](RenderScene scene, double expected, const std::string &label,
                           bool analog_absorption = false) {
        scene.instance_surface_transforms.resize(scene.geometry.instances.size());
        for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
            if (!instance_uses_texture_context(scene, i))
                continue;
            // Untextured diffuse/null fixtures must keep the optional table absent.
            // Once any instance needs context, retain a slot for every source.
            scene.instance_texture_frames.resize(scene.geometry.instances.size());
            const auto &surface = scene.instance_surface_transforms[i];
            require(prepare_texture_triangle_frames(
                        scene.geometry.meshes[scene.geometry.instances[i].mesh_index],
                        surface.shape, surface.placement, scene.instance_texture_frames[i])
                        .empty(),
                    label + " frames");
        }
        // Both transport kernels must specialize the explicit native profile.
        for (auto transport : {SpectralTransport::scalar, SpectralTransport::packet}) {
            scene.spectral_transport = transport;
            auto built = build_metal_renderer(scene, library, RenderBuildOptions{profile});
            if (transport == SpectralTransport::packet && !scene.media.empty()) {
                require(built.status == GeometryStatus::failure &&
                            built.diagnostic.find("requires vacuum") != std::string::npos,
                        "native profile preserves unsupported packet medium diagnostic");
                continue;
            }
            const std::string context =
                label + (transport == SpectralTransport::scalar ? " scalar" : " packet");
            available(built.status, context + ": " + built.diagnostic);
            // Four independent fixed seeds; Hoeffding's distribution-free bound
            // below is declared before observing the analog absorption outcomes.
            constexpr std::array<std::uint64_t, 4> seeds{19317, 27236, 35155, 43074};
            const std::size_t repeats = analog_absorption ? seeds.size() : 1;
            double sum = 0;
            std::size_t count = 0;
            for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
                RenderRequest request{.width = 1,
                                      .height = 1,
                                      .seed = seeds[repeat],
                                      .pixel_count = 1,
                                      .sample_count =
                                          analog_absorption
                                              ? 4096u
                                              : (transport == SpectralTransport::scalar ? 64u : 1u),
                                      .total_samples = analog_absorption ? 4096u : 64u};
                const auto result = built.scene->sample(request);
                available(result.status, context + ": " + result.diagnostic);
                require(result.guard_count > 0, context + " render guards");
                const auto close = [&](float radiance) {
                    require(std::isfinite(radiance), context + " nonfinite raw radiance");
                    if (analog_absorption) {
                        // Pure absorption and a unit environment yield killed or
                        // surviving analog paths, not deterministic transmittance.
                        require(radiance == 0 || radiance == 1,
                                context + " analog outcome actual=" + std::to_string(radiance));
                        sum += radiance;
                        ++count;
                    } else {
                        require(std::abs(radiance - expected) < 4e-5,
                                context + " independent raw-radiance oracle actual=" +
                                    std::to_string(radiance) + " expected=" +
                                    std::to_string(expected) + " tolerance=0.000040");
                    }
                };
                if (transport == SpectralTransport::scalar) {
                    require(result.samples.size() == request.sample_count,
                            context + " scalar accounting");
                    for (const auto &sample : result.samples)
                        close(sample.radiance);
                } else {
                    require(result.packet_samples.size() == 1, context + " packet accounting");
                    for (const auto &sample : result.packet_samples)
                        for (float radiance : sample.radiance)
                            close(radiance);
                }
            }
            if (analog_absorption) {
                constexpr std::size_t expected_count = 4 * 4096;
                // P(|mean-E| >= epsilon) <= 2 exp(-2 N epsilon^2).
                // A 1e-9 false-rejection probability plus a predeclared numerical
                // allowance covers the bounded ray-origin departure adaptation.
                const double tolerance = std::sqrt(std::log(2e9) / (2 * expected_count)) + 4e-5;
                require(count == expected_count, context + " analog sample accounting");
                const double mean = sum / count;
                std::cout << context << " mean=" << mean << " expected=" << expected
                          << " tolerance=" << tolerance << " count=" << count << '\n';
                require(std::abs(mean - expected) <= tolerance,
                        context + " independent Beer-Lambert mean actual=" + std::to_string(mean) +
                            " expected=" + std::to_string(expected) +
                            " tolerance=" + std::to_string(tolerance));
            }
        }
    };
    check(base(), 0, "native opaque blocks");
    auto alpha = base();
    alpha.materials[0].scattering = ScatteringKind::gltf;
    alpha.materials[0].gltf.double_sided = true;
    alpha.materials[0].gltf.alpha_mode = 1;
    alpha.materials[0].gltf.base_color[3] = 0;
    check(alpha, 1, "native glTF MASK alpha rejects");
    alpha.materials[0].gltf.base_color[3] = 1;
    check(alpha, 0, "native glTF MASK alpha accepts");
    // glTF materials cannot be mix children in the supported profile. Exercise
    // a supported diffuse-child mix with independent shape coverage instead.
    auto mixed = base();
    mixed.materials.push_back(mixed.materials[0]);
    SpectralMaterial mix;
    mix.scattering = ScatteringKind::mix;
    mix.surface.first_material = 0;
    mix.surface.second_material = 1;
    mixed.materials.push_back(mix);
    mixed.instance_materials[0] = 2;
    mixed.texture_graph.nodes.emplace_back(); // Scalar constant zero.
    mixed.instance_alpha_roots = {0};
    check(mixed, 1, "native mix shape alpha rejects");
    mixed.texture_graph.nodes[0].values[0] = 1;
    check(mixed, 0, "native mix shape alpha accepts");
    auto boundary = base();
    boundary.materials[0].scattering = ScatteringKind::null_interface;
    check(boundary, 1, "native null boundary passes through");
    boundary.volume_transport = true;
    boundary.integrator = IntegratorKind::volpath;
    HomogeneousMedium medium;
    medium.sigma_a.fill(.5f);
    boundary.media = {medium};
    boundary.geometry.meshes.push_back(triangle(2));
    boundary.geometry.instances.push_back({1, {}});
    // Enter the slab at the first plane and leave at the second.
    std::swap(boundary.geometry.meshes[0].triangles[1][1],
              boundary.geometry.meshes[0].triangles[1][2]);
    boundary.instance_materials = {0, 0};
    boundary.instance_media = {{0, no_medium}, {0, no_medium}};
    check(boundary, std::exp(-.5), "native medium transitions Beer-Lambert slab", true);
    auto invisible = base();
    invisible.area_emissions.emplace_back();
    invisible.instance_emissions = {0};
    invisible.texture_graph.nodes.emplace_back(); // Constant zero coverage.
    invisible.instance_alpha_roots = {0};
    invisible.instance_invisible_emitters = {1};
    check(invisible, 1, "native invisible emitter passes through");
}
void witnesses(const std::filesystem::path &library) {
    // These are characterization, deliberately not native-equals-compensated gates.
    auto g = test::triangle_fixture::regression();
    for (unsigned fresh = 0; fresh < 4; ++fresh) {
        auto built = build_metal_geometry(g, library, {profile});
        available(built.status, built.diagnostic);
        auto hit = built.scene->closest_hits(test::triangle_fixture::rays);
        auto shadow = built.scene->occlusion(test::triangle_fixture::rays);
        available(hit.status, hit.diagnostic);
        available(shadow.status, shadow.diagnostic);
        for (std::size_t i = 0; i < hit.hits.size(); ++i) {
            const auto expected = test::reference_closest(g, test::triangle_fixture::rays[i]);
            require(expected.size() == 1 && expected[0].primitive_index == (i ? 601u : 773u),
                    "independent edge witness oracle");
            const auto &h = hit.hits[i];
            std::cout << "CHARACTERIZE native edge build=" << fresh << " ray=" << i
                      << " hit=" << h.hit << " primitive=" << h.primitive_index
                      << " independent=" << expected[0].primitive_index << " t=" << h.t
                      << " occluded=" << unsigned(shadow.occluded[i]) << '\n';
        }
    }
    // The documented missing-neighbor witness from triangle_box_coverage_tests.
    GeometryMesh mesh;
    mesh.positions = {{-8192, -8192, 1}, {-8192, 8192, 1}, {8192, 8192, 1}, {8192, -8192, 1}};
    mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    g = {{mesh}, {{0, {}}}};
    const auto f = [](std::uint32_t bits) { return std::bit_cast<float>(bits); };
    const std::array<GeometryRay, 1> rays{
        {{{f(0x3fff2000), 0, f(0xbf7e3fee)}, {f(0xbf3504f3), f(0xb384c063), f(0x3f3504f3)}}}};
    auto built = build_metal_geometry(g, library, {profile});
    available(built.status, built.diagnostic);
    const auto expected = test::reference_closest(g, rays[0]);
    require(expected.size() == 1 && expected[0].primitive_index == 1,
            "missing-neighbor independent witness");
    auto hit = built.scene->closest_hits(rays);
    available(hit.status, hit.diagnostic);
    std::cout << "CHARACTERIZE native missing-neighbor hit=" << hit.hits[0].hit
              << " primitive=" << hit.hits[0].primitive_index << " independent=1\n";
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            const std::filesystem::path library = argc > 1 ? argv[1] : "";
            ordinary(library);
            callbacks(library);
            render_contracts(library);
            witnesses(library);
            std::cout << "PASS explicit native intersection profile\n";
            return 0;
        } catch (int code) {
            return code;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
