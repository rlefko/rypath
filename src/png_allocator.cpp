// SPDX-License-Identifier: GPL-3.0-only
#include "png_allocator.hpp"

#include <algorithm>
#include <cstdlib>

namespace {
thread_local std::size_t allocation_limit = std::size_t{128} * 1024 * 1024;
constexpr std::size_t hard_limit = std::size_t{1024} * 1024 * 1024;
} // namespace

rypath::PngAllocationScope::PngAllocationScope(std::size_t bytes) : previous_(allocation_limit) {
    allocation_limit = std::min(bytes, hard_limit);
}
rypath::PngAllocationScope::~PngAllocationScope() { allocation_limit = previous_; }

void *lodepng_malloc(std::size_t bytes) {
    return bytes <= allocation_limit ? std::malloc(bytes) : nullptr;
}
void *lodepng_realloc(void *pointer, std::size_t bytes) {
    return bytes <= allocation_limit ? std::realloc(pointer, bytes) : nullptr;
}
void lodepng_free(void *pointer) { std::free(pointer); }
