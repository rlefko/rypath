// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu_diagnostics.hpp"
#import <Metal/Metal.h>
#include <cstring>
#include <memory>
namespace rypath {
// One command owns one bounded sample buffer. Keep this object in its completion
// handler; command buffers do not define the host lifetime of counter storage.
class MetalGPUStages {
    static constexpr NSUInteger sample_limit = 4096;
    id<MTLCounterSampleBuffer> buffer_ = nil;
    std::vector<std::string> names_;
    std::string status_;
    bool overflow_ = false, split_ = false, encoding_failed_ = false;
    struct ResourceUse {
        id<MTLResource> resource;
        MTLResourceUsage usage;
    };
    std::vector<ResourceUse> resources_;
    MTLTimestamp cpu_start_ = 0, gpu_start_ = 0;

  public:
    explicit MetalGPUStages(id<MTLDevice> device) {
        if (![device supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary]) {
            if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
                status_ = "unavailable: dispatch and stage boundary counter sampling unsupported";
                return;
            }
            split_ = true;
        }
        id<MTLCounterSet> timestamps = nil;
        for (id<MTLCounterSet> set in device.counterSets)
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
                for (id<MTLCounter> counter in set.counters)
                    if ([counter.name isEqualToString:MTLCommonCounterTimestamp])
                        timestamps = set;
        if (!timestamps) {
            status_ = "unavailable: timestamp counter set absent";
            return;
        }
        auto descriptor = [MTLCounterSampleBufferDescriptor new];
        descriptor.counterSet = timestamps;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.sampleCount = sample_limit;
        descriptor.label = @"rypath dispatch timestamps";
        NSError *error = nil;
        buffer_ = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
        if (buffer_)
            [device sampleTimestamps:&cpu_start_ gpuTimestamp:&gpu_start_];
        status_ =
            buffer_
                ? (split_ ? "available: encoder-stage intervals; split-encoder structural overhead"
                          : "available: dispatch-boundary intervals")
                : "unavailable: counter sample buffer allocation failed";
        if (!buffer_ && error) {
            const char *domain = error.domain.UTF8String;
            const char *detail = error.localizedDescription.UTF8String;
            status_ += ": " + std::string(domain ? domain : "unknown domain") +
                       " code=" + std::to_string(error.code) + ": " +
                       std::string(detail ? detail : "no description");
        }
    }
    bool splits_encoders() const { return buffer_ && split_; }
    bool encoding_failed() const { return encoding_failed_; }
    void use_resource(id<MTLComputeCommandEncoder> encoder, id<MTLResource> resource,
                      MTLResourceUsage usage) {
        if (splits_encoders() && resource)
            resources_.push_back({resource, usage});
        [encoder useResource:resource usage:usage];
    }
    bool begin(id<MTLCommandBuffer> command, id<MTLComputeCommandEncoder> __strong &encoder,
               const char *name) {
        if (encoding_failed_)
            return false;
        if (!buffer_)
            return true;
        const bool sampled = names_.size() < sample_limit / 2;
        if (!sampled)
            overflow_ = true;
        if (split_) {
            [encoder endEncoding];
            auto pass = [MTLComputePassDescriptor computePassDescriptor];
            pass.dispatchType = MTLDispatchTypeSerial;
            if (sampled) {
                auto attachment = pass.sampleBufferAttachments[0];
                attachment.sampleBuffer = buffer_;
                attachment.startOfEncoderSampleIndex = names_.size() * 2;
                attachment.endOfEncoderSampleIndex = names_.size() * 2 + 1;
            }
            encoder = [command computeCommandEncoderWithDescriptor:pass];
            if (!encoder) {
                encoding_failed_ = true;
                return false;
            }
            encoder.label = [NSString stringWithUTF8String:name];
            for (const auto &use : resources_)
                [encoder useResource:use.resource usage:use.usage];
        } else if (sampled) {
            [encoder sampleCountersInBuffer:buffer_
                              atSampleIndex:names_.size() * 2
                                withBarrier:YES];
        }
        if (sampled)
            names_.emplace_back(name);
        return true;
    }
    void end(id<MTLComputeCommandEncoder> encoder) {
        // Stage samples are emitted by endEncoding after the caller's barriers.
        if (buffer_ && !split_ && !overflow_ && !names_.empty())
            [encoder sampleCountersInBuffer:buffer_
                              atSampleIndex:names_.size() * 2 - 1
                                withBarrier:YES];
    }
    void resolve(id<MTLCommandBuffer> command, GPUStageDiagnostics &out) const {
        out.requested = true;
        out.sampling_mode =
            buffer_ ? (split_ ? "encoder-stage-boundary" : "dispatch-boundary") : "unavailable";
        out.structural_overhead = splits_encoders();
        if (command.status != MTLCommandBufferStatusCompleted) {
            invalidate_gpu_stages(out, "invalid: command failed");
            return;
        }
        if (!buffer_ || overflow_) {
            invalidate_gpu_stages(out, overflow_ ? "unavailable: command sample cap exceeded"
                                                 : status_.c_str());
            return;
        }
        if (out.status == "disabled")
            out.status = status_;
        if (names_.empty())
            return;
        NSData *data = [buffer_ resolveCounterRange:NSMakeRange(0, names_.size() * 2)];
        if (!data || data.length != names_.size() * 2 * sizeof(MTLCounterResultTimestamp)) {
            invalidate_gpu_stages(out, "unavailable: counter resolve failed");
            return;
        }
        MTLTimestamp cpu_end = 0, gpu_end = 0;
        [command.device sampleTimestamps:&cpu_end gpuTimestamp:&gpu_end];
        if (!valid_gpu_stage_interval(cpu_start_, cpu_end) ||
            !valid_gpu_stage_interval(gpu_start_, gpu_end) || cpu_end == cpu_start_ ||
            gpu_end == gpu_start_) {
            invalidate_gpu_stages(out, "invalid: timestamp calibration");
            return;
        }
        // Metal CPU timestamps are nanoseconds; GPU timestamps use a device clock.
        const double scale =
            static_cast<double>(cpu_end - cpu_start_) / static_cast<double>(gpu_end - gpu_start_);
        for (std::size_t i = 0; i < names_.size(); ++i) {
            MTLCounterResultTimestamp pair[2]{};
            std::memcpy(pair, static_cast<const char *>(data.bytes) + i * sizeof(pair),
                        sizeof(pair));
            if (pair[0].timestamp < gpu_start_ || pair[1].timestamp > gpu_end) {
                invalidate_gpu_stages(out, "invalid: timestamp outside calibrated interval");
                return;
            }
            add_gpu_stage(out, names_[i], pair[0].timestamp, pair[1].timestamp, scale);
        }
    }
};
} // namespace rypath
