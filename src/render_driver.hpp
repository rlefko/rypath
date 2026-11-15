// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "metal_render.hpp"

namespace rypath::detail {
// Internal deterministic scheduling seam. Production always supplies the native
// Metal owner; this is not a selectable rendering backend.
struct RenderSampleSource {
    std::function<RenderBuildResult()> prepare;
    std::function<RenderSamplesResult(const RenderRequest &)> sample;
};
RenderResult drive_progressive(const RenderScene &scene, const RenderSettings &settings,
                               const RenderControl &control, const RenderSampleSource &source);
} // namespace rypath::detail
