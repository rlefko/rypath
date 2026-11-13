// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pbrt.hpp"

#include <variant>

namespace rypath::pbrt {

struct PlyBudget {
    std::size_t bytes;
    std::size_t files;
    std::size_t values;
};

struct PlyMesh {
    GeometryMesh geometry;
    std::size_t value_count;
};

struct PlyIssue {
    Severity severity;
    const char *code;
    std::string message;
};

using PlyResult = std::variant<PlyMesh, PlyIssue>;
PlyResult decode_ply(const std::filesystem::path &path, const Limits &limits, PlyBudget &remaining);

} // namespace rypath::pbrt
