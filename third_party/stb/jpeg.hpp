// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rypath::jpeg {

enum class Error { none, invalid, unsupported, dimension, retained, scratch, work };

struct Result {
    Error error;
    const char *detail;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;
    std::vector<std::uint8_t> rgb{};
};

struct Limits {
    std::uint32_t max_dimension;
    std::size_t retained_bytes;
    std::size_t scratch_bytes;
};

Result decode(std::span<const std::uint8_t> bytes, Limits limits);

} // namespace rypath::jpeg
