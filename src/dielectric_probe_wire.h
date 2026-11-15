// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace rypath {
namespace dielectric_probe {
struct Query {
    float eta, u_roughness, v_roughness;
    float ox, oy, oz, ix, iy, iz, uc, u, v;
    unsigned remap;
};
struct Result {
    float f, pdf, fresnel, alpha_x, alpha_y;
    float ix, iy, iz, weight, sample_pdf;
    unsigned prepared, smooth, evaluation_valid, fresnel_valid, valid, accepted, delta;
};
static_assert(sizeof(Query) == 52 && sizeof(Result) == 68, "Dielectric probe layout");
} // namespace dielectric_probe
} // namespace rypath
