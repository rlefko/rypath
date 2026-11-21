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
// Actual BDPT density conversion, observed through this existing full-render TU.
struct DensityQuery {
    render_wire::Point from, to, normal;
    render_wire::Word pdf_bits, infinite;
};
struct DensityResult {
    render_wire::Word pdf_bits, distance_squared_bits, nonzero_edge, magic;
};
static_assert(sizeof(DensityQuery) == 44 && sizeof(DensityResult) == 16,
              "Density conversion probe ABI");
struct OffsetQuery {
    render_wire::Word bits, axis, negative, move;
};
struct OffsetResult {
    render_wire::Word rounded[3], scalar_bits, scalar_valid, magic;
};
static_assert(sizeof(OffsetQuery) == 16 && sizeof(OffsetResult) == 24, "Offset rounding probe ABI");
} // namespace light_geometry_probe
} // namespace rypath
