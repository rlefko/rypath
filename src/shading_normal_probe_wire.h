// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
namespace rypath::shading_normal_probe {
struct Query {
    float wo_ns, wi_ns, wo_ng, wi_ng;
    float a, b;
};
struct Result {
    float applied, multiplied, scale;
    int exponent;
    render_wire::Word valid;
    float endpoint_applied, endpoint_multiplied;
    render_wire::Word reserved;
};
static_assert(sizeof(Query) == 24 && sizeof(Result) == 32, "Shading normal probe ABI");
} // namespace rypath::shading_normal_probe
