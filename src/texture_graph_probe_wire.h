// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "texture_graph_wire.h"

namespace rypath {
namespace texture_graph_probe {

struct Query {
    texture_graph_wire::Context ctx;
    texture_graph_wire::Word root;
    float wavelength;
};

struct Parameters {
    texture_graph_wire::Word node_count;
    texture_graph_wire::Word knot_count;
    texture_graph_wire::Word query_count;
};

static_assert(sizeof(Parameters) == 12 && alignof(Parameters) == 4,
              "texture graph probe parameter layout");
static_assert(sizeof(Query) == 80 && alignof(Query) == 4, "texture graph query layout");

} // namespace texture_graph_probe
} // namespace rypath
