// SPDX-License-Identifier: GPL-3.0-only
#include "metal_execution.hpp"
#include "metal_frame_access.hpp"
#include "metal_reconstruction_access.hpp"
#include "metalfx_denoiser.hpp"
#include "reconstruction.hpp"
#include "reconstruction_camera.hpp"
#include "reconstruction_guide_wire.h"
#include "rgb_spectrum.hpp"
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
    if (lease.camera().raster_camera)
        return reconstruction::Camera::build(*lease.camera().raster_camera, config);
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
    ReconstructionConfig config;
    std::uint32_t width = 0, height = 0, model = 0;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    std::unique_ptr<MetalFramePipeline> reset, surface, color;
    std::unique_ptr<MetalFXDenoiser> denoiser;
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
            config.mode != ReconstructionMode::metalfx)
            return "Invalid reconstruction mode";
        if (!std::isfinite(config.near_plane) || !(config.near_plane > 0) ||
            !std::isnormal(float(config.near_plane)))
            return "Reconstruction near plane must pack to a positive normal float";
        if (config.mode == ReconstructionMode::raw_rgb &&
            (config.output_width != width || config.output_height != height))
            return "Raw RGB reconstruction preserves input dimensions";
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
            actual.argument_count != 11 || actual.reserved[0] || actual.reserved[1])
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
        const auto pixels = std::uint64_t(width) * height;
        const auto row = (std::uint64_t(config.output_width) * 8 + 255) & ~std::uint64_t(255);
        const auto capture_bytes = row * config.output_height;
        // This bounds stream-owned resources, including two simultaneous captures.
        // MetalFX does not expose its internal allocation sizes; those are excluded.
        const auto owned_bytes = 42 * pixels + 16 * output_pixels + 2 * capture_bytes +
                                 sizeof(gw::Config) + sizeof(gw::Counters) +
                                 sizeof(gw::SpectralNode) * gw::node_count +
                                 argument_encoder.encodedLength + 2;
        if (capture_bytes > device.maxBufferLength || owned_bytes > (8ull << 30) ||
            (device.recommendedMaxWorkingSetSize &&
             owned_bytes > device.recommendedMaxWorkingSetSize))
            return "Reconstruction textures/captures exceed the owned-resource memory budget";
        if (config.mode == ReconstructionMode::metalfx) {
            build = lease.create_guide_pipeline(@"rypath_reconstruction_guide_surface");
            if (!build.pipeline)
                return build.diagnostic;
            surface = std::move(build.pipeline);
            auto fx = MetalFXDenoiser::create(
                device, {width, height, config.output_width, config.output_height});
            if (!fx.denoiser)
                return fx.diagnostic;
            denoiser = std::move(fx.denoiser);
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
        inputs.exposure = texture(device, requirements.exposure, MTLStorageModeShared);
        if (inputs.color == nil || inputs.depth == nil || inputs.motion == nil ||
            inputs.diffuse_albedo == nil || inputs.specular_albedo == nil || inputs.normal == nil ||
            inputs.roughness == nil || inputs.exposure == nil)
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
        bool all_times_valid = true;
        double gpu_seconds = 0;
        void timing(id<MTLCommandBuffer> command) {
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
                if (!error.empty()) {
                    state->previous_valid = false;
                    frame->info.status = ReconstructionStatus::failed;
                    frame->info.diagnostic = std::move(error);
                    frame->info.gpu_time_valid = false;
                } else {
                    state->previous = current;
                    state->previous_lens_radius = source->parameters().lens_radius;
                    state->previous_focal_distance = source->parameters().focal_distance;
                    state->previous_valid = state->config.mode == ReconstructionMode::metalfx;
                    state->epoch_known = true;
                    state->epoch = frame->info.source.epoch;
                    frame->info.status = ReconstructionStatus::complete;
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
            std::memcpy(s.configuration.contents, &cfg, sizeof(cfg));
            std::memset(s.counters.contents, 0, s.counters.length);
            [s.argument_encoder setArgumentBuffer:s.arguments offset:0];
            const std::array<id<MTLBuffer>, 4> buffers{s.configuration, source->raw_xyz(), s.nodes,
                                                       s.counters};
            for (unsigned i = 0; i < buffers.size(); ++i)
                [s.argument_encoder setBuffer:buffers[i] offset:0 atIndex:i];
            const std::array<id<MTLTexture>, 7> textures{
                s.inputs.color,          s.inputs.depth,           s.inputs.motion,
                s.inputs.diffuse_albedo, s.inputs.specular_albedo, s.inputs.normal,
                s.inputs.roughness};
            for (unsigned i = 0; i < textures.size(); ++i)
                [s.argument_encoder setTexture:textures[i] atIndex:i + 4];
            auto command = [s.queue commandBuffer];
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
            [encoder useResource:s.configuration usage:MTLResourceUsageRead];
            [encoder useResource:source->raw_xyz() usage:MTLResourceUsageRead];
            [encoder useResource:s.nodes usage:MTLResourceUsageRead];
            [encoder useResource:s.counters usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            for (auto input : textures)
                [encoder useResource:input usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            const auto dispatch = [&](const MetalFramePipeline &pipeline, NSUInteger count) {
                [encoder setComputePipelineState:pipeline.pipeline()];
                [encoder setBuffer:s.arguments offset:0 atIndex:1];
                [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(
                                                          count == 1 ? 1 : 32,
                                                          metal::group_width(s.device,
                                                                             pipeline.pipeline())),
                                                      1, 1)];
                [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
            };
            dispatch(*s.reset, 1);
            const auto pixels = std::uint64_t(s.width) * s.height;
            if (s.surface) {
                error = source->bind_guides(encoder, *s.surface, s.arguments);
                if (!error.empty()) {
                    [encoder endEncoding];
                    return nil;
                }
                dispatch(*s.surface, pixels);
            }
            dispatch(*s.color, pixels);
            [encoder endEncoding];
            if (!s.denoiser) {
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
            if (!state->denoiser) {
                finish();
                return;
            }
            // Only validated guide contents may reach MetalFX. Producer completion
            // establishes visibility; no ordering of a still-pending source is assumed.
            @autoreleasepool {
                auto second = [state->queue commandBuffer];
                if (second == nil) {
                    finish("MetalFX command creation failed");
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
                error = state->denoiser->encode(second, inputs);
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
                      self->finish(command_error(completed));
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
        const std::size_t pitch = (std::size_t(info.width) * 8 + 255) & ~std::size_t(255);
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
                    std::uint16_t bits;
                    std::memcpy(&bits, bytes + y * pitch + x * 8 + c * 2, 2);
                    const auto value = decode_half(bits);
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
                    impl_->previous_focal_distance != acquired.lease->parameters().focal_distance;
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
                       .defocus_approximation = impl_->config.mode == ReconstructionMode::metalfx &&
                                                acquired.lease->parameters().lens_radius > 0};
        auto job = std::make_shared<Impl::Job>();
        job->state = impl_;
        job->frame = frame;
        job->source = std::move(acquired.lease);
        job->current = *camera.camera;
        std::string error;
        auto command = job->prepare(error, *previous, !reset);
        if (command == nil) {
            result.diagnostic = std::move(error);
            return result;
        }
        {
            std::lock_guard lock(impl_->mutex);
            impl_->pending = true;
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
