// SPDX-License-Identifier: GPL-3.0-only

#include "render_wire.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
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
Vec operator+(Vec a, Vec b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
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
    for (unsigned i = 0; i < 8; ++i) {
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
    near(output[9 * 7 + 1], {-1, 0, 0}, "mirrored UV dpdu");
    near(output[9 * 7 + 2], {0, 1, 0}, "mirrored UV dpdv");
    for (unsigned i = 0; i < 8; ++i) {
        const auto *frame = output + 9 * i;
        const auto *retained = output + 9 * 8 + 8 * i;
        const Vec n = unpack(frame[0]), u = unpack(frame[1]), v = unpack(frame[2]);
        const Vec geometric = unpack(frame[8]);
        const auto face_forward = [&](Vec mapped) {
            return dot(mapped, geometric) < 0 ? -1.0 * mapped : mapped;
        };
        // Double tangent-plane projection, independent of device normalization.
        const Vec tangent = unit(u);
        const Vec mapped_n = face_forward(unit(0.5 * tangent + n));
        const Vec mapped_u = unit(tangent - dot(tangent, mapped_n) * mapped_n);
        const Vec mapped_v = cross(mapped_n, mapped_u);
        near(retained[0], mapped_n, "retained mapped normal");
        near(retained[1], mapped_u, "retained mapped dpdu projection");
        near(retained[2], mapped_v, "retained mapped dpdv orientation");
        near(retained[0], unpack(frame[5]), "map Frame output preserves original normal");
        near(retained[6], unpack(frame[5]), "map pointer call preserves original normal");
        // h(u,v,p)=2u+3v+p.x/4 is linear: exact partials include the
        // displacement's spatial derivative and height times normal derivatives.
        constexpr double height = 2.0;
        const Vec bumped_u = u + (2 + 0.25 * u[0]) * n + height * unpack(frame[3]);
        const Vec bumped_v = v + (3 + 0.25 * v[0]) * n + height * unpack(frame[4]);
        const Vec bumped_n = face_forward(unit(cross(bumped_u, bumped_v)));
        near(retained[3], bumped_n, "retained bumped normal");
        near(retained[4], bumped_u, "retained bumped dpdu partial");
        near(retained[5], bumped_v, "retained bumped dpdv partial");
        near(retained[3], unpack(frame[6]), "bump Frame output preserves original normal");
        near(retained[7], unpack(frame[6]), "bump pointer call preserves original normal");
        const auto orthogonal = [](Vec a, Vec b) { return std::abs(dot(unit(a), unit(b))) < 4e-6; };
        require(orthogonal(unpack(retained[0]), unpack(retained[1])) &&
                    orthogonal(unpack(retained[0]), unpack(retained[2])) &&
                    orthogonal(unpack(retained[1]), unpack(retained[2])),
                "Mapped frame must be orthogonal");
        require(orthogonal(unpack(retained[3]), unpack(retained[4])) &&
                    orthogonal(unpack(retained[3]), unpack(retained[5])),
                "Bump partials must remain tangent to the bumped normal");
    }
    std::cout << "Surface detail: 8 retained normal/bump frames, projected tangents and exact "
                 "displacement partials\n";
}

std::array<wire::SurfaceTransform, 8> transforms() {
    std::array<wire::SurfaceTransform, 8> xfs{};
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

void verify_scattering_frames(id<MTLDevice> device, id<MTLLibrary> library) {
    using Float4 = std::array<float, 4>;
    struct Case {
        Float4 normal, tangent;
        bool valid;
    };
    const auto bits = [](std::array<std::uint32_t, 3> words) {
        return Float4{std::bit_cast<float>(words[0]), std::bit_cast<float>(words[1]),
                      std::bit_cast<float>(words[2]), 0};
    };
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array cases{
        Case{{0, 0, 2, 0}, {3, 0, 0, 0}, true},
        Case{{0, 1, 0, 0}, {0, 0, -2, 0}, true},
        Case{{0, 0, 1, 0}, {1, 0, .125f, 0}, true},
        Case{{1, 2, 3, 0}, {2, -1, .1f, 0}, true},
        Case{{0, 0, 1e-30f, 0}, {1e30f, 0, 0, 0}, true},
        // Exact binary32 inputs from the authored Pavilion day/night failures.
        Case{bits({0x3f18c4c0, 0xbbb32205, 0x3f4d6a8c}), bits({0xbb22f179, 0x3ad1ebe7, 0x3af3cc6f}),
             true},
        Case{bits({0xbef3db65, 0xbe9970b5, 0x3f539efb}), bits({0xc351dbfd, 0x426fac99, 0xc2c662e4}),
             true},
        Case{{0, 0, 0, 0}, {1, 0, 0, 0}, false},
        Case{{0, 0, 1, 0}, {0, 0, 0, 0}, false},
        Case{{inf, 0, 1, 0}, {1, 0, 0, 0}, false},
        Case{{0, 0, 1, 0}, {1, inf, 0, 0}, false},
        Case{{nan, 0, 1, 0}, {1, 0, 0, 0}, false},
        Case{{0, 0, 1, 0}, {nan, 0, 0, 0}, false},
        Case{{0, 0, 1, 0}, {0, 0, 2, 0}, false},
        Case{{0, 0, 1, 0}, {0, 0, -2, 0}, false},
        Case{{1, 2, 3, 0}, {2, 4, 6, 0}, false},
        Case{{1, 2, 3, 0}, {-2, -4, -6, 0}, false},
        Case{{1, 2, 3, 0}, {1, 2, std::nextafter(3.0f, 4.0f), 0}, false},
    };
    std::array<Float4, 2 * cases.size()> inputs{};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        inputs[2 * i] = cases[i].normal;
        inputs[2 * i + 1] = cases[i].tangent;
    }
    NSError *error = nil;
    const auto kernel = [library newFunctionWithName:@"rypath_scattering_frame_probe"];
    require(kernel != nil, "Missing scattering frame kernel");
    const auto pipeline = [device newComputePipelineStateWithFunction:kernel error:&error];
    require(pipeline != nil,
            error ? error.localizedDescription.UTF8String : "Could not load frame pipeline");
    const auto input = [device newBufferWithBytes:inputs.data()
                                           length:sizeof(inputs)
                                          options:MTLResourceStorageModeShared];
    [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
        [device newBufferWithLength:(3 * cases.size() + 1) * sizeof(Float4)
                            options:MTLResourceStorageModeShared];
    const auto queue = [device newCommandQueue];
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(input != nil && output != nil && encoder != nil,
            "Could not allocate scattering frame resources");
    auto *values = static_cast<Float4 *>(output.contents);
    const Float4 sentinel{12345, 12345, 12345, 12345};
    values[3 * cases.size()] = sentinel;
    const auto count = static_cast<std::uint32_t>(cases.size());
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(cases.size() + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "Scattering frame probe failed");
    require(values[3 * cases.size()] == sentinel, "Scattering frame output guard changed");
    const auto vector = [](Float4 value) { return Vec{value[0], value[1], value[2]}; };
    const auto close = [](Vec actual, Vec expected) {
        for (unsigned component = 0; component < 3; ++component)
            require(std::isfinite(actual[component]) &&
                        std::abs(actual[component] - expected[component]) < 4e-6,
                    "Scattering frame direction differs from independent oracle");
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        for (unsigned axis = 0; axis < 3; ++axis)
            require(values[3 * i + axis][3] == (cases[i].valid ? 1.0f : 0.0f),
                    "Scattering frame accepted or rejected the wrong input");
        if (!cases[i].valid)
            continue;
        const Vec x = vector(values[3 * i]), y = vector(values[3 * i + 1]),
                  z = vector(values[3 * i + 2]);
        const Vec n = unit(vector(cases[i].normal));
        // Double cross products give an independent tangent-plane oracle.
        const Vec expected_y = unit(cross(n, vector(cases[i].tangent)));
        close(z, n);
        close(y, expected_y);
        close(x, cross(expected_y, n));
        for (const auto &axis : {x, y, z})
            require(std::abs(dot(axis, axis) - 1) < 4e-6, "Scattering frame axis is not unit");
        require(std::abs(dot(x, y)) < 4e-6 && std::abs(dot(x, z)) < 4e-6 &&
                    std::abs(dot(y, z)) < 4e-6,
                "Scattering frame axes are not orthogonal");
        close(cross(x, y), z);
        for (const Vec direction : {Vec{.2, -.7, .6}, Vec{4, -3, 2}}) {
            const Vec local{dot(x, direction), dot(y, direction), dot(z, direction)};
            close(local[0] * x + local[1] * y + local[2] * z, direction);
            const Vec world = direction[0] * x + direction[1] * y + direction[2] * z;
            close({dot(x, world), dot(y, world), dot(z, world)}, direction);
        }
    }
    std::cout << "Scattering frames: " << cases.size()
              << " axis, skew, Pavilion and invalid cases; direction/round-trip checks\n";
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
    const auto output = [device newBufferWithLength:17 * xfs.size() * sizeof(wire::Point)
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
    verify_scattering_frames(device, library);
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
