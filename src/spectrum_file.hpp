// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ply.hpp"

namespace rypath::pbrt {

struct SpectrumFileIssue {
    const char *code;
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
};

using SpectrumFileResult =
    std::variant<std::shared_ptr<const CheckedSpectrumSource>, SpectrumFileIssue>;
SpectrumFileResult decode_spectrum_file(const std::filesystem::path &path, const Limits &limits,
                                        PlyBudget &remaining);

} // namespace rypath::pbrt
