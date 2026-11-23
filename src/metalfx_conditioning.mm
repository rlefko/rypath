// SPDX-License-Identifier: GPL-3.0-only
#include "metalfx_conditioning.hpp"
#include "metal_execution.hpp"
#include "metal_frame_access.hpp"
#include "metal_reconstruction_access.hpp"
#include "metalfx_denoiser.hpp"
#include "reconstruction_camera.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string_view>
namespace rypath {
namespace {
using Clock = std::chrono::steady_clock;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::size_t stride(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatR16Float:
        return 2;
    case MTLPixelFormatR32Float:
    case MTLPixelFormatRG16Float:
        return 4;
    case MTLPixelFormatRGBA16Float:
        return 8;
    default:
        throw std::runtime_error("Unsupported conditioning guide format");
    }
}
id<MTLTexture> texture(id<MTLDevice> device, const MetalFXTextureRequirement &r) {
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:r.format
                                                                width:r.width
                                                               height:r.height
                                                            mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = r.usage;
    return [device newTextureWithDescriptor:d];
}
const MetalReconstructionTextureSnapshot &
guide(const MetalReconstructionSnapshot &s, const char *name, const MetalFXTextureRequirement &r) {
    const MetalReconstructionTextureSnapshot *found = nullptr;
    for (const auto &t : s.textures)
        if (t.name == name) {
            check(!found, "Duplicate conditioning guide");
            found = &t;
        }
    check(found && found->format == r.format && found->width == r.width &&
              found->height == r.height,
          "Missing or mismatched conditioning guide");
    check(found->bytes.size() == r.width * r.height * stride(r.format),
          "Conditioning guide byte count mismatch");
    const std::size_t component = r.format == MTLPixelFormatR32Float ? 4 : 2;
    for (std::size_t i = 0; i < found->bytes.size(); i += component) {
        float value;
        if (component == 4)
            std::memcpy(&value, found->bytes.data() + i, 4);
        else {
            _Float16 h;
            std::memcpy(&h, found->bytes.data() + i, 2);
            value = float(h);
        }
        check(std::isfinite(value), "Nonfinite conditioning guide");
        if (r.format == MTLPixelFormatR32Float || std::string_view(name) != "input.motion")
            check(value >= (std::string_view(name) == "input.normal" ? -1.f : 0.f) && value <= 1.f,
                  "Conditioning guide outside its normalized range");
    }
    return *found;
}
void upload(id<MTLDevice> device, id<MTLBlitCommandEncoder> blit, id<MTLTexture> target,
            const void *source, std::size_t size, std::vector<id<MTLBuffer>> &retained) {
    const auto bytes = target.width * stride(target.pixelFormat),
               row = (bytes + 255) & ~NSUInteger(255);
    check(size == bytes * target.height, "Conditioning upload size mismatch");
    auto staging = [device newBufferWithLength:row * target.height
                                       options:MTLResourceStorageModeShared];
    check(staging != nil, "Conditioning upload allocation failed");
    for (NSUInteger y = 0; y < target.height; ++y)
        std::memcpy(static_cast<std::byte *>(staging.contents) + y * row,
                    static_cast<const std::byte *>(source) + y * bytes, bytes);
    [blit copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:row
        sourceBytesPerImage:row * target.height
                 sourceSize:MTLSizeMake(target.width, target.height, 1)
                  toTexture:target
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    retained.push_back(staging);
}
} // namespace
struct MetalFXConditioningComparator::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    std::unique_ptr<MetalFXDenoiser> denoiser;
    MetalFXDenoiserInputs inputs;
    std::uint32_t width = 0, height = 0;
    double near_plane = .001;
    bool valid = false;
};
MetalFXConditioningComparator::MetalFXConditioningComparator(std::unique_ptr<Impl> p)
    : impl_(std::move(p)) {}
MetalFXConditioningComparator::~MetalFXConditioningComparator() = default;
std::unique_ptr<MetalFXConditioningComparator>
MetalFXConditioningComparator::create(MetalFrameLease &source, std::string &diagnostic,
                                      double near_plane) {
    diagnostic.clear();
    try {
        check(source.device() != nil && source.queue() != nil,
              "Conditioning requires a completed Metal source");
        const auto w = source.info().width, h = source.info().height;
        check(source.info().status == FrameStatus::complete && w && h && w <= 16384 && h <= 16384,
              "Invalid conditioning source extent/status");
        check(std::isfinite(near_plane) && near_plane > 0 && std::isnormal(float(near_plane)),
              "Invalid conditioning near plane");
        // Textures + padded upload/readback + CPU half/RGB planes, excluding
        // MetalFX private allocations and the caller-owned snapshot/mean.
        const auto budget =
            ((std::uint64_t(w) + 63) / 64 * 64) * ((std::uint64_t(h) + 63) / 64 * 64) * 160 +
            20 * 65536ull;
        check(budget <= (1ull << 30) && (!source.device().recommendedMaxWorkingSetSize ||
                                         budget <= source.device().recommendedMaxWorkingSetSize),
              "Conditioning resource budget exceeds 1 GiB/device budget");
        auto made = MetalFXDenoiser::create(source.device(), {w, h, w, h});
        check(made.denoiser != nullptr, made.diagnostic.c_str());
        auto p = std::make_unique<Impl>();
        p->device = source.device();
        p->queue = source.queue();
        p->width = w;
        p->height = h;
        p->near_plane = near_plane;
        p->denoiser = std::move(made.denoiser);
        const auto &r = p->denoiser->requirements();
        auto &i = p->inputs;
        i.color = texture(p->device, r.color);
        i.depth = texture(p->device, r.depth);
        i.motion = texture(p->device, r.motion);
        i.diffuse_albedo = texture(p->device, r.diffuse_albedo);
        i.specular_albedo = texture(p->device, r.specular_albedo);
        i.normal = texture(p->device, r.normal);
        i.roughness = texture(p->device, r.roughness);
        i.exposure = texture(p->device, r.exposure);
        i.output = texture(p->device, r.output);
        std::set<const void *> unique;
        for (id<MTLTexture> t : {i.color, i.depth, i.motion, i.diffuse_albedo, i.specular_albedo,
                                 i.normal, i.roughness, i.exposure, i.output})
            check(t != nil && unique.insert((__bridge const void *)t).second,
                  "Conditioning texture allocation/alias failure");
        return std::unique_ptr<MetalFXConditioningComparator>(
            new MetalFXConditioningComparator(std::move(p)));
    } catch (const std::exception &e) {
        diagnostic = e.what();
        return {};
    }
}
MetalFXConditioningResult MetalFXConditioningComparator::run(
    MetalFrameLease &source, const MetalReconstructionSnapshot &snapshot,
    const std::vector<std::array<float, 3>> &mean, float exposure, bool reset) {
    @autoreleasepool {
        const auto start = Clock::now();
        MetalFXConditioningResult result;
        id<MTLCommandBuffer> command = nil;
        id<MTLBlitCommandEncoder> active = nil;
        bool encoded = false;
        const auto finish = [&] {
            result.wall_seconds = std::chrono::duration<double>(Clock::now() - start).count();
            return std::move(result);
        };
        try {
            auto &s = *impl_;
            const auto &info = source.info();
            const auto &r = s.denoiser->requirements();
            check(source.device() == s.device && info.status == FrameStatus::complete &&
                      info.width == s.width && info.height == s.height,
                  "Conditioning source device/extent/status mismatch");
            check(snapshot.diagnostic.empty() &&
                      snapshot.info.status == ReconstructionStatus::complete &&
                      snapshot.info.mode == ReconstructionMode::metalfx &&
                      snapshot.info.width == s.width && snapshot.info.height == s.height &&
                      snapshot.info.source.status == FrameStatus::complete &&
                      snapshot.info.source.width == s.width &&
                      snapshot.info.source.height == s.height &&
                      snapshot.info.source.sample_index == info.sample_index &&
                      snapshot.info.source.epoch == info.epoch &&
                      snapshot.info.counters.failure == 0 &&
                      snapshot.info.counters.completed == std::uint64_t(s.width) * s.height,
                  "Conditioning snapshot does not match completed fresh MetalFX frame");
            check(std::isfinite(exposure) && exposure >= 0x1p-24f && exposure <= 65504.f,
                  "Invalid conditioning exposure");
            check(mean.size() == std::uint64_t(s.width) * s.height,
                  "Conditioning signed mean size mismatch");
            const std::array<const MetalReconstructionTextureSnapshot *, 6> guides{
                &guide(snapshot, "input.depth", r.depth),
                &guide(snapshot, "input.motion", r.motion),
                &guide(snapshot, "input.diffuse_albedo", r.diffuse_albedo),
                &guide(snapshot, "input.specular_albedo", r.specular_albedo),
                &guide(snapshot, "input.normal", r.normal),
                &guide(snapshot, "input.roughness", r.roughness)};
            std::vector<std::array<_Float16, 4>> half(mean.size());
            for (std::size_t p = 0; p < mean.size(); ++p) {
                bool negative = false, high = false;
                for (unsigned c = 0; c < 3; ++c) {
                    const float v = mean[p][c];
                    check(std::isfinite(v), "Nonfinite signed conditioning mean");
                    negative |= v < 0;
                    high |= v > 65504.f;
                    const float rectified = std::clamp(v, 0.f, 65504.f);
                    half[p][c] = static_cast<_Float16>(rectified);
                    result.audit.signed_mean_rgb[c] += v;
                    result.audit.rectified_mean_rgb[c] += rectified;
                    result.audit.input_half_mean_rgb[c] += float(half[p][c]);
                }
                half[p][3] = 1;
                result.audit.negative_pixels += negative;
                result.audit.high_pixels += high;
            }
            for (unsigned c = 0; c < 3; ++c) {
                result.audit.signed_mean_rgb[c] /= mean.size();
                result.audit.rectified_mean_rgb[c] /= mean.size();
                result.audit.input_half_mean_rgb[c] /= mean.size();
            }
            const reconstruction::CameraConfig cc{s.width, s.height, s.near_plane, {}};
            auto camera = source.camera().raster_camera
                              ? reconstruction::Camera::build(*source.camera().raster_camera, cc)
                              : reconstruction::Camera::build(source.camera().camera, cc);
            check(camera.camera.has_value(), camera.diagnostic.c_str());
            auto inputs = s.inputs;
            std::memcpy(&inputs.world_to_view, camera.camera->world_to_view().data(),
                        sizeof(inputs.world_to_view));
            std::memcpy(&inputs.view_to_clip, camera.camera->view_to_clip().data(),
                        sizeof(inputs.view_to_clip));
            result.history_reset = reset || snapshot.info.history_reset || !s.valid;
            inputs.reset_history = result.history_reset;
            command = [s.queue commandBuffer];
            auto blit = [command blitCommandEncoder];
            active = blit;
            check(command != nil && blit != nil, "Conditioning upload command allocation failed");
            std::vector<id<MTLBuffer>> retained;
            upload(s.device, blit, inputs.color, half.data(), half.size() * sizeof(half[0]),
                   retained);
            const std::array<id<MTLTexture>, 6> targets{
                inputs.depth,           inputs.motion, inputs.diffuse_albedo,
                inputs.specular_albedo, inputs.normal, inputs.roughness};
            for (unsigned k = 0; k < guides.size(); ++k)
                upload(s.device, blit, targets[k], guides[k]->bytes.data(), guides[k]->bytes.size(),
                       retained);
            const _Float16 hint = static_cast<_Float16>(exposure);
            result.exposure = float(hint);
            upload(s.device, blit, inputs.exposure, &hint, sizeof(hint), retained);
            [blit endEncoding];
            active = nil;
            const auto error = s.denoiser->encode(command, inputs);
            check(error.empty(), error.c_str());
            encoded = true;
            const auto row = (s.width * 8ull + 255) & ~255ull;
            auto readback = [s.device newBufferWithLength:row * s.height
                                                  options:MTLResourceStorageModeShared];
            auto reader = [command blitCommandEncoder];
            active = reader;
            check(readback != nil && reader != nil, "Conditioning readback allocation failed");
            [reader copyFromTexture:inputs.output
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(s.width, s.height, 1)
                                toBuffer:readback
                       destinationOffset:0
                  destinationBytesPerRow:row
                destinationBytesPerImage:row * s.height];
            [reader endEncoding];
            active = nil;
            const auto completed = metal::complete(command, "conditioning comparator");
            check(completed.empty(), completed.c_str());
            result.gpu_time_valid =
                command.GPUStartTime > 0 && std::isfinite(command.GPUStartTime) &&
                std::isfinite(command.GPUEndTime) && command.GPUEndTime >= command.GPUStartTime;
            result.gpu_seconds =
                result.gpu_time_valid ? command.GPUEndTime - command.GPUStartTime : 0;
            result.linear_rgb.resize(mean.size());
            for (unsigned y = 0; y < s.height; ++y)
                for (unsigned x = 0; x < s.width; ++x)
                    for (unsigned c = 0; c < 3; ++c) {
                        _Float16 v;
                        std::memcpy(&v,
                                    static_cast<const std::byte *>(readback.contents) + y * row +
                                        x * 8 + c * 2,
                                    2);
                        check(std::isfinite(float(v)), "Nonfinite conditioned MetalFX output");
                        result.linear_rgb[std::size_t(y) * s.width + x][c] = float(v);
                    }
            s.valid = true;
            result.status = GeometryStatus::success;
        } catch (const std::exception &e) {
            if (active)
                [active endEncoding];
            // If MetalFX already encoded, drain its retained work before reuse. The
            // next successful submission always resets this opaque temporal state.
            if (encoded && command.status < MTLCommandBufferStatusCommitted)
                (void)metal::complete(command, "conditioning failure drain");
            impl_->valid = false;
            result.linear_rgb.clear();
            result.diagnostic = e.what();
        }
        return finish();
    }
}
} // namespace rypath
