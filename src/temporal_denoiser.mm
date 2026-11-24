// SPDX-License-Identifier: GPL-3.0-only
#include "temporal_denoiser.hpp"
#include "metal_frame_access.hpp"
#include "metal_gpu_diagnostics.hpp"
#include "temporal_denoiser_wire.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace rypath {
namespace tw = temporal_denoiser_wire;
namespace {
constexpr std::uint32_t dimension_limit = 16384;
constexpr std::uint64_t resource_limit = 8ull << 30;
struct History {
    id<MTLTexture> mean = nil, variance = nil, count = nil;
    id<MTLTexture> depth = nil, normal = nil, identity = nil;
};
struct Spatial {
    id<MTLTexture> mean = nil, variance = nil;
};
std::string validate_texture(id<MTLTexture> texture, id<MTLDevice> device, std::uint32_t width,
                             std::uint32_t height, MTLPixelFormat format, MTLTextureUsage usage,
                             const char *name) {
    const auto prefix = std::string("Temporal denoiser ") + name + " texture ";
    if (!texture)
        return prefix + "is missing";
    if (texture.device != device)
        return prefix + "belongs to a different device";
    if (texture.parentTexture || texture.buffer || texture.heap || texture.iosurface != nullptr)
        return prefix + "requires standalone storage";
    if (texture.textureType != MTLTextureType2D || texture.sampleCount != 1 ||
        texture.arrayLength != 1 || texture.depth != 1 || texture.mipmapLevelCount != 1)
        return prefix + "must be a single-sample, single-mip 2D texture";
    if (texture.width != width || texture.height != height)
        return prefix + "extent mismatch";
    if (texture.pixelFormat != format)
        return prefix + "format mismatch";
    if ((texture.usage & usage) != usage)
        return prefix + "usage mismatch";
    if (texture.hazardTrackingMode == MTLHazardTrackingModeUntracked)
        return prefix + "requires tracked hazards";
    if (texture.storageMode == MTLStorageModeMemoryless)
        return prefix + "cannot be memoryless";
    return {};
}
id<MTLTexture> allocate(id<MTLDevice> device, std::uint32_t width, std::uint32_t height,
                        MTLPixelFormat format) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                         width:width
                                                                        height:height
                                                                     mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
    return [device newTextureWithDescriptor:descriptor];
}
} // namespace
struct TemporalDenoiser::Impl {
    id<MTLDevice> device = nil;
    id<MTLBuffer> counters = nil;
    std::uint32_t width = 0, height = 0, history_limit = 0, iterations = 0;
    bool variance_prefilter = false, strict_spatial_segments = false;
    SpatialVariancePolicy spatial_variance_policy = SpatialVariancePolicy::paired;
    std::array<History, 2> history;
    std::array<Spatial, 2> spatial;
    std::array<std::unique_ptr<MetalFramePipeline>, 5> pipelines;
    std::vector<Spatial> trace;
    unsigned current = 0;
    bool valid = false, pending = false;
    TemporalDenoiserStats last;
};
TemporalDenoiser::TemporalDenoiser(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TemporalDenoiser::~TemporalDenoiser() = default;
std::uint64_t TemporalDenoiser::owned_resource_bytes(std::uint32_t width, std::uint32_t height,
                                                     bool capture_diagnostics) {
    if (!width || !height || width > dimension_limit || height > dimension_limit)
        return std::numeric_limits<std::uint64_t>::max();
    const auto padded_width = (std::uint64_t(width) + 63) / 64 * 64;
    const auto padded_height = (std::uint64_t(height) + 63) / 64 * 64;
    return padded_width * padded_height * (capture_diagnostics ? 384 : 192) +
           (capture_diagnostics ? 30 : 18) * 65536ull;
}
TemporalDenoiserCreateResult TemporalDenoiser::create(MetalFrameLease &lease,
                                                      const TemporalDenoiserConfig &config) {
    const auto [width, height, history_limit, spatial_iterations, variance_prefilter,
                spatial_variance_policy, strict_spatial_segments, capture_diagnostics] = config;
    if (static_cast<unsigned>(spatial_variance_policy) > 2u ||
        (variance_prefilter && spatial_variance_policy != SpatialVariancePolicy::center))
        return {.diagnostic =
                    "Variance prefilter requires center policy; variance policy must be valid"};
    if (!lease.device())
        return {.diagnostic = "Temporal denoiser requires a Metal device"};
    const auto bytes = owned_resource_bytes(width, height, capture_diagnostics);
    if (bytes == std::numeric_limits<std::uint64_t>::max())
        return {.diagnostic = "Temporal denoiser dimensions must be in [1,16384]"};
    if (history_limit == 0 || history_limit > tw::maximum_history ||
        spatial_iterations > tw::maximum_iterations)
        return {.diagnostic =
                    "Temporal denoiser requires history_limit 1..64 and spatial_iterations 0..5"};
    if (bytes > resource_limit || (lease.device().recommendedMaxWorkingSetSize &&
                                   bytes > lease.device().recommendedMaxWorkingSetSize))
        return {.diagnostic = "Temporal denoiser owned resources exceed memory budget"};
    if (lease.info().width != width || lease.info().height != height)
        return {.diagnostic = "Temporal denoiser extent must match its source frame"};
    auto impl = std::make_unique<Impl>();
    impl->device = lease.device();
    impl->width = width;
    impl->height = height;
    impl->history_limit = history_limit;
    impl->iterations = spatial_iterations;
    impl->variance_prefilter = variance_prefilter;
    impl->spatial_variance_policy = spatial_variance_policy;
    impl->strict_spatial_segments = strict_spatial_segments;
    const std::array<NSString *, 5> entries{
        @"rypath_temporal_denoiser_reset", @"rypath_temporal_denoiser_accumulate",
        @"rypath_temporal_denoiser_spatial", @"rypath_temporal_denoiser_finish",
        @"rypath_temporal_denoiser_snapshot"};
    for (std::size_t i = 0; i < (capture_diagnostics ? entries.size() : entries.size() - 1); ++i) {
        auto built = lease.create_guide_pipeline(entries[i]);
        if (built.status != GeometryStatus::success || !built.pipeline)
            return {.diagnostic = "Temporal denoiser pipeline: " + built.diagnostic};
        impl->pipelines[i] = std::move(built.pipeline);
    }
    auto contract_pipeline = lease.create_guide_pipeline(@"rypath_temporal_denoiser_contract");
    if (contract_pipeline.status != GeometryStatus::success || !contract_pipeline.pipeline)
        return {.diagnostic =
                    "Temporal denoiser contract pipeline: " + contract_pipeline.diagnostic};
    auto contract_buffer = [impl->device newBufferWithLength:sizeof(tw::Contract)
                                                     options:MTLResourceStorageModeShared];
    auto contract_command = [lease.queue() commandBuffer];
    auto contract_encoder = [contract_command computeCommandEncoder];
    if (!contract_buffer || !contract_command || !contract_encoder)
        return {.diagnostic = "Temporal denoiser contract allocation failed"};
    [contract_encoder setComputePipelineState:contract_pipeline.pipeline->pipeline()];
    [contract_encoder setBuffer:contract_buffer offset:0 atIndex:0];
    [contract_encoder dispatchThreads:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [contract_encoder endEncoding];
    [contract_command commit];
    [contract_command waitUntilCompleted];
    if (contract_command.status != MTLCommandBufferStatusCompleted)
        return {.diagnostic = "Temporal denoiser contract GPU command failed"};
    tw::Contract contract{};
    std::memcpy(&contract, contract_buffer.contents, sizeof(contract));
    if (contract.version != tw::version || contract.config_bytes != sizeof(tw::Config) ||
        contract.counters_bytes != sizeof(tw::Counters) ||
        contract.texture_count != tw::spatial_group_id + 1u ||
        std::any_of(std::begin(contract.reserved), std::end(contract.reserved),
                    [](auto word) { return word != 0; }))
        return {.diagnostic =
                    "Temporal denoiser shader contract mismatch; rebuild the Metal library"};
    bool allocated = true;
    std::uint64_t actual = 0;
    const auto texture = [&](MTLPixelFormat format) {
        auto result = allocate(impl->device, width, height, format);
        allocated = allocated && result != nil;
        if (result)
            actual += result.allocatedSize;
        return result;
    };
    for (auto &bank : impl->history) {
        bank.mean = texture(MTLPixelFormatRGBA32Float);
        bank.variance = texture(MTLPixelFormatRGBA32Float);
        bank.count = texture(MTLPixelFormatR32Uint);
        bank.depth = texture(MTLPixelFormatR32Float);
        bank.normal = texture(MTLPixelFormatRGBA16Float);
        bank.identity = texture(MTLPixelFormatRGBA32Uint);
    }
    for (auto &bank : impl->spatial) {
        bank.mean = texture(MTLPixelFormatRGBA32Float);
        bank.variance = texture(MTLPixelFormatRGBA32Float);
    }
    if (capture_diagnostics) {
        impl->trace.resize(spatial_iterations + 1);
        for (auto &pass : impl->trace) {
            pass.mean = texture(MTLPixelFormatRGBA32Float);
            pass.variance = texture(MTLPixelFormatRGBA32Float);
        }
    }
    impl->counters = [impl->device newBufferWithLength:sizeof(tw::Counters)
                                               options:MTLResourceStorageModeShared];
    if (!allocated || !impl->counters)
        return {.diagnostic = "Temporal denoiser resource allocation failed"};
    actual += impl->counters.allocatedSize;
    if (actual > bytes)
        return {.diagnostic =
                    "Temporal denoiser allocation exceeds conservative resource estimate"};
    return {.denoiser = std::unique_ptr<TemporalDenoiser>(new TemporalDenoiser(std::move(impl)))};
}
std::string TemporalDenoiser::encode(id<MTLCommandBuffer> command,
                                     const TemporalDenoiserInputs &inputs, MetalGPUStages *stages) {
    // A rejected overlapping encode must not alter a running command's state.
    if (impl_->pending)
        return "Temporal denoiser already has a pending command";
    const auto reject = [&](std::string error) {
        impl_->valid = false;
        return error;
    };
    if (!command || command.device != impl_->device)
        return reject("Temporal denoiser command requires the same Metal device");
    if (command.status >= MTLCommandBufferStatusCommitted)
        return reject("Temporal denoiser command is already committed");
    struct Binding {
        id<MTLTexture> texture;
        MTLPixelFormat format;
        const char *name;
        unsigned index;
    };
    const std::array<Binding, 12> bindings{
        {{inputs.color, MTLPixelFormatRGBA32Float, "color", tw::color_id},
         {inputs.depth, MTLPixelFormatR32Float, "depth", tw::depth_id},
         {inputs.motion, MTLPixelFormatRG16Float, "motion", tw::motion_id},
         {inputs.diffuse_albedo, MTLPixelFormatRGBA16Float, "diffuse", tw::diffuse_id},
         {inputs.specular_albedo, MTLPixelFormatRGBA16Float, "specular", tw::specular_id},
         {inputs.normal, MTLPixelFormatRGBA16Float, "normal", tw::normal_id},
         {inputs.previous_depth, MTLPixelFormatR32Float, "previous depth", tw::previous_depth_id},
         {inputs.identity, MTLPixelFormatRGBA32Uint, "identity", tw::identity_id},
         {inputs.spatial_position, MTLPixelFormatRGBA32Float, "spatial position",
          tw::spatial_position_id},
         {inputs.spatial_normal, MTLPixelFormatRGBA32Float, "spatial normal",
          tw::spatial_normal_id},
         {inputs.spatial_group, MTLPixelFormatR32Uint, "spatial group", tw::spatial_group_id},
         {inputs.output, MTLPixelFormatRGBA32Float, "output", tw::output_id}}};
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        const auto error = validate_texture(
            binding.texture, impl_->device, impl_->width, impl_->height, binding.format,
            i == bindings.size() - 1 ? MTLTextureUsageShaderWrite : MTLTextureUsageShaderRead,
            binding.name);
        if (!error.empty())
            return reject(error);
        for (std::size_t j = 0; j < i; ++j)
            if (binding.texture == bindings[j].texture)
                return reject("Temporal denoiser textures must not alias");
    }
    auto encoder = [command computeCommandEncoder];
    encoder.label = @"signed temporal reconstruction";
    if (!encoder)
        return reject("Temporal denoiser compute encoder allocation failed");
    for (const auto &binding : bindings)
        [encoder setTexture:binding.texture atIndex:binding.index];
    const auto &previous = impl_->history[impl_->current];
    const auto &next = impl_->history[1 - impl_->current];
    const std::array<id<MTLTexture>, 12> history{
        previous.mean,   previous.variance, previous.count, previous.depth,
        previous.normal, previous.identity, next.mean,      next.variance,
        next.count,      next.depth,        next.normal,    next.identity};
    for (unsigned i = 0; i < history.size(); ++i)
        [encoder setTexture:history[i] atIndex:tw::history_mean_id + i];
    [encoder setBuffer:impl_->counters offset:0 atIndex:1];
    const bool split_encoders = stages && stages->splits_encoders();
    id<MTLTexture> source_mean = nil, source_variance = nil, destination_mean = nil,
                   destination_variance = nil;
    const auto dispatch = [&](unsigned index, const tw::Config &config, bool single = false) {
        static constexpr const char *labels[] = {"temporal.reset", "temporal.accumulate",
                                                 "temporal.spatial", "temporal.finish",
                                                 "temporal.snapshot"};
        if (stages && !stages->begin(command, encoder, labels[index]))
            return;
        if (split_encoders) {
            for (const auto &binding : bindings)
                [encoder setTexture:binding.texture atIndex:binding.index];
            for (unsigned i = 0; i < history.size(); ++i)
                [encoder setTexture:history[i] atIndex:tw::history_mean_id + i];
            [encoder setBuffer:impl_->counters offset:0 atIndex:1];
            [encoder setTexture:source_mean atIndex:tw::source_mean_id];
            [encoder setTexture:source_variance atIndex:tw::source_variance_id];
            [encoder setTexture:destination_mean atIndex:tw::destination_mean_id];
            [encoder setTexture:destination_variance atIndex:tw::destination_variance_id];
        }
        auto pipeline = impl_->pipelines[index]->pipeline();
        [encoder setComputePipelineState:pipeline];
        [encoder setBytes:&config length:sizeof(config) atIndex:0];
        const NSUInteger width =
            single ? 1 : std::min<NSUInteger>(32, pipeline.maxTotalThreadsPerThreadgroup);
        [encoder dispatchThreads:MTLSizeMake(single ? 1 : impl_->width, single ? 1 : impl_->height,
                                             1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        if (stages)
            stages->end(encoder);
        [encoder memoryBarrierWithScope:MTLBarrierScopeTextures | MTLBarrierScopeBuffers];
    };
    tw::Config config{impl_->width,
                      impl_->height,
                      impl_->history_limit,
                      inputs.reset_history || !impl_->valid ? 1u : 0u,
                      1,
                      impl_->iterations,
                      tw::version,
                      (impl_->variance_prefilter ? 1u : 0u) |
                          (impl_->strict_spatial_segments ? 2u : 0u) |
                          (static_cast<unsigned>(impl_->spatial_variance_policy) << 2u)};
    dispatch(0, config, true);
    if (split_encoders) {
        destination_mean = impl_->spatial[0].mean;
        destination_variance = impl_->spatial[0].variance;
    }
    [encoder setTexture:impl_->spatial[0].mean atIndex:tw::destination_mean_id];
    [encoder setTexture:impl_->spatial[0].variance atIndex:tw::destination_variance_id];
    dispatch(1, config);
    const auto snapshot = [&](unsigned pass, unsigned source) {
        if (impl_->trace.empty())
            return;
        source_mean = impl_->spatial[source].mean;
        source_variance = impl_->spatial[source].variance;
        destination_mean = impl_->trace[pass].mean;
        destination_variance = impl_->trace[pass].variance;
        [encoder setTexture:source_mean atIndex:tw::source_mean_id];
        [encoder setTexture:source_variance atIndex:tw::source_variance_id];
        [encoder setTexture:destination_mean atIndex:tw::destination_mean_id];
        [encoder setTexture:destination_variance atIndex:tw::destination_variance_id];
        dispatch(4, config);
    };
    unsigned source = 0;
    snapshot(0, source);
    for (unsigned i = 0; i < impl_->iterations; ++i) {
        config.step = 1u << i;
        config.iteration = i;
        if (split_encoders) {
            source_mean = impl_->spatial[source].mean;
            source_variance = impl_->spatial[source].variance;
            destination_mean = impl_->spatial[1 - source].mean;
            destination_variance = impl_->spatial[1 - source].variance;
        }
        [encoder setTexture:impl_->spatial[source].mean atIndex:tw::source_mean_id];
        [encoder setTexture:impl_->spatial[source].variance atIndex:tw::source_variance_id];
        [encoder setTexture:impl_->spatial[1 - source].mean atIndex:tw::destination_mean_id];
        [encoder setTexture:impl_->spatial[1 - source].variance
                    atIndex:tw::destination_variance_id];
        dispatch(2, config);
        source = 1 - source;
        snapshot(i + 1, source);
    }
    if (split_encoders)
        source_mean = impl_->spatial[source].mean;
    [encoder setTexture:impl_->spatial[source].mean atIndex:tw::source_mean_id];
    dispatch(3, config);
    [encoder endEncoding];
    if (stages && stages->encoding_failed())
        return reject("Profiling temporal compute encoder allocation failed");
    impl_->pending = true;
    return {};
}
std::string TemporalDenoiser::complete(bool command_succeeded) {
    if (!impl_->pending) {
        impl_->valid = false;
        return "Temporal denoiser has no pending command";
    }
    impl_->pending = false;
    impl_->last = {};
    if (!command_succeeded) {
        impl_->valid = false;
        return "Temporal denoiser GPU command failed";
    }
    tw::Counters counters{};
    std::memcpy(&counters, impl_->counters.contents, sizeof(counters));
    impl_->last = {counters.failure, counters.completed, counters.history_accepted,
                   counters.history_rejected};
    const auto pixels = std::uint64_t(impl_->width) * impl_->height;
    if (counters.failure || counters.completed != pixels ||
        std::uint64_t(counters.history_accepted) + counters.history_rejected != pixels) {
        impl_->valid = false;
        return "Temporal denoiser GPU arithmetic/count failure: flags=" +
               std::to_string(counters.failure) +
               " completed=" + std::to_string(counters.completed) +
               " expected=" + std::to_string(pixels);
    }
    impl_->current = 1 - impl_->current;
    impl_->valid = true;
    return {};
}
TemporalDenoiserStats TemporalDenoiser::stats() const { return impl_->last; }
std::vector<NamedMetalTexture> TemporalDenoiser::diagnostic_textures() const {
    if (impl_->pending || !impl_->valid || impl_->trace.empty())
        return {};
    const auto &h = impl_->history[impl_->current];
    std::vector<NamedMetalTexture> result{
        {"temporal.mean", h.mean},     {"temporal.variance", h.variance},
        {"temporal.count", h.count},   {"temporal.depth", h.depth},
        {"temporal.normal", h.normal}, {"temporal.identity", h.identity}};
    for (std::size_t i = 0; i < impl_->trace.size(); ++i) {
        const auto name = "spatial." + std::to_string(i);
        result.push_back({name + ".mean", impl_->trace[i].mean});
        result.push_back({name + ".variance", impl_->trace[i].variance});
    }
    return result;
}
} // namespace rypath
