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

// Calls the production helper from render.metal, with no copied conversion math.
kernel void rypath_probe_density_conversion(
    const device rypath::light_geometry_probe::DensityQuery *queries [[buffer(0)]],
    device rypath::light_geometry_probe::DensityResult *results [[buffer(1)]],
    constant uint &count [[buffer(2)]], uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    const auto q = queries[index];
    rypath_bdpt::Vertex from{}, to{};
    from.point.position = unpack(q.from);
    to.point.position = unpack(q.to);
    to.point.geometric_normal = unpack(q.normal);
    to.type = q.infinite ? rypath_bdpt::environment_vertex : rypath_bdpt::light_vertex;
    const float3 edge = to.point.position - from.point.position;
    results[index] = {
        as_type<uint>(rypath_bdpt::convert_density(as_type<float>(q.pdf_bits), from, to)),
        as_type<uint>(dot(edge, edge)), uint(any(edge != 0)), 0x44454e31u};
}

// Exercise actual shared production round_out, including inactive coordinates.
kernel void rypath_probe_offset_rounding(
    const device rypath::light_geometry_probe::OffsetQuery *queries [[buffer(0)]],
    device rypath::light_geometry_probe::OffsetResult *results [[buffer(1)]],
    constant uint &count [[buffer(2)]], uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    const auto q = queries[index];
    float3 position(.25f, -.5f, 1), normal(0);
    position[q.axis] = as_type<float>(q.bits);
    if (q.move)
        normal[q.axis] = q.negative ? -1.f : 1.f;
    const auto rounded = round_out(position, normal);
    const auto scalar = rypath_offset_rounding::outward(q.bits, q.negative != 0);
    results[index] = {
        {as_type<uint>(rounded.x), as_type<uint>(rounded.y), as_type<uint>(rounded.z)},
        scalar.bits,
        scalar.valid ? 1u : 0u,
        0x4f464631u};
}

namespace canonical_mis_probe {
using namespace rypath_bdpt;
namespace probe = rypath::light_geometry_probe;
inline void point(device wire::Point &out, float3 p) { out = {p.x, p.y, p.z}; }
// Output-reference construction is conditional on the separately reviewed ABI
// diagnostic/adoption decision. Top-level reflectance intentionally stays0.
inline void coating(thread rypath_packet::Prepared &prepared, uint material_seed, bool zero) {
    prepared = {};
    prepared.active_mask = 15;
    const float rho[4] = {.13526731729507446f, .08804553747177124f, .19378170371055603f,
                          .16280370950698853f};
    for (uint lane = 0; lane < 4; ++lane) {
        thread auto &p = prepared.lane[lane];
        p.kind = 5;
        p.wavelength = 400 + 100 * lane;
        thread auto &l = p.layered;
        rypath_dielectric::prepare(1.5f, .10201960802078247f, .10201960802078247f, false,
                                   l.interface);
        l.conductor_base = false;
        l.reflectance = zero ? 0 : rho[lane];
        l.thickness = .009999999776482582f;
        l.albedo = l.g = 0;
        l.max_depth = 10;
        l.samples = 1;
        l.material_seed = int(material_seed);
    }
}
inline bool context(const thread probe::CanonicalQuery &q, constant wire::Parameters &p,
                    thread Vertex &C, thread Vertex &P, thread Vertex &E) {
    const ScatteringFrame frame{float3(q.frame[0], q.frame[1], q.frame[2]),
                                float3(q.frame[3], q.frame[4], q.frame[5]),
                                float3(q.frame[6], q.frame[7], q.frame[8])};
    C = {};
    P = {};
    E = {};
    C.type = camera_vertex;
    C.path_id = 0;
    C.connectible = true;
    C.point.position = unpack(p.origin);
    C.point.geometric_normal = unpack(p.camera_normal);
    C.point.shading_normal = C.point.geometric_normal;
    C.beta = 1;
    P.type = surface_vertex;
    P.path_id = 1;
    P.connectible = true;
    P.seed = 0x1234u;
    P.point.position = 0;
    P.point.geometric_normal = frame.z;
    P.point.shading_normal = P.point.winding_normal = frame.z;
    P.frame = frame;
    coating(P.bsdf, q.material_seed, q.zero_base != 0);
    P.wo = normalize(C.point.position);
    P.beta = 1;
    const auto direction = normalize(frame.world(normalize(float3(q.wi[0], q.wi[1], q.wi[2]))));
    E.type = environment_vertex;
    E.path_id = 2;
    E.light = 0;
    E.connectible = true;
    E.point.position = direction;
    E.point.geometric_normal = E.point.shading_normal = -direction;
    E.wo = -direction;
    E.beta = 1;
    P.sampled_wo_local = frame.local(P.wo);
    P.sampled_wi_local = normalize(float3(q.wi[0], q.wi[1], q.wi[2]));
    P.sampled_prev_id = C.path_id;
    P.sampled_next_id = E.path_id;
    P.pair_valid = true;
    return rypath_packet::compatible(P.bsdf) && all(isfinite(direction));
}
// Independent represented-geometry operands, never vertex_pdf's cached-pair
// selection or the new represented_scattering_pdf helper. PDF math stays real.
inline float expected_density(const thread Resources &r, const thread Vertex &v,
                              const thread Vertex &before, const thread Vertex &after, Mode mode,
                              thread float3 &wo, thread float3 &wi) {
    const auto a = infinite(before) ? -before.point.geometric_normal
                                    : before.point.position - v.point.position;
    const auto b =
        infinite(after) ? -after.point.geometric_normal : after.point.position - v.point.position;
    wo = v.frame.local(normalize(a));
    wi = v.frame.local(normalize(b));
    if (v.type == camera_vertex)
        return vertex_pdf(r, v, before, after, mode);
    return convert_density(rypath_packet::pdf(v.bsdf, wo, wi, mode), v, after);
}
inline void protect(device probe::CanonicalProtected &out, const thread Vertex &v) {
    for (uint i = 0; i < 4; ++i) {
        out.beta[i] = v.beta[i];
        out.rho[i] = v.bsdf.lane[i].layered.reflectance;
        out.top_rho[i] = v.bsdf.lane[i].reflectance;
        out.kind[i] = v.bsdf.lane[i].kind;
    }
    point(out.sampled_wo, v.sampled_wo_local);
    point(out.sampled_wi, v.sampled_wi_local);
    out.path_id = v.path_id;
    out.prev_id = v.sampled_prev_id;
    out.next_id = v.sampled_next_id;
    out.pair_valid = v.pair_valid;
    out.delta = v.delta;
    out.active_mask = v.bsdf.active_mask;
}
// Read actual stored fields without an aggregate device-to-thread copy.
inline void protect(device probe::CanonicalProtected &out, const device Vertex &v) {
    for (uint i = 0; i < 4; ++i) {
        out.beta[i] = v.beta[i];
        out.rho[i] = v.bsdf.lane[i].layered.reflectance;
        out.top_rho[i] = v.bsdf.lane[i].reflectance;
        out.kind[i] = v.bsdf.lane[i].kind;
    }
    point(out.sampled_wo, v.sampled_wo_local);
    point(out.sampled_wi, v.sampled_wi_local);
    out.path_id = v.path_id;
    out.prev_id = v.sampled_prev_id;
    out.next_id = v.sampled_next_id;
    out.pair_valid = v.pair_valid;
    out.delta = v.delta;
    out.active_mask = v.bsdf.active_mask;
}
} // namespace canonical_mis_probe

// Separate entrypoint intentionally uses only APIs available in old22 source.
// The negative control excludes the later finalizer entrypoint, not this test.
kernel void rypath_probe_canonical_partition(
    const device rypath::light_geometry_probe::CanonicalQuery *queries [[buffer(0)]],
    constant wire::Parameters *parameters [[buffer(1)]],
    device rypath::light_geometry_probe::CanonicalPartition *results [[buffer(2)]],
    device uchar *scratch [[buffer(3)]], uint index [[thread_position_in_grid]]) {
    using namespace canonical_mis_probe;
    if (index >= probe::canonical_contexts)
        return;
    device auto &out = results[index];
    out = {};
    out.magic = 0x434d5031u;
    out.context = index;
    out.status = wire::invalid_arithmetic;
    out.vertex_bytes = sizeof(Vertex);
    out.vertex_alignment = alignof(Vertex);
    out.renderer = rypath::checkpoint_contract::renderer;
    const auto q = queries[index];
    constant auto &p = parameters[index];
    // Nontracing fixture: root must compile-check safe Resources{} construction
    // before dispatch; never substitute uninitialized opaque AS handles.
    Resources r{};
    r.alpha.parameters = &p;
    Vertex C{}, P{}, E{};
    if (!context(q, p, C, P, E))
        return;
    out.active_mask = P.bsdf.active_mask;
    out.compatible = rypath_packet::compatible(P.bsdf);
    for (uint i = 0; i < 4; ++i) {
        out.rho[i] = P.bsdf.lane[i].layered.reflectance;
        out.top_rho[i] = P.bsdf.lane[i].reflectance;
    }
    // OLD-source negative control still populates these with actual old
    // vertex_pdf; no unavailable canonical helper or hand-built factor is used.
    P.pdf_fwd = vertex_pdf(r, C, Vertex{}, P, Mode::Radiance);
    E.pdf_fwd = vertex_pdf(r, P, C, E, Mode::Radiance);
    auto camera = scratch + index * probe::canonical_scratch_slots * storage::vertex_bytes;
    auto light = camera + 3 * storage::vertex_bytes;
    Vertex sampled_camera{};
    float2 raster;
    float3 camera_direction;
    if (!camera_sample(p, P, float2(.5f), sampled_camera, raster, camera_direction))
        return;
    out.status = wire::success;
    for (uint strategy = 0; strategy < 3; ++strategy) {
        Vertex localC = C, localP = P, localE = E, sampled{};
        path_vertex(camera, 0) = C;
        path_vertex(camera, 1) = P;
        path_vertex(camera, 2) = E;
        float Cpdf = P.pdf_fwd, Spdf = E.pdf_fwd;
        const float Opdf = light_origin_pdf(r, E, P);
        float Lpdf = light_direction_pdf(r, E, P);
        if (strategy == 1) {
            sampled = E;
            sampled.path_id = 0xffffffffu;
            sampled.pdf_fwd = Opdf;
            localE = sampled;
            Spdf = vertex_pdf(r, P, C, sampled, Mode::Radiance);
        } else if (strategy == 2) {
            localE.path_id = 0x80000000u;
            localE.pdf_fwd = Opdf;
            localP.path_id = 0x80000001u;
            localP.wo = -E.point.geometric_normal;
            localP.sampled_wo_local = P.frame.local(localP.wo);
            localP.sampled_wi_local = P.frame.local(camera_direction);
            localP.sampled_prev_id = localE.path_id;
            localP.sampled_next_id = 0x80000002u;
            Lpdf = light_direction_pdf(r, localE, localP);
            localP.pdf_fwd = Lpdf;
            path_vertex(light, 0) = localE;
            path_vertex(light, 1) = localP;
            sampled = sampled_camera;
            localC = sampled;
            Cpdf = vertex_pdf(r, sampled, Vertex{}, localP, Mode::Radiance);
            Spdf = vertex_pdf(r, localP, sampled, localE, Mode::Radiance);
        }
        device auto &record = out.strategy[strategy];
        record.s = strategy;
        record.t = 3 - strategy;
        record.C = Cpdf;
        record.S = Spdf;
        record.O = Opdf;
        record.L = Lpdf;
        record.weight = mis_weight(r, light, camera, sampled, strategy, 3 - strategy);
        float3 wo, wi;
        record.expected_S = expected_density(r, localP, localC, localE, Mode::Radiance, wo, wi);
        point(record.canonical_wo, wo);
        point(record.canonical_wi, wi);
        record.ids[0] = localC.path_id;
        record.ids[1] = localP.path_id;
        record.ids[2] = localE.path_id;
        record.delta[0] = localC.delta;
        record.delta[1] = localP.delta;
        record.delta[2] = localE.delta;
        record.status = all(isfinite(float4(Cpdf, Spdf, Opdf, Lpdf))) &&
                                min(min(Cpdf, Spdf), min(Opdf, Lpdf)) > 0 &&
                                isfinite(record.weight) && record.weight >= 0 && record.weight <= 1
                            ? wire::success
                            : wire::invalid_arithmetic;
    }
    // Observe the ACTUAL stored vertices after every MIS strategy/private copy.
    // Early authored rho alone cannot detect the retained materialization failure.
    protect(out.stored_camera, path_vertex(camera, 1));
    protect(out.stored_light, path_vertex(light, 1));
}

// Kept separate so an old-source negative-control TU can exclude this entrypoint
// while compiling and dispatching the unchanged partition entrypoint above.
kernel void rypath_probe_canonical_finalizer(
    const device rypath::light_geometry_probe::CanonicalQuery *queries [[buffer(0)]],
    constant wire::Parameters *parameters [[buffer(1)]],
    device rypath::light_geometry_probe::CanonicalFinalizer *results [[buffer(2)]],
    device uchar *scratch [[buffer(3)]], uint index [[thread_position_in_grid]]) {
    using namespace canonical_mis_probe;
    if (index >= probe::canonical_helper_rows)
        return;
    device auto &out = results[index];
    out = {};
    const uint context_index = index < 8 ? index : 0;
    const uint control = index < 8 ? 0 : index - 7;
    out.magic = 0x434d4631u;
    out.index = index;
    out.context = context_index;
    out.control = control;
    out.vertex_bytes = sizeof(Vertex);
    out.vertex_alignment = alignof(Vertex);
    out.renderer = rypath::checkpoint_contract::renderer;
    out.status = wire::invalid_arithmetic;
    const auto q = queries[context_index];
    constant auto &p = parameters[context_index];
    Resources r{};
    r.alpha.parameters = &p; // Constructor compile admission pending; no tracing.
    Vertex C{}, P{}, E{};
    if (!context(q, p, C, P, E))
        return;
    const auto joint =
        rypath_packet::sample(P.bsdf, P.sampled_wo_local, .8f, float2(.3f, .7f), 0, Mode::Radiance);
    point(out.joint.wi, joint.wi);
    out.joint.pdf = joint.pdf;
    out.joint.reverse_pdf = joint.reverse_pdf;
    out.joint.eta = joint.eta;
    out.joint.valid = joint.valid;
    out.joint.accepted = joint.accepted;
    out.joint.delta = joint.delta;
    for (uint lane = 0; lane < 4; ++lane)
        out.joint.weight[lane] = joint.weight[lane];
    out.status = wire::success;
    if (!joint.valid || !joint.accepted)
        return;
    P.sampled_wi_local = joint.wi;
    P.delta = joint.delta;
    const auto evaluation =
        rypath_packet::evaluate(P.bsdf, P.sampled_wo_local, joint.wi, 0, Mode::Radiance);
    out.evaluation_valid = evaluation.valid;
    for (uint lane = 0; lane < 4; ++lane)
        out.f_cos_over_proxy[lane] = evaluation.f[lane] * abs(joint.wi.z) / joint.pdf;
    Vertex next = E;
    const auto direction = normalize(P.frame.world(joint.wi));
    next.point.position = direction;
    next.point.geometric_normal = next.point.shading_normal = -direction;
    next.beta = joint.weight;
    uint count = 2;
    Mode mode = Mode::Radiance;
    if (control == 1) {
        next.type = surface_vertex;
        next.point.position = 2 * direction;
        mode = Mode::Importance;
    } else if (control == 2) {
        count = 1;
        next = P;
        next.beta = joint.weight;
    } else if (control == 3) {
        // Constructed bookkeeping boundary; never claim this smooth sample is delta.
        P.delta = true;
    }
    auto path = scratch + index * probe::canonical_scratch_slots * storage::vertex_bytes;
    path_vertex(path, 0) = C;
    path_vertex(path, 1) = P;
    path_vertex(path, 0).pdf_rev = -3;
    next.pdf_fwd = -2;
    const Vertex middle = path_vertex(path, count - 1);
    protect(out.before, middle);
    out.count = count;
    out.mode = mode == Mode::Importance ? 1 : 0;
    out.prior_forward = next.pdf_fwd;
    out.prior_reverse = path_vertex(path, 0).pdf_rev;
    // Real public vertex_pdf calls define boundary expectations; no mirrored
    // recurrence or copied area-conversion arithmetic.
    out.expected_forward =
        middle.delta ? 0 : vertex_pdf(r, middle, count > 1 ? C : Vertex{}, next, mode);
    out.expected_reverse = count == 1 ? out.prior_reverse
                           : middle.delta
                               ? 0
                               : vertex_pdf(r, middle, next, C, rypath_packet::opposite(mode));
    out.called = 1;
    out.valid = finalize_walk_densities(r, path, count, next, mode);
    out.forward = next.pdf_fwd;
    out.reverse = path_vertex(path, 0).pdf_rev;
    protect(out.after, path_vertex(path, count - 1));
    for (uint lane = 0; lane < 4; ++lane)
        out.next_beta[lane] = next.beta[lane];
}

// Original full retention reproducer: all executed body statements retained.
// SPDX-License-Identifier: GPL-3.0-only
// Appended to an owned snapshot of the WHOLE production render.metal TU.
// Uses the isolated canonical-MIS transport/walk overlay; joint sampling/f unchanged.
namespace strategy_context21 {
using namespace rypath_bdpt;
inline void put(device Word *out, uint i, float x) { out[i] = as_type<uint>(x); }
inline void vector3(device Word *out, uint i, float3 x) {
    for (uint a = 0; a < 3; ++a)
        put(out, i + a, x[a]);
}
inline void vector4(device Word *out, uint i, float4 x) {
    for (uint a = 0; a < 4; ++a)
        put(out, i + a, x[a]);
}
inline rypath_packet::Prepared coating(uint material_seed, bool zero) {
    rypath_packet::Prepared prepared{};
    prepared.active_mask = 15;
    const float rho[4] = {.13526731729507446f, .08804553747177124f, .19378170371055603f,
                          .16280370950698853f};
    for (uint lane = 0; lane < 4; ++lane) {
        thread auto &p = prepared.lane[lane];
        p.kind = 5;
        p.wavelength = 400 + 100 * lane;
        thread auto &l = p.layered;
        rypath_dielectric::prepare(1.5f, .10201960802078247f, .10201960802078247f, false,
                                   l.interface);
        l.conductor_base = false;
        l.reflectance = zero ? 0 : rho[lane];
        l.thickness = .009999999776482582f;
        l.albedo = 0;
        l.g = 0;
        l.max_depth = 10;
        l.samples = 1;
        l.material_seed = int(material_seed);
    }
    return prepared;
}
} // namespace strategy_context21
kernel void strategy_context21_probe(
    const device strategy_context21::Query *queries [[buffer(0)]],
    constant wire::Parameters *parameters [[buffer(1)]], const device float *sky [[buffer(2)]],
    const device uint *metadata [[buffer(3)]], const device sobol::Tables &tables [[buffer(4)]],
    device uchar *scratch [[buffer(5)]], device strategy_context21::Result *results [[buffer(6)]],
    instance_acceleration_structure scene [[buffer(7)]], uint tid [[thread_position_in_grid]]) {
    using namespace strategy_context21;
    using namespace rypath_bdpt;
    if (tid >= cases)
        return;
    device auto *out = results[tid].words;
    for (uint i = 0; i < result_words; ++i)
        out[i] = 0;
    out[0] = magic;
    out[1] = tid;
    out[2] = 1;
    out[3] = 1;
    const auto q = queries[tid];
    const constant auto &p = parameters[tid];
    out[4] = q.frame_id;
    out[5] = q.angle_id;
    out[6] = q.zero_base;
    out[7] = q.material_seed;
    out[8] = sizeof(Vertex);
    out[9] = alignof(Vertex);
    out[10] = sizeof(wire::Parameters);
    out[11] = rypath::checkpoint_contract::renderer;
    if (p.instance_count != 0 || p.light_count != 1 || p.light_sampler != 2 || p.emitter_count ||
        p.positional_count || p.distant_count || p.environment_light != 0 || p.lens_radius != 0 ||
        p.bidirectional != 1 || p.sampler_kind != wire::independent_sampler ||
        p.material_seed != int(q.material_seed))
        return;
    wire::Sample sample{};
    sample.pixel = 0;
    sample.sample_low = tid;
    sample.status = wire::success;
    SurfaceAlpha alpha{};
    alpha.parameters = &p;
    alpha.material_data = metadata;
    alpha.sample = &sample;
    // Only uniform analytic environment, camera and Prepared coating operations
    // are admitted. Disabled graph/image/geometry paths must not dereference
    // their intentionally absent auxiliary buffers.
    Resources r{alpha, nullptr, nullptr, nullptr, sky, nullptr, {}, &tables, scene, scene};
    ScatteringFrame frame{float3(q.frame[0], q.frame[1], q.frame[2]),
                          float3(q.frame[3], q.frame[4], q.frame[5]),
                          float3(q.frame[6], q.frame[7], q.frame[8])};
    Vertex C{}, P{}, E{};
    C.type = camera_vertex;
    C.path_id = 0;
    C.connectible = true;
    C.point.position = unpack(p.origin);
    C.point.geometric_normal = unpack(p.camera_normal);
    C.point.shading_normal = C.point.geometric_normal;
    C.beta = 1;
    P.type = surface_vertex;
    P.path_id = 1;
    P.connectible = true;
    P.seed = 0x1234u;
    P.point.position = 0;
    P.point.geometric_normal = frame.z;
    P.point.shading_normal = frame.z;
    P.point.winding_normal = frame.z;
    P.frame = frame;
    P.bsdf = coating(q.material_seed, q.zero_base != 0);
    P.wo = normalize(C.point.position);
    P.beta = 1;
    const float3 e_direction = normalize(frame.world(normalize(float3(q.wi[0], q.wi[1], q.wi[2]))));
    E.type = environment_vertex;
    E.path_id = 2;
    E.light = 0;
    E.connectible = true;
    E.point.position = P.point.position + e_direction;
    E.point.geometric_normal = -e_direction;
    E.point.shading_normal = -e_direction;
    E.wo = -e_direction;
    E.beta = 1;
    // A physically valid explicit path, with its original represented local
    // direction retained exactly as a sampled continuation would retain it.
    // It is NOT claimed to be an actual generated hidden coating walk.
    P.sampled_wo_local = frame.local(P.wo);
    P.sampled_wi_local = normalize(float3(q.wi[0], q.wi[1], q.wi[2]));
    P.sampled_prev_id = C.path_id;
    P.sampled_next_id = E.path_id;
    P.pair_valid = true;
    P.pdf_fwd = vertex_pdf(r, C, Vertex{}, P, Mode::Radiance);
    E.pdf_fwd = vertex_pdf(r, P, C, E, Mode::Radiance);
    if (!all(isfinite(e_direction)) || P.sampled_wo_local.z <= 0 || P.sampled_wi_local.z <= 0 ||
        !(P.pdf_fwd > 0) || !(E.pdf_fwd > 0))
        return;
    auto camera = scratch + tid * 5 * storage::vertex_bytes;
    auto light = camera + 3 * storage::vertex_bytes;
    path_vertex(camera, 0) = C;
    path_vertex(camera, 1) = P;
    path_vertex(camera, 2) = E;
    Vertex sampled_camera{};
    float2 raster;
    float3 camera_direction;
    if (!camera_sample(p, P, float2(.5f), sampled_camera, raster, camera_direction))
        return;
    vector3(out, 24, P.sampled_wo_local);
    vector3(out, 27, P.sampled_wi_local);
    vector3(out, 30, P.wo);
    vector3(out, 33, e_direction);
    for (uint a = 0; a < 9; ++a)
        put(out, 36 + a, q.frame[a]);
    vector3(out, 45, C.point.position);
    put(out, 48, dot(C.point.position, C.point.position));
    vector3(out, 49, sampled_camera.point.position);
    put(out, 52, sampled_camera.beta[0]);
    vector4(out, 16, f(P, e_direction, Mode::Radiance));
    vector4(out, 20, importance_f_cos(P, camera_direction));
    const float q_scatter = E.pdf_fwd;
    const float q_light = direct_pdf(r, P, E);
    put(out, 12, q_scatter);
    put(out, 13, q_light);
    put(out, 14, power_weight(q_scatter, q_light));
    put(out, 15, power_weight(q_light, q_scatter));
    Wavelengths lambda{};
    for (uint lane = 0; lane < 4; ++lane) {
        lambda.lambda[lane] = 400 + 100 * lane;
        lambda.pdf[lane] = 1;
    }
    // These generated controls use the ACTUAL sampled hidden-path ratio.
    // Rejections stay zero and are retained, never replaced by f*cos/proxy.
    const auto joint =
        rypath_packet::sample(P.bsdf, P.sampled_wo_local, .8f, float2(.3f, .7f), 0, Mode::Radiance);
    vector4(out, 53, joint.weight);
    put(out, 57, joint.pdf);
    out[58] = joint.accepted;
    out[59] = joint.valid;
    vector3(out, 60, joint.wi);
    put(out, 63, joint.reverse_pdf);
    if (joint.accepted) {
        auto evaluated =
            rypath_packet::evaluate(P.bsdf, P.sampled_wo_local, joint.wi, 0, Mode::Radiance);
        vector4(out, 64, evaluated.f * (abs(joint.wi.z) / joint.pdf));
    }
    Vertex J = E;
    J.beta = joint.accepted ? joint.weight : float4(0);
    if (joint.accepted) {
        auto world = normalize(frame.world(joint.wi));
        J.point.position = world;
        J.point.geometric_normal = -world;
        J.point.shading_normal = -world;
        J.wo = -world;
        J.pdf_fwd = joint.pdf; // overwritten below by the actual deferred-density helper
    }
    Vertex jointP = P;
    if (joint.accepted)
        jointP.sampled_wi_local = joint.wi;
    jointP.delta = joint.accepted && joint.delta;
    path_vertex(camera, 1) = jointP;
    if (!finalize_walk_densities(r, camera, 2, J, Mode::Radiance))
        return;
    vector4(out, 109, J.beta);
    put(out, 113, J.pdf_fwd);
    put(out, 114, jointP.delta ? 0 : vertex_pdf(r, jointP, C, J, Mode::Radiance));
    put(out, 115, path_vertex(camera, 0).pdf_rev);
    put(out, 116, jointP.delta ? 0 : vertex_pdf(r, jointP, J, C, Mode::Importance));
    out[117] = jointP.delta;
    path_vertex(camera, 2) = J;
    sample.status = wire::success;
    vector4(out, 68, camera_connection(r, sample, lambda, tid, light, camera, 0, 3));
    out[72] = sample.status;
    path_vertex(camera, 1) = P;
    path_vertex(camera, 2) = E;
    sample.status = wire::success;
    vector4(out, 73, camera_connection(r, sample, lambda, tid, light, camera, 1, 2));
    out[77] = sample.status;
    // Deterministically repeat the same sample_li to OBSERVE its returned endpoint;
    // this is a diagnostic query, not a mutation of the camera_connection above.
    Vertex nee{};
    float3 nee_direction(0);
    float nee_pdf = 0;
    const auto draws = uniforms4(p, random(r, sample, tid, 133));
    out[82] = sample_li(r, P, lambda, draws, nee, nee_direction, nee_pdf);
    vector3(out, 78, nee_direction);
    put(out, 81, nee_pdf);
    vector4(out, 83, f(P, nee_direction, Mode::Radiance));
    put(out, 87,
        rypath_packet::pdf(P.bsdf, P.sampled_wo_local, frame.local(nee_direction), Mode::Radiance));
    vector4(out, 88, nee.beta);
    for (uint a = 0; a < 4; ++a)
        put(out, 92 + a, sky[a]);
    out[96] = C.path_id;
    out[97] = P.path_id;
    out[98] = E.path_id;
    out[99] = nee.path_id;
    for (uint a = 0; a < 4; ++a)
        put(out, 100 + a, P.bsdf.lane[a].layered.reflectance);
    out[104] = P.seed;
    out[105] = sample.pixel;
    out[106] = sample.sample_low;
    out[107] = sample.sample_high;
    out[108] = p.environment_light;
    for (uint strategy = 0; strategy < 3; ++strategy) {
        device auto *record = out + summary_words + strategy * strategy_words;
        record[0] = strategy;
        record[1] = 3 - strategy;
        record[2] = 1;
        Vertex localC = C, localP = P, localE = E, sampled{};
        path_vertex(camera, 0) = C;
        path_vertex(camera, 1) = P;
        path_vertex(camera, 2) = E;
        float Cpdf = P.pdf_fwd, Spdf = E.pdf_fwd;
        const float Opdf = light_origin_pdf(r, E, P);
        float Lpdf = light_direction_pdf(r, E, P);
        if (strategy == 1) {
            sampled = E;
            sampled.path_id = 0xffffffffu;
            sampled.pdf_fwd = Opdf;
            localE = sampled;
            Spdf = vertex_pdf(r, P, C, sampled, Mode::Radiance);
        } else if (strategy == 2) {
            localE.path_id = 0x80000000u;
            localE.pdf_fwd = Opdf;
            localP.path_id = 0x80000001u;
            localP.wo = e_direction;
            localP.sampled_wo_local = frame.local(e_direction);
            localP.sampled_wi_local = frame.local(camera_direction);
            localP.sampled_prev_id = localE.path_id;
            localP.sampled_next_id = 0x80000002u;
            Lpdf = (1 / (pi * p.scene_radius * p.scene_radius)) *
                   abs(dot(-E.point.geometric_normal, P.point.geometric_normal));
            localP.pdf_fwd = Lpdf;
            path_vertex(light, 0) = localE;
            path_vertex(light, 1) = localP;
            sampled = sampled_camera;
            localC = sampled;
            Cpdf = vertex_pdf(r, sampled, Vertex{}, localP, Mode::Radiance);
            Spdf = vertex_pdf(r, localP, sampled, localE, Mode::Radiance);
        }
        const float actual = mis_weight(r, light, camera, sampled, strategy, 3 - strategy);
        put(record, 4, actual);
        put(record, 5, Cpdf);
        put(record, 6, Spdf);
        put(record, 7, Opdf);
        put(record, 8, Lpdf);
        const auto pairC =
            localP.frame.local(normalize(localC.point.position - localP.point.position));
        const auto pairE = localP.frame.local(normalize(-localE.point.geometric_normal));
        vector3(record, 9, pairC);
        vector3(record, 12, pairE);
        vector3(record, 15, localP.pair_valid ? localP.sampled_wo_local : frame.local(localP.wo));
        vector3(record, 18, frame.local(camera_direction));
        record[21] = localC.path_id;
        record[22] = localP.path_id;
        record[23] = localE.path_id;
        record[24] = localP.seed;
        record[25] = localP.sampled_prev_id;
        record[26] = localP.sampled_next_id;
        record[27] = localP.pair_valid;
        vector4(record, 28,
                f(localP, strategy == 2 ? camera_direction : e_direction,
                  strategy == 2 ? Mode::Importance : Mode::Radiance));
        vector4(record, 32, importance_f_cos(localP, camera_direction));
        put(record, 40, localP.pdf_fwd);
        put(record, 41, localE.pdf_fwd);
        put(record, 42, localP.pdf_rev);
        put(record, 43, localE.pdf_rev);
        vector3(record, 44, pairC);
        vector3(record, 47, pairE);
        const auto forward = rypath_packet::pdf(localP.bsdf, pairC, pairE, Mode::Radiance);
        const auto reverse = rypath_packet::pdf(localP.bsdf, pairE, pairC, Mode::Importance);
        put(record, 50, forward);
        put(record, 51, reverse);
        put(record, 52, vertex_pdf(r, localP, localC, localE, Mode::Radiance));
        put(record, 53, vertex_pdf(r, localP, localE, localC, Mode::Importance));
        record[54] = sampled.path_id;
        record[55] = localP.seed;
        record[56] = localP.delta;
        record[57] = localE.delta;
        record[58] = sample.status;
        if (isfinite(actual) && actual >= 0 && actual <= 1 &&
            all(isfinite(float4(Cpdf, Spdf, Opdf, Lpdf))) &&
            all(float4(Cpdf, Spdf, Opdf, Lpdf) > 0))
            record[2] = 0;
    }
    bool valid = joint.valid && out[72] == wire::success && out[77] == wire::success && out[82];
    for (uint strategy = 0; strategy < 3; ++strategy)
        valid = valid && out[summary_words + strategy * strategy_words + 2] == 0;
    out[2] = valid ? 0 : 2;
}

// Separate post-completion readback; does not modify or aggregate-copy scratch.
kernel void rypath_probe_canonical_full_readback(const device uchar *scratch [[buffer(0)]],
                                                 device strategy_context21::SemanticVertex *results
                                                 [[buffer(1)]],
                                                 uint tid [[thread_position_in_grid]]) {
    using namespace strategy_context21;
    if (tid >= cases * 5)
        return;
    device auto &out = results[tid];
    out.magic = readback_magic;
    out.context = tid / 5;
    out.slot = tid % 5;
    out.field_count = vertex_fields;
    out.vertex_bytes = sizeof(rypath_bdpt::Vertex);
    out.vertex_alignment = alignof(rypath_bdpt::Vertex);
    const device auto &v = *reinterpret_cast<const device rypath_bdpt::Vertex *>(
        scratch + tid * rypath::packet_wire::vertex_bytes);
    out.fields[0] = as_type<uint>(v.point.position[0]);                // point.position.0
    out.fields[1] = as_type<uint>(v.point.position[1]);                // point.position.1
    out.fields[2] = as_type<uint>(v.point.position[2]);                // point.position.2
    out.fields[3] = as_type<uint>(v.point.error[0]);                   // point.error.0
    out.fields[4] = as_type<uint>(v.point.error[1]);                   // point.error.1
    out.fields[5] = as_type<uint>(v.point.error[2]);                   // point.error.2
    out.fields[6] = as_type<uint>(v.point.geometric_normal[0]);        // point.geometric_normal.0
    out.fields[7] = as_type<uint>(v.point.geometric_normal[1]);        // point.geometric_normal.1
    out.fields[8] = as_type<uint>(v.point.geometric_normal[2]);        // point.geometric_normal.2
    out.fields[9] = as_type<uint>(v.point.winding_normal[0]);          // point.winding_normal.0
    out.fields[10] = as_type<uint>(v.point.winding_normal[1]);         // point.winding_normal.1
    out.fields[11] = as_type<uint>(v.point.winding_normal[2]);         // point.winding_normal.2
    out.fields[12] = as_type<uint>(v.point.shading_normal[0]);         // point.shading_normal.0
    out.fields[13] = as_type<uint>(v.point.shading_normal[1]);         // point.shading_normal.1
    out.fields[14] = as_type<uint>(v.point.shading_normal[2]);         // point.shading_normal.2
    out.fields[15] = as_type<uint>(v.point.inverse_area_pdf);          // point.inverse_area_pdf
    out.fields[16] = as_type<uint>(v.point.world_extent);              // point.world_extent
    out.fields[17] = uint(v.point.patch);                              // point.patch
    out.fields[18] = as_type<uint>(v.point.analytic_position[0]);      // point.analytic_position.0
    out.fields[19] = as_type<uint>(v.point.analytic_position[1]);      // point.analytic_position.1
    out.fields[20] = as_type<uint>(v.point.analytic_position[2]);      // point.analytic_position.2
    out.fields[21] = as_type<uint>(v.point.analytic_normal[0]);        // point.analytic_normal.0
    out.fields[22] = as_type<uint>(v.point.analytic_normal[1]);        // point.analytic_normal.1
    out.fields[23] = as_type<uint>(v.point.analytic_normal[2]);        // point.analytic_normal.2
    out.fields[24] = as_type<uint>(v.point.analytic_dpdu[0]);          // point.analytic_dpdu.0
    out.fields[25] = as_type<uint>(v.point.analytic_dpdu[1]);          // point.analytic_dpdu.1
    out.fields[26] = as_type<uint>(v.point.analytic_dpdu[2]);          // point.analytic_dpdu.2
    out.fields[27] = as_type<uint>(v.point.analytic_dpdv[0]);          // point.analytic_dpdv.0
    out.fields[28] = as_type<uint>(v.point.analytic_dpdv[1]);          // point.analytic_dpdv.1
    out.fields[29] = as_type<uint>(v.point.analytic_dpdv[2]);          // point.analytic_dpdv.2
    out.fields[30] = as_type<uint>(v.point.analytic_dndu[0]);          // point.analytic_dndu.0
    out.fields[31] = as_type<uint>(v.point.analytic_dndu[1]);          // point.analytic_dndu.1
    out.fields[32] = as_type<uint>(v.point.analytic_dndu[2]);          // point.analytic_dndu.2
    out.fields[33] = as_type<uint>(v.point.analytic_dndv[0]);          // point.analytic_dndv.0
    out.fields[34] = as_type<uint>(v.point.analytic_dndv[1]);          // point.analytic_dndv.1
    out.fields[35] = as_type<uint>(v.point.analytic_dndv[2]);          // point.analytic_dndv.2
    out.fields[36] = as_type<uint>(v.frame.x[0]);                      // frame.x.0
    out.fields[37] = as_type<uint>(v.frame.x[1]);                      // frame.x.1
    out.fields[38] = as_type<uint>(v.frame.x[2]);                      // frame.x.2
    out.fields[39] = as_type<uint>(v.frame.y[0]);                      // frame.y.0
    out.fields[40] = as_type<uint>(v.frame.y[1]);                      // frame.y.1
    out.fields[41] = as_type<uint>(v.frame.y[2]);                      // frame.y.2
    out.fields[42] = as_type<uint>(v.frame.z[0]);                      // frame.z.0
    out.fields[43] = as_type<uint>(v.frame.z[1]);                      // frame.z.1
    out.fields[44] = as_type<uint>(v.frame.z[2]);                      // frame.z.2
    out.fields[45] = uint(v.bsdf.lane[0].kind);                        // lane0.kind
    out.fields[46] = as_type<uint>(v.bsdf.lane[0].reflectance);        // lane0.reflectance
    out.fields[47] = as_type<uint>(v.bsdf.lane[0].transmittance);      // lane0.transmittance
    out.fields[48] = as_type<uint>(v.bsdf.lane[0].wavelength);         // lane0.wavelength
    out.fields[49] = as_type<uint>(v.bsdf.lane[0].conductor.eta);      // lane0.conductor.eta
    out.fields[50] = as_type<uint>(v.bsdf.lane[0].conductor.k);        // lane0.conductor.k
    out.fields[51] = as_type<uint>(v.bsdf.lane[0].conductor.alpha_x);  // lane0.conductor.alpha_x
    out.fields[52] = as_type<uint>(v.bsdf.lane[0].conductor.alpha_y);  // lane0.conductor.alpha_y
    out.fields[53] = uint(v.bsdf.lane[0].conductor.smooth);            // lane0.conductor.smooth
    out.fields[54] = as_type<uint>(v.bsdf.lane[0].dielectric.eta);     // lane0.dielectric.eta
    out.fields[55] = as_type<uint>(v.bsdf.lane[0].dielectric.alpha_x); // lane0.dielectric.alpha_x
    out.fields[56] = as_type<uint>(v.bsdf.lane[0].dielectric.alpha_y); // lane0.dielectric.alpha_y
    out.fields[57] = uint(v.bsdf.lane[0].dielectric.smooth);           // lane0.dielectric.smooth
    out.fields[58] =
        as_type<uint>(v.bsdf.lane[0].layered.interface.eta); // lane0.layered.interface.eta
    out.fields[59] =
        as_type<uint>(v.bsdf.lane[0].layered.interface.alpha_x); // lane0.layered.interface.alpha_x
    out.fields[60] =
        as_type<uint>(v.bsdf.lane[0].layered.interface.alpha_y); // lane0.layered.interface.alpha_y
    out.fields[61] =
        uint(v.bsdf.lane[0].layered.interface.smooth); // lane0.layered.interface.smooth
    out.fields[62] =
        as_type<uint>(v.bsdf.lane[0].layered.conductor.eta); // lane0.layered.conductor.eta
    out.fields[63] = as_type<uint>(v.bsdf.lane[0].layered.conductor.k); // lane0.layered.conductor.k
    out.fields[64] =
        as_type<uint>(v.bsdf.lane[0].layered.conductor.alpha_x); // lane0.layered.conductor.alpha_x
    out.fields[65] =
        as_type<uint>(v.bsdf.lane[0].layered.conductor.alpha_y); // lane0.layered.conductor.alpha_y
    out.fields[66] =
        uint(v.bsdf.lane[0].layered.conductor.smooth);            // lane0.layered.conductor.smooth
    out.fields[67] = uint(v.bsdf.lane[0].layered.conductor_base); // lane0.layered.conductor_base
    out.fields[68] = as_type<uint>(v.bsdf.lane[0].layered.reflectance); // lane0.layered.reflectance
    out.fields[69] = as_type<uint>(v.bsdf.lane[0].layered.thickness);   // lane0.layered.thickness
    out.fields[70] = as_type<uint>(v.bsdf.lane[0].layered.albedo);      // lane0.layered.albedo
    out.fields[71] = as_type<uint>(v.bsdf.lane[0].layered.g);           // lane0.layered.g
    out.fields[72] = uint(v.bsdf.lane[0].layered.max_depth);            // lane0.layered.max_depth
    out.fields[73] = uint(v.bsdf.lane[0].layered.samples);              // lane0.layered.samples
    out.fields[74] = uint(v.bsdf.lane[0].layered.material_seed);  // lane0.layered.material_seed
    out.fields[75] = as_type<uint>(v.bsdf.lane[0].gltf.color);    // lane0.gltf.color
    out.fields[76] = as_type<uint>(v.bsdf.lane[0].gltf.metallic); // lane0.gltf.metallic
    out.fields[77] = as_type<uint>(v.bsdf.lane[0].gltf.alpha);    // lane0.gltf.alpha
    out.fields[78] =
        as_type<uint>(v.bsdf.lane[0].gltf.specular_probability);  // lane0.gltf.specular_probability
    out.fields[79] = uint(v.bsdf.lane[0].table != nullptr);       // lane0.table_nonnull
    out.fields[80] = uint(v.bsdf.lane[0].rgb_table != nullptr);   // lane0.rgb_table_nonnull
    out.fields[81] = uint(v.bsdf.lane[1].kind);                   // lane1.kind
    out.fields[82] = as_type<uint>(v.bsdf.lane[1].reflectance);   // lane1.reflectance
    out.fields[83] = as_type<uint>(v.bsdf.lane[1].transmittance); // lane1.transmittance
    out.fields[84] = as_type<uint>(v.bsdf.lane[1].wavelength);    // lane1.wavelength
    out.fields[85] = as_type<uint>(v.bsdf.lane[1].conductor.eta); // lane1.conductor.eta
    out.fields[86] = as_type<uint>(v.bsdf.lane[1].conductor.k);   // lane1.conductor.k
    out.fields[87] = as_type<uint>(v.bsdf.lane[1].conductor.alpha_x);  // lane1.conductor.alpha_x
    out.fields[88] = as_type<uint>(v.bsdf.lane[1].conductor.alpha_y);  // lane1.conductor.alpha_y
    out.fields[89] = uint(v.bsdf.lane[1].conductor.smooth);            // lane1.conductor.smooth
    out.fields[90] = as_type<uint>(v.bsdf.lane[1].dielectric.eta);     // lane1.dielectric.eta
    out.fields[91] = as_type<uint>(v.bsdf.lane[1].dielectric.alpha_x); // lane1.dielectric.alpha_x
    out.fields[92] = as_type<uint>(v.bsdf.lane[1].dielectric.alpha_y); // lane1.dielectric.alpha_y
    out.fields[93] = uint(v.bsdf.lane[1].dielectric.smooth);           // lane1.dielectric.smooth
    out.fields[94] =
        as_type<uint>(v.bsdf.lane[1].layered.interface.eta); // lane1.layered.interface.eta
    out.fields[95] =
        as_type<uint>(v.bsdf.lane[1].layered.interface.alpha_x); // lane1.layered.interface.alpha_x
    out.fields[96] =
        as_type<uint>(v.bsdf.lane[1].layered.interface.alpha_y); // lane1.layered.interface.alpha_y
    out.fields[97] =
        uint(v.bsdf.lane[1].layered.interface.smooth); // lane1.layered.interface.smooth
    out.fields[98] =
        as_type<uint>(v.bsdf.lane[1].layered.conductor.eta); // lane1.layered.conductor.eta
    out.fields[99] = as_type<uint>(v.bsdf.lane[1].layered.conductor.k); // lane1.layered.conductor.k
    out.fields[100] =
        as_type<uint>(v.bsdf.lane[1].layered.conductor.alpha_x); // lane1.layered.conductor.alpha_x
    out.fields[101] =
        as_type<uint>(v.bsdf.lane[1].layered.conductor.alpha_y); // lane1.layered.conductor.alpha_y
    out.fields[102] =
        uint(v.bsdf.lane[1].layered.conductor.smooth);             // lane1.layered.conductor.smooth
    out.fields[103] = uint(v.bsdf.lane[1].layered.conductor_base); // lane1.layered.conductor_base
    out.fields[104] =
        as_type<uint>(v.bsdf.lane[1].layered.reflectance);             // lane1.layered.reflectance
    out.fields[105] = as_type<uint>(v.bsdf.lane[1].layered.thickness); // lane1.layered.thickness
    out.fields[106] = as_type<uint>(v.bsdf.lane[1].layered.albedo);    // lane1.layered.albedo
    out.fields[107] = as_type<uint>(v.bsdf.lane[1].layered.g);         // lane1.layered.g
    out.fields[108] = uint(v.bsdf.lane[1].layered.max_depth);          // lane1.layered.max_depth
    out.fields[109] = uint(v.bsdf.lane[1].layered.samples);            // lane1.layered.samples
    out.fields[110] = uint(v.bsdf.lane[1].layered.material_seed);  // lane1.layered.material_seed
    out.fields[111] = as_type<uint>(v.bsdf.lane[1].gltf.color);    // lane1.gltf.color
    out.fields[112] = as_type<uint>(v.bsdf.lane[1].gltf.metallic); // lane1.gltf.metallic
    out.fields[113] = as_type<uint>(v.bsdf.lane[1].gltf.alpha);    // lane1.gltf.alpha
    out.fields[114] =
        as_type<uint>(v.bsdf.lane[1].gltf.specular_probability); // lane1.gltf.specular_probability
    out.fields[115] = uint(v.bsdf.lane[1].table != nullptr);     // lane1.table_nonnull
    out.fields[116] = uint(v.bsdf.lane[1].rgb_table != nullptr); // lane1.rgb_table_nonnull
    out.fields[117] = uint(v.bsdf.lane[2].kind);                 // lane2.kind
    out.fields[118] = as_type<uint>(v.bsdf.lane[2].reflectance); // lane2.reflectance
    out.fields[119] = as_type<uint>(v.bsdf.lane[2].transmittance);      // lane2.transmittance
    out.fields[120] = as_type<uint>(v.bsdf.lane[2].wavelength);         // lane2.wavelength
    out.fields[121] = as_type<uint>(v.bsdf.lane[2].conductor.eta);      // lane2.conductor.eta
    out.fields[122] = as_type<uint>(v.bsdf.lane[2].conductor.k);        // lane2.conductor.k
    out.fields[123] = as_type<uint>(v.bsdf.lane[2].conductor.alpha_x);  // lane2.conductor.alpha_x
    out.fields[124] = as_type<uint>(v.bsdf.lane[2].conductor.alpha_y);  // lane2.conductor.alpha_y
    out.fields[125] = uint(v.bsdf.lane[2].conductor.smooth);            // lane2.conductor.smooth
    out.fields[126] = as_type<uint>(v.bsdf.lane[2].dielectric.eta);     // lane2.dielectric.eta
    out.fields[127] = as_type<uint>(v.bsdf.lane[2].dielectric.alpha_x); // lane2.dielectric.alpha_x
    out.fields[128] = as_type<uint>(v.bsdf.lane[2].dielectric.alpha_y); // lane2.dielectric.alpha_y
    out.fields[129] = uint(v.bsdf.lane[2].dielectric.smooth);           // lane2.dielectric.smooth
    out.fields[130] =
        as_type<uint>(v.bsdf.lane[2].layered.interface.eta); // lane2.layered.interface.eta
    out.fields[131] =
        as_type<uint>(v.bsdf.lane[2].layered.interface.alpha_x); // lane2.layered.interface.alpha_x
    out.fields[132] =
        as_type<uint>(v.bsdf.lane[2].layered.interface.alpha_y); // lane2.layered.interface.alpha_y
    out.fields[133] =
        uint(v.bsdf.lane[2].layered.interface.smooth); // lane2.layered.interface.smooth
    out.fields[134] =
        as_type<uint>(v.bsdf.lane[2].layered.conductor.eta); // lane2.layered.conductor.eta
    out.fields[135] =
        as_type<uint>(v.bsdf.lane[2].layered.conductor.k); // lane2.layered.conductor.k
    out.fields[136] =
        as_type<uint>(v.bsdf.lane[2].layered.conductor.alpha_x); // lane2.layered.conductor.alpha_x
    out.fields[137] =
        as_type<uint>(v.bsdf.lane[2].layered.conductor.alpha_y); // lane2.layered.conductor.alpha_y
    out.fields[138] =
        uint(v.bsdf.lane[2].layered.conductor.smooth);             // lane2.layered.conductor.smooth
    out.fields[139] = uint(v.bsdf.lane[2].layered.conductor_base); // lane2.layered.conductor_base
    out.fields[140] =
        as_type<uint>(v.bsdf.lane[2].layered.reflectance);             // lane2.layered.reflectance
    out.fields[141] = as_type<uint>(v.bsdf.lane[2].layered.thickness); // lane2.layered.thickness
    out.fields[142] = as_type<uint>(v.bsdf.lane[2].layered.albedo);    // lane2.layered.albedo
    out.fields[143] = as_type<uint>(v.bsdf.lane[2].layered.g);         // lane2.layered.g
    out.fields[144] = uint(v.bsdf.lane[2].layered.max_depth);          // lane2.layered.max_depth
    out.fields[145] = uint(v.bsdf.lane[2].layered.samples);            // lane2.layered.samples
    out.fields[146] = uint(v.bsdf.lane[2].layered.material_seed);  // lane2.layered.material_seed
    out.fields[147] = as_type<uint>(v.bsdf.lane[2].gltf.color);    // lane2.gltf.color
    out.fields[148] = as_type<uint>(v.bsdf.lane[2].gltf.metallic); // lane2.gltf.metallic
    out.fields[149] = as_type<uint>(v.bsdf.lane[2].gltf.alpha);    // lane2.gltf.alpha
    out.fields[150] =
        as_type<uint>(v.bsdf.lane[2].gltf.specular_probability); // lane2.gltf.specular_probability
    out.fields[151] = uint(v.bsdf.lane[2].table != nullptr);     // lane2.table_nonnull
    out.fields[152] = uint(v.bsdf.lane[2].rgb_table != nullptr); // lane2.rgb_table_nonnull
    out.fields[153] = uint(v.bsdf.lane[3].kind);                 // lane3.kind
    out.fields[154] = as_type<uint>(v.bsdf.lane[3].reflectance); // lane3.reflectance
    out.fields[155] = as_type<uint>(v.bsdf.lane[3].transmittance);      // lane3.transmittance
    out.fields[156] = as_type<uint>(v.bsdf.lane[3].wavelength);         // lane3.wavelength
    out.fields[157] = as_type<uint>(v.bsdf.lane[3].conductor.eta);      // lane3.conductor.eta
    out.fields[158] = as_type<uint>(v.bsdf.lane[3].conductor.k);        // lane3.conductor.k
    out.fields[159] = as_type<uint>(v.bsdf.lane[3].conductor.alpha_x);  // lane3.conductor.alpha_x
    out.fields[160] = as_type<uint>(v.bsdf.lane[3].conductor.alpha_y);  // lane3.conductor.alpha_y
    out.fields[161] = uint(v.bsdf.lane[3].conductor.smooth);            // lane3.conductor.smooth
    out.fields[162] = as_type<uint>(v.bsdf.lane[3].dielectric.eta);     // lane3.dielectric.eta
    out.fields[163] = as_type<uint>(v.bsdf.lane[3].dielectric.alpha_x); // lane3.dielectric.alpha_x
    out.fields[164] = as_type<uint>(v.bsdf.lane[3].dielectric.alpha_y); // lane3.dielectric.alpha_y
    out.fields[165] = uint(v.bsdf.lane[3].dielectric.smooth);           // lane3.dielectric.smooth
    out.fields[166] =
        as_type<uint>(v.bsdf.lane[3].layered.interface.eta); // lane3.layered.interface.eta
    out.fields[167] =
        as_type<uint>(v.bsdf.lane[3].layered.interface.alpha_x); // lane3.layered.interface.alpha_x
    out.fields[168] =
        as_type<uint>(v.bsdf.lane[3].layered.interface.alpha_y); // lane3.layered.interface.alpha_y
    out.fields[169] =
        uint(v.bsdf.lane[3].layered.interface.smooth); // lane3.layered.interface.smooth
    out.fields[170] =
        as_type<uint>(v.bsdf.lane[3].layered.conductor.eta); // lane3.layered.conductor.eta
    out.fields[171] =
        as_type<uint>(v.bsdf.lane[3].layered.conductor.k); // lane3.layered.conductor.k
    out.fields[172] =
        as_type<uint>(v.bsdf.lane[3].layered.conductor.alpha_x); // lane3.layered.conductor.alpha_x
    out.fields[173] =
        as_type<uint>(v.bsdf.lane[3].layered.conductor.alpha_y); // lane3.layered.conductor.alpha_y
    out.fields[174] =
        uint(v.bsdf.lane[3].layered.conductor.smooth);             // lane3.layered.conductor.smooth
    out.fields[175] = uint(v.bsdf.lane[3].layered.conductor_base); // lane3.layered.conductor_base
    out.fields[176] =
        as_type<uint>(v.bsdf.lane[3].layered.reflectance);             // lane3.layered.reflectance
    out.fields[177] = as_type<uint>(v.bsdf.lane[3].layered.thickness); // lane3.layered.thickness
    out.fields[178] = as_type<uint>(v.bsdf.lane[3].layered.albedo);    // lane3.layered.albedo
    out.fields[179] = as_type<uint>(v.bsdf.lane[3].layered.g);         // lane3.layered.g
    out.fields[180] = uint(v.bsdf.lane[3].layered.max_depth);          // lane3.layered.max_depth
    out.fields[181] = uint(v.bsdf.lane[3].layered.samples);            // lane3.layered.samples
    out.fields[182] = uint(v.bsdf.lane[3].layered.material_seed);  // lane3.layered.material_seed
    out.fields[183] = as_type<uint>(v.bsdf.lane[3].gltf.color);    // lane3.gltf.color
    out.fields[184] = as_type<uint>(v.bsdf.lane[3].gltf.metallic); // lane3.gltf.metallic
    out.fields[185] = as_type<uint>(v.bsdf.lane[3].gltf.alpha);    // lane3.gltf.alpha
    out.fields[186] =
        as_type<uint>(v.bsdf.lane[3].gltf.specular_probability); // lane3.gltf.specular_probability
    out.fields[187] = uint(v.bsdf.lane[3].table != nullptr);     // lane3.table_nonnull
    out.fields[188] = uint(v.bsdf.lane[3].rgb_table != nullptr); // lane3.rgb_table_nonnull
    out.fields[189] = uint(v.bsdf.active_mask);                  // active_mask
    out.fields[190] = as_type<uint>(v.beta[0]);                  // beta.0
    out.fields[191] = as_type<uint>(v.beta[1]);                  // beta.1
    out.fields[192] = as_type<uint>(v.beta[2]);                  // beta.2
    out.fields[193] = as_type<uint>(v.beta[3]);                  // beta.3
    out.fields[194] = as_type<uint>(v.wo[0]);                    // wo.0
    out.fields[195] = as_type<uint>(v.wo[1]);                    // wo.1
    out.fields[196] = as_type<uint>(v.wo[2]);                    // wo.2
    out.fields[197] = as_type<uint>(v.uv[0]);                    // uv.0
    out.fields[198] = as_type<uint>(v.uv[1]);                    // uv.1
    out.fields[199] = as_type<uint>(v.pdf_fwd);                  // pdf_fwd
    out.fields[200] = as_type<uint>(v.pdf_rev);                  // pdf_rev
    out.fields[201] = uint(v.type);                              // type
    out.fields[202] = uint(v.instance);                          // instance
    out.fields[203] = uint(v.primitive);                         // primitive
    out.fields[204] = uint(v.kind);                              // kind
    out.fields[205] = uint(v.light);                             // light
    out.fields[206] = uint(v.seed);                              // seed
    out.fields[207] = uint(v.path_id);                           // path_id
    out.fields[208] = uint(v.sampled_prev_id);                   // sampled_prev_id
    out.fields[209] = uint(v.sampled_next_id);                   // sampled_next_id
    out.fields[210] = uint(v.pair_valid);                        // pair_valid
    out.fields[211] = as_type<uint>(v.sampled_wo_local[0]);      // sampled_wo_local.0
    out.fields[212] = as_type<uint>(v.sampled_wo_local[1]);      // sampled_wo_local.1
    out.fields[213] = as_type<uint>(v.sampled_wo_local[2]);      // sampled_wo_local.2
    out.fields[214] = as_type<uint>(v.sampled_wi_local[0]);      // sampled_wi_local.0
    out.fields[215] = as_type<uint>(v.sampled_wi_local[1]);      // sampled_wi_local.1
    out.fields[216] = as_type<uint>(v.sampled_wi_local[2]);      // sampled_wi_local.2
    out.fields[217] = uint(v.delta);                             // delta
    out.fields[218] = uint(v.connectible);                       // connectible
    out.fields[219] = uint(v.delta_light);                       // delta_light
}
