// SPDX-License-Identifier: GPL-3.0-only
#include "metal_frame_access.hpp"
#include "reconstruction_camera.hpp"
#include "reconstruction_guide_wire.h"
#include "rgb_spectrum.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using namespace rypath;
namespace gw = reconstruction_guide_wire;
namespace rc = reconstruction;
void need(bool x, const std::string &why) {
    if (!x)
        throw std::runtime_error(why);
}
void close(double actual, double expected, double tolerance, const std::string &label) {
    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
         label + " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}
std::string error(NSError *e) {
    return e ? e.localizedDescription.UTF8String : "No Metal diagnostic";
}
unsigned width = 5, height = 3, pixels = width * height;
constexpr std::array<float, 12> rgb_matrix{3.240444291419f, -1.537133830545f, -.498529890988f, 0,
                                           -.969266752560f, 1.876012242980f,  .041556048487f,  0,
                                           .055643519672f,  -.204026238798f,  1.057226873771f, 0};
struct RasterPose {
    float origin_x = 0;
    float yaw = 0;
};
RasterCamera raster(RasterPose pose = {}) {
    const auto [origin_x, yaw] = pose;
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {
        {origin_x, 0, 0}, {-.3f * c + s, -.3f, .3f * s + c}, {.6f * c, 0, -.6f * s}, {0, .6f, 0}};
}
RenderScene base(const RasterCamera &camera) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = camera;
    scene.environment.fill(.5f);
    scene.spectral_transport = SpectralTransport::packet;
    scene.max_depth = 2;
    SpectralMaterial material;
    material.kind = SpectralMaterialKind::constant;
    material.constant = .37f;
    scene.materials.push_back(material);
    return scene;
}
// Test geometry uses an explicit depth followed by its material index.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void plane(RenderScene &s, float z, unsigned material = 0) {
    GeometryMesh m;
    m.positions = {{-16, -16, z}, {16, -16, z}, {16, 16, z}, {-16, 16, z}};
    m.triangles = {{0, 2, 1}, {0, 3, 2}};
    const auto i = unsigned(s.geometry.meshes.size());
    s.geometry.meshes.push_back(std::move(m));
    s.geometry.instances.push_back({i, {}});
    s.instance_materials.push_back(material);
    s.instance_emissions.push_back(no_area_emission);
    s.instance_surface_transforms.emplace_back();
}
rc::Camera camera(const RasterCamera &r, double near = .1) {
    auto b = rc::Camera::build(r, {width, height, near, {}});
    if (!b.camera)
        throw std::runtime_error(b.diagnostic);
    return *b.camera;
}
std::array<gw::SpectralNode, 16> nodes() {
    std::array<gw::SpectralNode, 16> out{};
    std::array<double, 3> white{};
    for (unsigned i = 0; i < 16; ++i) {
        const float lambda = 360.f + (float(i) + .5f) * (470.f / 16);
        const auto cie = pbrt_cie_xyz(lambda);
        const auto d65 = pbrt_d65()[std::size_t(std::round(lambda) - 360)];
        std::array<double, 3> weight{};
        for (unsigned c = 0; c < 3; ++c) {
            for (unsigned k = 0; k < 3; ++k)
                weight[c] += rgb_matrix[c * 4 + k] * cie[k] * d65;
            white[c] += weight[c];
        }
        out[i] = {lambda, float(weight[0]), float(weight[1]), float(weight[2])};
    }
    for (auto &n : out) {
        n.r = float(n.r / white[0]);
        n.g = float(n.g / white[1]);
        n.b = float(n.b / white[2]);
    }
    return out;
}
gw::Config config(const rc::Camera &current, const rc::Camera &previous) {
    gw::Config c{};
    c.width = width;
    c.height = height;
    c.model = render_wire::pbrt_model;
    c.previous_valid = 1;
    c.spectral_nodes = 16;
    std::copy(current.world_to_view().begin(), current.world_to_view().end(),
              c.current_world_to_view);
    std::copy(current.view_to_clip().begin(), current.view_to_clip().end(), c.current_view_to_clip);
    std::copy(previous.world_to_view().begin(), previous.world_to_view().end(),
              c.previous_world_to_view);
    std::copy(previous.view_to_clip().begin(), previous.view_to_clip().end(),
              c.previous_view_to_clip);
    for (unsigned i = 0; i < 3; ++i) {
        c.current_camera_to_matrix_origin[i] =
            float(current.origin()[i] - current.matrix_origin()[i]);
        c.previous_camera_to_matrix_origin[i] =
            float(current.origin()[i] - previous.matrix_origin()[i]);
    }
    std::copy(rgb_matrix.begin(), rgb_matrix.end(), c.xyz_to_rgb);
    return c;
}
float half(unsigned short h) {
    const unsigned exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value;
    if (exponent == 0)
        value = std::ldexp(float(mantissa), -24);
    else if (exponent == 31)
        value = mantissa ? std::numeric_limits<float>::quiet_NaN()
                         : std::numeric_limits<float>::infinity();
    else
        value = std::ldexp(1 + float(mantissa) / 1024, int(exponent) - 15);
    return h & 0x8000 ? -value : value;
}
struct Captured {
    gw::Counters counters{};
    std::array<std::vector<float>, 9> texture;
    std::vector<std::uint32_t> identity;
    FrameCapture raw_before, raw_after;
};
Captured guide(const RenderScene &scene, gw::Config cfg, const std::string &library,
               bool color_overflow = false, bool padded_color = false) {
    const auto diagnostic = validate_render_scene(scene);
    need(diagnostic.empty(), "Guide fixture: " + diagnostic);
    FrameConfig settings;
    settings.width = width;
    settings.height = height;
    settings.estimator = FrameEstimator::independent;
    auto built = build_frame_session(scene, settings, library);
    need(built.status == GeometryStatus::success && built.session, built.diagnostic);
    auto submitted = built.session->submit({.sample_index = (1ull << 32) + 29});
    need(submitted.status == GeometryStatus::success && submitted.frame, submitted.diagnostic);
    need(submitted.frame->wait().status == FrameStatus::complete, "Raw guide source completion");
    Captured out;
    out.raw_before = submitted.frame->capture();
    need(out.raw_before.status == GeometryStatus::success, "Raw guide source capture");
    auto acquired = MetalFrameAccess::acquire(*submitted.frame);
    need(acquired.status == FrameStatus::complete && acquired.lease, acquired.diagnostic);
    auto &lease = *acquired.lease;
    std::array<std::unique_ptr<MetalFramePipeline>, 4> pipeline;
    const std::array<NSString *, 4> names{
        @"rypath_reconstruction_guide_contract", @"rypath_reconstruction_guide_reset",
        @"rypath_reconstruction_guide_surface", @"rypath_reconstruction_guide_color"};
    for (unsigned i = 0; i < 4; ++i) {
        auto p = lease.create_guide_pipeline(names[i]);
        need(p.status == GeometryStatus::success && p.pipeline, p.diagnostic);
        pipeline[i] = std::move(p.pipeline);
    }
    auto device = lease.device();
    auto command = [lease.queue() commandBuffer];
    auto encoder = [command computeCommandEncoder];
    need(command != nil && encoder != nil, "Guide contract command");
    auto contract = [device newBufferWithLength:sizeof(gw::Contract)
                                        options:MTLResourceStorageModeShared];
    need(contract != nil, "Guide contract buffer");
    [encoder setComputePipelineState:pipeline[0]->pipeline()];
    [encoder setBuffer:contract offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    need(command.status == MTLCommandBufferStatusCompleted,
         "Guide contract execution: " + error(command.error));
    gw::Contract actual{};
    std::memcpy(&actual, contract.contents, sizeof(actual));
    need(actual.version == gw::contract && actual.config_bytes == 368 &&
             actual.counters_bytes == 64 && actual.node_bytes == 16 && actual.nodes == 16 &&
             actual.argument_count == 13,
         "Native reconstruction guide contract");
    const auto spectral = nodes();
    auto config_buffer = [device newBufferWithBytes:&cfg
                                             length:sizeof(cfg)
                                            options:MTLResourceStorageModeShared];
    auto node_buffer = [device newBufferWithBytes:spectral.data()
                                           length:sizeof(spectral)
                                          options:MTLResourceStorageModeShared];
    auto counters = [device newBufferWithLength:sizeof(gw::Counters)
                                        options:MTLResourceStorageModeShared];
    auto argument_encoder = pipeline[2]->argument_encoder();
    need(argument_encoder != nil, "Guide argument encoder");
    auto arguments = [device newBufferWithLength:argument_encoder.encodedLength
                                         options:MTLResourceStorageModeShared];
    need(config_buffer != nil && node_buffer != nil && counters != nil && arguments != nil,
         "Guide input buffers");
    const bool signed_color = (cfg.flags & gw::signed_temporal) != 0;
    const std::array<MTLPixelFormat, 9> format{signed_color ? MTLPixelFormatRGBA32Float
                                                            : MTLPixelFormatRGBA16Float,
                                               MTLPixelFormatR32Float,
                                               MTLPixelFormatRG16Float,
                                               MTLPixelFormatRGBA16Float,
                                               MTLPixelFormatRGBA16Float,
                                               MTLPixelFormatRGBA16Float,
                                               MTLPixelFormatR16Float,
                                               MTLPixelFormatR32Float,
                                               MTLPixelFormatRGBA32Uint};
    const std::array<unsigned, 9> channels{4, 1, 2, 4, 4, 4, 1, 1, 4};
    const auto component_bytes = [&](unsigned i) {
        return i == 1 || i >= 7 || (i == 0 && signed_color) ? 4u : 2u;
    };
    std::array<id<MTLTexture>, 9> texture{};
    std::array<id<MTLBuffer>, 9> readback{};
    std::array<NSUInteger, 9> row_bytes{};
    for (unsigned i = 0; i < 9; ++i) {
        auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format[i]
                                                                    width:width
                                                                   height:height
                                                                mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
        texture[i] = [device newTextureWithDescriptor:d];
        row_bytes[i] = (width * channels[i] * component_bytes(i) + 255) & ~NSUInteger(255);
        readback[i] = [device newBufferWithLength:row_bytes[i] * height
                                          options:MTLResourceStorageModeShared];
        need(texture[i] != nil && readback[i] != nil, "Guide texture/readback allocation");
        std::memset(readback[i].contents, 0, readback[i].length);
    }
    [argument_encoder setArgumentBuffer:arguments offset:0];
    [argument_encoder setBuffer:config_buffer offset:0 atIndex:gw::config_id];
    [argument_encoder setBuffer:lease.raw_xyz() offset:0 atIndex:gw::raw_xyz_id];
    [argument_encoder setBuffer:node_buffer offset:0 atIndex:gw::nodes_id];
    [argument_encoder setBuffer:counters offset:0 atIndex:gw::counters_id];
    for (unsigned i = 0; i < 9; ++i)
        [argument_encoder setTexture:texture[i] atIndex:gw::color_id + i];
    const auto resources = [&](id<MTLComputeCommandEncoder> e) {
        for (auto b : {config_buffer, node_buffer, lease.raw_xyz()})
            [e useResource:b usage:MTLResourceUsageRead];
        [e useResource:counters usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        for (auto t : texture)
            [e useResource:t usage:MTLResourceUsageWrite];
    };
    command = [lease.queue() commandBuffer];
    need(command != nil, "Guide command");
    auto initialize = [command blitCommandEncoder];
    need(initialize != nil, "Initialize guide outputs for explicit failure cases");
    for (unsigned i = 0; i < 9; ++i)
        [initialize copyFromBuffer:readback[i]
                      sourceOffset:0
                 sourceBytesPerRow:row_bytes[i]
               sourceBytesPerImage:row_bytes[i] * height
                        sourceSize:MTLSizeMake(width, height, 1)
                         toTexture:texture[i]
                  destinationSlice:0
                  destinationLevel:0
                 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [initialize endEncoding];
    encoder = [command computeCommandEncoder];
    need(encoder != nil, "Guide compute encoder");
    resources(encoder);
    [encoder setComputePipelineState:pipeline[1]->pipeline()];
    [encoder setBuffer:arguments offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    if (color_overflow) {
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        need(command.status == MTLCommandBufferStatusCompleted, "Counter setup command");
        gw::Counters seed{};
        seed.completed = UINT32_MAX - (pixels - 1);
        std::memcpy(counters.contents, &seed, sizeof(seed));
        command = [lease.queue() commandBuffer];
        encoder = [command computeCommandEncoder];
        need(command != nil && encoder != nil, "Color overflow command");
        resources(encoder);
    } else {
        need(lease.bind_guides(encoder, *pipeline[2], arguments).empty(),
             "Bind actual resident geometry for guides");
        [encoder dispatchThreads:MTLSizeMake(pixels, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    }
    const auto color_pipeline = pipeline[3]->pipeline();
    const auto color_threads =
        std::min<NSUInteger>(64, color_pipeline.maxTotalThreadsPerThreadgroup);
    [encoder setComputePipelineState:color_pipeline];
    [encoder setBuffer:arguments offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(pixels + (padded_color ? 17 : 0), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(color_threads, 1, 1)];
    [encoder endEncoding];
    auto blit = [command blitCommandEncoder];
    need(blit != nil, "Guide readback encoder");
    for (unsigned i = 0; i < 9; ++i)
        [blit copyFromTexture:texture[i]
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width, height, 1)
                            toBuffer:readback[i]
                   destinationOffset:0
              destinationBytesPerRow:row_bytes[i]
            destinationBytesPerImage:row_bytes[i] * height];
    [blit endEncoding];
    need(lease.retain_until_completed(command).empty(),
         "Pin frame through guide consumer completion");
    [command commit];
    [command waitUntilCompleted];
    need(command.status == MTLCommandBufferStatusCompleted,
         "Guide execution: " + error(command.error));
    std::memcpy(&out.counters, counters.contents, sizeof(out.counters));
    out.identity.resize(std::size_t(pixels) * 4);
    for (unsigned i = 0; i < 9; ++i) {
        out.texture[i].resize(std::size_t(pixels) * channels[i]);
        const auto *bytes = static_cast<const unsigned char *>(readback[i].contents);
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < std::size_t(width) * channels[i]; ++x) {
                if (i == 8) {
                    std::memcpy(&out.identity[y * width * 4 + x], bytes + y * row_bytes[i] + x * 4,
                                4);
                } else if (component_bytes(i) == 4) {
                    float value;
                    std::memcpy(&value, bytes + y * row_bytes[i] + x * 4, 4);
                    out.texture[i][y * width * channels[i] + x] = value;
                } else {
                    unsigned short h;
                    std::memcpy(&h, bytes + y * row_bytes[i] + x * 2, 2);
                    out.texture[i][y * width * channels[i] + x] = half(h);
                }
            }
    }
    out.raw_after = submitted.frame->capture();
    need(out.raw_after.status == GeometryStatus::success &&
             out.raw_after.raw_xyz == out.raw_before.raw_xyz,
         "Guide generation preserves raw XYZ bit for bit");
    return out;
}
void color_check(const Captured &out, const gw::Config &cfg) {
    for (unsigned i = 0; i < pixels; ++i) {
        for (unsigned c = 0; c < 3; ++c) {
            double rgb = 0;
            for (unsigned k = 0; k < 3; ++k)
                rgb += cfg.xyz_to_rgb[c * 4 + k] * double(out.raw_before.raw_xyz[i][k]);
            const bool signed_color = (cfg.flags & gw::signed_temporal) != 0;
            const auto expected = signed_color ? rgb : std::clamp(rgb, 0., 65504.);
            double magnitude = 0;
            for (unsigned k = 0; k < 3; ++k)
                magnitude +=
                    std::abs(cfg.xyz_to_rgb[c * 4 + k] * double(out.raw_before.raw_xyz[i][k]));
            const double tolerance =
                signed_color ? 32 * std::numeric_limits<float>::epsilon() * magnitude + 1e-6
                             : .001 + std::abs(expected) * .001;
            close(out.texture[0][4 * i + c], expected, tolerance,
                  "Raw XYZ guide-only RGB conversion/half packing");
        }
        close(out.texture[0][4 * i + 3], 1, 0, "Opaque color alpha");
    }
}
rc::Point hit_plane(const rc::Camera &cam, unsigned pixel, rc::Point normal, rc::Point point) {
    const auto ray = cam.center_ray(pixel % width, pixel / width);
    if (!ray)
        throw std::runtime_error("CPU center ray");
    double numerator = 0, denominator = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        numerator += normal[axis] * (point[axis] - ray->origin[axis]);
        denominator += normal[axis] * ray->direction[axis];
    }
    const double distance = numerator / denominator;
    return {ray->origin[0] + distance * ray->direction[0],
            ray->origin[1] + distance * ray->direction[1],
            ray->origin[2] + distance * ray->direction[2]};
}
struct SurfaceOracle {
    rc::Point normal;
    rc::Point plane_point;
    std::array<double, 3> diffuse;
    bool signed_guides = false;
    unsigned kind = render_wire::triangle_primitive;
    unsigned material = 0;
};
void surface_checks(const Captured &out, const rc::Camera &current, const rc::Camera &previous,
                    const SurfaceOracle &expected) {
    const auto &normal = expected.normal;
    const auto &plane_point = expected.plane_point;
    const auto &diffuse = expected.diffuse;
    need(out.counters.failure == 0 && out.counters.completed == pixels &&
             out.counters.background == 0,
         "Complete surface/color guide accounting");
    for (unsigned i = 0; i < pixels; ++i) {
        const auto point = hit_plane(current, i, normal, plane_point);
        const auto projected = current.project(point);
        const auto movement = rc::motion(current, previous, point);
        need(projected.status == rc::ProjectionStatus::valid,
             "Independent plane projection oracle");
        if (!movement.pixels)
            throw std::runtime_error("Independent plane motion oracle");
        close(out.texture[1][i], projected.depth, 1e-6, "Analytic plane reversed depth");
        if (expected.signed_guides) {
            const auto old_projection = previous.project(point);
            need(old_projection.status == rc::ProjectionStatus::valid,
                 "Independent previous-depth plane projection");
            close(out.texture[7][i], old_projection.depth, 1e-6,
                  "Expected previous projected surface depth");
            need(out.identity[4 * std::size_t(i)] == 0 &&
                     out.identity[4 * i + 2] == expected.kind &&
                     out.identity[4 * i + 3] == expected.material,
                 "Surface identity source/kind/resolved material");
            if (expected.kind == render_wire::triangle_primitive &&
                std::abs(point[0] - point[1]) > 1e-5)
                need(out.identity[4 * i + 1] == (point[1] < point[0] ? 0u : 1u),
                     "Independent plane triangle half-space identity");
            else
                need(out.identity[4 * i + 1] <=
                         (expected.kind == render_wire::triangle_primitive ? 1u : 0u),
                     "Surface primitive identity range");
        }
        for (unsigned axis = 0; axis < 3; ++axis) {
            close(out.texture[5][4 * i + axis], normal[axis], .002, "World-space geometric normal");
            close(out.texture[3][4 * i + axis], diffuse[axis], .001, "Node-weighted diffuse guide");
            close(out.texture[4][4 * i + axis], 0, 0, "Diffuse material zero specular guide");
        }
        close(out.texture[5][4 * i + 3], 1, 0, "Surface normal validity alpha");
        close(out.texture[6][i], 1, 0, "Diffuse roughness proxy");
        for (unsigned axis = 0; axis < 2; ++axis)
            close(out.texture[2][2 * i + axis], (*movement.pixels)[axis],
                  .002 + std::abs((*movement.pixels)[axis]) * .001,
                  "Current-to-previous motion versus CPU projection");
    }
}
std::array<double, 3> linear_color(bool dense) {
    std::array<double, 3> sum{}, white{};
    if (!dense) {
        for (const auto &n : nodes()) {
            const double rho = .2 + .6 * (n.wavelength - 360) / 470;
            const std::array<double, 3> w{n.r, n.g, n.b};
            for (unsigned c = 0; c < 3; ++c)
                sum[c] += w[c] * rho;
        }
        return sum;
    }
    for (unsigned wavelength = 360; wavelength <= 830; ++wavelength) {
        const auto cie = pbrt_cie_xyz(wavelength);
        const auto d65 = pbrt_d65()[wavelength - 360];
        const auto step = wavelength == 360 || wavelength == 830 ? .5 : 1.;
        for (unsigned c = 0; c < 3; ++c) {
            double weight = 0;
            for (unsigned k = 0; k < 3; ++k)
                weight += rgb_matrix[4 * c + k] * cie[k] * d65 * step;
            white[c] += weight;
            sum[c] += weight * (.2 + .6 * (wavelength - 360) / 470);
        }
    }
    for (unsigned c = 0; c < 3; ++c)
        sum[c] /= white[c];
    return sum;
}
void counter_sizes(const std::string &library) {
    for (const unsigned size : {1u, 31u, 32u, 33u}) {
        width = size;
        height = size == 1 ? 1 : 3;
        pixels = width * height;
        const auto r = raster();
        const auto cam = camera(r);
        auto cfg = config(cam, cam);
        // Force unambiguous negative/high witnesses for every nonzero sky sample.
        // Also dispatch padding so out-of-image lanes must contribute zero.
        std::fill(std::begin(cfg.xyz_to_rgb), std::end(cfg.xyz_to_rgb), 0.f);
        cfg.xyz_to_rgb[0] = -1.f;
        cfg.xyz_to_rgb[5] = 1e9f;
        for (const bool signed_color : {false, true}) {
            cfg.flags = signed_color ? gw::signed_temporal : 0u;
            const auto out = guide(base(r), cfg, library, false, true);
            need(out.counters.failure == 0 && out.counters.completed == pixels &&
                     out.counters.color_negative == pixels && out.counters.color_high == pixels,
                 "Exact color counters across partial and full SIMD groups");
            color_check(out, cfg);
        }
        cfg.xyz_to_rgb[0] = std::numeric_limits<float>::infinity();
        const auto failed = guide(base(r), cfg, library, false, true);
        need((failed.counters.failure & gw::invalid_value) != 0 && failed.counters.completed == 0 &&
                 failed.counters.color_negative == 0 && failed.counters.color_high == 0,
             "Nonfinite color contributes no success counters in a partial group");
    }
}
void cases(const std::string &library) {
    const auto original = raster(), moved = raster({.origin_x = .3f, .yaw = .2f});
    const auto old_camera = camera(original), current = camera(moved);
    auto scene = base(moved);
    plane(scene, 3);
    auto cfg = config(current, old_camera);
    const auto flat = guide(scene, cfg, library);
    surface_checks(flat, current, old_camera,
                   {.normal = {0, 0, -1}, .plane_point = {0, 0, 3}, .diffuse = {.37, .37, .37}});
    color_check(flat, cfg);
    // Keep the legacy half/clamp path above, and independently qualify signed
    // float conversion plus the extra temporal correspondence attachments.
    for (const auto pose : {RasterPose{}, RasterPose{.origin_x = .3f}, RasterPose{.yaw = .2f},
                            RasterPose{.origin_x = .3f, .yaw = .2f}}) {
        const auto r = raster(pose);
        const auto now = camera(r);
        auto signed_scene = base(r);
        signed_scene.materials.push_back(signed_scene.materials.front());
        plane(signed_scene, 3, 1);
        auto signed_cfg = config(now, old_camera);
        signed_cfg.flags |= gw::signed_temporal;
        const auto out = guide(signed_scene, signed_cfg, library);
        surface_checks(out, now, old_camera,
                       {.normal = {0, 0, -1},
                        .plane_point = {0, 0, 3},
                        .diffuse = {.37, .37, .37},
                        .signed_guides = true,
                        .material = 1});
        color_check(out, signed_cfg);
    }
    auto blue = base(original);
    blue.environment.fill(0);
    for (unsigned wavelength = 430; wavelength <= 470; ++wavelength)
        blue.environment[wavelength - 360] = 1;
    auto signed_cfg = config(old_camera, old_camera);
    signed_cfg.flags |= gw::signed_temporal;
    const auto signed_blue = guide(blue, signed_cfg, library);
    color_check(signed_blue, signed_cfg);
    need(signed_blue.counters.color_negative > 0 &&
             std::ranges::any_of(signed_blue.texture[0], [](float x) { return x < 0; }),
         "Signed spectral color must retain an actual negative RGB witness");
    // Background correspondence ignores camera translation but retains rotation.
    for (const auto source : {raster({.origin_x = .7f}), moved}) {
        const auto now = camera(source);
        auto c = config(now, old_camera);
        c.flags |= gw::signed_temporal;
        const auto out = guide(base(source), c, library);
        need(out.counters.failure == 0 && out.counters.completed == pixels &&
                 out.counters.background == pixels,
             "Background guide accounting");
        for (unsigned i = 0; i < pixels; ++i) {
            const auto ray = now.center_ray(i % width, i / width);
            if (!ray)
                throw std::runtime_error("Background CPU ray");
            const auto motion = rc::background_motion(now, old_camera, ray->direction);
            if (!motion.pixels)
                throw std::runtime_error("Background CPU projection");
            close(out.texture[1][i], 0, 0, "Infinite background zero depth");
            close(out.texture[7][i], 0, 0, "Infinite background previous depth");
            for (unsigned component = 0; component < 4; ++component)
                need(out.identity[4 * i + component] == UINT32_MAX, "Sky identity sentinel");
            close(out.texture[5][4 * i + 3], 0, 0, "Background normal invalid alpha");
            for (unsigned axis = 0; axis < 2; ++axis)
                close(out.texture[2][2 * i + axis], (*motion.pixels)[axis],
                      .002 + std::abs((*motion.pixels)[axis]) * .001,
                      "Rotation-only background motion");
        }
        color_check(out, c);
    }
    auto passthrough = base(original);
    plane(passthrough, 4);
    SpectralMaterial null;
    null.kind = SpectralMaterialKind::constant;
    null.scattering = ScatteringKind::null_interface;
    passthrough.materials.push_back(null);
    plane(passthrough, 1, 1);
    plane(passthrough, 2);
    texture_graph_wire::Node zero;
    passthrough.texture_graph.nodes.push_back(zero);
    passthrough.instance_alpha_roots.assign(3, UINT32_MAX);
    passthrough.instance_alpha_roots[2] = 0;
    passthrough.instance_invisible_emitters.assign(3, 0);
    passthrough.instance_invisible_emitters[2] = 1;
    passthrough.area_emissions.emplace_back();
    passthrough.instance_emissions[2] = 0;
    passthrough.instance_texture_frames.resize(3);
    const auto &surface = passthrough.instance_surface_transforms[2];
    need(prepare_texture_triangle_frames(passthrough.geometry.meshes[2], surface.shape,
                                         surface.placement, passthrough.instance_texture_frames[2])
             .empty(),
         "Alpha fixture derivative frames");
    const auto fixed = config(old_camera, old_camera);
    const auto skipped = guide(passthrough, fixed, library);
    surface_checks(skipped, old_camera, old_camera,
                   {.normal = {0, 0, -1}, .plane_point = {0, 0, 4}, .diffuse = {.37, .37, .37}});
    need(skipped.counters.passthrough >= pixels,
         "Null surfaces count accepted passthrough callbacks");
    auto patch = base(original);
    GeometryMesh mesh;
    mesh.positions = {{-2, -2, 0}, {2, -2, 0}, {-2, 2, 0}, {2, 2, 0}};
    mesh.patches = {{0, 1, 2, 3}};
    patch.geometry.meshes.push_back(mesh);
    const float angle = .3f, c = std::cos(angle), s = std::sin(angle);
    GeometryTransform transform{{2 * c, 0, s, .25f, 0, .5f, 0, 0, -2 * s, 0, c, 4}};
    patch.geometry.instances.push_back({0, transform});
    patch.instance_materials.push_back(0);
    const auto curved = guide(patch, fixed, library);
    surface_checks(curved, old_camera, old_camera,
                   {.normal = {s, 0, c}, .plane_point = {.25, 0, 4}, .diffuse = {.37, .37, .37}});
    auto spectrum = base(original);
    spectrum.materials[0].kind = SpectralMaterialKind::sampled;
    spectrum.materials[0].constant = 0;
    spectrum.materials[0].knots = {{360, .2f}, {830, .8f}};
    plane(spectrum, 3);
    const auto spectral = guide(spectrum, fixed, library);
    const auto expected = linear_color(false), dense = linear_color(true);
    surface_checks(spectral, old_camera, old_camera,
                   {.normal = {0, 0, -1}, .plane_point = {0, 0, 3}, .diffuse = expected});
    for (unsigned channel = 0; channel < 3; ++channel)
        close(expected[channel], dense[channel], .03,
              "Sixteen-node guide approximation versus independent dense D65 spectral integral");
    need(!rc::Camera::build(original, {width, height, 0, {}}).camera,
         "Zero near plane rejected by CPU camera");
    const auto near_camera = camera(original, 4);
    const auto invalid = guide(spectrum, config(near_camera, old_camera), library);
    need((invalid.counters.failure & gw::invalid_projection) != 0 &&
             invalid.counters.completed == 0,
         "Guide near-plane violation fails visibly instead of clamping depth");
    const auto overflow = guide(scene, cfg, library, true);
    need((overflow.counters.failure & gw::counter_overflow) != 0,
         "Color completion-counter overflow fails visibly");
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Usage: reconstruction-guide-tests LIBRARY.metallib");
            const auto device = query_metal_device();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
            if (device.status == MetalStatus::no_device)
                return 77;
#endif
            need(device.status == MetalStatus::success, device.diagnostic);
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
            if (!device.device.compute_ray_tracing)
                return 77;
#endif
            need(device.device.compute_ray_tracing, "Guide tests require compute ray tracing");
            cases(argv[1]);
            counter_sizes(argv[1]);
            std::cout << "Reconstruction guide native validation passed\n";
            return 0;
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
