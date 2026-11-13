// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace rypath {
namespace texture_probe {

struct Vector {
    float x, y, z, w;
};

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
};

static_assert(sizeof(Result) == 24 * sizeof(Vector) && sizeof(Vector) == 16,
              "Texture probe result layout");

} // namespace texture_probe
} // namespace rypath
