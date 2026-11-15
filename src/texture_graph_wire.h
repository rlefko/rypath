// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "render_wire.h"

namespace rypath {
namespace texture_graph_wire {

using Word = render_wire::Word;
using Point = render_wire::Point;

enum : Word { no_texture = 0xffffffffu, max_depth = 64, max_visits = 4096, max_octaves = 32 };
enum : Word { scalar, spectrum };
enum : Word {
    constant_node,
    knots,
    rgb_sigmoid,
    blackbody,
    image,
    scale,
    mix,
    directionmix,
    bilerp,
    checkerboard,
    dots,
    fbm,
    wrinkled,
    windy,
    marble,
};
enum : Word { uv, planar, spherical, cylindrical, spatial };

#if defined(__METAL_VERSION__)
#define RYPATH_GRAPH_DEFAULT(...)
#else
#define RYPATH_GRAPH_DEFAULT(...) {__VA_ARGS__}
#endif

struct Mapping {
    Word kind RYPATH_GRAPH_DEFAULT(uv);
    // UV: su, sv, du, dv. Planar: vs.xyz, vt.xyz, du, dv.
    float values[8] RYPATH_GRAPH_DEFAULT(1, 1, 0, 0);
    // Three rows of the inverse declaration transform, in the evaluation frame.
    float texture_from_render[12] RYPATH_GRAPH_DEFAULT(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0);
};

struct Node {
    Word kind RYPATH_GRAPH_DEFAULT(constant_node);
    Word type RYPATH_GRAPH_DEFAULT(scalar);
    // Scale: texture, factor. Mix: tex1, tex2, amount. Bilerp: v00, v01, v10, v11.
    // Directionmix and checkerboard: tex1, tex2. Dots: geometric outside, inside.
    Word children[4] RYPATH_GRAPH_DEFAULT(no_texture, no_texture, no_texture, no_texture);
    // Knot begin/count, image index, or procedural octave count.
    Word payload[2] RYPATH_GRAPH_DEFAULT();
    // Constant, temperature, sigmoid coefficients, direction, or roughness/scale/variation.
    float values[4] RYPATH_GRAPH_DEFAULT();
    Mapping mapping RYPATH_GRAPH_DEFAULT();
};

#undef RYPATH_GRAPH_DEFAULT

struct Context {
    Point p;
    Point n;
    Point dpdx;
    Point dpdy;
    float u;
    float v;
    float dudx;
    float dvdx;
    float dudy;
    float dvdy;
};

struct Mapped2D {
    float s;
    float t;
    float dsdx;
    float dtdx;
    float dsdy;
    float dtdy;
};

static_assert(sizeof(Mapping) == 84 && alignof(Mapping) == 4 &&
                  __builtin_offsetof(Mapping, texture_from_render) == 36,
              "texture graph mapping layout");
static_assert(sizeof(Node) == 132 && alignof(Node) == 4 &&
                  __builtin_offsetof(Node, payload) == 24 &&
                  __builtin_offsetof(Node, values) == 32 && __builtin_offsetof(Node, mapping) == 48,
              "texture graph node layout");
static_assert(sizeof(Context) == 72 && alignof(Context) == 4 &&
                  __builtin_offsetof(Context, u) == 48,
              "texture graph context layout");
static_assert(sizeof(Mapped2D) == 24 && alignof(Mapped2D) == 4, "mapped coordinates layout");

} // namespace texture_graph_wire
} // namespace rypath
