// SPDX-License-Identifier: GPL-3.0-only
#include "metalfx_denoiser.hpp"

#if __has_include(<MetalFX/MTLFXTemporalDenoisedScaler.h>)
// The denoiser header inherits a protocol declared by the umbrella's earlier
// imports, so it is not self-contained in the installed SDK.
#import <MetalFX/MetalFX.h>
#define RYPATH_HAS_METALFX_DENOISER 1
#else
#define RYPATH_HAS_METALFX_DENOISER 0
#endif

#include <cmath>
#include <utility>

namespace rypath {

struct MetalFXDenoiser::Impl {
    id<MTLDevice> device = nil;
    // Opaque strong ownership keeps SDK-26 types out of the SDK-independent
    // lifetime path. Typed accesses are guarded at compile time and runtime.
    id scaler = nil;
    MetalFXDenoiserRequirements requirements;
};

namespace {
bool finite_matrix(simd_float4x4 matrix) {
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r)
            if (!std::isfinite(matrix.columns[c][r]))
                return false;
    return true;
}

std::string validate_texture(id<MTLTexture> texture, id<MTLDevice> device,
                             const MetalFXTextureRequirement &required, const char *name) {
    const std::string prefix = std::string("MetalFX ") + name + " texture ";
    if (texture == nil)
        return prefix + "is missing";
    if (texture.device != device)
        return prefix + "belongs to a different device";
    // Restrict this adapter to owned standalone allocations: object inequality
    // cannot establish nonaliasing for texture views or shared backing storage.
    if (texture.parentTexture != nil || texture.buffer != nil || texture.heap != nil ||
        texture.iosurface != nullptr)
        return prefix + "requires standalone storage (no views, buffers, heaps, or IOSurfaces)";
    if (texture.textureType != MTLTextureType2D || texture.sampleCount != 1 ||
        texture.arrayLength != 1 || texture.depth != 1)
        return prefix + "must be a single-sample 2D texture";
    if (texture.width != required.width || texture.height != required.height)
        return prefix + "dimensions do not match the scaler";
    if (texture.pixelFormat != required.format)
        return prefix + "format does not match the scaler";
    if ((texture.usage & required.usage) != required.usage)
        return prefix + "usage does not include the scaler requirements";
    return {};
}
} // namespace

MetalFXDenoiser::MetalFXDenoiser(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalFXDenoiser::~MetalFXDenoiser() = default;

const MetalFXDenoiserRequirements &MetalFXDenoiser::requirements() const {
    return impl_->requirements;
}

MetalFXDenoiserCreateResult MetalFXDenoiser::create(id<MTLDevice> device,
                                                    const MetalFXDenoiserConfig &config) {
    if (device == nil)
        return {.diagnostic = "MetalFX denoising requires a Metal device"};
    if (!config.input_width || !config.input_height || !config.output_width ||
        !config.output_height || config.output_width < config.input_width ||
        config.output_height < config.input_height)
        return {.diagnostic = "MetalFX denoising requires nonzero dimensions and no downscaling"};
    // Target-M4-Max reconstruction dimension budget. Validate before creating
    // the scaler, which can allocate its internal history immediately.
    // Apple9's 2D limit is 16384 (Metal Feature Set Tables, October 2026).
    constexpr std::uint32_t dimension_limit = 16384;
    if (config.input_width > dimension_limit || config.input_height > dimension_limit ||
        config.output_width > dimension_limit || config.output_height > dimension_limit)
        return {.diagnostic = "MetalFX dimensions exceed the supported range (16384 per axis)"};
#if RYPATH_HAS_METALFX_DENOISER
    if (@available(macOS 26.0, *)) {
        if (![MTLFXTemporalDenoisedScalerDescriptor supportsDevice:device])
            return {.diagnostic = "MetalFX denoising is unsupported on this device"};
        const float min_scale =
            [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMinScaleForDevice:device];
        const float max_scale =
            [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMaxScaleForDevice:device];
        const double sx = double(config.output_width) / config.input_width;
        const double sy = double(config.output_height) / config.input_height;
        if (!std::isfinite(min_scale) || !std::isfinite(max_scale) || min_scale <= 0 ||
            max_scale < min_scale || sx < min_scale || sx > max_scale || sy < min_scale ||
            sy > max_scale)
            return {.diagnostic =
                        "MetalFX denoising scale is outside the device's supported range"};
        auto desc = [MTLFXTemporalDenoisedScalerDescriptor new];
        desc.inputWidth = config.input_width;
        desc.inputHeight = config.input_height;
        desc.outputWidth = config.output_width;
        desc.outputHeight = config.output_height;
        desc.colorTextureFormat = MTLPixelFormatRGBA16Float;
        desc.depthTextureFormat = MTLPixelFormatR32Float;
        desc.motionTextureFormat = MTLPixelFormatRG16Float;
        desc.diffuseAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
        desc.specularAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
        desc.normalTextureFormat = MTLPixelFormatRGBA16Float;
        desc.roughnessTextureFormat = MTLPixelFormatR16Float;
        desc.outputTextureFormat = MTLPixelFormatRGBA16Float;
        desc.autoExposureEnabled = NO;
        desc.requiresSynchronousInitialization = YES;
        desc.reactiveMaskTextureEnabled = NO;
        desc.specularHitDistanceTextureEnabled = NO;
        desc.denoiseStrengthMaskTextureEnabled = NO;
        desc.transparencyOverlayTextureEnabled = NO;
        id<MTLFXTemporalDenoisedScaler> scaler = [desc newTemporalDenoisedScalerWithDevice:device];
        if (scaler == nil)
            return {.diagnostic = "MetalFX denoised scaler creation failed"};
        auto impl = std::make_unique<Impl>();
        impl->device = device;
        impl->scaler = scaler;
        const auto input = [&](MTLPixelFormat format, MTLTextureUsage usage) {
            return MetalFXTextureRequirement{format, usage, config.input_width,
                                             config.input_height};
        };
        auto &r = impl->requirements;
        r.color = input(desc.colorTextureFormat, scaler.colorTextureUsage);
        r.depth = input(desc.depthTextureFormat, scaler.depthTextureUsage);
        r.motion = input(desc.motionTextureFormat, scaler.motionTextureUsage);
        r.diffuse_albedo = input(desc.diffuseAlbedoTextureFormat, scaler.diffuseAlbedoTextureUsage);
        r.specular_albedo =
            input(desc.specularAlbedoTextureFormat, scaler.specularAlbedoTextureUsage);
        r.normal = input(desc.normalTextureFormat, scaler.normalTextureUsage);
        r.roughness = input(desc.roughnessTextureFormat, scaler.roughnessTextureUsage);
        r.exposure = {MTLPixelFormatR16Float, MTLTextureUsageShaderRead, 1, 1};
        r.output = {desc.outputTextureFormat, scaler.outputTextureUsage, config.output_width,
                    config.output_height};
        return {.denoiser = std::unique_ptr<MetalFXDenoiser>(new MetalFXDenoiser(std::move(impl)))};
    }
    return {.diagnostic = "MetalFX denoising requires macOS 26 or newer"};
#else
    return {.diagnostic = "MetalFX denoising is unavailable in the build SDK"};
#endif
}

std::string MetalFXDenoiser::encode(id<MTLCommandBuffer> command,
                                    const MetalFXDenoiserInputs &inputs) {
    if (command == nil)
        return "MetalFX denoising command buffer is missing";
    if (command.device != impl_->device)
        return "MetalFX denoising command buffer belongs to a different device";
    if (command.status >= MTLCommandBufferStatusCommitted)
        return "MetalFX denoising command buffer is already committed";
    if (!finite_matrix(inputs.world_to_view) || !finite_matrix(inputs.view_to_clip) ||
        !std::isfinite(inputs.motion_scale_x) || !std::isfinite(inputs.motion_scale_y) ||
        !std::isfinite(inputs.jitter_x) || !std::isfinite(inputs.jitter_y))
        return "MetalFX denoising camera, motion scale, and jitter must be finite";
    const auto &r = impl_->requirements;
    struct Binding {
        id<MTLTexture> texture;
        const MetalFXTextureRequirement &required;
        const char *name;
    };
    const Binding bindings[] = {{inputs.color, r.color, "color"},
                                {inputs.depth, r.depth, "depth"},
                                {inputs.motion, r.motion, "motion"},
                                {inputs.diffuse_albedo, r.diffuse_albedo, "diffuse albedo"},
                                {inputs.specular_albedo, r.specular_albedo, "specular albedo"},
                                {inputs.normal, r.normal, "normal"},
                                {inputs.roughness, r.roughness, "roughness"},
                                {inputs.exposure, r.exposure, "exposure"},
                                {inputs.output, r.output, "output"}};
    for (const auto &binding : bindings) {
        auto error =
            validate_texture(binding.texture, impl_->device, binding.required, binding.name);
        if (!error.empty())
            return error;
    }
    for (const auto &binding : bindings)
        if (binding.texture == inputs.output && &binding.required != &r.output)
            return "MetalFX denoising output must not alias an input texture";
    if (inputs.output.storageMode != MTLStorageModePrivate)
        return "MetalFX denoising output texture must use private storage";
    if (inputs.fence != nil && inputs.fence.device != impl_->device)
        return "MetalFX denoising fence belongs to a different device";
#if RYPATH_HAS_METALFX_DENOISER
    if (@available(macOS 26.0, *)) {
        id<MTLFXTemporalDenoisedScaler> scaler = impl_->scaler;
        scaler.colorTexture = inputs.color;
        scaler.depthTexture = inputs.depth;
        scaler.motionTexture = inputs.motion;
        scaler.diffuseAlbedoTexture = inputs.diffuse_albedo;
        scaler.specularAlbedoTexture = inputs.specular_albedo;
        scaler.normalTexture = inputs.normal;
        scaler.roughnessTexture = inputs.roughness;
        scaler.exposureTexture = inputs.exposure;
        scaler.outputTexture = inputs.output;
        scaler.worldToViewMatrix = inputs.world_to_view;
        scaler.viewToClipMatrix = inputs.view_to_clip;
        scaler.motionVectorScaleX = inputs.motion_scale_x;
        scaler.motionVectorScaleY = inputs.motion_scale_y;
        scaler.jitterOffsetX = inputs.jitter_x;
        scaler.jitterOffsetY = inputs.jitter_y;
        scaler.depthReversed = inputs.depth_reversed;
        scaler.shouldResetHistory = inputs.reset_history;
        scaler.preExposure = 1;
        scaler.fence = inputs.fence;
        [scaler encodeToCommandBuffer:command];
        return {};
    }
    return "MetalFX denoising requires macOS 26 or newer";
#else
    return "MetalFX denoising is unavailable in the build SDK";
#endif
}

} // namespace rypath
