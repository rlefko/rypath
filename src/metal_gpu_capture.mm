// SPDX-License-Identifier: GPL-3.0-only
#include "gpu_capture.hpp"
#import <Metal/Metal.h>
#include <cstdlib>
namespace rypath {
struct GPUCapture::Impl {
    bool active = false;
    id<MTLDevice> device = nil;
};
GPUCapture::GPUCapture() : impl_(std::make_unique<Impl>()) {}
GPUCapture::~GPUCapture() { stop(); }
std::string GPUCapture::start(const std::filesystem::path &path) {
    const char *enabled = std::getenv("MTL_CAPTURE_ENABLED");
    if (!enabled || std::string(enabled) != "1")
        return "GPU capture requires MTL_CAPTURE_ENABLED=1 before process launch";
    if (std::filesystem::exists(path) || std::filesystem::is_symlink(path))
        return "GPU capture destination must be fresh";
    auto manager = [MTLCaptureManager sharedCaptureManager];
    if (![manager supportsDestination:MTLCaptureDestinationGPUTraceDocument])
        return "GPU trace document capture unavailable";
    if (manager.isCapturing || impl_->active)
        return "another GPU capture is active";
    impl_->device = MTLCreateSystemDefaultDevice();
    if (!impl_->device)
        return "GPU capture Metal device unavailable";
    auto descriptor = [MTLCaptureDescriptor new];
    descriptor.captureObject = impl_->device;
    descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
    descriptor.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    NSError *error = nil;
    if (![manager startCaptureWithDescriptor:descriptor error:&error]) {
        const char *message = error.localizedDescription.UTF8String;
        return "GPU capture start failed: " + std::string(message ? message : "unknown error");
    }
    impl_->active = true;
    return {};
}
void GPUCapture::stop() {
    if (impl_->active) {
        [[MTLCaptureManager sharedCaptureManager] stopCapture];
        impl_->active = false;
    }
}
} // namespace rypath
