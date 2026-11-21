// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/render_wire.h"
namespace rypath {
namespace light_sampling_probe {
using namespace render_wire;
struct Query {
    Point2 u;
};
struct Result {
    Point direction;
    float sampled_pdf, evaluated_pdf, sphere_pdf;
    Word light, valid;
    float pmf, queried_pmf;
};
static_assert(sizeof(Query) == 8 && sizeof(Result) == 40, "Light probe ABI");
} // namespace light_sampling_probe
} // namespace rypath
