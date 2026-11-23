// SPDX-License-Identifier: GPL-3.0-only
#include "../tests/light_sampling_probe_wire.h"
#include "image_lighting.h"
using namespace metal;
namespace wire = rypath::render_wire;
static float3 unpack(wire::Point p) { return float3(p.x, p.y, p.z); }
#include "light_emission_sampling.h"
#include "light_sampling.h"
kernel void rypath_probe_light_sampling(
    const device rypath::light_sampling_probe::Query *queries [[buffer(0)]],
    device rypath::light_sampling_probe::Result *results [[buffer(1)]],
    constant wire::Parameters &p [[buffer(2)]], const device uint *data [[buffer(3)]],
    const device wire::PositionalLight &light [[buffer(4)]], constant uint &count [[buffer(5)]],
    uint i [[thread_position_in_grid]]) {
    if (i >= count)
        return;
    auto u = float2(queries[i].u.x, queries[i].u.y);
    device auto &r = results[i];
    float3 direction;
    float density;
    r.valid = rypath_light_emission::sample(light, data, u, direction, density);
    r.direction = {direction.x, direction.y, direction.z};
    r.sampled_pdf = density;
    r.evaluated_pdf = rypath_light_emission::pdf(light, data, direction);
    float z = 1 - 2 * u.x, phi = 2 * 3.14159265358979323846f * u.y, s = sqrt(max(0.f, 1 - z * z));
    r.sphere_pdf = rypath_light_emission::pdf(light, data, float3(s * cos(phi), s * sin(phi), z));
    auto choice = rypath_light_sampling::sample(p, data, {float3(0, 0, 3), float3(0)}, u.x);
    r.light = choice.light;
    r.pmf = choice.pmf;
    r.queried_pmf = rypath_light_sampling::pmf(p, data, {float3(0, 0, 3), float3(0)}, choice.light);
}
