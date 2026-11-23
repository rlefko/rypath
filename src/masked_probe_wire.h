// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "geometry_wire.h"

namespace rypath {
namespace masked_probe {

using geometry_wire::Closest;
using geometry_wire::Ray;
using geometry_wire::Word;

enum : Word { accept, reject, reject_near, invalid, accept_then_reject, accept_then_invalid };
enum : Word { absent_primitive = 0xffffffffu, unexpected_triangle_primitive = 1u << 2 };

struct Case {
    Ray ray;
    Word triangle_rule;
    Word patch_rule;
    Word ignore_instance;
    Word any_hit;
    Word expected_triangle_primitives[2];
};

struct Result {
    Closest hit;
    Word triangle_calls;
    Word patch_calls;
    Word triangle_primitive_mask;
    Word repeated_triangle_values_changed;
};

struct HashCase {
    Word words[7]; // Binary32 bits in PBRT origin, direction, t order.
};

static_assert(sizeof(HashCase) == 28, "masked hash probe input layout");
static_assert(sizeof(Case) == 56 && sizeof(Result) == 44, "masked probe layout");

} // namespace masked_probe
} // namespace rypath
