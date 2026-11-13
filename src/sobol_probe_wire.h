// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "sobol_shared.h"

namespace rypath {
namespace sobol {

// Internal numerical probe for host/MSL sampling conformance tests.
struct Probe {
    Word exponent;
    Word x;
    Word y;
    Word sample_low;
    Word sample_high;
    Word dimension;
    Word mode;
    Word seed;
};
struct ProbeResult {
    Word index_low;
    Word index_high;
    Word value;
    Word hash;
    float unit;
    float camera;
};
static_assert(sizeof(Probe) == 32 && alignof(Probe) == 4, "Sobol probe layout");
static_assert(sizeof(ProbeResult) == 24 && alignof(ProbeResult) == 4, "Sobol result layout");

} // namespace sobol
} // namespace rypath
