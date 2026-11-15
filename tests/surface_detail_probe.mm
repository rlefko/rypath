// SPDX-License-Identifier: GPL-3.0-only

#include "render_wire.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <cmath>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace {
namespace wire = rypath::render_wire;

void require(bool success, std::string_view message) {
    if (!success) {
        throw std::runtime_error(std::string(message));
    }
}

using Vec = std::array<double, 3>;
Vec operator-(Vec a, Vec b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec operator*(double s, Vec v) { return {s * v[0], s * v[1], s * v[2]}; }
double dot(Vec a, Vec b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(Vec a, Vec b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec unit(Vec v) { return (1.0 / std::sqrt(dot(v, v))) * v; }

Vec unpack(wire::Point point) { return {point.x, point.y, point.z}; }

void near(wire::Point point, Vec expected, std::string_view label) {
    constexpr double tolerance = 4.0e-6;
    const auto actual = unpack(point);
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!(std::abs(actual[i] - expected[i]) <= tolerance)) {
            std::cerr << label << " component " << i << " was " << actual[i] << ", expected "
                      << expected[i] << '\n';
            throw std::runtime_error("Metal surface detail differs from CPU oracle");
        }
    }
}

void verify(const wire::Point *output) {
    for (unsigned i = 0; i < 7; ++i) {
        near(output[9 * i + 7], {1, 1, 1}, "surface probe status");
    }
    near(output[0], {0, 0, 1}, "unmodified triangle shading normal");
    near(output[1], {0, 6, 0}, "transformed authored tangent");
    near(output[2], {-3, 0, 0}, "shape-frame bitangent before placement");
    near(output[3], {0, 0, 0}, "constant triangle normal derivative u");
    near(output[4], {0, 0, 0}, "constant triangle normal derivative v");
    near(output[5], unit({0, 0.5, 1}), "tangent-space normal rotation");
    near(output[6], unit(cross({0, 6, 2}, {-3, 0, 2.25})), "bump tangent offset frame");

    near(output[9], {0, 0, 1}, "mirrored placement keeps geometric orientation");
    near(output[10], {0, 6, 0}, "mirrored transformed tangent");
    near(output[11], {3, 0, 0}, "mirrored transformed bitangent");
    near(output[14], unit({0, 0.5, 1}), "mirrored tangent-space normal");
    near(output[15], -1.0 * unit(cross({0, 6, 2}, {3, 0, 3.75})), "mirrored bump hemisphere");

    const Vec raw_normal{0, 0.25 / 6, 1.25 / 4};
    near(output[18], unit(raw_normal), "interpolated normals inverse transpose");
    near(output[21], {0, 1.0 / 6, 0.25}, "authored normal derivative through both transforms");
    near(output[22], {0, 0, 0}, "independent zero authored normal derivative");

    const Vec a{-0.25, -0.125, 1};
    const auto n = unit(a);
    const auto derivative = [&](Vec da) {
        return (1.0 / std::sqrt(dot(a, a))) * (da - dot(da, n) * n);
    };
    near(output[27], n, "curved patch geometric normal");
    near(output[28], {1, 0, 0.25}, "curved patch dpdu");
    near(output[29], {0, 1, 0.125}, "curved patch dpdv");
    near(output[30], derivative({0, -0.5, 0}), "analytical patch dndu");
    near(output[31], derivative({-0.5, 0, 0}), "analytical patch dndv");

    near(output[36], unit({0, -0.125, -1.125}), "reverse-oriented patch interpolated normal");
    near(output[39], {0, -0.5, -0.5}, "reverse-oriented patch normal derivative u");
    near(output[40], {0, 0.25, 0.25}, "reverse-oriented patch normal derivative v");
    require(std::abs(dot(unpack(output[37]), unpack(output[36]))) < 2e-6 &&
                std::abs(dot(unpack(output[38]), unpack(output[36]))) < 2e-6,
            "Rotated patch frame must remain tangent to its authored shading normal");
    const Vec placed_normal = unit({10, 0, -2});
    near(output[45], placed_normal, "placement face-forwards shading to geometry");
    near(output[53], unit({10, 0, 1}), "definition geometry determines placed orientation");
    near(output[50], unit(Vec{placed_normal[0], 0.5, placed_normal[2]}),
         "map uses the shape-authoritative placed frame");
    near(output[55], {10, 0, 0}, "large authored tangent is reduced");
    near(output[56], {0, 10, 0}, "large tangent reduction retains frame ratio");
    near(output[60], unit(cross({10, 0, 4.5}, {0, 10, 3})),
         "bump uses reduced authored tangent scale");
}

std::array<wire::SurfaceTransform, 7> transforms() {
    std::array<wire::SurfaceTransform, 7> xfs{};
    const auto diagonal = [](float(&matrix)[9], float x, float y, float z) {
        matrix[0] = x;
        matrix[4] = y;
        matrix[8] = z;
    };
    for (auto &xf : xfs) {
        xf.vertex_count = 4;
        diagonal(xf.shape, 1, 1, 1);
        diagonal(xf.shape_normal, 1, 1, 1);
        diagonal(xf.placement, 1, 1, 1);
        diagonal(xf.placement_normal, 1, 1, 1);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        auto &xf = xfs[i];
        diagonal(xf.shape, 2, 3, 1);
        diagonal(xf.shape_normal, 0.5f, 1.0f / 3, 1);
        const float sx = i == 1 ? -1.0f : 1.0f;
        diagonal(xf.placement, sx, 2, 4);
        diagonal(xf.placement_normal, sx, 0.5f, 0.25f);
        xf.has_tangents = 1;
    }
    xfs[2].has_normals = xfs[4].has_normals = 1;
    xfs[4].authored_reverse = xfs[4].shape_reverse_winding = 1;
    xfs[5].has_normals = xfs[5].has_tangents = 1;
    xfs[5].vertex_begin = 4;
    xfs[5].vertex_count = 3;
    diagonal(xfs[5].placement, 0.1f, 1, 1);
    diagonal(xfs[5].placement_normal, 10, 1, 1);
    xfs[6].has_tangents = 1;
    xfs[6].vertex_begin = 7;
    xfs[6].vertex_count = 3;
    return xfs;
}

void run(id<MTLDevice> device, const char *path) {
    NSError *error = nil;
    const auto url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    const auto library = [device newLibraryWithURL:url error:&error];
    require(library != nil,
            error ? error.localizedDescription.UTF8String : "Could not open library");
    const auto kernel = [library newFunctionWithName:@"rypath_surface_detail_probe"];
    require(kernel != nil, "Missing surface detail kernel");
    const auto pipeline = [device newComputePipelineStateWithFunction:kernel error:&error];
    require(pipeline != nil,
            error ? error.localizedDescription.UTF8String : "Could not load surface pipeline");
    constexpr std::array vertices{
        wire::SurfaceVertex{{0, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        wire::SurfaceVertex{{1, 0, 0}, {0, 1, 2}, {0, 1, 0}},
        wire::SurfaceVertex{{0, 1, 0}, {0, 0, 1}, {0, 1, 0}},
        wire::SurfaceVertex{{1, 1, 0.5}, {0, 0, 1}, {0, 1, 0}},
        wire::SurfaceVertex{{0, 0, 0}, {-1, 0, 2}, {0, 1, 0}},
        wire::SurfaceVertex{{1, 0, -1}, {-1, 0, 2}, {0, 1, 0}},
        wire::SurfaceVertex{{0, 1, 0}, {-1, 0, 2}, {0, 1, 0}},
        wire::SurfaceVertex{{0, 0, 0}, {0, 0, 1}, {1e9f, 0, 0}},
        wire::SurfaceVertex{{1, 0, 0}, {0, 0, 1}, {1e9f, 0, 0}},
        wire::SurfaceVertex{{0, 1, 0}, {0, 0, 1}, {1e9f, 0, 0}},
    };
    const auto xfs = transforms();
    const auto source = [device newBufferWithBytes:vertices.data()
                                            length:sizeof(vertices)
                                           options:MTLResourceStorageModeShared];
    const auto matrix = [device newBufferWithBytes:xfs.data()
                                            length:sizeof(xfs)
                                           options:MTLResourceStorageModeShared];
    const auto output = [device newBufferWithLength:9 * xfs.size() * sizeof(wire::Point)
                                            options:MTLResourceStorageModeShared];
    const auto queue = [device newCommandQueue];
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(source != nil && matrix != nil && output != nil && encoder != nil,
            "Could not allocate Metal surface probe resources");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:source offset:0 atIndex:0];
    [encoder setBuffer:matrix offset:0 atIndex:1];
    [encoder setBuffer:output offset:0 atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(xfs.size(), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "Metal surface probe failed");
    verify(static_cast<const wire::Point *>(output.contents));
}
} // namespace

int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            require(argc == 2, "Expected a Metal shader library path");
            const auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP surface detail probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Surface detail probe requires a Metal device");
            run(device, argv[1]);
        } catch (const std::exception &error) {
            std::cerr << "Surface detail probe: " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
