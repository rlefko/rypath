// SPDX-License-Identifier: GPL-3.0-only
#include "../src/medium_probe_wire.h"
#include "medium.h"
namespace probe = rypath::medium_probe;
kernel void rypath_medium_probe(const device probe::Query *queries [[buffer(0)]],
                                device probe::Result *results [[buffer(1)]],
                                constant uint &count [[buffer(2)]],
                                uint id [[thread_position_in_grid]]) {
    if (id >= count)
        return;
    const auto q = queries[id];
    const auto s = rypath_medium::segment(q.sigma_a, q.sigma_s, q.Le, q.distance);
    const float3 wo(q.ox, q.oy, q.oz), wi(q.ix, q.iy, q.iz);
    const auto sampled = rypath_medium::phase_sample(wo, q.g, float2(q.u, q.v));
    results[id] = {s.transmission,
                   s.emission,
                   rypath_medium::flight(q.sigma_a + q.sigma_s, q.u),
                   rypath_medium::phase(wo, wi, q.g),
                   sampled.x,
                   sampled.y,
                   sampled.z,
                   rypath_medium::phase(wo, sampled, q.g),
                   s.valid};
}
