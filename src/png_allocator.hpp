// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>

namespace rypath {
// LodePNG's allocator API has no user pointer. Scope its policy to this thread,
// retaining the previous value for nested calls and exception unwinding.
class PngAllocationScope {
  public:
    explicit PngAllocationScope(std::size_t bytes);
    ~PngAllocationScope();
    PngAllocationScope(const PngAllocationScope &) = delete;
    PngAllocationScope &operator=(const PngAllocationScope &) = delete;

  private:
    std::size_t previous_;
};
} // namespace rypath
