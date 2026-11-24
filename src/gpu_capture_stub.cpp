// SPDX-License-Identifier: GPL-3.0-only
#include "gpu_capture.hpp"
namespace rypath {
struct GPUCapture::Impl {};
GPUCapture::GPUCapture() : impl_(std::make_unique<Impl>()) {}
GPUCapture::~GPUCapture() = default;
std::string GPUCapture::start(const std::filesystem::path &) {
    return "GPU capture unavailable: Metal backend disabled";
}
void GPUCapture::stop() {}
} // namespace rypath
