// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "composition_wire.h"
namespace closest_seed_probe {
using W = rypath::geometry_wire::Word;
// test0=actual packet wrapper;1=nooffer/no cap;2=nooffer/computedcap;
// 3=synthetic coincident later-ID seed;4=invalid seed ID;5=anyhit+seed.
struct Case {
    shadow_composition::Case base;
    W test, seed_ordinal;
};
struct Result {
    rypath::geometry_wire::Closest hit, seed;
    W alpha_calls, opaque_calls, seed_available, has_cap, cap_bits, test;
    W pair[8]; // two explicitly encoded extended Float components, no host/MSL bool ABI
};
static_assert(sizeof(Case) == 284, "seed case ABI");
static_assert(sizeof(Result) == 112, "seed result ABI");
} // namespace closest_seed_probe
