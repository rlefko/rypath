// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace rypath {
namespace hdr_probe {
struct Query {
    unsigned texture, native;
    float u, v, dsdx, dtdx, dsdy, dtdy;
};
struct Result {
    float r, g, b, scalar;
};
static_assert(sizeof(Query) == 32 && sizeof(Result) == 16, "HDR probe wire layout");
} // namespace hdr_probe
} // namespace rypath
