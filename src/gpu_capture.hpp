// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <memory>
#include <string>
namespace rypath {
// A scope closes capture even when submission, reconstruction or reporting throws.
class GPUCapture {
  public:
    GPUCapture();
    ~GPUCapture();
    GPUCapture(const GPUCapture &) = delete;
    GPUCapture &operator=(const GPUCapture &) = delete;
    std::string start(const std::filesystem::path &);
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rypath
