// SPDX-License-Identifier: GPL-3.0-only

#include "surface_detail.h"

using namespace metal;
namespace wire = rypath::render_wire;
namespace detail = rypath_surface_detail;

static wire::Point pack(float3 point) { return {point.x, point.y, point.z}; }

struct Displacement {
    bool operator()(rypath::texture_graph_wire::Context context, thread float &height) const {
        // The point term also detects shifts taken along the wrong tangent basis.
        height = 2.0f * context.u + 3.0f * context.v + 0.25f * context.p.x;
        return context.n.z == 1.0f && context.dpdx.x == 1.0f && context.dudx == 0.25f;
    }
};

kernel void rypath_surface_detail_probe(const device wire::SurfaceVertex *vertices [[buffer(0)]],
                                        const device wire::SurfaceTransform *transforms
                                        [[buffer(1)]],
                                        device wire::Point *result [[buffer(2)]],
                                        uint id [[thread_position_in_grid]]) {
    const device auto &xf = transforms[id];
    const uint indices[] = {0, 1, 2, 3};
    const float2 uvs[] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    const float2 hit(0.25f, 0.5f);
    detail::Frame frame;
    const bool formed = id == 3 || id == 4
                            ? detail::patch(vertices, xf, indices, hit, uvs, frame)
                            : detail::triangle(vertices, xf, indices, hit, uvs, frame);
    float3 mapped(0), bumped(0);
    bool map_ok = false, bump_ok = false;
    if (formed) {
        const rypath::texture_graph_wire::Context context{
            {0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, hit.x, hit.y, 0.25f, 0, 0, 0.25f};
        map_ok =
            detail::normal_map(frame, float3(0.75f, 0.5f, 1.0f), frame.geometric_normal, mapped);
        bump_ok = detail::bump_map(frame, context, frame.geometric_normal, Displacement{}, bumped);
    } else {
        frame = {float3(NAN), float3(NAN), float3(NAN), float3(NAN), float3(NAN), float3(NAN)};
    }
    result += 9 * id;
    result[0] = pack(frame.n);
    result[1] = pack(frame.dpdu);
    result[2] = pack(frame.dpdv);
    result[3] = pack(frame.dndu);
    result[4] = pack(frame.dndv);
    result[5] = pack(mapped);
    result[6] = pack(bumped);
    result[7] = {float(formed), float(map_ok), float(bump_ok)};
    result[8] = pack(frame.geometric_normal);
}
