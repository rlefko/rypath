// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/triangle_traversal_bounds.h"

// Keep this exceptional closest-hit work out of every inlined traversal body.
[[clang::noinline]] inline triangle_extended::Conversion
triangle_pruning_cap(const device rypath::geometry_wire::Triangle &triangle, float4x3 transform,
                     float3 origin, float3 direction, triangle_extended::Pair distance,
                     float minimum, float maximum) {
    triangle_traversal::Input input;
    for (uint axis = 0; axis < 3; ++axis) {
        input.origin[axis] = as_type<uint>(origin[axis]);
        input.direction[axis] = as_type<uint>(direction[axis]);
        for (uint corner = 0; corner < 3; ++corner)
            input.corners[corner * 3 + axis] = as_type<uint>(triangle.corners[corner][axis]);
        for (uint column = 0; column < 4; ++column)
            input.transform[axis * 4 + column] = as_type<uint>(transform[column][axis]);
    }
    return triangle_traversal::pruning_cap(triangle_traversal::exit_upper(input), distance,
                                           as_type<uint>(minimum), as_type<uint>(maximum));
}
