// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace rypath {
namespace medium_probe {
struct Query {
    float sigma_a, sigma_s, Le, distance, g;
    float ox, oy, oz, ix, iy, iz, u, v;
};
struct Result {
    float transmission, emission, distance, phase, x, y, z, sample_pdf;
    unsigned valid;
};
static_assert(sizeof(Query) == 52 && sizeof(Result) == 36, "Medium probe layout");
} // namespace medium_probe
} // namespace rypath
