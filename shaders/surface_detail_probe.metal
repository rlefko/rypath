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
    float2 uvs[] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    if (id == 7) {
        uvs[1].x = -1;
        uvs[3].x = -1;
    }
    const float2 hit(0.25f, 0.5f);
    detail::Frame frame;
    const bool formed = id == 3 || id == 4
                            ? detail::patch(vertices, xf, indices, hit, uvs, frame)
                            : detail::triangle(vertices, xf, indices, hit, uvs, frame);
    float3 mapped(0), bumped(0), retained_mapped(NAN), retained_bumped(NAN);
    detail::Frame map_frame = {}, bump_frame = {};
    bool map_ok = false, bump_ok = false;
    if (formed) {
        const rypath::texture_graph_wire::Context context{
            {0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, hit.x, hit.y, 0.25f, 0, 0, 0.25f};
        map_ok =
            detail::normal_map(frame, float3(0.75f, 0.5f, 1.0f), frame.geometric_normal, mapped);
        bump_ok = detail::bump_map(frame, context, frame.geometric_normal, Displacement{}, bumped);
        map_ok = detail::normal_map(frame, float3(0.75f, 0.5f, 1.0f), frame.geometric_normal,
                                    retained_mapped, &map_frame) &&
                 map_ok;
        bump_ok = detail::bump_map(frame, context, frame.geometric_normal, Displacement{},
                                   retained_bumped, &bump_frame) &&
                  bump_ok;
    } else {
        frame = {float3(NAN), float3(NAN), float3(NAN), float3(NAN), float3(NAN), float3(NAN)};
    }
    device auto *retained = result + 9 * 8 + 8 * id;
    retained[0] = pack(map_frame.n);
    retained[1] = pack(map_frame.dpdu);
    retained[2] = pack(map_frame.dpdv);
    retained[3] = pack(bump_frame.n);
    retained[4] = pack(bump_frame.dpdu);
    retained[5] = pack(bump_frame.dpdv);
    retained[6] = pack(retained_mapped);
    retained[7] = pack(retained_bumped);
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
