// SPDX-License-Identifier: GPL-3.0-only
#include "geometry_wire.h"
#include "light_geometry_sampling_probe_wire.h"
#include "light_geometry_sampling_shared.h"
#include "metal_execution.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace rypath;
namespace wire = render_wire;
namespace probe = light_geometry_probe;
namespace math = rypath_light_geometry_math;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename Range> id<MTLBuffer> buffer(id<MTLDevice> device, const Range &values) {
    const auto bytes = std::as_bytes(std::span(values));
    return [device newBufferWithBytes:bytes.data()
                               length:bytes.size()
                              options:MTLResourceStorageModeShared];
}
struct Case {
    std::string name;
    wire::Point reference;
    wire::Point normal{};
    unsigned emitter;
    bool cone = false;
    float solid_angle = 0;
};
wire::Instance instance() {
    wire::Instance result{};
    result.matrix[0] = result.matrix[5] = result.matrix[10] = 1;
    result.normal_begin = wire::no_normals;
    result.analytic_begin = UINT32_MAX;
    return result;
}
void run(id<MTLDevice> device, const std::filesystem::path &library_path) {
    std::string diagnostic;
    const auto library = metal::load_library(device, library_path, diagnostic);
    require(library != nil, diagnostic);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_probe_light_geometry", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto queue = [device newCommandQueue];
    require(queue != nil, "Geometry probe queue allocation failed");
    std::vector<wire::Point> positions;
    std::vector<wire::Instance> instances;
    std::vector<wire::Emitter> emitters;
    std::vector<wire::Word> data;
    const std::array triangles{wire::Triangle{{0, 1, 2}}};
    const std::array patches{wire::Patch{{0, 1, 2, 3}}};
    const auto add_mesh = [&](std::span<const wire::Point> vertices, bool patch,
                              wire::Instance transform) {
        transform.vertex_begin = static_cast<wire::Word>(positions.size());
        transform.triangle_count = patch ? 0 : 1;
        transform.patch_count = patch ? 1 : 0;
        positions.insert(positions.end(), vertices.begin(), vertices.end());
        const auto index = static_cast<wire::Word>(instances.size());
        instances.push_back(transform);
        emitters.push_back({index, 0, patch ? wire::patch_primitive : wire::triangle_primitive});
        return static_cast<unsigned>(emitters.size() - 1);
    };
    const std::array triangle{wire::Point{-1, -1, 0}, wire::Point{1, -1, 0}, wire::Point{0, 1, 0}};
    const std::array rectangle{wire::Point{-1, -.5f, 0}, wire::Point{1, -.5f, 0},
                               wire::Point{-1, .5f, 0}, wire::Point{1, .5f, 0}};
    const auto tri = add_mesh(triangle, false, instance());
    auto reflected = instance();
    reflected.matrix[0] = -1;
    const auto reflected_tri = add_mesh(triangle, false, reflected);
    const auto rect = add_mesh(rectangle, true, instance());
    auto shear = instance();
    shear.matrix[1] = .3f;
    const auto sheared = add_mesh(rectangle, true, shear);
    auto warped_vertices = rectangle;
    warped_vertices[3].z = 1;
    const auto warped = add_mesh(warped_vertices, true, instance());
    const auto add_analytic = [&](geometry_wire::Analytic shape, wire::Instance transform) {
        transform.analytic_begin = static_cast<wire::Word>(data.size());
        transform.analytic_count = 1;
        const auto offset = data.size();
        data.resize(offset + sizeof(shape) / sizeof(wire::Word));
        std::memcpy(data.data() + offset, &shape, sizeof(shape));
        const auto index = static_cast<wire::Word>(instances.size());
        instances.push_back(transform);
        emitters.push_back({index, 0, wire::analytic_primitive});
        return static_cast<unsigned>(emitters.size() - 1);
    };
    geometry_wire::Analytic sphere{.kind = geometry_wire::analytic,
                                   .subtype = 1,
                                   .radius = .7f,
                                   .z_min = -.7f,
                                   .z_max = .7f,
                                   .phi_max = 2 * math::pi};
    const auto full_sphere = add_analytic(sphere, instance());
    auto scaled = reflected;
    scaled.matrix[0] = -2;
    scaled.matrix[5] = scaled.matrix[10] = 2;
    const auto reflected_sphere = add_analytic(sphere, scaled);
    auto nonuniform = instance();
    nonuniform.matrix[0] = 2;
    const auto stretched_sphere = add_analytic(sphere, nonuniform);
    auto clipped = sphere;
    clipped.z_min = 0;
    clipped.phi_max = math::pi;
    const auto clipped_sphere = add_analytic(clipped, instance());
    auto disk_shape = sphere;
    disk_shape.subtype = 2;
    disk_shape.inner_radius = .1f;
    const auto disk = add_analytic(disk_shape, shear);
    auto cylinder_shape = sphere;
    cylinder_shape.subtype = 3;
    const auto cylinder = add_analytic(cylinder_shape, nonuniform);
    const math::V3 ta{-1, -1, 0}, tb{1, -1, 0}, tc{0, 1, 0};
    const auto triangle_angle = [&](math::V3 reference) {
        return math::triangle_solid_angle(ta, tb, tc, reference);
    };
    const auto rect_angle = [&](math::V3 reference) {
        return math::rectangle_solid_angle({-1, -.5f, 0}, {1, -.5f, 0}, {-1, .5f, 0}, {1, .5f, 0},
                                           reference);
    };
    const std::vector<Case> cases{
        {"Triangle Spherical", {.2f, .3f, 1}, {}, tri, false, triangle_angle({.2f, .3f, 1})},
        {"Triangle Cosine", {.2f, .3f, 1}, {0, 0, 1}, tri, false, triangle_angle({.2f, .3f, 1})},
        {"Triangle Far Spherical", {0, 0, 60}, {}, tri, false, triangle_angle({0, 0, 60})},
        {"Triangle Far Area", {0, 0, 1000}, {}, tri, false, triangle_angle({0, 0, 1000})},
        {"Triangle Near Area", {0, 0, .00001f}, {}, tri},
        {"Triangle Reflected",
         {0, 0, 1},
         {0, 0, 1},
         reflected_tri,
         false,
         triangle_angle({0, 0, 1})},
        {"Rectangle Spherical", {.2f, .1f, 1}, {}, rect, false, rect_angle({.2f, .1f, 1})},
        {"Rectangle Cosine", {.2f, .1f, 1}, {0, 0, 1}, rect, false, rect_angle({.2f, .1f, 1})},
        {"Rectangle Far Spherical", {0, 0, 100}, {}, rect, false, rect_angle({0, 0, 100})},
        {"Rectangle Far Area", {0, 0, 1000}, {}, rect, false, rect_angle({0, 0, 1000})},
        {"Rectangle Sheared", {.2f, .1f, 1}, {}, sheared},
        {"Bilinear Nonplanar", {.2f, .1f, 2}, {}, warped},
        {"Sphere Outside", {0, 0, 2}, {}, full_sphere, true, 1 / math::sphere_cone_pdf(.7f, 4)},
        {"Sphere Inside", {0, 0, 0}, {}, full_sphere},
        {"Sphere On Boundary", {0, 0, .7f}, {}, full_sphere},
        {"Sphere Reflected Similarity",
         {0, 0, 3},
         {},
         reflected_sphere,
         true,
         1 / math::sphere_cone_pdf(1.4f, 9)},
        {"Sphere Nonuniform", {0, 0, 3}, {}, stretched_sphere},
        {"Sphere Clipped", {0, 0, 3}, {}, clipped_sphere},
        {"Disk Clipped Sheared", {.2f, .1f, 2}, {}, disk},
        {"Cylinder Clipped Nonuniform", {.2f, .1f, 3}, {}, cylinder}};
    constexpr unsigned n = 64;
    std::vector<probe::Query> queries;
    for (const auto &test : cases)
        for (unsigned y = 0; y < n; ++y)
            for (unsigned x = 0; x < n; ++x)
                queries.push_back(
                    {test.reference, test.normal, {(x + .5f) / n, (y + .5f) / n}, test.emitter});
    const auto query_buffer = buffer(device, queries), position_buffer = buffer(device, positions);
    const auto triangle_buffer = buffer(device, triangles), patch_buffer = buffer(device, patches);
    const auto instance_buffer = buffer(device, instances), data_buffer = buffer(device, data);
    const auto emitter_buffer = buffer(device, emitters);
    const auto output = [device newBufferWithLength:queries.size() * sizeof(probe::Result)
                                            options:MTLResourceStorageModeShared];
    wire::Parameters parameters{};
    parameters.instance_count = static_cast<wire::Word>(instances.size());
    const auto count = static_cast<wire::Word>(queries.size());
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(query_buffer != nil && position_buffer != nil && triangle_buffer != nil &&
                patch_buffer != nil && instance_buffer != nil && data_buffer != nil &&
                emitter_buffer != nil && output != nil && command != nil && encoder != nil,
            "Geometry probe allocation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:query_buffer offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBuffer:position_buffer offset:0 atIndex:2];
    [encoder setBuffer:triangle_buffer offset:0 atIndex:3];
    [encoder setBuffer:patch_buffer offset:0 atIndex:4];
    [encoder setBuffer:instance_buffer offset:0 atIndex:5];
    [encoder setBuffer:data_buffer offset:0 atIndex:6];
    [encoder setBuffer:emitter_buffer offset:0 atIndex:7];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:8];
    [encoder setBytes:&count length:sizeof(count) atIndex:9];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Incident geometry probe");
    require(diagnostic.empty(), diagnostic);
    const auto results = static_cast<const probe::Result *>(output.contents);
    for (std::size_t test_index = 0; test_index < cases.size(); ++test_index) {
        const auto &test = cases[test_index];
        double solid_angle_estimate = 0;
        for (unsigned i = 0; i < n * n; ++i) {
            const auto &result = results[test_index * n * n + i];
            require(result.valid && result.sampled_pdf > 0 && std::isfinite(result.area_pdf),
                    test.name + " invalid proposal");
            require(result.sampled_pdf == result.queried_pdf, test.name + " sample/PDF mismatch");
            require(bool(result.cone) == test.cone, test.name + " incorrect cone/fallback policy");
            require(!test.cone || result.far_root_pdf == 0,
                    test.name + " incorrectly assigned cone density to far root");
            const auto normal = result.normal;
            require(std::abs(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z - 1) <
                        2e-5,
                    test.name + " invalid surface normal");
            solid_angle_estimate += 1. / result.sampled_pdf / (n * n);
        }
        if (test.solid_angle > 0)
            require(std::abs(solid_angle_estimate - test.solid_angle) < .005 * test.solid_angle,
                    test.name + " failed independent reciprocal-PDF solid-angle moment");
        std::cout << test.name << ": " << n * n << " proposals passed\n";
    }
    std::cout << "Incident geometry Metal: " << queries.size()
              << " production-adapter proposals passed\n";
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            const auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP incident geometry probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Incident geometry probe requires a Metal device");
            require(argc == 2, "Expected standalone geometry probe metallib path");
            run(device, argv[1]);
        } catch (const std::exception &error) {
            std::cerr << "Incident geometry probe: " << error.what() << '\n';
            return 1;
        }
    }
}
