// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace rypath {
namespace layered_probe {
struct Query {
    float eta, ax, ay, base_eta, base_k, base_ax, base_ay;
    float reflectance, thickness, albedo, g;
    float ox, oy, oz, ix, iy, iz, uc, u, v;
    unsigned conductor_base, max_depth, samples, seed, operation;
};
struct Result {
    float f, pdf, ix, iy, iz, weight, sample_pdf;
    float conditional_pdf, conditional_weight, hg_pdf;
    unsigned valid, accepted, delta, evaluation_valid;
};
static_assert(sizeof(Query) == 100 && sizeof(Result) == 56, "Layered probe layout");
} // namespace layered_probe
} // namespace rypath
