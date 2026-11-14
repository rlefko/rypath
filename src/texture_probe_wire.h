// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace rypath {
namespace texture_probe {

struct Vector {
    float x, y, z, w;
};

struct PointCase {
    unsigned texture;
    float u, v, half_width;
};

enum : unsigned { point_lod_seam_case = 30, point_case_count };

struct EwaCase {
    unsigned texture;
    float u, v, dsdx, dtdx, dsdy, dtdy;
};

enum : unsigned { ewa_case_count = 39 };

struct Result {
    Vector primary[2];
    Vector parallel;
    Vector rotated[2];
    Vector sheared[2];
    Vector grazing[2];
    Vector patch[3];
    Vector patch_derivatives;
    Vector mirrored[2];
    Vector degenerate[2];
    Vector sanitized;
    Vector fine;
    Vector middle;
    Vector coarse;
    Vector wrapped;
    Vector black;
    Vector spectrum;
    Vector trilinear_base;
    Vector trilinear_integer;
    Vector trilinear_middle;
    Vector trilinear_black;
    Vector trilinear_coarse;
    Vector singleton_narrow;
    Vector singleton_wide;
    Vector invalid_upper;
    Vector invalid_filter;
    Vector point[point_case_count];
    Vector ewa[ewa_case_count];
};

static_assert(sizeof(Result) == (33 + point_case_count + ewa_case_count) * sizeof(Vector) &&
                  sizeof(Vector) == 16 && sizeof(PointCase) == 16 && sizeof(EwaCase) == 28 &&
                  __builtin_offsetof(EwaCase, dsdx) == 12,
              "Texture probe result layout");

} // namespace texture_probe
} // namespace rypath
