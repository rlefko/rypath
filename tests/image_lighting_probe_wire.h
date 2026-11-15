// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "../src/render_wire.h"

namespace rypath {
namespace image_lighting_probe {

struct Query {
    render_wire::Point direction;
    render_wire::Point2 uv;
    render_wire::Point2 sample;
    float wavelength;
};

struct Result {
    float area;
    float environment;
    float query_pdf;
    float sample_pdf;
    render_wire::Point sampled;
    render_wire::Word valid;
    float scalar_bilinear;
    float scalar_point;
    float scalar_ewa;
    float scalar_coarsest;
};

struct StressResult {
    float sampled_pdf;
    float queried_pdf;
    float direction_delta;
    float bright_texel_radiance;
    render_wire::Word initial_cell_matches;
    render_wire::Word valid;
};

static_assert(sizeof(Query) == 32 && sizeof(Result) == 48, "Image lighting probe layout");
static_assert(sizeof(StressResult) == 24, "Image lighting stress probe layout");

} // namespace image_lighting_probe
} // namespace rypath
