// SPDX-License-Identifier: GPL-3.0-only
// Low-level actual opaque-validation helper plus generic masked fallback.
// Tests TLAS masks, precise original bounds, and callback behavior. Compact
// metadata does not establish actual renderer material/global eligibility.
#include "composition_wire.h"
#include "intersection.h"
using namespace metal;
using namespace metal::raytracing;
namespace cw = shadow_composition;
namespace gw = rypath::geometry_wire;
struct CompositionAlpha {
    const device cw::Case *parameters;
    const device cw::Instance *instances;
    const device cw::Material *materials;
    uint alpha_calls = 0, opaque_calls = 0;
    MaskedAlphaResult operator()(uint instance, uint, uint, intersection_type, float2, float) {
        if (instance != parameters->alpha_ordinal) {
            ++opaque_calls;
            return {true, true};
        }
        ++alpha_calls;
        return {parameters->rule == 0, parameters->rule != 2}; // accept/reject/invalid
    }
};
kernel void composition_shadow(const device cw::Case *cases [[buffer(0)]],
                               device cw::Result *output [[buffer(1)]],
                               constant uint &count [[buffer(2)]],
                               instance_acceleration_structure scene [[buffer(3)]],
                               instance_acceleration_structure hints [[buffer(4)]],
                               uint lane [[thread_position_in_grid]]) {
    if (lane >= count)
        return;
    const device auto &c = cases[lane];
    ray r(float3(c.ray.origin[0], c.ray.origin[1], c.ray.origin[2]),
          float3(c.ray.direction[0], c.ray.direction[1], c.ray.direction[2]), c.ray.t_min,
          c.ray.t_max);
    CompositionAlpha alpha{&c, c.instances, c.materials};
    const auto h = native_opaque_shadow_hint(r, hints, c.mask, alpha);
    uint status;
    if (!h.valid)
        status = gw::invalid_hit;
    else if (h.available)
        status = 1;
    else {
        const auto hit = trace_validated_surface(r, scene, c.mask, alpha, true);
        status = !hit.valid ? gw::invalid_hit : hit.type == intersection_type::none ? 0u : 1u;
    }
    output[lane] = {status,
                    alpha.alpha_calls,
                    alpha.opaque_calls,
                    uint(h.available),
                    uint(h.valid),
                    h.available ? h.surface.instance_id : 0xffffffffu,
                    h.available ? h.surface.primitive_id : 0xffffffffu};
}
