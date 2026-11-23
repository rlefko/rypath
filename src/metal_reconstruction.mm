// SPDX-License-Identifier: GPL-3.0-only
#include "metal_execution.hpp"
#include "metal_frame_access.hpp"
#include "metal_gpu_diagnostics.hpp"
#include "metal_reconstruction_access.hpp"
#include "metalfx_denoiser.hpp"
#include "reconstruction.hpp"
#include "reconstruction_camera.hpp"
#include "reconstruction_guide_cache.hpp"
#include "reconstruction_guide_wire.h"
#include "rgb_spectrum.hpp"
#include "temporal_denoiser.hpp"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>

namespace rypath {
namespace {
namespace gw = reconstruction_guide_wire;
struct OutputLease {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLTexture> texture = nil;
};
id<MTLTexture> texture(id<MTLDevice> device, const MetalFXTextureRequirement &required,
                       MTLStorageMode storage = MTLStorageModePrivate) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:required.format
                                                                         width:required.width
                                                                        height:required.height
                                                                     mipmapped:NO];
    descriptor.storageMode = storage;
    descriptor.usage = required.usage;
    return [device newTextureWithDescriptor:descriptor];
}
std::array<float, 12> rgb_matrix(std::uint32_t model) {
    if (model == render_wire::demo_model)
        return {3.2404542f, -1.5371385f, -.4985314f, 0,          -.9692660f, 1.8760108f,
                .0415560f,  0,           .0556434f,  -.2040259f, 1.0572252f, 0};
    return {3.240444291419f, -1.537133830545f, -.498529890988f, 0,
            -.969266752560f, 1.876012242980f,  .041556048487f,  0,
            .055643519672f,  -.204026238798f,  1.057226873771f, 0};
}
std::array<gw::SpectralNode, gw::node_count> spectral_nodes(std::uint32_t model) {
    // Deterministic 16-bin midpoint D65/CIE quadrature, channel-normalized so
    // unit reflectance gives RGB white. It is a guide approximation, not transport.
    const auto matrix = rgb_matrix(model);
    std::array<gw::SpectralNode, gw::node_count> nodes{};
    std::array<double, 3> normalization{};
    for (unsigned i = 0; i < nodes.size(); ++i) {
        const float wavelength = 360.f + (float(i) + .5f) * (470.f / nodes.size());
        const auto xyz = pbrt_cie_xyz(wavelength);
        const auto illuminant = pbrt_d65()[std::size_t(std::round(wavelength) - 360)];
        std::array<double, 3> rgb{};
        for (unsigned c = 0; c < 3; ++c) {
            for (unsigned k = 0; k < 3; ++k)
                rgb[c] += matrix[c * 4 + k] * xyz[k];
            rgb[c] *= illuminant;
            normalization[c] += rgb[c];
        }
        nodes[i] = {wavelength, float(rgb[0]), float(rgb[1]), float(rgb[2])};
    }
    for (auto &node : nodes) {
        node.r = float(node.r / normalization[0]);
        node.g = float(node.g / normalization[1]);
        node.b = float(node.b / normalization[2]);
    }
    return nodes;
}
reconstruction::CameraBuildResult make_camera(const MetalFrameLease &lease, double near_plane) {
    const reconstruction::CameraConfig config{
        lease.info().width, lease.info().height, near_plane, {}};
    const auto &raster = lease.camera().raster_camera;
    if (raster)
        return reconstruction::Camera::build(*raster, config);
    return reconstruction::Camera::build(lease.camera().camera, config);
}
simd_float4x4 simd_matrix(const reconstruction::Matrix &source) {
    simd_float4x4 matrix;
    static_assert(sizeof(matrix) == sizeof(source));
    std::memcpy(&matrix, source.data(), sizeof(matrix));
    return matrix;
}
float decode_half(std::uint16_t value) {
    const std::uint32_t sign = (std::uint32_t(value) & 0x8000u) << 16;
    const std::uint32_t exponent = (value >> 10) & 31u, fraction = value & 1023u;
    if (!exponent) {
        if (!fraction)
            return std::bit_cast<float>(sign);
        const auto shift = std::countl_zero(fraction) - 21;
        return std::bit_cast<float>(sign | ((113u - shift) << 23) |
                                    (((fraction << shift) & 1023u) << 13));
    }
    return std::bit_cast<float>(sign | ((exponent == 31 ? 255u : exponent + 112u) << 23) |
                                (fraction << 13));
}
} // namespace
struct ReconstructedFrame::Impl {
    std::shared_ptr<OutputLease> output;
    ReconstructionInfo info;
    mutable std::mutex mutex, capture_mutex;
    mutable std::condition_variable completed;
};
struct ReconstructionStream::Impl {
    std::mutex mutex;
    bool pending = false, previous_valid = false, epoch_known = false;
    std::uint64_t epoch = 0;
    std::optional<reconstruction::Camera> previous;
    float previous_lens_radius = 0, previous_focal_distance = 0;
    std::uint32_t previous_camera_medium = UINT32_MAX;
    bool previous_approximation = false;
    std::optional<FrameCamera> previous_frame_camera;
    std::optional<std::vector<std::uint32_t>> guide_key;
    gw::Counters guide_counts{};
    ReconstructionConfig config;
    std::weak_ptr<ReconstructedFrame::Impl> last_completed;
    std::uint32_t width = 0, height = 0, model = 0;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    std::unique_ptr<MetalFramePipeline> reset, surface, color;
    std::unique_ptr<MetalFXDenoiser> denoiser;
    std::unique_ptr<TemporalDenoiser> temporal;
    id<MTLTexture> previous_depth = nil, identity = nil;
    id<MTLArgumentEncoder> argument_encoder = nil;
    id<MTLBuffer> configuration = nil, nodes = nil, counters = nil, arguments = nil;
    MetalFXDenoiserInputs inputs;
    struct Slot {
        id<MTLTexture> texture = nil;
        std::weak_ptr<OutputLease> active;
    };
    std::array<Slot, 2> slots;

    std::string prepare(MetalFrameLease &lease, ReconstructionConfig settings) {
        config = settings;
        width = lease.info().width;
        height = lease.info().height;
        model = lease.parameters().model;
        device = lease.device();
        queue = lease.queue();
        if (!config.output_width)
            config.output_width = width;
        if (!config.output_height)
            config.output_height = height;
        if (width > reconstruction_dimension_limit || height > reconstruction_dimension_limit ||
            config.output_width > reconstruction_dimension_limit ||
            config.output_height > reconstruction_dimension_limit)
            return "Reconstruction dimensions exceed the supported 16384-pixel limit";
        if (config.mode != ReconstructionMode::raw_rgb &&
            config.mode != ReconstructionMode::metalfx &&
            config.mode != ReconstructionMode::temporal)
            return "Invalid reconstruction mode";
        if (!std::isfinite(config.near_plane) || !(config.near_plane > 0) ||
            !std::isnormal(float(config.near_plane)))
            return "Reconstruction near plane must pack to a positive normal float";
        if (config.mode != ReconstructionMode::metalfx &&
            (config.output_width != width || config.output_height != height))
            return "Raw RGB and temporal reconstruction preserve input dimensions";
        const auto output_pixels = std::uint64_t(config.output_width) * config.output_height;
        if (!output_pixels || output_pixels > UINT32_MAX)
            return "Invalid reconstruction output dimensions";
        auto camera = make_camera(lease, config.near_plane);
        if (!camera.camera)
            return camera.diagnostic;
        auto contract = lease.create_guide_pipeline(@"rypath_reconstruction_guide_contract");
        if (!contract.pipeline)
            return contract.diagnostic;
        auto check = [device newBufferWithLength:sizeof(gw::Contract)
                                         options:MTLResourceStorageModeShared];
        if (check == nil)
            return "Reconstruction contract allocation failed";
        std::memset(check.contents, 0, check.length);
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (command == nil || encoder == nil)
            return "Reconstruction contract encoder creation failed";
        [encoder setComputePipelineState:contract.pipeline->pipeline()];
        [encoder setBuffer:check offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        auto error = metal::complete(command, "reconstruction guide contract");
        if (!error.empty())
            return error;
        gw::Contract actual{};
        std::memcpy(&actual, check.contents, sizeof(actual));
        if (actual.version != gw::contract || actual.config_bytes != sizeof(gw::Config) ||
            actual.node_bytes != sizeof(gw::SpectralNode) ||
            actual.counters_bytes != sizeof(gw::Counters) || actual.nodes != gw::node_count ||
            actual.argument_count != 13 || actual.reserved[0] || actual.reserved[1])
            return "Reconstruction guide shader ABI mismatch; rebuild the metallib";
        auto build = lease.create_guide_pipeline(@"rypath_reconstruction_guide_reset");
        if (!build.pipeline)
            return build.diagnostic;
        reset = std::move(build.pipeline);
        build = lease.create_guide_pipeline(@"rypath_reconstruction_guide_color");
        if (!build.pipeline)
            return build.diagnostic;
        color = std::move(build.pipeline);
        argument_encoder = color->argument_encoder();
        if (argument_encoder == nil)
            return "Reconstruction guide argument encoder is unavailable";
        const bool signed_color = config.mode == ReconstructionMode::temporal;
        const auto pixel_bytes = signed_color ? 16u : 8u;
        const auto row =
            (std::uint64_t(config.output_width) * pixel_bytes + 255) & ~std::uint64_t(255);
        const auto capture_bytes = row * config.output_height;
        // This bounds stream-owned resources, including two simultaneous captures.
        // MetalFX does not expose its internal allocation sizes; those are excluded.
        const auto temporal_bytes =
            signed_color
                ? TemporalDenoiser::owned_resource_bytes(width, height, config.capture_diagnostics)
                : 0;
        if (temporal_bytes == UINT64_MAX)
            return "Temporal reconstruction resource size is unrepresentable";
        const auto padded_pixels = [](std::uint32_t w, std::uint32_t h) {
            return ((std::uint64_t(w) + 63) / 64 * 64) * ((std::uint64_t(h) + 63) / 64 * 64);
        };
        const auto owned_bytes =
            (62 + (signed_color ? 8 : 0)) * padded_pixels(width, height) +
            2ull * pixel_bytes * padded_pixels(config.output_width, config.output_height) +
            2 * capture_bytes + 16 * 65536ull + temporal_bytes + sizeof(gw::Config) +
            sizeof(gw::Counters) + sizeof(gw::SpectralNode) * gw::node_count +
            argument_encoder.encodedLength + 2;
        if (capture_bytes > device.maxBufferLength || owned_bytes > (8ull << 30) ||
            (device.recommendedMaxWorkingSetSize &&
             owned_bytes > device.recommendedMaxWorkingSetSize))
            return "Reconstruction textures/captures exceed the owned-resource memory budget";
        if (config.mode != ReconstructionMode::raw_rgb) {
            build = lease.create_guide_pipeline(@"rypath_reconstruction_guide_surface");
            if (!build.pipeline)
                return build.diagnostic;
            surface = std::move(build.pipeline);
        }
        if (config.mode == ReconstructionMode::metalfx) {
            auto fx = MetalFXDenoiser::create(
                device, {width, height, config.output_width, config.output_height});
            if (!fx.denoiser)
                return fx.diagnostic;
            denoiser = std::move(fx.denoiser);
        } else if (signed_color) {
            auto built = TemporalDenoiser::create(
                lease, {.width = width,
                        .height = height,
                        .history_limit = config.history_limit,
                        .spatial_iterations = config.spatial_iterations,
                        .capture_diagnostics = config.capture_diagnostics});
            if (!built.denoiser)
                return built.diagnostic;
            temporal = std::move(built.denoiser);
        }
        MetalFXDenoiserRequirements requirements;
        if (denoiser)
            requirements = denoiser->requirements();
        else {
            const auto read_write = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            requirements.color = {MTLPixelFormatRGBA16Float, read_write, width, height};
            requirements.depth = {MTLPixelFormatR32Float, read_write, width, height};
            requirements.motion = {MTLPixelFormatRG16Float, read_write, width, height};
            requirements.diffuse_albedo = requirements.specular_albedo = requirements.normal =
                requirements.color;
            requirements.roughness = {MTLPixelFormatR16Float, read_write, width, height};
            requirements.exposure = {MTLPixelFormatR16Float, MTLTextureUsageShaderRead, 1, 1};
            requirements.output = {MTLPixelFormatRGBA16Float, read_write, config.output_width,
                                   config.output_height};
            if (signed_color) {
                requirements.color.format = MTLPixelFormatRGBA32Float;
                requirements.output.format = MTLPixelFormatRGBA32Float;
            }
        }
        requirements.output.usage |= MTLTextureUsageShaderRead;
        const auto input_texture = [&](MetalFXTextureRequirement required) {
            required.usage |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            return texture(device, required);
        };
        inputs.color = input_texture(requirements.color);
        inputs.depth = input_texture(requirements.depth);
        inputs.motion = input_texture(requirements.motion);
        inputs.diffuse_albedo = input_texture(requirements.diffuse_albedo);
        inputs.specular_albedo = input_texture(requirements.specular_albedo);
        inputs.normal = input_texture(requirements.normal);
        inputs.roughness = input_texture(requirements.roughness);
        previous_depth =
            input_texture({MTLPixelFormatR32Float, MTLTextureUsageShaderRead, width, height});
        identity =
            input_texture({MTLPixelFormatRGBA32Uint, MTLTextureUsageShaderRead, width, height});
        inputs.exposure = texture(device, requirements.exposure, MTLStorageModeShared);
        if (inputs.color == nil || inputs.depth == nil || inputs.motion == nil ||
            inputs.diffuse_albedo == nil || inputs.specular_albedo == nil || inputs.normal == nil ||
            inputs.roughness == nil || inputs.exposure == nil || previous_depth == nil ||
            identity == nil)
            return "Reconstruction guide texture allocation failed";
        const std::uint16_t exposure = 0x3c00;
        [inputs.exposure replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                           mipmapLevel:0
                             withBytes:&exposure
                           bytesPerRow:2];
        for (auto &slot : slots) {
            slot.texture = texture(device, requirements.output);
            if (slot.texture == nil)
                return "Reconstruction output texture allocation failed";
        }
        configuration = [device newBufferWithLength:sizeof(gw::Config)
                                            options:MTLResourceStorageModeShared];
        counters = [device newBufferWithLength:sizeof(gw::Counters)
                                       options:MTLResourceStorageModeShared];
        const auto table = spectral_nodes(model);
        nodes = [device newBufferWithBytes:table.data()
                                    length:sizeof(table)
                                   options:MTLResourceStorageModeShared];
        arguments = [device newBufferWithLength:argument_encoder.encodedLength
                                        options:MTLResourceStorageModeShared];
        if (configuration == nil || counters == nil || nodes == nil || arguments == nil)
            return "Reconstruction guide buffer allocation failed";
        return {};
    }
    struct Job : std::enable_shared_from_this<Job> {
        std::shared_ptr<Impl> state;
        std::shared_ptr<ReconstructedFrame::Impl> frame;
        std::unique_ptr<MetalFrameLease> source;
        std::optional<reconstruction::Camera> current;
        std::vector<std::uint32_t> guide_key;
        bool all_times_valid = true;
        double gpu_seconds = 0;
        bool camera_changed() const {
            const auto prior = state->previous_frame_camera;
            if (!prior)
                return true;
            const auto &a = *prior;
            const auto &b = source->camera();
            const auto same = [](GeometryPoint x, GeometryPoint y) {
                return x.x == y.x && x.y == y.y && x.z == y.z;
            };
            if (a.raster_camera.has_value() != b.raster_camera.has_value() ||
                a.perspective_lens.has_value() != b.perspective_lens.has_value())
                return true;
            if (a.raster_camera) {
                if (!b.raster_camera)
                    return true;
                const auto &x = *a.raster_camera;
                const auto &y = *b.raster_camera;
                if (!same(x.origin, y.origin) || !same(x.corner, y.corner) || !same(x.dx, y.dx) ||
                    !same(x.dy, y.dy))
                    return true;
            } else if (!same(a.camera.origin, b.camera.origin) ||
                       !same(a.camera.target, b.camera.target) || !same(a.camera.up, b.camera.up) ||
                       a.camera.vertical_fov_degrees != b.camera.vertical_fov_degrees) {
                return true;
            }
            if (a.perspective_lens) {
                if (!b.perspective_lens)
                    return true;
                const auto &x = *a.perspective_lens;
                const auto &y = *b.perspective_lens;
                if (x.radius != y.radius || x.focal_distance != y.focal_distance ||
                    !same(x.camera_x, y.camera_x) || !same(x.camera_y, y.camera_y))
                    return true;
            }
            return false;
        }
        std::shared_ptr<MetalGPUStages> stages;
        void timing(id<MTLCommandBuffer> command) {
            if (stages) {
                std::lock_guard lock(frame->mutex);
                stages->resolve(command, frame->info.gpu_stages);
            }
            const bool valid = command.status == MTLCommandBufferStatusCompleted &&
                               command.GPUStartTime > 0 && std::isfinite(command.GPUStartTime) &&
                               std::isfinite(command.GPUEndTime) &&
                               command.GPUEndTime >= command.GPUStartTime &&
                               std::isfinite(command.GPUEndTime - command.GPUStartTime);
            all_times_valid &= valid;
            if (valid)
                gpu_seconds += command.GPUEndTime - command.GPUStartTime;
            all_times_valid &= std::isfinite(gpu_seconds);
        }
        void finish(std::string error = {}) {
            {
                std::lock_guard state_lock(state->mutex);
                std::lock_guard frame_lock(frame->mutex);
                state->pending = false;
                state->last_completed.reset();
                if (!error.empty()) {
                    state->previous_valid = false;
                    state->guide_key.reset();
                    frame->info.status = ReconstructionStatus::failed;
                    frame->info.diagnostic = std::move(error);
                    frame->info.gpu_time_valid = false;
                    invalidate_gpu_stages(frame->info.gpu_stages, "invalid: reconstruction failed");
                } else {
                    if (state->config.cache_surface_guides && state->surface) {
                        state->guide_key = std::move(guide_key);
                        std::memcpy(&state->guide_counts, state->counters.contents,
                                    sizeof(state->guide_counts));
                    }
                    state->previous = current;
                    state->previous_lens_radius = source->parameters().lens_radius;
                    state->previous_focal_distance = source->parameters().focal_distance;
                    state->previous_camera_medium = source->parameters().camera_medium;
                    state->previous_frame_camera = source->camera();
                    state->previous_approximation =
                        frame->info.defocus_approximation ||
                        frame->info.counters.media_approximation ||
                        frame->info.counters.transmission_approximation ||
                        frame->info.counters.mix_fallback;
                    state->previous_valid = state->config.mode != ReconstructionMode::raw_rgb;
                    state->epoch_known = true;
                    state->epoch = frame->info.source.epoch;
                    frame->info.status = ReconstructionStatus::complete;
                    state->last_completed = frame;
                    frame->info.gpu_time_valid = all_times_valid;
                    frame->info.gpu_seconds = all_times_valid ? gpu_seconds : 0;
                }
            }
            frame->completed.notify_all();
        }
        static std::string command_error(id<MTLCommandBuffer> command) {
            if (command.status == MTLCommandBufferStatusCompleted)
                return {};
            std::string error = "Reconstruction GPU command failed";
            if (command.error)
                error += ": " + std::string(command.error.localizedDescription.UTF8String);
            return error;
        }
        id<MTLCommandBuffer> prepare(std::string &error, const reconstruction::Camera &previous,
                                     bool previous_valid) {
            auto &s = *state;
            if (!current) {
                error = "Reconstruction job camera is unavailable";
                return nil;
            }
            gw::Config cfg{};
            cfg.width = s.width;
            cfg.height = s.height;
            cfg.model = s.model;
            cfg.previous_valid = previous_valid;
            std::ranges::copy(current->world_to_view(), cfg.current_world_to_view);
            std::ranges::copy(current->view_to_clip(), cfg.current_view_to_clip);
            std::ranges::copy(previous.world_to_view(), cfg.previous_world_to_view);
            std::ranges::copy(previous.view_to_clip(), cfg.previous_view_to_clip);
            for (unsigned axis = 0; axis < 3; ++axis) {
                cfg.current_camera_to_matrix_origin[axis] =
                    float(current->origin()[axis] - current->matrix_origin()[axis]);
                cfg.previous_camera_to_matrix_origin[axis] =
                    float(current->origin()[axis] - previous.matrix_origin()[axis]);
                if (!std::isfinite(cfg.current_camera_to_matrix_origin[axis]) ||
                    !std::isfinite(cfg.previous_camera_to_matrix_origin[axis])) {
                    error = "Reconstruction camera-origin delta exceeds the guide domain";
                    return nil;
                }
            }
            const auto matrix = rgb_matrix(s.model);
            std::ranges::copy(matrix, cfg.xyz_to_rgb);
            cfg.spectral_nodes = gw::node_count;
            cfg.flags = source->parameters().medium_count ? gw::media_scene : 0;
            if (s.temporal)
                cfg.flags |= gw::signed_temporal;
            std::memcpy(s.configuration.contents, &cfg, sizeof(cfg));

            [s.argument_encoder setArgumentBuffer:s.arguments offset:0];
            const std::array<id<MTLBuffer>, 4> buffers{s.configuration, source->raw_xyz(), s.nodes,
                                                       s.counters};
            for (unsigned i = 0; i < buffers.size(); ++i)
                [s.argument_encoder setBuffer:buffers[i] offset:0 atIndex:i];
            const std::array<id<MTLTexture>, 9> textures{
                s.inputs.color,          s.inputs.depth,           s.inputs.motion,
                s.inputs.diffuse_albedo, s.inputs.specular_albedo, s.inputs.normal,
                s.inputs.roughness,      s.previous_depth,         s.identity};
            for (unsigned i = 0; i < textures.size(); ++i)
                [s.argument_encoder setTexture:textures[i] atIndex:i + 4];
            auto command = [s.queue commandBuffer];
            command.label = @"rypath reconstruction guides and color";
            stages = s.config.profile_stages ? std::make_shared<MetalGPUStages>(s.device) : nullptr;
            auto encoder = [command computeCommandEncoder];
            if (command == nil || encoder == nil) {
                error = "Reconstruction command creation failed";
                return nil;
            }
            // This validates session/profile ownership before accepting work.
            error = source->bind_guides(encoder, *s.color, s.arguments);
            if (!error.empty()) {
                [encoder endEncoding];
                return nil;
            }
            const auto use_resource = [&](id<MTLResource> resource, MTLResourceUsage usage) {
                if (stages)
                    stages->use_resource(encoder, resource, usage);
                else
                    [encoder useResource:resource usage:usage];
            };
            use_resource(s.configuration, MTLResourceUsageRead);
            use_resource(source->raw_xyz(), MTLResourceUsageRead);
            use_resource(s.nodes, MTLResourceUsageRead);
            use_resource(s.counters, MTLResourceUsageRead | MTLResourceUsageWrite);
            for (auto input : textures)
                use_resource(input, MTLResourceUsageRead | MTLResourceUsageWrite);
            encoder.label = @"reconstruction guides and color";
            const auto dispatch = [&](const MetalFramePipeline &pipeline, NSUInteger count,
                                      const char *label) {
                if (!error.empty())
                    return;
                if (stages && !stages->begin(command, encoder, label)) {
                    error = "Profiling reconstruction encoder allocation failed";
                    return;
                }
                if (stages && stages->splits_encoders()) {
                    error = source->bind_guides(
                        encoder, s.surface && &pipeline == s.surface.get() ? *s.surface : *s.color,
                        s.arguments);
                    if (!error.empty())
                        return;
                }
                [encoder setComputePipelineState:pipeline.pipeline()];
                [encoder setBuffer:s.arguments offset:0 atIndex:1];
                [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(
                                                          count == 1 ? 1 : 32,
                                                          metal::group_width(s.device,
                                                                             pipeline.pipeline())),
                                                      1, 1)];
                if (stages)
                    stages->end(encoder);
                [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
            };
            if (s.config.cache_surface_guides && s.surface)
                guide_key = reconstruction::surface_guide_key(source->guide_parameters(),
                                                              source->texture_parameters(), cfg);
            const bool hit = s.config.cache_surface_guides && s.surface && s.guide_key &&
                             *s.guide_key == guide_key;
            frame->info.guide_cache_hit = hit;
            frame->info.generated_surface_pixels =
                s.surface && !hit ? std::uint64_t(s.width) * s.height : 0;
            if (hit) {
                // Seed logical surface counts, but every color conversion is fresh.
                auto counts = s.guide_counts;
                counts.failure = counts.completed = counts.color_negative = counts.color_high = 0;
                std::memcpy(s.counters.contents, &counts, sizeof(counts));
            } else {
                // Invalidate before any overwrite; only whole-job success republishes.
                s.guide_key.reset();
                std::memset(s.counters.contents, 0, s.counters.length);
                dispatch(*s.reset, 1, "reconstruction.reset");
            }
            const auto pixels = std::uint64_t(s.width) * s.height;
            if (s.surface && !hit) {
                error = source->bind_guides(encoder, *s.surface, s.arguments);
                if (!error.empty()) {
                    [encoder endEncoding];
                    return nil;
                }
                dispatch(*s.surface, pixels, "reconstruction.surface");
            }
            dispatch(*s.color, pixels, "reconstruction.color");
            if (!error.empty()) {
                [encoder endEncoding];
                return nil;
            }
            [encoder endEncoding];
            if (!s.denoiser && !s.temporal) {
                auto blit = [command blitCommandEncoder];
                if (blit == nil) {
                    error = "RGB output blit encoder creation failed";
                    return nil;
                }
                [blit copyFromTexture:s.inputs.color
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(s.width, s.height, 1)
                            toTexture:frame->output->texture
                     destinationSlice:0
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
            }
            error = source->retain_until_completed(command);
            if (!error.empty())
                return nil;
            return command;
        }
        void first_finished(id<MTLCommandBuffer> command) {
            timing(command);
            auto error = command_error(command);
            if (!error.empty()) {
                finish(std::move(error));
                return;
            }
            gw::Counters counters{};
            std::memcpy(&counters, state->counters.contents, sizeof(counters));
            {
                std::lock_guard lock(frame->mutex);
                static_assert(sizeof(frame->info.counters) == sizeof(counters));
                std::memcpy(&frame->info.counters, &counters, sizeof(counters));
            }
            if (counters.failure ||
                counters.completed != std::uint64_t(state->width) * state->height) {
                finish("Reconstruction guide validation failed (flags " +
                       std::to_string(counters.failure) + ", completed " +
                       std::to_string(counters.completed) + ")");
                return;
            }
            if (!state->denoiser && !state->temporal) {
                finish();
                return;
            }
            // Only validated guide contents may reach a temporal filter. Producer completion
            // establishes visibility; no ordering of a still-pending source is assumed.
            @autoreleasepool {
                auto second = [state->queue commandBuffer];
                second.label = @"rypath temporal reconstruction";
                stages = state->config.profile_stages
                             ? std::make_shared<MetalGPUStages>(state->device)
                             : nullptr;
                if (second == nil) {
                    finish("Temporal reconstruction command creation failed");
                    return;
                }
                if (!current) {
                    finish("Reconstruction job camera is unavailable");
                    return;
                }
                auto inputs = state->inputs;
                inputs.output = frame->output->texture;
                inputs.world_to_view = simd_matrix(current->world_to_view());
                inputs.view_to_clip = simd_matrix(current->view_to_clip());
                inputs.motion_scale_x = inputs.motion_scale_y = 1;
                inputs.jitter_x = inputs.jitter_y = 0;
                inputs.depth_reversed = true;
                inputs.reset_history = frame->info.history_reset;
                if (state->temporal) {
                    const bool approximate = frame->info.defocus_approximation ||
                                             counters.media_approximation ||
                                             counters.transmission_approximation ||
                                             counters.mix_fallback || state->previous_approximation;
                    if (approximate && camera_changed()) {
                        std::lock_guard lock(frame->mutex);
                        frame->info.history_reset = true;
                    }
                    error = state->temporal->encode(second,
                                                    {.color = inputs.color,
                                                     .depth = inputs.depth,
                                                     .motion = inputs.motion,
                                                     .diffuse_albedo = inputs.diffuse_albedo,
                                                     .specular_albedo = inputs.specular_albedo,
                                                     .normal = inputs.normal,
                                                     .previous_depth = state->previous_depth,
                                                     .identity = state->identity,
                                                     .output = inputs.output,
                                                     .reset_history = frame->info.history_reset},
                                                    stages.get());
                } else {
                    error = state->denoiser->encode(second, inputs);
                    if (stages) {
                        std::lock_guard lock(frame->mutex);
                        frame->info.gpu_stages.requested = true;
                        invalidate_gpu_stages(frame->info.gpu_stages,
                                              "unavailable: opaque MetalFX dispatch stages");
                    }
                }
                if (!error.empty()) {
                    finish(std::move(error));
                    return;
                }
                {
                    std::lock_guard lock(frame->mutex);
                    ++frame->info.command_count;
                }
                const auto self = shared_from_this();
                [second addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                  @autoreleasepool {
                      self->timing(completed);
                      auto error = command_error(completed);
                      if (self->state->temporal) {
                          const auto diagnostic = self->state->temporal->complete(error.empty());
                          if (error.empty())
                              error = diagnostic;
                          const auto stats = self->state->temporal->stats();
                          std::lock_guard lock(self->frame->mutex);
                          self->frame->info.history_accepted = stats.history_accepted;
                          self->frame->info.history_rejected = stats.history_rejected;
                      }
                      self->finish(std::move(error));
                  }
                }];
                [second commit];
            }
        }
    };
};
ReconstructedFrame::ReconstructedFrame(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
ReconstructedFrame::~ReconstructedFrame() = default;
ReconstructedFrame::ReconstructedFrame(ReconstructedFrame &&) noexcept = default;
ReconstructedFrame &ReconstructedFrame::operator=(ReconstructedFrame &&) noexcept = default;
ReconstructionStream::ReconstructionStream(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
ReconstructionStream::~ReconstructionStream() = default;
ReconstructionStream::ReconstructionStream(ReconstructionStream &&) noexcept = default;
ReconstructionStream &ReconstructionStream::operator=(ReconstructionStream &&) noexcept = default;
ReconstructionInfo ReconstructedFrame::poll() const {
    if (!impl_)
        return {.status = ReconstructionStatus::failed,
                .diagnostic = "Reconstructed frame is moved-from"};
    std::lock_guard lock(impl_->mutex);
    return impl_->info;
}
ReconstructionInfo ReconstructedFrame::wait() const {
    if (!impl_)
        return poll();
    std::unique_lock lock(impl_->mutex);
    impl_->completed.wait(lock,
                          [&] { return impl_->info.status != ReconstructionStatus::pending; });
    return impl_->info;
}
ReconstructionCapture ReconstructedFrame::capture() const {
    @autoreleasepool {
        ReconstructionCapture result;
        if (!impl_) {
            result.diagnostic = "Reconstructed frame is moved-from";
            return result;
        }
        std::lock_guard capture_lock(impl_->capture_mutex);
        const auto info = wait();
        result.width = info.width;
        result.height = info.height;
        if (info.status != ReconstructionStatus::complete) {
            result.diagnostic = info.diagnostic;
            return result;
        }
        const bool float32 = info.mode == ReconstructionMode::temporal;
        const std::size_t pixel_bytes = float32 ? 16 : 8;
        const std::size_t pitch = (std::size_t(info.width) * pixel_bytes + 255) & ~std::size_t(255);
        const auto lease = impl_->output;
        auto staging = [lease->device newBufferWithLength:pitch * info.height
                                                  options:MTLResourceStorageModeShared];
        if (staging == nil) {
            result.diagnostic = "Reconstruction capture allocation failed";
            return result;
        }
        auto command = [lease->queue commandBuffer];
        auto encoder = [command blitCommandEncoder];
        if (command == nil || encoder == nil) {
            result.diagnostic = "Reconstruction capture encoder creation failed";
            return result;
        }
        [encoder copyFromTexture:lease->texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(info.width, info.height, 1)
                            toBuffer:staging
                   destinationOffset:0
              destinationBytesPerRow:pitch
            destinationBytesPerImage:pitch * info.height];
        [encoder endEncoding];
        result.diagnostic = metal::complete(command, "reconstruction capture");
        if (!result.diagnostic.empty())
            return result;
        result.linear_rgb.resize(std::size_t(info.width) * info.height);
        const auto *bytes = static_cast<const unsigned char *>(staging.contents);
        for (std::size_t y = 0; y < info.height; ++y)
            for (std::size_t x = 0; x < info.width; ++x)
                for (unsigned c = 0; c < 3; ++c) {
                    float value;
                    if (float32) {
                        std::memcpy(&value, bytes + y * pitch + x * 16 + std::size_t(c) * 4, 4);
                    } else {
                        std::uint16_t bits;
                        std::memcpy(&bits, bytes + y * pitch + x * 8 + std::size_t(c) * 2, 2);
                        value = decode_half(bits);
                    }
                    if (!std::isfinite(value)) {
                        result.diagnostic = "Reconstruction capture contains nonfinite RGB";
                        result.linear_rgb.clear();
                        return result;
                    }
                    result.linear_rgb[y * info.width + x][c] = value;
                }
        result.status = GeometryStatus::success;
        return result;
    }
}
ReconstructionBuildResult build_reconstruction_stream(const Frame &source,
                                                      const ReconstructionConfig &config) {
    @autoreleasepool {
        ReconstructionBuildResult result;
        auto acquired = MetalFrameAccess::acquire(source);
        result.source_status = acquired.status;
        if (!acquired.lease) {
            result.diagnostic = acquired.diagnostic.empty()
                                    ? "Reconstruction requires a completed source frame"
                                    : acquired.diagnostic;
            return result;
        }
        auto impl = std::make_shared<ReconstructionStream::Impl>();
        result.diagnostic = impl->prepare(*acquired.lease, config);
        if (!result.diagnostic.empty())
            return result;
        result.stream =
            std::unique_ptr<ReconstructionStream>(new ReconstructionStream(std::move(impl)));
        result.status = GeometryStatus::success;
        return result;
    }
}
ReconstructionSubmitResult ReconstructionStream::submit(const Frame &source, bool reset_history) {
    @autoreleasepool {
        ReconstructionSubmitResult result;
        if (!impl_) {
            result.diagnostic = "Reconstruction stream is moved-from";
            return result;
        }
        auto acquired = MetalFrameAccess::acquire(source);
        result.source_status = acquired.status;
        if (!acquired.lease) {
            result.diagnostic = acquired.diagnostic.empty()
                                    ? "Reconstruction requires a completed source frame"
                                    : acquired.diagnostic;
            return result;
        }
        if (acquired.info.width != impl_->width || acquired.info.height != impl_->height ||
            acquired.lease->parameters().model != impl_->model) {
            result.diagnostic = "Reconstruction source dimensions/model differ from the stream";
            return result;
        }
        auto camera = make_camera(*acquired.lease, impl_->config.near_plane);
        if (!camera.camera) {
            result.diagnostic = camera.diagnostic;
            return result;
        }
        typename Impl::Slot *slot = nullptr;
        bool reset = true;
        std::optional<reconstruction::Camera> previous;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->pending) {
                result.busy = true;
                result.diagnostic = "Reconstruction frame is pending";
                return result;
            }
            for (auto &candidate : impl_->slots)
                if (candidate.active.expired()) {
                    slot = &candidate;
                    break;
                }
            if (!slot) {
                result.busy = true;
                result.diagnostic = "Both reconstruction output slots are retained";
                return result;
            }
            reset = reset_history || !impl_->previous_valid || !impl_->epoch_known ||
                    impl_->epoch != acquired.info.epoch ||
                    impl_->previous_lens_radius != acquired.lease->parameters().lens_radius ||
                    impl_->previous_focal_distance != acquired.lease->parameters().focal_distance ||
                    impl_->previous_camera_medium != acquired.lease->parameters().camera_medium;
            if (reset)
                impl_->guide_key.reset();
            previous = reset ? camera.camera : impl_->previous;
        }
        auto frame = std::make_shared<ReconstructedFrame::Impl>();
        frame->output = std::make_shared<OutputLease>();
        frame->output->device = impl_->device;
        frame->output->queue = impl_->queue;
        frame->output->texture = slot->texture;
        frame->info = {.source = acquired.info,
                       .mode = impl_->config.mode,
                       .width = impl_->config.output_width,
                       .height = impl_->config.output_height,
                       .history_reset = reset,
                       .defocus_approximation = impl_->config.mode != ReconstructionMode::raw_rgb &&
                                                acquired.lease->parameters().lens_radius > 0};
        auto job = std::make_shared<Impl::Job>();
        job->state = impl_;
        job->frame = frame;
        job->source = std::move(acquired.lease);
        job->current = *camera.camera;
        std::string error;
        if (!previous) {
            result.diagnostic = "Reconstruction previous camera is unavailable";
            return result;
        }
        auto command = job->prepare(error, *previous, !reset);
        if (command == nil) {
            impl_->guide_key.reset();
            impl_->previous_valid = false;
            result.diagnostic = std::move(error);
            return result;
        }
        {
            std::lock_guard lock(impl_->mutex);
            impl_->pending = true;
            impl_->last_completed.reset();
            if (reset)
                impl_->previous_valid = false;
            slot->active = frame->output;
        }
        frame->info.command_count = 1;
        [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
          @autoreleasepool {
              job->first_finished(completed);
          }
        }];
        [command commit];
        result.frame =
            std::unique_ptr<ReconstructedFrame>(new ReconstructedFrame(std::move(frame)));
        result.status = GeometryStatus::success;
        return result;
    }
}
MetalReconstructionGuideSnapshot
MetalReconstructionStreamProbe::capture_guides(const ReconstructionStream &stream) {
    MetalReconstructionGuideSnapshot result;
    if (!stream.impl_) {
        result.diagnostic = "Reconstruction stream is moved-from";
        return result;
    }
    auto &s = *stream.impl_;
    std::lock_guard lock(s.mutex);
    if (s.pending || !s.previous_valid || !s.surface) {
        result.diagnostic = "Surface guide snapshot requires a successful completed job";
        return result;
    }
    const std::array<id<MTLTexture>, 8> textures{
        s.inputs.depth,  s.inputs.motion,    s.inputs.diffuse_albedo, s.inputs.specular_albedo,
        s.inputs.normal, s.inputs.roughness, s.previous_depth,        s.identity};
    constexpr std::array<unsigned, 8> pixel_bytes{4, 4, 8, 8, 8, 2, 4, 16};
    std::array<id<MTLBuffer>, 8> staging{};
    auto command = [s.queue commandBuffer];
    auto blit = [command blitCommandEncoder];
    if (command == nil || blit == nil) {
        result.diagnostic = "Surface guide readback encoder creation failed";
        return result;
    }
    for (unsigned i = 0; i < textures.size(); ++i) {
        const auto row = (std::uint64_t(s.width) * pixel_bytes[i] + 255) & ~255ull;
        staging[i] = [s.device newBufferWithLength:row * s.height
                                           options:MTLResourceStorageModeShared];
        if (staging[i] == nil) {
            [blit endEncoding];
            result.diagnostic = "Surface guide readback allocation failed";
            return result;
        }
        [blit copyFromTexture:textures[i]
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(s.width, s.height, 1)
                            toBuffer:staging[i]
                   destinationOffset:0
              destinationBytesPerRow:row
            destinationBytesPerImage:row * s.height];
    }
    [blit endEncoding];
    result.diagnostic = metal::complete(command, "surface guide readback");
    if (!result.diagnostic.empty())
        return result;
    for (unsigned i = 0; i < textures.size(); ++i) {
        const auto bytes = std::size_t(s.width) * pixel_bytes[i];
        const auto row = (bytes + 255) & ~std::size_t(255);
        result.textures[i].resize(bytes * s.height);
        for (unsigned y = 0; y < s.height; ++y)
            std::memcpy(result.textures[i].data() + y * bytes,
                        static_cast<const std::byte *>(staging[i].contents) + y * row, bytes);
    }
    return result;
}
MetalReconstructionSnapshot
MetalReconstructionStreamProbe::capture_diagnostics(const ReconstructionStream &stream) {
    MetalReconstructionSnapshot result;
    if (!stream.impl_) {
        result.diagnostic = "Reconstruction stream is moved-from";
        return result;
    }
    auto &s = *stream.impl_;
    std::lock_guard lock(s.mutex);
    auto frame = s.last_completed.lock();
    if (!s.config.capture_diagnostics || s.pending || !frame) {
        result.diagnostic =
            "Diagnostic snapshot requires opt-in and a retained last successful output";
        return result;
    }
    {
        std::lock_guard frame_lock(frame->mutex);
        result.info = frame->info;
    }
    std::vector<NamedMetalTexture> textures{{"input.color", s.inputs.color},
                                            {"output", frame->output->texture}};
    if (s.surface) {
        const std::array<NamedMetalTexture, 6> guides{
            {{"input.depth", s.inputs.depth},
             {"input.motion", s.inputs.motion},
             {"input.diffuse_albedo", s.inputs.diffuse_albedo},
             {"input.specular_albedo", s.inputs.specular_albedo},
             {"input.normal", s.inputs.normal},
             {"input.roughness", s.inputs.roughness}}};
        textures.insert(textures.end(), guides.begin(), guides.end());
    }
    if (s.denoiser)
        textures.push_back({"input.exposure", s.inputs.exposure});
    if (s.temporal) {
        textures.push_back({"input.previous_depth", s.previous_depth});
        textures.push_back({"input.identity", s.identity});
        auto temporal = s.temporal->diagnostic_textures();
        if (temporal.empty()) {
            result.diagnostic = "Temporal diagnostic history is unavailable";
            return result;
        }
        textures.insert(textures.end(), temporal.begin(), temporal.end());
    }
    const auto pixel_bytes = [](MTLPixelFormat format) -> std::uint32_t {
        switch (format) {
        case MTLPixelFormatR16Float:
            return 2;
        case MTLPixelFormatR32Float:
        case MTLPixelFormatR32Uint:
        case MTLPixelFormatRG16Float:
            return 4;
        case MTLPixelFormatRGBA16Float:
            return 8;
        case MTLPixelFormatRGBA32Float:
        case MTLPixelFormatRGBA32Uint:
            return 16;
        default:
            return 0;
        }
    };
    std::uint64_t total = 0;
    for (const auto &entry : textures) {
        const auto texture = entry.texture;
        const auto bytes = pixel_bytes(texture.pixelFormat);
        if (!texture || !bytes || texture.width > reconstruction_dimension_limit ||
            texture.height > reconstruction_dimension_limit) {
            result.diagnostic = "Invalid diagnostic texture format or extent";
            return result;
        }
        total += std::uint64_t(texture.width) * texture.height * bytes;
    }
    // An explicit snapshot is intentionally bounded even on very large devices.
    // Stream allocations have their own conservative resource accounting.
    if (total > (1ull << 30)) {
        result.diagnostic = "Diagnostic snapshot exceeds the 1 GiB readback limit";
        return result;
    }
    try {
        for (const auto &entry : textures) {
            const auto texture = entry.texture;
            const auto bytes = std::size_t(texture.width) * pixel_bytes(texture.pixelFormat);
            const auto row = (bytes + 255) & ~std::size_t(255);
            auto staging = [s.device newBufferWithLength:row * texture.height
                                                 options:MTLResourceStorageModeShared];
            auto command = [s.queue commandBuffer];
            auto blit = [command blitCommandEncoder];
            if (!staging || !command || !blit) {
                if (blit)
                    [blit endEncoding];
                result.textures.clear();
                result.diagnostic = "Diagnostic snapshot staging allocation failed";
                return result;
            }
            [blit copyFromTexture:texture
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                                toBuffer:staging
                       destinationOffset:0
                  destinationBytesPerRow:row
                destinationBytesPerImage:row * texture.height];
            [blit endEncoding];
            result.diagnostic = metal::complete(command, "diagnostic snapshot");
            if (!result.diagnostic.empty()) {
                result.textures.clear();
                return result;
            }
            MetalReconstructionTextureSnapshot snapshot;
            snapshot.name = entry.name;
            snapshot.width = std::uint32_t(texture.width);
            snapshot.height = std::uint32_t(texture.height);
            snapshot.format = texture.pixelFormat;
            snapshot.bytes.resize(bytes * texture.height);
            for (std::size_t y = 0; y < texture.height; ++y)
                std::memcpy(snapshot.bytes.data() + y * bytes,
                            static_cast<const std::byte *>(staging.contents) + y * row, bytes);
            result.textures.push_back(std::move(snapshot));
        }
    } catch (const std::bad_alloc &) {
        result.textures.clear();
        result.diagnostic = "Diagnostic snapshot host allocation failed";
    }
    return result;
}
struct MetalReconstructedFrameLease::Impl {
    std::shared_ptr<ReconstructedFrame::Impl> frame;
};
MetalReconstructedFrameLease::MetalReconstructedFrameLease(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
MetalReconstructedFrameLease::~MetalReconstructedFrameLease() = default;
MetalReconstructedFrameLease::MetalReconstructedFrameLease(
    MetalReconstructedFrameLease &&) noexcept = default;
MetalReconstructedFrameLease &
MetalReconstructedFrameLease::operator=(MetalReconstructedFrameLease &&) noexcept = default;
id<MTLDevice> MetalReconstructedFrameLease::device() const {
    return impl_ ? impl_->frame->output->device : nil;
}
id<MTLCommandQueue> MetalReconstructedFrameLease::queue() const {
    return impl_ ? impl_->frame->output->queue : nil;
}
id<MTLTexture> MetalReconstructedFrameLease::output_texture() const {
    return impl_ ? impl_->frame->output->texture : nil;
}
std::string
MetalReconstructedFrameLease::retain_until_completed(id<MTLCommandBuffer> command) const {
    if (!impl_)
        return "Reconstructed frame lease is moved-from";
    if (command == nil || command.device != device() ||
        command.status >= MTLCommandBufferStatusCommitted)
        return "Attach output retention to a same-device consumer command before commit";
    const auto retained = impl_->frame;
    [command addCompletedHandler:^(id<MTLCommandBuffer>) {
      (void)retained;
    }];
    return {};
}
MetalReconstructedFrameAcquireResult
MetalReconstructedFrameAccess::acquire(const ReconstructedFrame &frame) {
    MetalReconstructedFrameAcquireResult result;
    if (!frame.impl_) {
        result.diagnostic = "Reconstructed frame is moved-from";
        return result;
    }
    {
        std::lock_guard lock(frame.impl_->mutex);
        result.info = frame.impl_->info;
    }
    result.status = result.info.status;
    result.diagnostic = result.info.diagnostic;
    if (result.status != ReconstructionStatus::complete)
        return result;
    auto impl = std::make_unique<MetalReconstructedFrameLease::Impl>();
    impl->frame = frame.impl_;
    result.lease = std::unique_ptr<MetalReconstructedFrameLease>(
        new MetalReconstructedFrameLease(std::move(impl)));
    return result;
}
} // namespace rypath
