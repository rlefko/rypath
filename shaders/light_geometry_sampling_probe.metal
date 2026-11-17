// SPDX-License-Identifier: GPL-3.0-only
#include "../tests/light_geometry_sampling_probe_wire.h"
// Exercise the production surface reconstruction and geometry proposal adapter.
#include "render.metal"
kernel void rypath_probe_light_geometry(
    const device rypath::light_geometry_probe::Query *queries [[buffer(0)]],
    device rypath::light_geometry_probe::Result *results [[buffer(1)]],
    const device wire::Point *positions [[buffer(2)]],
    const device wire::Triangle *triangles [[buffer(3)]],
    const device wire::Patch *patches [[buffer(4)]],
    const device wire::Instance *instances [[buffer(5)]],
    const device wire::Word *data [[buffer(6)]], const device wire::Emitter *emitters [[buffer(7)]],
    constant wire::Parameters &parameters [[buffer(8)]], constant uint &count [[buffer(9)]],
    uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    SurfaceAlpha alpha{};
    alpha.positions = positions;
    alpha.triangles = triangles;
    alpha.patches = patches;
    alpha.instances = instances;
    alpha.material_data = data;
    alpha.parameters = &parameters;
    const auto query = queries[index];
    const auto emitter = emitters[query.emitter];
    const float3 reference = unpack(query.reference), normal = unpack(query.normal);
    float2 uv;
    SurfacePoint point{};
    float pdf = 0;
    const bool valid = rypath_light_geometry::sample(
        alpha, emitter, reference, normal, float2(query.sample.x, query.sample.y), uv, point, pdf);
    float3 center;
    float radius;
    const bool cone = rypath_light_geometry::sphere_cone(alpha, emitter, reference, center, radius);
    SurfacePoint far = point;
    if (cone)
        far.position = 2 * center - point.position;
    results[index] = {
        {point.position.x, point.position.y, point.position.z},
        {point.geometric_normal.x, point.geometric_normal.y, point.geometric_normal.z},
        {uv.x, uv.y},
        pdf,
        valid ? rypath_light_geometry::pdf(alpha, emitter, reference, normal, uv, point) : 0,
        valid ? rypath_light_geometry::area_pdf(alpha, emitter, uv, point) : 0,
        cone ? rypath_light_geometry::pdf(alpha, emitter, reference, normal, uv, far) : 0,
        valid ? 1u : 0u,
        cone ? 1u : 0u};
}
