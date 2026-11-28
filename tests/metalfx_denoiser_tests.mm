// SPDX-License-Identifier: GPL-3.0-only
// Standalone native qualification. Synthetic planes test API execution only;
// they do not establish path-traced image quality or interactive performance.
#include "coherent_film_jitter.hpp"
#include "metalfx_denoiser.hpp"
#include "render.hpp"

#import <Foundation/Foundation.h>
#if __has_include(<MetalFX/MTLFXTemporalDenoisedScaler.h>)
#import <MetalFX/MetalFX.h>
#define RYPATH_TEST_HAS_DENOISER 1
#else
#define RYPATH_TEST_HAS_DENOISER 0
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace rypath;
using Color = std::array<float, 4>;
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
void rejected(const std::string &diagnostic, std::string_view expected) {
    require(diagnostic.find(expected) != std::string::npos,
            "Expected diagnostic containing '" + std::string(expected) + "', got: " + diagnostic);
}
#if RYPATH_TEST_HAS_DENOISER
id<MTLTexture> texture(id<MTLDevice> device, const MetalFXTextureRequirement &r,
                       MTLStorageMode storage = MTLStorageModePrivate) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:r.format
                                                                         width:r.width
                                                                        height:r.height
                                                                     mipmapped:NO];
    descriptor.usage = r.usage;
    descriptor.storageMode = storage;
    auto result = [device newTextureWithDescriptor:descriptor];
    require(result != nil, "Texture allocation failed");
    return result;
}
MetalFXDenoiserInputs inputs(id<MTLDevice> device, const MetalFXDenoiserRequirements &r) {
    MetalFXDenoiserInputs result;
    result.color = texture(device, r.color);
    result.depth = texture(device, r.depth);
    result.motion = texture(device, r.motion);
    result.diffuse_albedo = texture(device, r.diffuse_albedo);
    result.specular_albedo = texture(device, r.specular_albedo);
    result.normal = texture(device, r.normal);
    result.roughness = texture(device, r.roughness);
    result.exposure = texture(device, r.exposure);
    result.output = texture(device, r.output);
    // Positive-Z view, 90-degree square projection, infinite far, near=.1.
    // The synthetic plane is at view Z=2, so its reversed depth is .05.
    result.world_to_view = matrix_identity_float4x4;
    result.view_to_clip = {};
    result.view_to_clip.columns[0].x = 1;
    result.view_to_clip.columns[1].y = 1;
    result.view_to_clip.columns[2].w = 1;
    result.view_to_clip.columns[3].z = .1f;
    return result;
}
void completed(id<MTLCommandBuffer> command) {
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted,
            command.error ? command.error.localizedDescription.UTF8String : "GPU command failed");
}
NSUInteger channels(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatRGBA16Float:
        return 4;
    case MTLPixelFormatRG16Float:
        return 2;
    case MTLPixelFormatR16Float:
    case MTLPixelFormatR32Float:
        return 1;
    default:
        throw std::runtime_error("Unexpected test texture format");
    }
}
// GPU upload producer. Buffers stay alive through completion; private texture
// contents are never read or modified by the CPU through texture pointers.
void upload(id<MTLDevice> device, id<MTLBlitCommandEncoder> encoder, id<MTLTexture> destination,
            const std::function<Color(NSUInteger, NSUInteger)> &value,
            std::vector<id<MTLBuffer>> &retained) {
    const NSUInteger count = channels(destination.pixelFormat);
    const bool full = destination.pixelFormat == MTLPixelFormatR32Float;
    const NSUInteger texel_bytes = count * (full ? sizeof(float) : sizeof(_Float16));
    const NSUInteger row = ((destination.width * texel_bytes + 255) / 256) * 256;
    auto staging = [device newBufferWithLength:row * destination.height
                                       options:MTLResourceStorageModeShared];
    require(staging != nil, "Upload staging allocation failed");
    std::memset(staging.contents, 0, staging.length);
    auto *bytes = static_cast<unsigned char *>(staging.contents);
    for (NSUInteger y = 0; y < destination.height; ++y)
        for (NSUInteger x = 0; x < destination.width; ++x) {
            const auto rgba = value(x, y);
            for (NSUInteger c = 0; c < count; ++c)
                if (full)
                    std::memcpy(bytes + y * row + x * texel_bytes, &rgba[c], sizeof(float));
                else {
                    const auto half = static_cast<_Float16>(rgba[c]);
                    std::memcpy(bytes + y * row + x * texel_bytes + c * sizeof(half), &half,
                                sizeof(half));
                }
        }
    [encoder copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:row
        sourceBytesPerImage:row * destination.height
                 sourceSize:MTLSizeMake(destination.width, destination.height, 1)
                  toTexture:destination
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    retained.push_back(staging);
}
void validation_cases(MetalFXDenoiser &effect, id<MTLDevice> device, id<MTLCommandQueue> queue,
                      const MetalFXDenoiserInputs &valid) {
    const auto &r = effect.requirements();
    auto command = [queue commandBuffer];
    require(command != nil, "Validation command allocation failed");
    const auto reject_encode = [&](const MetalFXDenoiserInputs &candidate,
                                   std::string_view expected) {
        const auto diagnostic = effect.encode(command, candidate);
        // MetalFX can retain heaps for encoded work. An unexpectedly accepted
        // negative case must complete before scaler/resources are destroyed.
        if (diagnostic.empty())
            completed(command);
        rejected(diagnostic, expected);
    };
    rejected(effect.encode(nil, valid), "command buffer is missing");
    auto bad = valid;
    bad.normal = nil;
    reject_encode(bad, "normal texture is missing");
    auto requirement = r.color;
    requirement.format = MTLPixelFormatRGBA32Float;
    bad = valid;
    bad.color = texture(device, requirement);
    reject_encode(bad, "format");
    requirement = r.motion;
    --requirement.width;
    bad = valid;
    bad.motion = texture(device, requirement);
    reject_encode(bad, "dimensions");
    if (r.color.usage == MTLTextureUsageUnknown) {
        std::cout << "Missing-usage check unavailable: MetalFX requests no color usage bits\n";
    } else {
        requirement = r.color;
        // Unknown=0 can be expanded by Metal to a conservative usage mask.
        // Request an explicit mask that omits a required bit instead.
        requirement.usage = (r.color.usage & MTLTextureUsageShaderRead)
                                ? MTLTextureUsageRenderTarget
                                : MTLTextureUsageShaderRead;
        bad = valid;
        bad.color = texture(device, requirement);
        std::cout << "Color usage masks: required=" << r.color.usage
                  << " requested=" << requirement.usage << " actual=" << bad.color.usage << '\n';
        if ((bad.color.usage & r.color.usage) != r.color.usage)
            reject_encode(bad, "usage");
        else
            std::cout << "Missing-usage check unavailable: device expanded explicit usage mask\n";
    }
    bad = valid;
    bad.output = texture(device, r.output, MTLStorageModeShared);
    reject_encode(bad, "private storage");
    requirement = r.color;
    requirement.usage |= r.output.usage;
    bad = valid;
    bad.color = bad.output = texture(device, requirement);
    reject_encode(bad, "alias");
    requirement.usage |= MTLTextureUsagePixelFormatView;
    auto backing = texture(device, requirement);
    bad = valid;
    bad.color = [backing newTextureViewWithPixelFormat:requirement.format];
    bad.output = [backing newTextureViewWithPixelFormat:requirement.format];
    require(bad.color != nil && bad.output != nil && bad.color != bad.output,
            "Distinct sibling views for shared-storage rejection");
    reject_encode(bad, "standalone storage");
    bad = valid;
    bad.world_to_view.columns[0].x = std::numeric_limits<float>::quiet_NaN();
    reject_encode(bad, "must be finite");
    bad = valid;
    bad.view_to_clip.columns[3].w = std::numeric_limits<float>::infinity();
    reject_encode(bad, "must be finite");
    bad = valid;
    bad.motion_scale_x = std::numeric_limits<float>::infinity();
    reject_encode(bad, "must be finite");
    bad = valid;
    bad.jitter_y = std::numeric_limits<float>::quiet_NaN();
    reject_encode(bad, "must be finite");
    bad = valid;
    requirement = r.exposure;
    requirement.height = 2;
    bad.exposure = texture(device, requirement);
    reject_encode(bad, "exposure texture dimensions");

    bool second_device = false;
    for (id<MTLDevice> other in MTLCopyAllDevices()) {
        if (other.registryID == device.registryID)
            continue;
        second_device = true;
        bad = valid;
        bad.color = texture(other, r.color);
        reject_encode(bad, "different device");
        auto other_command = [[other newCommandQueue] commandBuffer];
        rejected(effect.encode(other_command, valid), "different device");
    }
    if (!second_device)
        std::cout << "Cross-device texture/command checks unavailable: one physical GPU\n";
    // Empty command submission is validation work, permitted only during the
    // explicit native qualification window like the plane encodes below.
    completed(command);
    rejected(effect.encode(command, valid), "already committed");
}
double jitter_registration(id<MTLDevice> device, id<MTLCommandQueue> queue, float api_sign,
                           bool physical_projection, bool triangle = false) {
    auto built = MetalFXDenoiser::create(device, {128, 128, 256, 256});
    require(built.denoiser != nullptr, built.diagnostic);
    auto frame = inputs(device, built.denoiser->requirements());
    const auto signal = [](double x, double y) {
        return float(.5 + .2 * std::sin(2 * 3.141592653589793 * .125 * x) +
                     .15 * std::sin(2 * 3.141592653589793 * .1875 * y));
    };
    id<MTLBuffer> readback = nil;
    const NSUInteger row = ((frame.output.width * 8 + 255) / 256) * 256;
    std::array<float, 16> base_projection{};
    std::memcpy(base_projection.data(), &frame.view_to_clip, sizeof(base_projection));
    constexpr unsigned samples = 48;
    for (unsigned sample = 0; sample < samples; ++sample) {
        @autoreleasepool {
            const auto film_words = coherent_film_words(197, sample);
            auto jitter = coherent_film_offset(film_words);
            if (triangle) {
                std::array<float, 2> u{sample_uniform(film_words[0]),
                                       sample_uniform(film_words[1])};
                constexpr std::array<std::array<float, 2>, 8> diagonal{{{0, 0},
                                                                        {1 - 0x1p-24f, 0},
                                                                        {.5f, .5f},
                                                                        {.75f, .25f},
                                                                        {.25f, .75f},
                                                                        {.51f, .49f},
                                                                        {.00005f, .99995f},
                                                                        {.99995f, .99995f}}};
                if (sample < diagonal.size())
                    u = diagonal[sample];
                jitter = {triangle_filter_offset(u[0], 1), triangle_filter_offset(u[1], 1)};
            }
            auto command = [queue commandBuffer];
            auto producer = [command blitCommandEncoder];
            require(command != nil && producer != nil, "Jitter producer allocation");
            std::vector<id<MTLBuffer>> staging;
            const auto pattern = [&](NSUInteger x, NSUInteger y) {
                const float v = signal(double(x) + .5 + jitter[0], double(y) + .5 + jitter[1]);
                return Color{v, v, v, 1};
            };
            const auto constant = [](Color c) { return [c](NSUInteger, NSUInteger) { return c; }; };
            upload(device, producer, frame.color, pattern, staging);
            upload(device, producer, frame.diffuse_albedo, pattern, staging);
            upload(device, producer, frame.depth, constant({.05f, 0, 0, 0}), staging);
            upload(device, producer, frame.motion, constant({0, 0, 0, 0}), staging);
            upload(device, producer, frame.specular_albedo, constant({0, 0, 0, 1}), staging);
            upload(device, producer, frame.normal, constant({0, 0, -1, 1}), staging);
            upload(device, producer, frame.roughness, constant({1, 0, 0, 0}), staging);
            upload(device, producer, frame.exposure, constant({1, 0, 0, 0}), staging);
            [producer endEncoding];
            frame.reset_history = sample == 0;
            if (physical_projection) {
                const auto projected = coherent_film_projection(base_projection, 128, 128, jitter);
                std::memcpy(&frame.view_to_clip, projected.data(), sizeof(projected));
            }
            frame.jitter_x = api_sign * jitter[0];
            frame.jitter_y = api_sign * jitter[1];
            require(built.denoiser->encode(command, frame).empty(), "Jitter encode");
            if (sample + 1 == samples) {
                readback = [device newBufferWithLength:row * frame.output.height
                                               options:MTLResourceStorageModeShared];
                auto reader = [command blitCommandEncoder];
                require(readback != nil && reader != nil, "Jitter readback allocation");
                [reader copyFromTexture:frame.output
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(0, 0, 0)
                                  sourceSize:MTLSizeMake(frame.output.width, frame.output.height, 1)
                                    toBuffer:readback
                           destinationOffset:0
                      destinationBytesPerRow:row
                    destinationBytesPerImage:row * frame.output.height];
                [reader endEncoding];
            }
            completed(command);
        }
    }
    double squared_error = 0;
    std::uint64_t count = 0;
    for (NSUInteger y = 16; y + 16 < frame.output.height; ++y)
        for (NSUInteger x = 16; x + 16 < frame.output.width; ++x) {
            const auto *pixel = reinterpret_cast<const _Float16 *>(
                static_cast<const unsigned char *>(readback.contents) + row * y + x * 8);
            const double expected = signal((double(x) + .5) / 2, (double(y) + .5) / 2);
            require(std::isfinite(float(pixel[0])), "Jitter output nonfinite");
            squared_error += (double(pixel[0]) - expected) * (double(pixel[0]) - expected);
            ++count;
        }
    return squared_error / double(count);
}
void plane_sequence(MetalFXDenoiser &effect, id<MTLDevice> device, id<MTLCommandQueue> queue,
                    MetalFXDenoiserInputs frame, bool noisy, Color base = {.25f, .5f, .75f, 1},
                    float exposure = 1, bool small_feature = false) {
    id<MTLBuffer> readback = nil;
    const NSUInteger row = ((frame.output.width * 8 + 255) / 256) * 256;
    for (unsigned sample = 0; sample < 4; ++sample) {
        @autoreleasepool {
            auto command = [queue commandBuffer];
            require(command != nil, "Plane command allocation failed");
            auto producer = [command blitCommandEncoder];
            require(producer != nil, "Plane producer allocation failed");
            std::vector<id<MTLBuffer>> staging;
            const auto constant = [](Color rgba) {
                return [rgba](NSUInteger, NSUInteger) { return rgba; };
            };
            upload(
                device, producer, frame.color,
                [noisy, sample, base, small_feature](NSUInteger x, NSUInteger y) {
                    const std::uint32_t hash = (std::uint32_t(x) * 1664525u) ^
                                               (std::uint32_t(y) * 1013904223u) ^
                                               (sample * 747796405u);
                    const float noise = noisy ? (float(hash & 255u) / 255 - .5f) * .0625f : 0;
                    const bool feature = small_feature && x >= 62 && x < 66 && y >= 62 && y < 66;
                    return Color{base[0] + noise + (feature ? 2.f : 0.f), base[1] + noise,
                                 base[2] + noise, 1};
                },
                staging);
            upload(device, producer, frame.depth, constant({.05f, 0, 0, 0}), staging);
            upload(device, producer, frame.motion, constant({0, 0, 0, 0}), staging);
            upload(
                device, producer, frame.diffuse_albedo,
                [base, small_feature](NSUInteger x, NSUInteger y) {
                    const bool feature = small_feature && x >= 62 && x < 66 && y >= 62 && y < 66;
                    return Color{feature ? 1.f : std::min(base[0], 1.f), std::min(base[1], 1.f),
                                 std::min(base[2], 1.f), 1};
                },
                staging);
            upload(device, producer, frame.specular_albedo, constant({0, 0, 0, 1}), staging);
            upload(device, producer, frame.normal, constant({0, 0, -1, 1}), staging);
            upload(device, producer, frame.roughness, constant({1, 0, 0, 0}), staging);
            upload(device, producer, frame.exposure, constant({exposure, 0, 0, 0}), staging);
            [producer endEncoding];
            frame.reset_history = sample == 0;
            const auto diagnostic = effect.encode(command, frame);
            require(diagnostic.empty(), diagnostic);
            if (sample == 3) {
                readback = [device newBufferWithLength:row * frame.output.height
                                               options:MTLResourceStorageModeShared];
                require(readback != nil, "Readback allocation failed");
                auto reader = [command blitCommandEncoder];
                require(reader != nil, "Readback encoder allocation failed");
                [reader copyFromTexture:frame.output
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(0, 0, 0)
                                  sourceSize:MTLSizeMake(frame.output.width, frame.output.height, 1)
                                    toBuffer:readback
                           destinationOffset:0
                      destinationBytesPerRow:row
                    destinationBytesPerImage:row * frame.output.height];
                [reader endEncoding];
            }
            // Serial qualification intentionally waits. Production adapter
            // encode() never commits, waits, captures, or reads texture contents.
            completed(command);
        }
    }
    std::array<double, 3> mean{};
    double feature_red = 0, surround_red = 0;
    unsigned feature_count = 0, surround_count = 0;
    for (NSUInteger y = 0; y < frame.output.height; ++y)
        for (NSUInteger x = 0; x < frame.output.width; ++x) {
            const auto *texel = reinterpret_cast<const _Float16 *>(
                static_cast<const unsigned char *>(readback.contents) + y * row + 8 * x);
            for (unsigned c = 0; c < 3; ++c) {
                const double value = texel[c];
                require(std::isfinite(value) && value >= -1 && value <= 4,
                        "Synthetic denoised plane contains invalid or grossly out-of-range RGB");
                mean[c] += value;
            }
            if (small_feature && x >= 62 && x < 66 && y >= 62 && y < 66) {
                feature_red += float(texel[0]);
                ++feature_count;
            } else if (small_feature && x >= 58 && x < 70 && y >= 58 && y < 70) {
                surround_red += float(texel[0]);
                ++surround_count;
            }
        }
    if (small_feature) {
        require(feature_count == 16 && surround_count == 128, "Native feature ROI accounting");
        const double contrast = feature_red / feature_count - surround_red / surround_count;
        std::cout << "Native 4x4 feature contrast " << contrast << " (input 2)\n";
        require(contrast > .25, "Noise-free small feature erased by denoising");
    }
    const double pixels = double(frame.output.width) * frame.output.height;
    for (unsigned c = 0; c < 3; ++c) {
        mean[c] /= pixels;
        // Predeclared broad integration gate; not a perceptual quality metric.
        const double expected = base[c] + (small_feature && c == 0 ? 32. / pixels : 0.);
        require(std::abs(mean[c] - expected) <= (noisy ? .35 : .1 + .1 * expected),
                "Synthetic denoised plane mean is outside the broad integration gate");
    }
    std::cout << frame.color.width << "x" << frame.color.height << " -> " << frame.output.width
              << "x" << frame.output.height << (noisy ? " noisy" : " constant") << " mean RGB "
              << mean[0] << ", " << mean[1] << ", " << mean[2] << '\n';
}
#endif
int run(bool require_supported) {
    rejected(MetalFXDenoiser::create(nil, {128, 128, 128, 128}).diagnostic, "Metal device");
    auto device = MTLCreateSystemDefaultDevice();
    const auto unavailable = [&](const std::string &message) {
        std::cout << "MetalFX runtime qualification unavailable: " << message << '\n';
        return require_supported ? 1 : 77;
    };
    if (device == nil)
        return unavailable("no Metal device");
    rejected(MetalFXDenoiser::create(device, {0, 128, 128, 128}).diagnostic, "nonzero");
    rejected(MetalFXDenoiser::create(device, {128, 128, 64, 64}).diagnostic, "downscaling");
    rejected(MetalFXDenoiser::create(device, {16385, 1, 16385, 1}).diagnostic, "supported range");
    rejected(MetalFXDenoiser::create(device, {128, 128, 32768, 32768}).diagnostic,
             "supported range");
#if RYPATH_TEST_HAS_DENOISER
    if (@available(macOS 26.0, *)) {
        if (![MTLFXTemporalDenoisedScalerDescriptor supportsDevice:device]) {
            const auto result = MetalFXDenoiser::create(device, {128, 128, 128, 128});
            require(!result.denoiser, "Unsupported device unexpectedly created a scaler");
            rejected(result.diagnostic, "unsupported");
            return unavailable(result.diagnostic);
        }
        std::cout << "Device: " << device.name.UTF8String << '\n';
        rejected(MetalFXDenoiser::create(device, {1, 1, UINT32_MAX, UINT32_MAX}).diagnostic,
                 "supported range");
        auto queue = [device newCommandQueue];
        require(queue != nil, "Command queue allocation failed");
        auto native = MetalFXDenoiser::create(device, {128, 128, 128, 128});
        require(native.denoiser != nullptr, native.diagnostic);
        const auto frame = inputs(device, native.denoiser->requirements());
        validation_cases(*native.denoiser, device, queue, frame);
        plane_sequence(*native.denoiser, device, queue, frame, false);
        plane_sequence(*native.denoiser, device, queue, frame, true);
        // Native-only color/detail regressions. Exposure is metadata, not a
        // brightness compensation; compare output against the same linear input.
        plane_sequence(*native.denoiser, device, queue, frame, false, {.02f, .05f, 2.f, 1}, .25f);
        plane_sequence(*native.denoiser, device, queue, frame, false, {.02f, .05f, 2.f, 1}, 4.f);
        plane_sequence(*native.denoiser, device, queue, frame, false, {.02f, .05f, .1f, 1}, 1.f,
                       true);
        auto upscaled = MetalFXDenoiser::create(device, {128, 128, 256, 256});
        require(upscaled.denoiser != nullptr, upscaled.diagnostic);
        const auto smaller = inputs(device, upscaled.denoiser->requirements());
        plane_sequence(*upscaled.denoiser, device, queue, smaller, false);
        plane_sequence(*upscaled.denoiser, device, queue, smaller, true);
        const double negative = jitter_registration(device, queue, -1, false);
        const double positive = jitter_registration(device, queue, 1, false);
        std::cout << "Jitter analytic registration MSE: negative-film=" << negative
                  << "; positive-film=" << positive << '\n';
        const double negative_physical = jitter_registration(device, queue, -1, true);
        const double positive_physical = jitter_registration(device, queue, 1, true);
        std::cout << "Jitter physical-projection registration MSE: negative-film="
                  << negative_physical << "; positive-film=" << positive_physical << '\n';
        require(negative_physical < positive_physical * .9,
                "MetalFX jitter sign fails analytic oracle with the physical projection");
        if (std::getenv("RYPATH_PROBE_TRIANGLE_JITTER")) {
            try {
                const double triangle_reference =
                    jitter_registration(device, queue, -1, false, true);
                const double triangle_reference_opposite =
                    jitter_registration(device, queue, 1, false, true);
                const double triangle_physical = jitter_registration(device, queue, -1, true, true);
                const double triangle_opposite = jitter_registration(device, queue, 1, true, true);
                std::cout
                    << "Triangle-radius1 registration MSE (includes diagonal offsets near support "
                       "endpoints): reference="
                    << triangle_reference
                    << "; reference-opposite-sign=" << triangle_reference_opposite
                    << "; physical=" << triangle_physical << "; opposite-sign=" << triangle_opposite
                    << "; physical-over-box=" << triangle_physical / negative_physical << '\n';
            } catch (const std::exception &error) {
                std::cout << "Triangle-radius1 registration unsupported: " << error.what() << '\n';
            }
        }
        std::cout << "Reference-projection sign comparison: "
                  << (negative < positive * .9   ? "negative-film lower error"
                      : positive < negative * .9 ? "positive-film lower error"
                                                 : "ambiguous")
                  << '\n';
        std::cout << "MetalFX synthetic runtime qualification passed; image quality unqualified\n";
        return 0;
    }
    const auto result = MetalFXDenoiser::create(device, {128, 128, 128, 128});
    rejected(result.diagnostic, "macOS 26");
    return unavailable(result.diagnostic);
#else
    const auto result = MetalFXDenoiser::create(device, {128, 128, 128, 128});
    rejected(result.diagnostic, "build SDK");
    return unavailable(result.diagnostic);
#endif
}
} // namespace

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--require-supported")) {
        std::cerr << "Usage: metalfx_denoiser_tests [--require-supported]\n";
        return 1;
    }
    @autoreleasepool {
        try {
            return run(argc == 2);
        } catch (const std::exception &error) {
            std::cerr << "MetalFX denoiser qualification: " << error.what() << '\n';
            return 1;
        }
    }
}
