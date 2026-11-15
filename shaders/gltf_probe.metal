// SPDX-License-Identifier: GPL-3.0-only
#include "../src/gltf_probe_wire.h"
#include "gltf_bsdf.h"
namespace probe = rypath::gltf_probe;
kernel void rypath_gltf_probe(const device probe::Query *queries [[buffer(0)]],
                              device probe::Result *results [[buffer(1)]],
                              constant uint &count [[buffer(2)]],
                              uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    const auto q = queries[index];
    probe::Result r = {};
    rypath_gltf::Parameters p;
    r.prepared = rypath_gltf::prepare(q.color, q.metallic, q.roughness, p);
    if (r.prepared) {
        r.alpha = p.alpha;
        r.probability = p.specular_probability;
        const float3 o(q.ox, q.oy, q.oz), i(q.ix, q.iy, q.iz);
        const auto e = rypath_gltf::evaluate(p, o, i);
        const auto reverse = rypath_gltf::evaluate(p, i, o);
        r.f = e.f;
        r.pdf = e.pdf;
        r.evaluation_valid = e.valid;
        r.reverse_f = reverse.f;
        r.reverse_valid = reverse.valid;
        const auto s = rypath_gltf::sample(p, o, q.choice, float2(q.u, q.v));
        r.ix = s.wi.x;
        r.iy = s.wi.y;
        r.iz = s.wi.z;
        r.weight = s.weight;
        r.sample_pdf = s.pdf;
        r.valid = s.valid;
        r.accepted = s.accepted;
        r.delta = s.delta;
        if (s.accepted && !s.delta) {
            const auto sampled = rypath_gltf::evaluate(p, o, s.wi);
            r.sampled_f = sampled.f;
            r.sampled_evaluation_pdf = sampled.pdf;
        }
    }
    results[index] = r;
}
