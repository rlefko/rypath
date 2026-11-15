// SPDX-License-Identifier: GPL-3.0-only

#include "../src/texture_graph_probe_wire.h"
#include "image_texture.h"
#include "texture_graph.h"

using namespace rypath;
namespace wire = texture_graph_wire;

struct GraphProbeSampler {
    const device float *rgb_table;

    bool operator()(uint image, bool spectrum, bool unbounded, wire::Mapped2D c, float,
                    thread float &value) const {
        value = c.s + c.t + c.dsdx + 2.0f * c.dtdx + 3.0f * c.dsdy + 4.0f * c.dtdy;
        return !spectrum && !unbounded && image == 0;
    }

    bool operator()(float r, float g, float b, float wavelength, bool unbounded,
                    thread float &value) const {
        value = unbounded ? rypath_image::unbounded_spectrum(metal::float3(r, g, b), wavelength,
                                                             rgb_table)
                          : rypath_image::spectrum(metal::float3(r, g, b), wavelength, rgb_table);
        return true;
    }
};

kernel void rypath_texture_graph_probe(const device wire::Node *nodes [[buffer(0)]],
                                       const device render_wire::SpectralKnot *knots [[buffer(1)]],
                                       const device texture_graph_probe::Query *queries
                                       [[buffer(2)]],
                                       const device float *rgb_table [[buffer(3)]],
                                       constant texture_graph_probe::Parameters &p [[buffer(4)]],
                                       device float *out [[buffer(5)]],
                                       uint lane [[thread_position_in_grid]]) {
    if (lane >= p.query_count) {
        return;
    }
    const auto q = queries[lane];
    float value = 0;
    const GraphProbeSampler sampler{rgb_table};
    const bool valid = texture_graph::evaluate(nodes, p.node_count, knots, p.knot_count, q.root,
                                               q.ctx, q.wavelength, sampler, sampler, value);
    out[lane] = valid ? value : NAN;
}
