// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included inside rypath_bdpt after packet_pt_trace and hybrid geometry. The
// scalar hybrid state/chart headers must already have been included globally.
namespace enhanced_hybrid_runtime {
namespace g = enhanced_hybrid_geometry;
namespace hs = ::rypath_enhanced_hybrid;
namespace chart = ::rypath_enhanced_chart;
namespace fp = ::rypath_enhanced_footprint;
namespace ep = ::rypath_enhanced_path;
namespace ew = ::rypath::enhanced_wire;
using Number = hs::Number;

inline g::Status prepare_primary(thread hs::HybridState &state, float3 camera,
                                 const thread SurfacePoint &primary) {
    if (state.primary_ready)
        return g::Status::invalid_arithmetic;
    g::Signed direction[3];
    const auto edge = g::physical_direction_impl(camera, primary.position, direction);
    if (edge.status == g::Status::invalid_arithmetic || !g::unit(primary.geometric_normal))
        return g::Status::invalid_arithmetic;
    state.primary_distance_squared = edge.distance_squared;
    state.primary_cosine = 0;
    state.primary_ready = 1;
    if (edge.status == g::Status::outside_support)
        return g::Status::ok; // Zero primary footprint admits no owned interior chart.
    Number cosine{0, 0};
    const auto status = g::normalized_cosine(direction, primary.geometric_normal, cosine);
    if (status == g::Status::invalid_arithmetic)
        return status;
    if (status == g::Status::outside_support)
        return g::Status::ok;
    const auto value = ::rypath_reuse_mis::materialize_contribution(cosine);
    if (value.status != ::rypath_reservoir::Status::ok || !enhanced_light::positive(value.value) ||
        !fp::valid_cosine(value.value))
        return g::Status::invalid_arithmetic;
    state.primary_cosine = value.value;
    return g::Status::ok;
}

inline bool direct_root(const thread Resources &r, uint instance_id, uint primitive, uint kind) {
    const constant auto &p = *r.alpha.parameters;
    if (instance_id >= p.instance_count || kind != wire::triangle_primitive)
        return false;
    const device auto &instance = r.alpha.instances[instance_id];
    if (primitive >= instance.triangle_count || instance.material >= p.material_count ||
        instance.alpha_root != ew::invalid || instance.invisible_emitter ||
        instance.inside_medium != ew::invalid || instance.outside_medium != ew::invalid)
        return false;
    const uint family = r.alpha.materials[instance.material].scattering;
    return family == 0 || family == 1;
}

inline g::Status owned_point(const thread Resources &r, const thread ew::Record &record,
                             uint selected_slot, uint depth, thread SurfacePoint &point) {
    if (record.tag != chart::interior_zomega || !chart::valid_chart(record, selected_slot, depth))
        return g::Status::invalid_arithmetic;
    const constant auto &p = *r.alpha.parameters;
    if (record.instance >= p.instance_count || record.kind != wire::triangle_primitive ||
        record.primitive >= r.alpha.instances[record.instance].triangle_count)
        return g::Status::invalid_arithmetic;
    if (!direct_root(r, record.instance, record.primitive, record.kind))
        return g::Status::outside_support;
    const float2 uv(as_type<float>(record.coordinate_bits[0]),
                    as_type<float>(record.coordinate_bits[1]));
    return surface(r, uv, record.instance, record.primitive, record.kind, PointSource::area_sample,
                   point)
               ? g::Status::ok
               : g::Status::invalid_arithmetic;
}

inline float3 world_omega(const thread ew::Record &record) {
    return float3(as_type<float>(record.light_id), as_type<float>(record.reserved),
                  as_type<float>(record.coordinate_bits[2]));
}
inline float3 snapshot_wo(const thread hs::PredecessorSnapshot &snapshot) {
    return float3(snapshot.wo[0], snapshot.wo[1], snapshot.wo[2]);
}
inline float4 snapshot_beta(const thread hs::PredecessorSnapshot &snapshot) {
    return float4(snapshot.beta[0], snapshot.beta[1], snapshot.beta[2], snapshot.beta[3]);
}
inline bool nonnegative(float x) {
    return isfinite(x) && (!(as_type<uint>(x) & 0x80000000u) || !(as_type<uint>(x) & 0x7fffffffu));
}
template <typename Previous>
inline bool snapshot_valid(const thread hs::HybridState &s, const thread Previous &previous) {
    return s.predecessor_valid == 1 && previous.type == surface_vertex &&
           s.predecessor.path_id == previous.path_id && g::unit(snapshot_wo(s.predecessor)) &&
           finite_nonnegative(snapshot_beta(s.predecessor)) &&
           nonnegative(s.predecessor.normal_offset);
}
template <typename Previous>
inline float3 snapshot_offset(const thread hs::PredecessorSnapshot &snapshot,
                              const thread Previous &previous, float3 direction) {
    const float3 n = dot(direction, previous.point.geometric_normal) < 0
                         ? -previous.point.geometric_normal
                         : previous.point.geometric_normal;
    // Exactly the original offset expression with its direction-independent dot
    // product captured before HeroPrevious discards the point-error vector.
    return round_out(previous.point.position + snapshot.normal_offset * n, n);
}
template <typename Current>
inline bool capture_predecessor(thread hs::HybridState &s, const thread Current &current,
                                float4 direct_beta, bool admitted,
                                hs::PredecessorCertificate certificate) {
    s.predecessor = {};
    s.predecessor_valid = 0;
    s.certificate = {};
    if (!admitted)
        return true;
    const float offset_distance = dot(abs(current.point.geometric_normal), current.point.error);
    if (!g::unit(current.wo) || !finite_nonnegative(direct_beta) || !nonnegative(offset_distance))
        return false;
    for (uint k = 0; k < 4; ++k)
        s.predecessor.beta[k] = direct_beta[k];
    for (uint k = 0; k < 3; ++k)
        s.predecessor.wo[k] = current.wo[k];
    s.predecessor.path_id = current.path_id;
    s.predecessor.normal_offset = offset_distance;
    s.predecessor_valid = 1;
    s.certificate = certificate;
    return true;
}

template <typename Previous>
inline g::Status certify_connection(thread Resources &r, thread wire::Sample &sample,
                                    const thread Wavelengths &lambda,
                                    const thread hs::HybridState &state,
                                    const thread Previous &previous,
                                    const thread ew::Record &record, float3 direction) {
    if (!snapshot_valid(state, previous) || !g::unit(direction))
        return g::Status::invalid_arithmetic;
    const float3 origin = snapshot_offset(state.predecessor, previous, direction);
    if (!all(isfinite(origin)))
        return g::Status::invalid_arithmetic;
    const auto saved_ray = r.alpha.incoming;
    const auto hit = packet_pt_trace(r, sample, lambda, ray(origin, direction));
    r.alpha.incoming = saved_ray;
    if (!hit.valid) {
        sample.status = wire::invalid_hit;
        return g::Status::invalid_arithmetic;
    }
    // The closest query certifies identity only. Its returned UV must NEVER
    // replace the owned coordinate, even if the offset changes its final bits.
    return hit.type == intersection_type::triangle && hit.geometry_id == 0 &&
                   hit.instance_id == record.instance && hit.primitive_id == record.primitive &&
                   hit.primitive_kind == wire::triangle_primitive
               ? g::Status::ok
               : g::Status::outside_support;
}

inline uint needed_lanes(const thread rypath_hero::Density &P, float4 beta) {
    uint result = 0;
    for (uint k = 0; k < 4; ++k)
        if (P.lane[k].fraction > 0 || (as_type<uint>(beta[k]) & 0x7fffffffu))
            result |= 1u << k;
    return result;
}
inline bool prepare_future_payload(const thread Resources &r, const device wire::Material &material,
                                   graph_wire::Context context, const thread Wavelengths &lambda,
                                   thread HeroFrozenBSDF &old, const thread rypath_hero::Density &P,
                                   float4 beta, thread HeroFrozenBSDF &future) {
    const uint needed = needed_lanes(P, beta);
    if (!(needed & 1u) || (needed & ~15u))
        return false;
    if (!(needed & ~old.active_mask)) {
        future = old;
        future.active_mask = needed;
        return true;
    }
    // A zero-beta lane can still need its PDF. An unprepared old lane is not a
    // physical zero: resolve the same root/context/lambda for the future only.
    const uint original_mask = old.active_mask;
    if (!prepare_hero_current(r, material, context, lambda, future, original_mask | needed))
        return false;
    // Preserve the additional immutable values in the ordinary frozen history
    // without changing its active lanes. The rolling certificate later applies
    // its explicit query_mask to a copy, never to the physical predecessor.
    old = future;
    old.active_mask = original_mask;
    future.active_mask = needed;
    return true;
}
inline rypath_hero_scattering::Sample forced_sample(const thread HeroFrozenBSDF &bsdf,
                                                    const thread ScatteringFrame &frame, float3 wo,
                                                    float3 wi, uint seed) {
    const auto value = g::forced_direction(bsdf, frame, wo, wi, seed);
    rypath_hero_scattering::Sample out{};
    out.wi = frame.local(wi);
    out.numerator = value.numerator;
    out.pdf = value.q;
    out.eta = value.eta;
    out.valid = value.status == g::Status::ok;
    out.accepted = out.valid && value.support;
    return out;
}

inline bool record_choice(thread hs::HybridState &state, uint k, bool eligible) {
    if (!fp::valid_first(state.first) || state.first.selected != fp::no_pivot ||
        state.first.next != k || k < 2 || k > fp::maximum_pivot)
        return false;
    ++state.first.next;
    if (eligible)
        state.first.selected = k;
    return true;
}
template <typename Previous>
inline g::Status predecessor_would_reconnect(const thread hs::HybridState &state,
                                             const thread Previous &previous,
                                             float3 forced_direction, thread bool &eligible) {
    eligible = false;
    const auto certificate = state.certificate;
    if (!certificate.base_admissible)
        return g::Status::ok;
    if (certificate.base_admissible != 1 || !snapshot_valid(state, previous) ||
        state.primary_ready != 1 || !(certificate.query_mask & 1u) ||
        (certificate.query_mask & ~15u))
        return g::Status::invalid_arithmetic;
    auto queried = previous.bsdf;
    // Those payload words were prepared in the predecessor's original context
    // before it was frozen. This clone must never become physical history.
    queried.active_mask = certificate.query_mask;
    const auto result = g::forced_direction(queried, previous.frame, snapshot_wo(state.predecessor),
                                            forced_direction, previous.seed);
    if (result.status != g::Status::ok)
        return g::Status::invalid_arithmetic; // Base certificate admitted this frozen family.
    if (!result.support)
        return g::Status::ok;
    if (queried.kind == 0) {
        // Diffuse skips the inverse footprint, but still has an outgoing
        // support predicate (including finite grazing/hemisphere boundaries).
        eligible = true;
        return g::Status::ok;
    }
    if (queried.kind != 1)
        return g::Status::invalid_arithmetic;
    const auto threshold =
        fp::primary_threshold(state.primary_distance_squared, state.primary_cosine);
    if (threshold.status != fp::Status::ok)
        return g::Status::invalid_arithmetic;
    const auto inverse = fp::inverse_footprint(certificate.edge_distance_squared,
                                               ::rypath_reuse_mis::decompose_positive(result.q[0]),
                                               certificate.cosine_at_predecessor, threshold.area);
    if (inverse.status != fp::Status::ok)
        return g::Status::invalid_arithmetic;
    eligible = inverse.decision == fp::Decision::eligible;
    return g::Status::ok;
}
} // namespace enhanced_hybrid_runtime
