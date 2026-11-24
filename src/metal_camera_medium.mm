// SPDX-License-Identifier: GPL-3.0-only
#include "camera_medium.hpp"
#include "metal_execution.hpp"
#include "metal_frame_access.hpp"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>

namespace rypath {
namespace {
namespace cm = camera_medium_wire;
constexpr std::size_t guard_bytes = 32;
std::string failure_description(cm::Status status) {
    switch (status) {
    case cm::Status::success:
        return {};
    case cm::Status::invalid_config:
        return "Invalid camera medium query configuration";
    case cm::Status::invalid_hit:
        return "Camera medium traversal returned an invalid hit";
    case cm::Status::invalid_geometry:
        return "Camera medium boundary geometry is invalid";
    case cm::Status::invalid_medium:
        return "Camera medium index is outside the scene media";
    case cm::Status::inconsistent_side:
        return "Known camera medium disagrees with the incoming interface side";
    case cm::Status::unresolved_boundary:
        return "Camera move has a tangent or numerically unresolved interface ordering";
    case cm::Status::unresolved_endpoint:
        return "Camera destination lies on or within an unresolved boundary endpoint region";
    case cm::Status::boundary_exhaustion:
        return "Camera medium crossing limit exhausted";
    case cm::Status::unrepresentable_segment:
        return "Camera movement is outside the supported binary32 segment range";
    case cm::Status::unsupported_interface:
        return "Camera medium interfaces require provably opaque alpha";
    }
    return "Unknown camera medium query status";
}
} // namespace
struct CameraMediumTracker::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLArgumentEncoder> argument_encoder = nil;
    id<MTLBuffer> arguments = nil, config = nil, output = nil;
    std::unique_ptr<MetalFramePipeline> pipeline;
};
CameraMediumTracker::CameraMediumTracker(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CameraMediumTracker::~CameraMediumTracker() = default;
CameraMediumTracker::CameraMediumTracker(CameraMediumTracker &&) noexcept = default;
CameraMediumTracker &CameraMediumTracker::operator=(CameraMediumTracker &&) noexcept = default;

CameraMediumBuildResult CameraMediumTracker::build(const Frame &frame) {
    @autoreleasepool {
        CameraMediumBuildResult result;
        auto acquired = MetalFrameAccess::acquire(frame);
        if (!acquired.lease) {
            result.diagnostic =
                acquired.diagnostic.empty()
                    ? "A completed frame is required to build a camera medium tracker"
                    : acquired.diagnostic;
            return result;
        }
        auto &lease = *acquired.lease;
        auto impl = std::make_unique<Impl>();
        impl->device = lease.device();
        impl->queue = lease.queue();
        auto built = lease.create_guide_pipeline(@"rypath_camera_medium_contract");
        if (!built.pipeline) {
            result.diagnostic = built.diagnostic;
            return result;
        }
        auto check = [impl->device newBufferWithLength:sizeof(cm::Contract)
                                               options:MTLResourceStorageModeShared];
        auto command = [impl->queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (check == nil || command == nil || encoder == nil) {
            result.diagnostic = "Camera medium contract command allocation failed";
            return result;
        }
        std::memset(check.contents, 0, check.length);
        [encoder setComputePipelineState:built.pipeline->pipeline()];
        [encoder setBuffer:check offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        result.diagnostic = lease.retain_until_completed(command);
        if (!result.diagnostic.empty())
            return result;
        result.diagnostic = metal::complete(command, "camera medium contract");
        if (!result.diagnostic.empty())
            return result;
        cm::Contract actual{};
        std::memcpy(&actual, check.contents, sizeof(actual));
        if (actual.version != cm::contract || actual.config_bytes != sizeof(cm::Config) ||
            actual.result_bytes != sizeof(cm::Result) || actual.maximum != cm::maximum_crossings ||
            std::any_of(std::begin(actual.reserved), std::end(actual.reserved),
                        [](auto word) { return word != 0; })) {
            result.diagnostic = "Camera medium shader ABI mismatch; rebuild the metallib";
            return result;
        }
        built = lease.create_guide_pipeline(@"rypath_camera_medium_classify");
        if (!built.pipeline) {
            result.diagnostic = built.diagnostic;
            return result;
        }
        impl->pipeline = std::move(built.pipeline);
        impl->argument_encoder = impl->pipeline->argument_encoder();
        if (impl->argument_encoder == nil) {
            result.diagnostic = "Camera medium argument encoder creation failed";
            return result;
        }
        impl->arguments = [impl->device newBufferWithLength:impl->argument_encoder.encodedLength
                                                    options:MTLResourceStorageModeShared];
        impl->config = [impl->device newBufferWithLength:sizeof(cm::Config)
                                                 options:MTLResourceStorageModeShared];
        impl->output = [impl->device newBufferWithLength:sizeof(cm::Result) + guard_bytes
                                                 options:MTLResourceStorageModeShared];
        if (impl->arguments == nil || impl->config == nil || impl->output == nil) {
            result.diagnostic = "Camera medium query buffer allocation failed";
            return result;
        }
        [impl->argument_encoder setArgumentBuffer:impl->arguments offset:0];
        [impl->argument_encoder setBuffer:impl->config offset:0 atIndex:cm::config_id];
        [impl->argument_encoder setBuffer:impl->output offset:0 atIndex:cm::result_id];
        result.status = GeometryStatus::success;
        result.tracker =
            std::unique_ptr<CameraMediumTracker>(new CameraMediumTracker(std::move(impl)));
        return result;
    }
}
CameraMediumResult CameraMediumTracker::classify(const Frame &frame, GeometryPoint destination,
                                                 std::uint32_t crossing_limit) {
    @autoreleasepool {
        CameraMediumResult result;
        if (!impl_) {
            result.diagnostic = "Camera medium tracker is moved-from";
            return result;
        }
        if (!std::isfinite(destination.x) || !std::isfinite(destination.y) ||
            !std::isfinite(destination.z) || crossing_limit == 0 ||
            crossing_limit > cm::maximum_crossings) {
            result.diagnostic = "Camera destination must be finite and crossing limit in [1,4096]";
            return result;
        }
        auto acquired = MetalFrameAccess::acquire(frame);
        if (!acquired.lease) {
            result.diagnostic = acquired.diagnostic.empty()
                                    ? "Camera movement requires a completed current frame"
                                    : acquired.diagnostic;
            return result;
        }
        auto &lease = *acquired.lease;
        if (lease.device() != impl_->device) {
            result.diagnostic = "Camera medium tracker and frame use different Metal devices";
            return result;
        }
        const cm::Config config{
            cm::contract, crossing_limit, {0, 0}, {destination.x, destination.y, destination.z}, 0};
        std::memcpy(impl_->config.contents, &config, sizeof(config));
        std::memset(impl_->output.contents, 0xa5, impl_->output.length);
        auto command = [impl_->queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (command == nil || encoder == nil) {
            result.diagnostic = "Camera medium query command allocation failed";
            return result;
        }
        result.diagnostic = lease.bind_guides(encoder, *impl_->pipeline, impl_->arguments);
        if (!result.diagnostic.empty()) {
            [encoder endEncoding];
            return result;
        }
        [encoder useResource:impl_->config usage:MTLResourceUsageRead];
        [encoder useResource:impl_->output usage:MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        result.diagnostic = lease.retain_until_completed(command);
        if (!result.diagnostic.empty())
            return result;
        result.diagnostic = metal::complete(command, "camera medium classification");
        if (!result.diagnostic.empty())
            return result;
        const double start = command.GPUStartTime, end = command.GPUEndTime;
        if (std::isfinite(start) && std::isfinite(end) && start > 0 && end >= start) {
            result.gpu_time_valid = true;
            result.gpu_seconds = end - start;
        }
        const auto *bytes = static_cast<const unsigned char *>(impl_->output.contents);
        if (!std::all_of(bytes + sizeof(cm::Result), bytes + impl_->output.length,
                         [](auto byte) { return byte == 0xa5; })) {
            result.diagnostic = "Camera medium result guard was overwritten";
            return result;
        }
        cm::Result value{};
        std::memcpy(&value, bytes, sizeof(value));
        const auto &parameters = lease.parameters();
        if (static_cast<std::uint32_t>(value.status) >
                static_cast<std::uint32_t>(cm::Status::unsupported_interface) ||
            value.crossings > crossing_limit || value.reserved[0] || value.reserved[1] ||
            (value.medium != no_medium && value.medium >= parameters.medium_count)) {
            result.diagnostic = "Camera medium result contains invalid status or accounting";
            return result;
        }
        result.failure = value.status;
        result.medium = value.medium;
        result.crossings = value.crossings;
        if (value.status != cm::Status::success) {
            std::ostringstream description;
            description << failure_description(value.status) << " after " << value.crossings
                        << " crossings";
            if (value.last_instance != 0xffffffffu)
                description << " (native instance " << value.last_instance << ", primitive "
                            << value.last_primitive << ", kind " << value.last_kind << ')';
            result.diagnostic = description.str();
            return result;
        }
        result.status = GeometryStatus::success;
        return result;
    }
}
} // namespace rypath
