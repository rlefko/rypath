// SPDX-License-Identifier: GPL-3.0-only
#include "../src/conductor_probe_wire.h"
#include "conductor.h"
namespace probe = rypath::conductor_probe;
kernel void rypath_conductor_probe(const device probe::Query *queries [[buffer(0)]],
                                   device probe::Result *results [[buffer(1)]],
                                   constant uint &count [[buffer(2)]],
                                   uint id [[thread_position_in_grid]]) {
    if (id >= count)
        return;
    auto q = queries[id];
    rypath_conductor::Parameters p;
    probe::Result r = {};
    r.prepared = rypath_conductor::prepare(q.eta, q.k, q.u_roughness, q.v_roughness, q.remap, p);
    if (r.prepared) {
        r.smooth = p.smooth;
        r.alpha_x = p.alpha_x;
        r.alpha_y = p.alpha_y;
        auto e = rypath_conductor::evaluate(p, float3(q.ox, q.oy, q.oz), float3(q.ix, q.iy, q.iz));
        r.f = e.f;
        r.pdf = e.pdf;
        r.evaluation_valid = e.valid;
        r.fresnel_valid = rypath_conductor::fresnel(abs(q.oz), p.eta, p.k, r.fresnel);
        auto s = rypath_conductor::sample(p, float3(q.ox, q.oy, q.oz), float2(q.u, q.v));
        r.ix = s.wi.x;
        r.iy = s.wi.y;
        r.iz = s.wi.z;
        r.weight = s.weight;
        r.sample_pdf = s.pdf;
        r.valid = s.valid;
        r.accepted = s.accepted;
        r.delta = s.delta;
    }
    results[id] = r;
}
