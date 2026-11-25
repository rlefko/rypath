// SPDX-License-Identifier: GPL-3.0-only
#include "metal_frame_access.hpp"
#include "reconstruction_camera.hpp"
#include "reconstruction_guide_wire.h"
#include "rgb_spectrum.hpp"
#include "texture_footprint.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
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
std::string case_context;
void need(bool x, const std::string &why) {
    if (!x)
        throw std::runtime_error(case_context.empty() ? why : case_context + ": " + why);
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
    std::array<std::vector<float>, 13> texture;
    std::vector<std::uint32_t> identity, continuity;
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
    need(lease.reconstruction_groups() == nil,
         "Ordinary scene/frame preparation must not build continuity");
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
             actual.counters_bytes == sizeof(gw::Counters) && actual.node_bytes == 16 &&
             actual.nodes == 16 && actual.argument_count == gw::continuity_id + 1,
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
    const std::array<MTLPixelFormat, 13> format{signed_color ? MTLPixelFormatRGBA32Float
                                                             : MTLPixelFormatRGBA16Float,
                                                MTLPixelFormatR32Float,
                                                MTLPixelFormatRG16Float,
                                                MTLPixelFormatRGBA16Float,
                                                MTLPixelFormatRGBA16Float,
                                                MTLPixelFormatRGBA16Float,
                                                MTLPixelFormatR16Float,
                                                MTLPixelFormatR32Float,
                                                MTLPixelFormatRGBA32Uint,
                                                MTLPixelFormatRGBA32Float,
                                                MTLPixelFormatRGBA32Float,
                                                MTLPixelFormatR32Uint,
                                                MTLPixelFormatR32Uint};
    const std::array<unsigned, 13> channels{4, 1, 2, 4, 4, 4, 1, 1, 4, 4, 4, 1, 1};
    const auto component_bytes = [&](unsigned i) {
        return i == 1 || i >= 7 || (i == 0 && signed_color) ? 4u : 2u;
    };
    std::array<id<MTLTexture>, 13> texture{};
    std::array<id<MTLBuffer>, 13> readback{};
    std::array<NSUInteger, 13> row_bytes{};
    for (unsigned i = 0; i < 13; ++i) {
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
    for (unsigned i = 0; i < 13; ++i)
        [argument_encoder setTexture:texture[i] atIndex:gw::color_id + i];
    need(lease.prepare_reconstruction_groups().empty(),
         "Lazy reconstruction continuity preparation");
    auto continuity = lease.reconstruction_groups();
    out.continuity.resize(continuity.length / sizeof(std::uint32_t));
    std::memcpy(out.continuity.data(), continuity.contents, continuity.length);
    need(lease.prepare_reconstruction_groups().empty() &&
             lease.reconstruction_groups() == continuity,
         "Continuity metadata must be prepared once per immutable scene");
    [argument_encoder setBuffer:lease.reconstruction_groups() offset:0 atIndex:gw::continuity_id];
    const auto resources = [&](id<MTLComputeCommandEncoder> e) {
        for (auto b : {config_buffer, node_buffer, lease.raw_xyz(), lease.reconstruction_groups()})
            [e useResource:b usage:MTLResourceUsageRead];
        [e useResource:counters usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        for (auto t : texture)
            [e useResource:t usage:MTLResourceUsageWrite];
    };
    command = [lease.queue() commandBuffer];
    need(command != nil, "Guide command");
    auto initialize = [command blitCommandEncoder];
    need(initialize != nil, "Initialize guide outputs for explicit failure cases");
    for (unsigned i = 0; i < 13; ++i)
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
    for (unsigned i = 0; i < 13; ++i)
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
    for (unsigned i = 0; i < 13; ++i) {
        out.texture[i].resize(std::size_t(pixels) * channels[i]);
        const auto *bytes = static_cast<const unsigned char *>(readback[i].contents);
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < std::size_t(width) * channels[i]; ++x) {
                if (i == 8) {
                    std::memcpy(&out.identity[y * width * 4 + x], bytes + y * row_bytes[i] + x * 4,
                                4);
                } else if (i == 12) {
                    std::uint32_t value;
                    std::memcpy(&value, bytes + y * row_bytes[i] + x * 4, 4);
                    out.texture[i][y * width + x] = float(value);
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
void prepare_guide_texture_context(RenderScene &scene) {
    const std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Validation requires no frame entries for untextured diffuse instances.
    // Rebuild after geometry/material changes instead of retaining old tables.
    scene.instance_texture_frames.assign(scene.geometry.instances.size(), {});
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        if (!instance_uses_texture_context(scene, i))
            continue;
        auto &xf = scene.instance_surface_transforms[i];
        xf.shape = xf.placement = identity;
        need(prepare_texture_triangle_frames(scene.geometry.meshes[i], xf.shape, xf.placement,
                                             scene.instance_texture_frames[i])
                 .empty(),
             "Appearance fixture texture context");
    }
}
void appearance_cases(const std::string &library) {
    width = 5;
    height = 3;
    pixels = width * height;
    const auto r = raster();
    const auto cam = camera(r);
    const auto prepare = prepare_guide_texture_context;
    const auto fresnel = [](double cosine, double n = .3, double k = 3.) {
        const std::complex<double> eta(n, k);
        const auto eta2 = eta * eta;
        const auto root = std::sqrt(eta2 - (1 - cosine * cosine));
        return .5 * (std::norm((cosine - root) / (cosine + root)) +
                     std::norm((eta2 * cosine - root) / (eta2 * cosine + root)));
    };
    auto scene = base(r);
    scene.materials[0].scattering = ScatteringKind::conductor;
    scene.materials[0].constant = 0;
    scene.materials[0].eta = {{{360, .3f}, {830, .3f}}, true};
    scene.materials[0].k = {{{360, 3.f}, {830, 3.f}}, true};
    auto target = base(r).materials.front();
    target.kind = SpectralMaterialKind::sampled;
    target.constant = 0;
    target.knots = {{360, .02f}, {430, .9f}, {470, .9f}, {500, .02f}, {830, .02f}};
    scene.materials.push_back(target);
    plane(scene, 3);
    plane(scene, -3, 1);
    case_context = "appearance/mirror reflected high-chroma plane";
    prepare(scene);
    auto cfg = config(cam, cam);
    cfg.flags = gw::appearance_guides;
    const auto replaced = guide(scene, cfg, library);
    need(replaced.counters.failure == 0 && replaced.counters.appearance_replacement == pixels &&
             replaced.counters.directional_approximation == pixels &&
             replaced.counters.appearance_fallback == 0,
         "Mirror appearance replacement accounting");
    std::array<double, 3> target_rgb{};
    const auto spectral = nodes();
    for (const auto &node : spectral) {
        double rho = 0;
        for (std::size_t k = 1; k < target.knots.size(); ++k) {
            const auto a = target.knots[k - 1], b = target.knots[k];
            if (node.wavelength >= a.wavelength && node.wavelength <= b.wavelength)
                rho = a.value + (b.value - a.value) * (node.wavelength - a.wavelength) /
                                    (b.wavelength - a.wavelength);
        }
        const std::array<double, 3> weight{node.r, node.g, node.b};
        for (unsigned c = 0; c < 3; ++c)
            target_rgb[c] += weight[c] * rho;
    }
    for (unsigned i = 0; i < pixels; ++i) {
        const auto ray = cam.center_ray(i % width, i / width);
        need(ray.has_value(), "Appearance oracle camera ray");
        const double F = fresnel(ray->direction[2]);
        for (unsigned c = 0; c < 3; ++c) {
            close(replaced.texture[3][4 * i + c], std::clamp(F * target_rgb[c], 0., 1.), .002,
                  "Directional Fresnel times high-chroma reflected albedo");
            close(replaced.texture[4][4 * i + c], 0, 0,
                  "Replaced specular guide is separate proxy");
        }
    }
    // A tiny reflected patch affects only the central pinhole guide pixel.
    for (auto &point : scene.geometry.meshes[1].positions) {
        point.x *= .005f;
        point.y *= .005f;
    }
    case_context = "appearance/mirror small reflected feature";
    prepare(scene);
    const auto feature = guide(scene, cfg, library);
    need(feature.counters.failure == 0 && feature.counters.appearance_replacement == 1 &&
             feature.counters.appearance_fallback == pixels - 1,
         "Bounded guide tracing must preserve a small reflected feature");
    const unsigned center = (height / 2) * width + width / 2;
    need(feature.texture[3][4 * center + 2] > .3f,
         "High-chroma small reflection missing from appearance albedo");
    // Thin glass blends two deterministically resolved surfaces. At normal
    // incidence eta1.5 gives F=.04, thin-sheet F=2*.04/1.04.
    scene = base(r);
    scene.materials[0].scattering = ScatteringKind::thin_dielectric;
    scene.materials[0].constant = 0;
    scene.materials[0].eta = {{{360, 1.5f}, {830, 1.5f}}, true};
    auto dark = base(r).materials.front(), light = dark;
    dark.constant = .2f;
    light.constant = .8f;
    scene.materials.push_back(dark);
    scene.materials.push_back(light);
    plane(scene, 3);
    plane(scene, -3, 1);
    plane(scene, 6, 2);
    case_context = "appearance/thin sheet front normal incidence";
    prepare(scene);
    const auto glass = guide(scene, cfg, library);
    need(glass.counters.failure == 0 && glass.counters.appearance_replacement == pixels,
         "Thin-glass reflected/transmitted replacement accounting");
    const double F = .08 / 1.04;
    for (unsigned c = 0; c < 3; ++c)
        close(glass.texture[3][4 * center + c], F * .2 + (1 - F) * .8, .002,
              "Thin-sheet Fresnel blend independent normal-incidence oracle");
    // Backside oblique thin sheets still see air on both sides. At |cos|=.5,
    // treating the sign as an interior eta1.5 interface would incorrectly TIR.
    const RasterCamera oblique{
        {0, 0, 0}, {float(std::sqrt(3.)) - .3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
    const auto oblique_camera = camera(oblique);
    const auto center_ray = oblique_camera.center_ray(width / 2, height / 2);
    need(center_ray.has_value(), "Backside oblique thin-sheet camera");
    const double cosine = center_ray->direction[2];
    close(cosine, .5, 1e-7, "Independent oblique incidence");
    const double transmitted_cosine = std::sqrt(1 - (1 - cosine * cosine) / (1.5 * 1.5));
    const double rs = (cosine - 1.5 * transmitted_cosine) / (cosine + 1.5 * transmitted_cosine);
    const double rp = (1.5 * cosine - transmitted_cosine) / (1.5 * cosine + transmitted_cosine);
    const double interface_fresnel = .5 * (rs * rs + rp * rp);
    const double thin_fresnel = 2 * interface_fresnel / (1 + interface_fresnel);
    const auto enlarge = [](RenderScene &s) {
        for (auto &mesh : s.geometry.meshes)
            for (auto &p : mesh.positions) {
                p.x *= 4;
                p.y *= 4;
            }
    };
    auto backside = base(oblique);
    auto sheet = scene.materials.front();
    backside.materials = {sheet, dark, light};
    plane(backside, 3);
    plane(backside, -3, 1);
    plane(backside, 6, 2);
    for (auto &triangle : backside.geometry.meshes[0].triangles)
        std::swap(triangle[1], triangle[2]); // +Z normal: negative incidence.
    enlarge(backside);
    case_context = "appearance/thin sheet backside oblique primary";
    prepare(backside);
    auto oblique_cfg = config(oblique_camera, oblique_camera);
    oblique_cfg.flags = gw::appearance_guides;
    const auto primary_backside = guide(backside, oblique_cfg, library);
    need(primary_backside.counters.failure == 0 &&
             primary_backside.counters.appearance_replacement == pixels,
         "Backside primary thin-sheet replacement accounting");
    for (unsigned c = 0; c < 3; ++c)
        close(primary_backside.texture[3][4 * center + c],
              thin_fresnel * .2 + (1 - thin_fresnel) * .8, .002,
              "Independent backside oblique primary thin-sheet Fresnel blend");
    // A solid dielectric really is exiting the denser medium here and must TIR.
    backside.materials[0].scattering = ScatteringKind::dielectric;
    case_context = "appearance/solid dielectric backside total internal reflection";
    const auto solid_backside = guide(backside, oblique_cfg, library);
    need(solid_backside.counters.failure == 0, "Backside solid dielectric guide validity");
    for (unsigned c = 0; c < 3; ++c)
        close(solid_backside.texture[3][4 * center + c], .2, .002,
              "Signed solid dielectric total-internal reflection is retained");
    // The same backside sheet encountered on a mirror's secondary guide path
    // transmits straight through and attenuates by (1-thin_F), never fake TIR.
    auto secondary = base(oblique);
    auto mirror = secondary.materials.front();
    mirror.scattering = ScatteringKind::conductor;
    mirror.constant = 0;
    mirror.eta = {{{360, .3f}, {830, .3f}}, true};
    mirror.k = {{{360, 3.f}, {830, 3.f}}, true};
    secondary.materials = {mirror, sheet, light};
    plane(secondary, 3);
    plane(secondary, -3, 1); // -Z normal, reflected incoming ray travels toward -Z.
    plane(secondary, -6, 2);
    enlarge(secondary);
    case_context = "appearance/thin sheet backside oblique secondary";
    prepare(secondary);
    const auto secondary_backside = guide(secondary, oblique_cfg, library);
    need(secondary_backside.counters.failure == 0 &&
             secondary_backside.counters.appearance_replacement == pixels &&
             secondary_backside.counters.appearance_bounded >= pixels,
         "Backside secondary thin-sheet continuation accounting");
    for (unsigned c = 0; c < 3; ++c)
        close(secondary_backside.texture[3][4 * center + c],
              fresnel(cosine) * (1 - thin_fresnel) * .8, .002,
              "Independent backside oblique secondary thin-sheet transmission oracle");
    // Both supported coatings flip backside wo into their exterior hemisphere.
    // Pin the existing F + (1-F)^2 substrate proxy, not an exact layered integral.
    for (const auto family : {ScatteringKind::coated_diffuse, ScatteringKind::coated_conductor}) {
        auto coating_scene = base(oblique);
        auto &coating = coating_scene.materials[0];
        coating.scattering = family;
        coating.constant = family == ScatteringKind::coated_diffuse ? .6f : 0.f;
        if (family == ScatteringKind::coated_diffuse)
            coating.eta = {{{360, 1.5f}, {830, 1.5f}}, true};
        else {
            coating.surface.interface_eta = {{{360, 1.5f}, {830, 1.5f}}, true};
            coating.eta = {{{360, .3f}, {830, .3f}}, true};
            coating.k = {{{360, 3.f}, {830, 3.f}}, true};
        }
        plane(coating_scene, 3);
        for (auto &triangle : coating_scene.geometry.meshes[0].triangles)
            std::swap(triangle[1], triangle[2]);
        enlarge(coating_scene);
        case_context = family == ScatteringKind::coated_diffuse
                           ? "appearance/coated diffuse backside oblique"
                           : "appearance/coated conductor backside oblique";
        prepare(coating_scene);
        const auto coated_backside = guide(coating_scene, oblique_cfg, library);
        need(coated_backside.counters.failure == 0 &&
                 coated_backside.counters.layered_approximation == pixels,
             "Backside oblique coating approximation accounting");
        const double attenuation = (1 - interface_fresnel) * (1 - interface_fresnel);
        for (unsigned c = 0; c < 3; ++c) {
            close(coated_backside.texture[3][4 * center + c],
                  family == ScatteringKind::coated_diffuse ? attenuation * .6 : 0, .002,
                  "Independent backside coating exterior-Fresnel substrate oracle");
            close(coated_backside.texture[4][4 * center + c],
                  interface_fresnel + (family == ScatteringKind::coated_conductor
                                           ? attenuation * fresnel(cosine, .3 / 1.5, 3. / 1.5)
                                           : 0),
                  .002, "Independent backside coating exterior-Fresnel specular oracle");
        }
    }
    case_context = "appearance/reject signed temporal policy combination";
    auto forbidden = cfg;
    forbidden.flags |= gw::signed_temporal;
    need((guide(scene, forbidden, library).counters.failure & gw::invalid_config) != 0,
         "Appearance guides must reject signed temporal correspondence");
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
void transformed_spatial_guides(const std::string &library) {
    // A nonuniform scale plus shear maps local z=0 to the plane with normal
    // (-.5,.25,1). Test both a local origin and a large exactly represented
    // camera/instance translation; expected guides remain camera-relative.
    for (const GeometryPoint origin : {GeometryPoint{0, 0, 0}, GeometryPoint{4096, -8192, 16384}}) {
        auto r = raster();
        r.origin = origin;
        auto scene = base(r);
        GeometryMesh mesh;
        mesh.positions = {{-16, -16, 0}, {16, -16, 0}, {0, 16, 0}};
        mesh.triangles = {{0, 1, 2}};
        scene.geometry.meshes.push_back(mesh);
        // Local z is zero on every vertex. Keep its world-x coefficient zero
        // so the inverse is dyadic, avoiding unrelated native inverse-transform
        // cancellation at large origins without changing the tested world plane.
        GeometryTransform transform{
            {2, .25f, 0, origin.x + .25f, 0, .5f, 0, origin.y + .125f, 1, 0, 1, origin.z + 4.f}};
        scene.geometry.instances.push_back({0, transform});
        scene.instance_materials.push_back(0);
        scene.instance_emissions.push_back(no_area_emission);
        scene.instance_surface_transforms.emplace_back();
        const auto now = camera(r);
        auto cfg = config(now, now);
        cfg.flags |= gw::signed_temporal;
        const auto out = guide(scene, cfg, library);
        need(out.counters.failure == 0 && out.counters.completed == pixels &&
                 out.counters.background == 0,
             "Transformed/rebased triangle guides complete");
        constexpr std::array<double, 3> plane_normal{-.5, .25, 1};
        const double normal_length = std::sqrt(1.3125);
        // n dot (.25,.125,4) = 3.90625. Use authored raster basis directly,
        // independent of shader transforms, CPU Camera rays and hit helpers.
        for (unsigned p = 0; p < pixels; ++p) {
            const double u = (double(p % width) + .5) / width,
                         v = (double(p / width) + .5) / height;
            const std::array<double, 3> ray{double(r.corner.x) + u * r.dx.x + v * r.dy.x,
                                            double(r.corner.y) + u * r.dx.y + v * r.dy.y,
                                            double(r.corner.z) + u * r.dx.z + v * r.dy.z};
            const double parameter = 3.90625 / (-.5 * ray[0] + .25 * ray[1] + ray[2]);
            for (unsigned axis = 0; axis < 3; ++axis) {
                close(out.texture[9][4 * p + axis], ray[axis] * parameter, 2e-4,
                      "Independent transformed/rebased camera-relative position");
                close(out.texture[10][4 * p + axis], plane_normal[axis] / normal_length, 2e-6,
                      "Independent inverse-transpose geometric normal");
            }
            close(out.texture[9][4 * p + 3], 0, 0, "Spatial position padding");
            close(out.texture[10][4 * p + 3], 0, 0, "Geometric normal padding");
        }
    }
}
void continuity_alignment(const std::string &library) {
    auto r = raster();
    auto scene = base(r);
    plane(scene, 3);
    plane(scene, 6);
    scene.geometry.meshes[1].positions[3].z = 6.1f;
    auto &matrix = scene.geometry.instances[1].transform.values;
    matrix[0] = 1e-5f;
    matrix[5] = 1e5f;
    matrix[10] = 1e3f;
    // Reused mesh instance must retain the same packed group range.
    scene.geometry.instances.push_back(scene.geometry.instances[0]);
    scene.instance_materials.push_back(0);
    scene.instance_emissions.push_back(no_area_emission);
    scene.instance_surface_transforms.emplace_back();
    auto result = guide(scene, config(camera(r), camera(r)), library);
    need(result.continuity == std::vector<std::uint32_t>{0, 0, 0, 1},
         "Multi-mesh indexed continuity ranges and repeated transformed instances");
}
void protection_cases(const std::string &library, bool validate_only = false) {
    const auto r = raster();
    auto cfg = config(camera(r), camera(r));
    cfg.flags = gw::signed_temporal;
    unsigned validated = 0;
    auto check = [&](RenderScene scene, bool expected, const std::string &label) {
        case_context = "primary spatial protection/" + label;
        plane(scene, 3);
        // Non-diffuse materials require the same texture context as transport,
        // even when no image texture is present. Diffuse-only fixtures retain
        // the existing no-frame-table contract.
        if (std::any_of(scene.materials.begin(), scene.materials.end(), [](const auto &m) {
                return m.scattering != ScatteringKind::diffuse &&
                       m.scattering != ScatteringKind::null_interface;
            }))
            prepare_guide_texture_context(scene);
        const auto diagnostic = validate_render_scene(scene);
        need(diagnostic.empty(), "Guide fixture: " + diagnostic);
        ++validated;
        if (validate_only)
            return;
        const auto result = guide(scene, cfg, library);
        need(result.counters.failure == 0, label + " guide generation");
        for (const auto value : result.texture[12])
            need(value == float(expected), label + " primary spatial protection");
        need(result.raw_before.raw_xyz == result.raw_after.raw_xyz,
             label + " must preserve raw transport");
    };
    auto diffuse = base(r);
    check(diffuse, false, "Diffuse");
    auto delta = diffuse.materials[0];
    delta.scattering = ScatteringKind::dielectric;
    delta.constant = 0; // Dielectrics carry IOR, never diffuse reflectance.
    delta.eta = {{{360, 1.5f}, {830, 1.5f}}, true};
    auto smooth = diffuse;
    smooth.materials[0] = delta;
    check(smooth, true, "Smooth dielectric");
    smooth.materials[0].eta = {{{360, 1.f}, {830, 1.f}}, true};
    check(smooth, true, "Index-matched delta transmission");
    for (float amount : {0.f, .25f, 1.f}) {
        auto mixed = diffuse;
        auto root = diffuse.materials[0];
        root.scattering = ScatteringKind::mix;
        root.surface.first_material = 1;
        root.surface.second_material = 2;
        root.surface.mix_amount = amount;
        mixed.materials = {root, diffuse.materials[0], delta};
        check(mixed, amount > 0, "Mix positive leaf");
        std::swap(mixed.materials[1], mixed.materials[2]);
        check(mixed, amount < 1, "Mix opposite positive leaf");
    }
    auto coat = diffuse;
    coat.materials[0] = delta;
    coat.materials[0].scattering = ScatteringKind::coated_diffuse;
    coat.materials[0].constant = .37f; // Explicit coated diffuse substrate.
    check(coat, true, "Smooth coating mixed RGB");
    coat.materials[0].u_roughness = coat.materials[0].v_roughness = .2f;
    check(coat, false, "Rough constant-pair IOR coating remains filterable");
    auto rough = smooth;
    rough.materials[0] = delta;
    rough.materials[0].u_roughness = rough.materials[0].v_roughness = .2f;
    check(rough, false, "Rough constant-pair IOR glass remains filterable");
    rough.materials[0].eta.extend_endpoints = false;
    check(rough, false, "Full-domain constant pair needs no endpoint extension");
    rough.materials[0].eta = {{{500, 1.5f}, {600, 1.5f}}, true};
    check(rough, false, "Short constant pair with extension remains filterable");
    rough.materials[0].eta.extend_endpoints = false;
    check(rough, true, "Short unextended pair has off-domain delta transmission");
    auto short_coat = coat;
    short_coat.materials[0].eta = rough.materials[0].eta;
    check(short_coat, true, "Short unextended coating pair has unknown delta support");
    short_coat.materials[0].eta.extend_endpoints = true;
    check(short_coat, false, "Extended constant coating pair remains filterable");
    rough.materials[0].eta = {{{360, 1.f}, {550, 1.5f}, {830, 1.f}}, true};
    check(rough, true, "Off-550 index-matched transmission");
    for (float amount : {0.f, 1e-30f}) {
        case_context = amount == 0 ? "primary spatial protection/zero-weight null mixture"
                                   : "primary spatial protection/positive-weight null mixture";
        auto mixed = diffuse;
        auto root = diffuse.materials[0];
        auto null = diffuse.materials[0];
        root.scattering = ScatteringKind::mix;
        root.surface.first_material = 1;
        root.surface.second_material = 2;
        root.surface.mix_amount = amount;
        null.scattering = ScatteringKind::null_interface;
        mixed.materials = {root, diffuse.materials[0], null};
        need(validate_render_scene(mixed).find("Mix materials cannot contain null boundaries") !=
                 std::string::npos,
             "Null mix remains explicitly unsupported even at zero weight");
    }
    for (bool shared : {false, true}) {
        auto graph = diffuse;
        const unsigned levels = shared ? 10 : 64;
        graph.materials.assign(levels + 1, diffuse.materials[0]);
        for (unsigned i = 0; i < levels; ++i) {
            auto &m = graph.materials[i];
            m.scattering = ScatteringKind::mix;
            m.surface.mix_amount = .5f;
            m.surface.first_material = i + 1;
            m.surface.second_material = shared ? i + 1 : levels;
        }
        check(graph, shared,
              shared ? "Shared diffuse DAG has bounded unknown support"
                     : "64-level diffuse chain stays unprotected");
    }
    auto sky = base(r);
    case_context = "primary spatial protection/sky";
    need(validate_render_scene(sky).empty(), "Sky fixture must validate");
    ++validated;
    if (validate_only) {
        std::cout << "Primary protection CPU fixture validation: " << validated
                  << " valid fixtures and two expected null-mix rejections passed\n";
        return;
    }
    const auto background = guide(sky, cfg, library);
    need(background.counters.failure == 0, "Sky guide generation");
    for (const auto value : background.texture[12])
        need(value == 0, "Sky must remain spatially supported");
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
            need(argc == 2,
                 "Usage: reconstruction-guide-tests LIBRARY.metallib | --validate-fixtures");
            if (std::string(argv[1]) == "--validate-fixtures") {
                protection_cases("", true);
                return 0;
            }
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
            case_context = "primary spatial protection";
            protection_cases(argv[1]);
            case_context = "general guides";
            cases(argv[1]);
            case_context = "appearance guides";
            appearance_cases(argv[1]);
            case_context = "continuity alignment";
            continuity_alignment(argv[1]);
            case_context = "transformed spatial guides";
            transformed_spatial_guides(argv[1]);
            case_context = "color counter sizes";
            counter_sizes(argv[1]);
            std::cout << "Reconstruction guide native validation passed\n";
            return 0;
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
