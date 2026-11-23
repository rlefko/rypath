// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "geometry_wire.h"
namespace shadow_composition {
using Word = rypath::geometry_wire::Word;
struct Instance {
    Word triangle_count, material, alpha_root, invisible_emitter, inside_medium, outside_medium;
};
struct Material {
    Word scattering;
};
// Compact metadata for the shared templated predicate. This is not renderer
// RenderScene eligibility or its buffer ABI; values are authored by each fixture.
struct Case {
    rypath::geometry_wire::Ray ray;
    Word mask, alpha_ordinal, rule, instance_count, material_count;
    Instance instances[8];
    Material materials[8];
};
struct Result {
    Word status, alpha_calls, opaque_calls, hint_available, hint_valid, hint_instance,
        hint_primitive;
};
static_assert(sizeof(Case) == 276, "composition input ABI");
static_assert(sizeof(Result) == 28, "composition result ABI");
} // namespace shadow_composition
