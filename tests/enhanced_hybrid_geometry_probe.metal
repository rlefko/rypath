// SPDX-License-Identifier: GPL-3.0-only
#include "../shaders/render.metal"
#include "../src/enhanced_footprint_shared.h"
namespace rypath_bdpt {
#include "../shaders/enhanced_hybrid_geometry.h"
}
// Raw word schema is test-local and identical in the host:64-word query/result.
// Pure helper calls only: no Resources aggregate, AS query, random draw or trace.
kernel void rypath_enhanced_hybrid_geometry_probe(device const uint *queries [[buffer(0)]],
                                                  device uint *results [[buffer(1)]],
                                                  constant uint &count [[buffer(2)]],
                                                  uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    namespace g = rypath_bdpt::enhanced_hybrid_geometry;
    namespace fp = rypath_enhanced_footprint;
    using namespace rypath_bdpt;
    const auto q = queries + 64u * index;
    auto out = results + 64u * index;
    for (uint k = 0; k < 64; ++k)
        out[k] = 0;
    HeroFrozenBSDF material{};
    material.kind = q[1];
    material.active_mask = q[2];
    for (uint k = 0; k < 24; ++k)
        material.payload[k] = q[8 + k];
    if (q[0] == 0) {
        const auto family = g::classify(material);
        ScatteringFrame frame{float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
        if (q[3] == 1)
            frame = {float3(0, 0, -1), float3(0, 1, 0), float3(1, 0, 0)};
        if (q[3] == 2)
            frame.x.x = NAN;
        const float3 wo(as_type<float>(q[32]), as_type<float>(q[33]), as_type<float>(q[34]));
        const float3 wi(as_type<float>(q[36]), as_type<float>(q[37]), as_type<float>(q[38]));
        const auto result = g::forced_direction(material, frame, wo, wi, 17);
        out[0] = uint(result.status);
        out[1] = uint(family.family);
        out[2] = as_type<uint>(family.alpha);
        out[3] = uint(result.support);
        for (uint k = 0; k < 4; ++k) {
            out[4 + k] = as_type<uint>(result.numerator[k]);
            out[8 + k] = as_type<uint>(result.q[k]);
            out[12 + k] = as_type<uint>(result.eta[k]);
        }
        out[16] = uint(result.transmission);
        if (material.kind == 1 && family.status == g::Status::ok) {
            const auto e = rypath_conductor::evaluate(hero_frozen_conductor(material, 0),
                                                      frame.local(wo), frame.local(wi));
            float3 m;
            const bool cutoff = rypath_conductor::unit(frame.local(wo) + frame.local(wi), m) &&
                                m.z * m.z * m.z * m.z < 1e-16f;
            out[18] = uint(e.valid) | (uint(e.pdf == 0) << 1) | (uint(e.f == 0) << 2) |
                      (uint(cutoff) << 3);
        }
        // Native general renderer evaluation is a SECOND oracle, not the host's
        // analytic normal-incidence/diffuse result. Only compare valid support.
        if (result.status == g::Status::ok) {
            const auto reference =
                hero_current_evaluate(material, frame.local(wo), frame.local(wi), 17);
            uint mismatch = uint(!reference.valid);
            for (uint k = 0; k < 4; ++k) {
                mismatch |= uint(as_type<uint>(result.numerator[k]) !=
                                 as_type<uint>(reference.f[k] * abs(frame.local(wi).z)));
                mismatch |= uint(as_type<uint>(result.q[k]) != as_type<uint>(reference.pdf[k]));
            }
            out[17] = mismatch;
        }
    } else if (q[0] == 1) {
        const float3 from(as_type<float>(q[4]), as_type<float>(q[5]), as_type<float>(q[6]));
        const float3 fn(as_type<float>(q[8]), as_type<float>(q[9]), as_type<float>(q[10]));
        const float3 to(as_type<float>(q[12]), as_type<float>(q[13]), as_type<float>(q[14]));
        const float3 tn(as_type<float>(q[16]), as_type<float>(q[17]), as_type<float>(q[18]));
        const auto e = g::edge(from, fn, to, tn);
        out[0] = uint(e.status);
        for (uint k = 0; k < 3; ++k)
            out[20 + k] = as_type<uint>(e.direction[k]);
        out[24] = as_type<uint>(e.distance_squared.fraction);
        out[25] = as_type<uint>(e.distance_squared.exponent);
        out[26] = as_type<uint>(e.G_destination.fraction);
        out[27] = as_type<uint>(e.G_destination.exponent);
        out[28] = as_type<uint>(e.G_source.fraction);
        out[29] = as_type<uint>(e.G_source.exponent);
        out[30] = as_type<uint>(e.cosine_source);
        out[31] = as_type<uint>(e.cosine_destination);
    } else if (q[0] == 3) {
        const float3 from(as_type<float>(q[4]), as_type<float>(q[5]), as_type<float>(q[6]));
        const float3 to(as_type<float>(q[12]), as_type<float>(q[13]), as_type<float>(q[14]));
        const auto physical = g::physical_direction(from, to);
        out[0] = uint(physical.status);
        for (uint k = 0; k < 3; ++k)
            out[20 + k] = as_type<uint>(physical.direction[k]);
        out[24] = as_type<uint>(physical.distance_squared.fraction);
        out[25] = as_type<uint>(physical.distance_squared.exponent);
        // Fixed tangent normals: existence of a physical edge is independent
        // of the material/normal chart's geometric-cosine support predicate.
        const auto e = g::edge(from, float3(1, 0, 0), to, float3(1, 0, 0));
        out[32] = uint(e.status);
    } else if (q[0] == 8) {
        const float3 current(as_type<float>(q[4]), as_type<float>(q[5]), as_type<float>(q[6]));
        const float3 previous(as_type<float>(q[12]), as_type<float>(q[13]), as_type<float>(q[14]));
        const float3 ray_wo(as_type<float>(q[32]), as_type<float>(q[33]), as_type<float>(q[34]));
        const auto incoming = g::incoming_direction(current, previous, ray_wo);
        const auto geometric = g::physical_direction(current, previous);
        const auto e = g::edge(current, float3(0, 0, 1), previous, float3(0, 0, 1));
        out[0] = uint(incoming.status);
        for (uint k = 0; k < 3; ++k) {
            out[20 + k] = as_type<uint>(incoming.direction[k]);
            out[36 + k] = as_type<uint>(geometric.direction[k]);
        }
        out[24] = as_type<uint>(incoming.distance_squared.fraction);
        out[25] = as_type<uint>(incoming.distance_squared.exponent);
        out[32] = uint(geometric.status);
        out[33] = uint(e.status);
    } else if (q[0] == 4) {
        namespace runtime = rypath_bdpt::enhanced_hybrid_runtime;
        rypath_enhanced_hybrid::HybridState state{};
        SurfacePoint point{};
        point.position =
            float3(as_type<float>(q[12]), as_type<float>(q[13]), as_type<float>(q[14]));
        point.geometric_normal =
            float3(as_type<float>(q[16]), as_type<float>(q[17]), as_type<float>(q[18]));
        const float3 camera(as_type<float>(q[4]), as_type<float>(q[5]), as_type<float>(q[6]));
        out[0] = uint(runtime::prepare_primary(state, camera, point));
        if (q[3] == 1)
            out[36] = uint(runtime::prepare_primary(state, camera, point));
        out[24] = as_type<uint>(state.primary_distance_squared.fraction);
        out[25] = as_type<uint>(state.primary_distance_squared.exponent);
        out[30] = as_type<uint>(state.primary_cosine);
        out[31] = state.primary_ready;
        const auto threshold =
            fp::primary_threshold(state.primary_distance_squared, state.primary_cosine);
        out[37] = uint(threshold.decision);
        out[38] = uint(threshold.status);
    } else if (q[0] == 5) {
        namespace runtime = rypath_bdpt::enhanced_hybrid_runtime;
        HeroCurrentVertex current{};
        current.point.position =
            float3(as_type<float>(q[4]), as_type<float>(q[5]), as_type<float>(q[6]));
        current.point.error =
            float3(as_type<float>(q[8]), as_type<float>(q[9]), as_type<float>(q[10]));
        current.point.geometric_normal =
            float3(as_type<float>(q[16]), as_type<float>(q[17]), as_type<float>(q[18]));
        HeroPrevious previous{};
        previous.point.position = current.point.position;
        previous.point.geometric_normal = current.point.geometric_normal;
        rypath_enhanced_hybrid::PredecessorSnapshot snapshot{};
        snapshot.normal_offset = dot(abs(current.point.geometric_normal), current.point.error);
        const float3 direction(as_type<float>(q[32]), as_type<float>(q[33]), as_type<float>(q[34]));
        const auto origin = runtime::snapshot_offset(snapshot, previous, direction);
        const auto reference = offset(current, direction);
        out[0] = uint(g::Status::ok);
        for (uint k = 0; k < 3; ++k) {
            out[20 + k] = as_type<uint>(origin[k]);
            out[32 + k] = as_type<uint>(reference[k]);
        }
        out[24] = as_type<uint>(snapshot.normal_offset);
        out[35] = uint(any(as_type<uint3>(origin) != as_type<uint3>(reference)));
    } else if (q[0] == 6) {
        rypath_hero::Density P{};
        for (uint k = 0; k < 4; ++k)
            P.lane[k] = {as_type<float>(q[8 + 2 * k]), as_type<int>(q[9 + 2 * k])};
        const float4 beta(as_type<float>(q[32]), as_type<float>(q[33]), as_type<float>(q[34]),
                          as_type<float>(q[35]));
        out[0] = uint(g::Status::ok);
        out[40] = rypath_bdpt::enhanced_hybrid_runtime::needed_lanes(P, beta);
    } else if (q[0] == 7) {
        namespace runtime = rypath_bdpt::enhanced_hybrid_runtime;
        HeroPrevious previous{};
        previous.type = surface_vertex;
        previous.path_id = 2;
        previous.bsdf = material;
        previous.frame = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
        rypath_enhanced_hybrid::HybridState state{};
        state.predecessor_valid = q[47];
        state.predecessor.path_id = 2 + q[5];
        state.predecessor.beta[0] = 1;
        for (uint k = 0; k < 3; ++k)
            state.predecessor.wo[k] = as_type<float>(q[32 + k]);
        state.primary_ready = q[6];
        state.primary_distance_squared = {as_type<float>(q[40]), as_type<int>(q[41])};
        state.primary_cosine = as_type<float>(q[42]);
        state.certificate = {
            {as_type<float>(q[44]), as_type<int>(q[45])}, as_type<float>(q[46]), q[3], q[4]};
        const float3 direction(as_type<float>(q[36]), as_type<float>(q[37]), as_type<float>(q[38]));
        bool eligible = false;
        out[0] = uint(runtime::predecessor_would_reconnect(state, previous, direction, eligible));
        out[40] = uint(eligible);
    } else {
        auto pivot = material;
        pivot.kind = q[4];
        if (pivot.kind == 1 && q[5] != 0)
            for (uint k = 0; k < 4; ++k) {
                hero_frozen_store_float(pivot, 5u * k + 2u, as_type<float>(q[5]));
                hero_frozen_store_float(pivot, 5u * k + 3u, as_type<float>(q[5]));
            }
        float previous_q = as_type<float>(q[32]), pivot_q = as_type<float>(q[33]);
        if (q[3] == 4) {
            const ScatteringFrame frame{float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
            const auto a =
                g::forced_direction(material, frame, float3(0, 0, 1), float3(0, 0, 1), 17);
            const auto b = g::forced_direction(pivot, frame, float3(0, 0, 1), float3(0, 0, 1), 17);
            if (a.status != g::Status::ok || b.status != g::Status::ok) {
                out[0] = uint(g::Status::invalid_arithmetic);
                return;
            }
            previous_q = a.q[0];
            pivot_q = b.q[0];
        }
        const auto primary = g::edge(float3(0), float3(0, 0, 1), float3(0, 0, 1), float3(0, 0, 1));
        auto connection =
            g::edge(float3(0, 0, 1), float3(0, 0, 1), float3(0, 0, 2), float3(0, 0, 1));
        if (q[3] == 2)
            connection.distance_squared.exponent = 0x7fffffff;
        const auto input = g::footprint_input(material, pivot, primary, connection, previous_q,
                                              pivot_q, q[3] == 1);
        out[0] = uint(input.status);
        if (input.status == g::Status::ok) {
            const auto result = fp::classify(input.value);
            out[40] = uint(result.status);
            out[41] = uint(result.decision);
            out[42] = uint(result.forward_pass);
            out[43] = uint(result.inverse_pass);
            out[44] = as_type<uint>(input.value.predecessor_alpha);
            out[45] = as_type<uint>(input.value.primary_cosine);
            out[46] = as_type<uint>(input.value.edge_distance_squared.fraction);
            out[47] = as_type<uint>(input.value.edge_distance_squared.exponent);
            out[48] = as_type<uint>(input.value.predecessor_pdf.fraction);
            out[49] = as_type<uint>(input.value.predecessor_pdf.exponent);
            out[50] = as_type<uint>(input.value.pivot_outgoing_pdf.fraction);
            out[51] = as_type<uint>(input.value.pivot_outgoing_pdf.exponent);
        }
    }
}
