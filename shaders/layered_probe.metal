// SPDX-License-Identifier: GPL-3.0-only
#include "../src/layered_probe_wire.h"
#include "layered.h"
namespace probe = rypath::layered_probe;
kernel void rypath_layered_probe(const device probe::Query *queries [[buffer(0)]],
                                 device probe::Result *results [[buffer(1)]],
                                 constant uint &count [[buffer(2)]],
                                 uint id [[thread_position_in_grid]]) {
    if (id >= count)
        return;
    auto q = queries[id];
    probe::Result r = {};
    rypath_layered::Parameters p;
    bool valid =
        rypath_dielectric::prepare(q.eta, q.ax, q.ay, false, p.interface) &&
        rypath_conductor::prepare(q.base_eta, q.base_k, q.base_ax, q.base_ay, false, p.conductor);
    p.conductor_base = q.conductor_base;
    p.reflectance = q.reflectance;
    p.thickness = q.thickness;
    p.albedo = q.albedo;
    p.g = q.g;
    p.max_depth = q.max_depth;
    p.samples = q.samples;
    p.material_seed = q.material_seed;
    float3 o(q.ox, q.oy, q.oz), i(q.ix, q.iy, q.iz);
    float2 u(q.u, q.v);
    rypath_layered::Evaluation e{0, 0, false};
    rypath_layered::Sample s{float3(0), 0, 0, false, false, false};
    if (valid) {
        if (q.operation == 1) {
            e = rypath_transmission::thin_evaluate(q.eta, o, i);
            s = rypath_transmission::thin_sample(q.eta, o, q.uc, u);
        } else if (q.operation == 2) {
            e = rypath_transmission::diffuse_evaluate(q.reflectance, q.albedo, o, i);
            s = rypath_transmission::diffuse_sample(q.reflectance, q.albedo, o, q.uc, u);
        } else {
            e = rypath_layered::evaluate(p, o, i, q.seed);
            s = rypath_layered::sample(p, o, q.uc, u, q.seed);
            auto c = rypath_layered::top_sample(p, o, q.uc, u, 2);
            r.conditional_pdf = c.pdf;
            r.conditional_weight = c.weight;
            r.hg_pdf = rypath_layered::phase(o, i, q.g);
        }
    }
    r.f = e.f;
    r.pdf = e.pdf;
    r.evaluation_valid = e.valid;
    r.ix = s.wi.x;
    r.iy = s.wi.y;
    r.iz = s.wi.z;
    r.weight = s.weight;
    r.sample_pdf = s.pdf;
    r.valid = s.valid;
    r.accepted = s.accepted;
    r.delta = s.delta;
    results[id] = r;
}
