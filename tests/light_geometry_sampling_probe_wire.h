// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/render_wire.h"
namespace rypath {
namespace light_geometry_probe {
struct Query {
    render_wire::Point reference;
    render_wire::Point normal;
    render_wire::Point2 sample;
    render_wire::Word emitter;
};
struct Result {
    render_wire::Point position;
    render_wire::Point normal;
    render_wire::Point2 uv;
    float sampled_pdf;
    float queried_pdf;
    float area_pdf;
    float far_root_pdf;
    render_wire::Word valid;
    render_wire::Word cone;
};
static_assert(sizeof(Query) == 36 && sizeof(Result) == 56, "Geometry proposal probe ABI");
} // namespace light_geometry_probe
} // namespace rypath
