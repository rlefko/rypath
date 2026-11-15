// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace rypath {
enum class BsdfTableKind { measured, fourier };
struct BsdfTable {
    BsdfTableKind kind;
    std::vector<std::uint32_t> words;
    std::size_t source_bytes = 0;
    std::size_t working_bytes = 0;
};
struct BsdfTableResult {
    std::shared_ptr<const BsdfTable> table;
    std::string diagnostic;
};
struct BsdfTableLimits {
    std::size_t remaining_budget = 512ULL * 1024 * 1024;
    std::size_t per_file_budget = 128ULL * 1024 * 1024;
};
// Empty diagnostic means the canonical packed payload is structurally valid.
std::string validate_bsdf_table(const BsdfTable &);
BsdfTableResult read_bsdf_table(const std::filesystem::path &, BsdfTableKind, BsdfTableLimits = {});
} // namespace rypath
