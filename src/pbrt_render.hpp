// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pbrt.hpp"
#include "render.hpp"

namespace rypath::pbrt {

struct RenderTranslation {
    std::optional<RenderScene> scene;
    RenderSettings settings;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> provenance;
    int status = 1;

    int exit_code() const { return status; }
};

// Translate a successful structural check into owned renderer data. Structural
// failures keep their original status and diagnostics in the checked result.
RenderTranslation translate(const Result &checked, const Location &entry,
                            const Limits &limits = {});

} // namespace rypath::pbrt
