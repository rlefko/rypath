// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace rypath {
namespace gltf_probe {
struct Query {
    float color, metallic, roughness;
    float ox, oy, oz, ix, iy, iz;
    float choice, u, v;
};
struct Result {
    float alpha, probability, f, pdf, reverse_f;
    float ix, iy, iz, weight, sample_pdf;
    float sampled_f, sampled_evaluation_pdf;
    unsigned prepared, evaluation_valid, reverse_valid, valid, accepted, delta;
};
static_assert(sizeof(Query) == 48 && sizeof(Result) == 72, "glTF probe layout");
} // namespace gltf_probe
} // namespace rypath
