// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
namespace rypath {
namespace qmc_probe {
struct Query {
    render_wire::Word kind, width, height, x, y, sample_low, sample_high, dimension, mode, seed,
        exponent, log_spp;
};
struct Result {
    render_wire::Word values[8];
};
static_assert(sizeof(Query) == 48 && sizeof(Result) == 32, "QMC probe layout");
} // namespace qmc_probe
} // namespace rypath
