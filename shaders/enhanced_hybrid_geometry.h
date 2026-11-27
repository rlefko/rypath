// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Include INSIDE rypath_bdpt after hero_pt_helpers.h/enhanced_light.h; include
// enhanced_footprint_shared.h at GLOBAL scope first. No Resources/AS ownership.
#if (defined(__FAST_MATH__) && __FAST_MATH__) ||                                                   \
    (defined(__FAST_RELAXED_MATH__) && __FAST_RELAXED_MATH__)
#error "Enhanced hybrid geometry requires strict -fno-fast-math"
#endif
namespace enhanced_hybrid_geometry {
namespace rm = ::rypath_reuse_mis;
namespace fp = ::rypath_enhanced_footprint;
namespace el = enhanced_light;
enum class Status : uint { ok, unsupported, outside_support, invalid_arithmetic };
struct FamilyInfo {
    fp::Family family = fp::Family::unsupported;
    float alpha = 0;
    Status status = Status::unsupported;
};
inline FamilyInfo classify(const thread HeroFrozenBSDF &p) {
    if (!(p.active_mask & 1u) || (p.active_mask & ~15u))
        return {fp::Family::unsupported, 0, Status::invalid_arithmetic};
    if (p.kind != 0 && p.kind != 1)
        return {};
    for (uint k = 0; k < 4; ++k) {
        if (!(p.active_mask & (1u << k)))
            continue;
        if (p.kind == 0) {
            if (!el::nonnegative(hero_frozen_diffuse(p, k)))
                return {fp::Family::unsupported, 0, Status::invalid_arithmetic};
        } else {
            const auto q = hero_frozen_conductor(p, k);
            if (p.payload[5u * k + 4u] > 1 || !el::nonnegative(q.eta) || !el::nonnegative(q.k) ||
                (!el::positive(q.eta) && !el::positive(q.k)) || !el::nonnegative(q.alpha_x) ||
                !el::nonnegative(q.alpha_y) ||
                (!q.smooth && (!el::positive(q.alpha_x) || !el::positive(q.alpha_y))))
                return {fp::Family::unsupported, 0, Status::invalid_arithmetic};
        }
    }
    if (p.kind == 0)
        return {fp::Family::diffuse, 1, Status::ok};
    const auto hero = hero_frozen_conductor(p, 0);
    if (hero.smooth)
        return {};
    const float alpha =
        as_type<uint>(hero.alpha_x) < as_type<uint>(hero.alpha_y) ? hero.alpha_x : hero.alpha_y;
    return {fp::Family::rough_conductor, alpha, Status::ok};
}
inline bool unit(float3 v) { return all(isfinite(v)) && abs(dot(v, v) - 1.f) <= 0x1p-16f; }
inline bool frame_valid(const thread ScatteringFrame &f) {
    return unit(f.x) && unit(f.y) && unit(f.z) && abs(dot(f.x, f.y)) <= 0x1p-16f &&
           abs(dot(f.x, f.z)) <= 0x1p-16f && abs(dot(f.y, f.z)) <= 0x1p-16f;
}
struct Forced {
    float4 numerator = 0, q = 0, eta = 1;
    Status status = Status::invalid_arithmetic;
    bool support = false, transmission = false;
};
inline Forced forced_direction(const thread HeroFrozenBSDF &p, const thread ScatteringFrame &frame,
                               float3 wo_world, float3 wi_world, uint seed) {
    Forced out;
    const auto family = classify(p);
    out.status = family.status;
    if (out.status != Status::ok)
        return out;
    out.status = Status::invalid_arithmetic;
    if (!frame_valid(frame) || !unit(wo_world) || !unit(wi_world))
        return out;
    const float3 wo = frame.local(wo_world), wi = frame.local(wi_world);
    const float cosine = abs(wi.z);
    for (uint k = 0; k < 4; ++k) {
        if (!(p.active_mask & (1u << k)))
            continue;
        const auto e =
            p.kind == 0
                ? rypath_transmission::diffuse_evaluate(hero_frozen_diffuse(p, k), 0, wo, wi)
                : rypath_conductor::evaluate(hero_frozen_conductor(p, k), wo, wi);
        if (!e.valid || !el::nonnegative(e.f) || !el::nonnegative(e.pdf))
            return out;
        const bool continuous_side =
            el::positive(abs(wo.z)) && el::positive(abs(wi.z)) &&
            ((as_type<uint>(wo.z) ^ as_type<uint>(wi.z)) & 0x80000000u) == 0;
        if (continuous_side && p.kind == 0 && el::positive(hero_frozen_diffuse(p, k)) &&
            !el::positive(e.f))
            return out; // A mathematically positive continuous factor was lost numerically.
        if (continuous_side && p.kind == 1 && !hero_frozen_conductor(p, k).smooth &&
            !el::positive(e.pdf)) {
            float3 m;
            // This exact cutoff is part of conductor::distribution's pinned
            // model. Other lost positive PDFs remain arithmetic range failures.
            if (!rypath_conductor::unit(wo + wi, m) || !(m.z * m.z * m.z * m.z < 1e-16f))
                return out;
        }
        out.q[k] = e.pdf;
        out.numerator[k] = e.f * cosine;
        if (!el::nonnegative(out.numerator[k]) ||
            (el::positive(e.f) && el::positive(cosine) && !el::positive(out.numerator[k])))
            return out; // Nonzero product outside FLOAT range is not physical black.
    }
    (void)seed; // Supported families have no hidden-walk/random evaluation.
    out.status = Status::ok;
    out.support = el::positive(out.q[0]);
    return out;
}
// Internal signed exponents derive ONLY from finite binary32 components (roughly
// [-149,129], then bounded products/differences); no arbitrary serialized Number
// enters these arithmetic helpers. Signed scaled components avoid overflow in
// finite HDR endpoint subtraction
// and FTZ when two finite coordinates differ by a subnormal amount.
struct Signed {
    rm::Scaled magnitude{0, 0};
    bool negative = false;
};
inline bool empty(Signed x) { return x.magnitude.fraction == 0; }
inline Signed component(float x) {
    const uint word = as_type<uint>(x), magnitude = word & 0x7fffffffu;
    if (!magnitude)
        return {};
    return {rm::decompose_positive(as_type<float>(magnitude)), (word & 0x80000000u) != 0};
}
inline Signed signed_add(Signed a, Signed b) {
    if (empty(a))
        return b;
    if (empty(b))
        return a;
    if (a.negative == b.negative)
        return {rm::sum(a.magnitude, b.magnitude), a.negative};
    if (rm::less(a.magnitude, b.magnitude)) {
        const auto swap = a;
        a = b;
        b = swap;
    }
    const int gap = a.magnitude.exponent - b.magnitude.exponent;
    if (gap > 25)
        return a;
    const float difference =
        a.magnitude.fraction - rypath_reservoir::scale(b.magnitude.fraction, -gap);
    if (difference == 0)
        return {};
    return {rm::normalized(difference, a.magnitude.exponent), a.negative};
}
inline Signed difference(float to, float from) {
    auto a = component(to), b = component(from);
    b.negative = !b.negative;
    return signed_add(a, b);
}
inline Signed dot_scaled(const thread Signed *direction, float3 normal,
                         thread bool &uncertain_zero) {
    Signed sum{};
    uncertain_zero = false;
    for (uint k = 0; k < 3; ++k) {
        const auto n = component(normal[k]);
        if (empty(n) || empty(direction[k]))
            continue;
        const float product = n.magnitude.fraction * direction[k].magnitude.fraction;
        uncertain_zero |= fma(n.magnitude.fraction, direction[k].magnitude.fraction, -product) != 0;
        const Signed term{rm::product(n.magnitude, direction[k].magnitude),
                          n.negative != direction[k].negative};
        if (!empty(sum)) {
            const int exponent = max(sum.magnitude.exponent, term.magnitude.exponent);
            const int ga = exponent - sum.magnitude.exponent;
            const int gb = exponent - term.magnitude.exponent;
            if (ga > 25 || gb > 25) {
                uncertain_zero = true;
            } else {
                const float a = rypath_reservoir::scale(sum.magnitude.fraction, -ga) *
                                (sum.negative ? -1.f : 1.f);
                const float b = rypath_reservoir::scale(term.magnitude.fraction, -gb) *
                                (term.negative ? -1.f : 1.f);
                const float value = a + b, bv = value - a;
                uncertain_zero |= (a - (value - bv)) + (b - bv) != 0;
            }
        }
        sum = signed_add(sum, term);
    }
    return sum;
}
inline rm::Scaled square_root(rm::Scaled x) {
    if (x.exponent & 1) {
        x.fraction *= 2;
        --x.exponent;
    }
    return rm::normalized(sqrt(x.fraction), x.exponent / 2);
}
inline bool same_magnitude(rm::Scaled a, rm::Scaled b) {
    return as_type<uint>(a.fraction) == as_type<uint>(b.fraction) && a.exponent == b.exponent;
}
// Literal-vector normalized abs cosine from Lagrange's identity, not a FLOAT
// clamp: |dot|/sqrt(dot^2+cross^2) = 1/sqrt(1+cross^2/dot^2).
// The represented nonnegative ratio makes the result <=1 by construction.
inline Status normalized_cosine(const thread Signed *direction, float3 normal,
                                thread rm::Scaled &cosine) {
    bool uncertain = false;
    const auto dot_value = dot_scaled(direction, normal, uncertain);
    if (empty(dot_value))
        return uncertain ? Status::invalid_arithmetic : Status::outside_support;
    rm::Scaled cross_squared{0, 0};
    bool have_cross = false;
    for (uint k = 0; k < 3; ++k) {
        const uint j = (k + 1) % 3, l = (k + 2) % 3;
        const auto nj = component(normal[j]), nl = component(normal[l]);
        const bool equal_sign =
            (direction[j].negative != nl.negative) == (direction[l].negative != nj.negative);
        // These factor identities certify an exact zero cross component even
        // if each identical multiplication individually has a rounded residue.
        if (equal_sign && ((same_magnitude(direction[j].magnitude, direction[l].magnitude) &&
                            same_magnitude(nl.magnitude, nj.magnitude)) ||
                           (same_magnitude(direction[j].magnitude, nj.magnitude) &&
                            same_magnitude(nl.magnitude, direction[l].magnitude))))
            continue;
        Signed terms[3]{direction[j], direction[l], {}};
        terms[1].negative = !terms[1].negative;
        bool cross_uncertain = false;
        const auto cross_value =
            dot_scaled(terms, float3(normal[l], normal[j], 0), cross_uncertain);
        if (empty(cross_value)) {
            if (cross_uncertain)
                return Status::invalid_arithmetic;
            continue;
        }
        const auto square = rm::product(cross_value.magnitude, cross_value.magnitude);
        cross_squared = have_cross ? rm::sum(cross_squared, square) : square;
        have_cross = true;
    }
    const rm::Scaled one{.5f, 1};
    cosine = one;
    if (have_cross) {
        const auto dot_squared = rm::product(dot_value.magnitude, dot_value.magnitude);
        const auto ratio = rm::quotient(cross_squared, dot_squared);
        cosine = rm::quotient(one, square_root(rm::sum(one, ratio)));
    }
    return Status::ok;
}
// Independent of BSDF family and geometric normal support. A tangent edge
// still has a physical direction. This is a local helper result, not a wire ABI.
struct PhysicalDirection {
    float3 direction = 0;
    rm::Scaled distance_squared{0, 0};
    Status status = Status::invalid_arithmetic;
};
// Keep the original edge arithmetic and its scaled unit components together;
// edge's cosine construction must not reconstruct them from rounded FLOAT.
inline PhysicalDirection physical_direction_impl(float3 from, float3 to, thread Signed *direction) {
    PhysicalDirection out;
    if (!all(isfinite(from)) || !all(isfinite(to)))
        return out;
    Signed delta[3];
    bool have = false;
    for (uint k = 0; k < 3; ++k) {
        delta[k] = difference(to[k], from[k]);
        if (empty(delta[k]))
            continue;
        const auto square = rm::product(delta[k].magnitude, delta[k].magnitude);
        out.distance_squared = have ? rm::sum(out.distance_squared, square) : square;
        have = true;
    }
    if (!have) {
        out.status = Status::outside_support;
        return out;
    }
    const auto distance = square_root(out.distance_squared);
    for (uint k = 0; k < 3; ++k) {
        if (empty(delta[k])) {
            direction[k] = {};
            continue;
        }
        direction[k] = {rm::quotient(delta[k].magnitude, distance), delta[k].negative};
        const auto q = rm::materialize_contribution(direction[k].magnitude);
        if (q.status != rm::Status::ok || !el::positive(q.value))
            return out; // Explicit unit-direction range failure, not coincident geometry.
        out.direction[k] =
            rm::from_bits(as_type<uint>(q.value) | (direction[k].negative ? 0x80000000u : 0u));
    }
    out.status = Status::ok;
    return out;
}
inline PhysicalDirection physical_direction(float3 from, float3 to) {
    Signed direction[3];
    auto out = physical_direction_impl(from, to, direction);
    if (out.status == Status::ok && !unit(out.direction))
        out.status = Status::invalid_arithmetic;
    return out;
}
struct Edge {
    float3 direction = 0;
    rm::Scaled distance_squared{0, 0}, G_destination{0, 0}, G_source{0, 0};
    float cosine_source = 0, cosine_destination = 0;
    Status status = Status::invalid_arithmetic;
};
inline Edge edge(float3 from, float3 from_normal, float3 to, float3 to_normal) {
    Edge out;
    if (!all(isfinite(from)) || !all(isfinite(to)) || !unit(from_normal) || !unit(to_normal))
        return out;
    Signed direction[3];
    const auto physical = physical_direction_impl(from, to, direction);
    out.direction = physical.direction;
    out.distance_squared = physical.distance_squared;
    if (physical.status != Status::ok) {
        out.status = physical.status;
        return out;
    }
    rm::Scaled source{0, 0}, destination{0, 0};
    const auto source_status = normalized_cosine(direction, from_normal, source);
    const auto destination_status = normalized_cosine(direction, to_normal, destination);
    if (source_status != Status::ok || destination_status != Status::ok) {
        out.status = source_status == Status::invalid_arithmetic ||
                             destination_status == Status::invalid_arithmetic
                         ? Status::invalid_arithmetic
                         : Status::outside_support;
        return out;
    }
    const auto cs = rm::materialize_contribution(source);
    const auto cd = rm::materialize_contribution(destination);
    if (cs.status != rm::Status::ok || cd.status != rm::Status::ok || !el::positive(cs.value) ||
        !el::positive(cd.value) || !fp::valid_cosine(cs.value) || !fp::valid_cosine(cd.value) ||
        !unit(out.direction))
        return out;
    out.cosine_source = cs.value;
    out.cosine_destination = cd.value;
    out.G_source = rm::quotient(source, out.distance_squared);
    out.G_destination = rm::quotient(destination, out.distance_squared);
    out.status = Status::ok;
    return out;
}
inline bool valid_edge(const thread Edge &e) {
    return e.status == Status::ok && unit(e.direction) && fp::valid_positive(e.distance_squared) &&
           fp::valid_positive(e.G_source) && fp::valid_positive(e.G_destination) &&
           fp::valid_cosine(e.cosine_source) && fp::valid_cosine(e.cosine_destination) &&
           el::positive(e.cosine_source) && el::positive(e.cosine_destination);
}
struct FootprintInput {
    fp::Input value;
    Status status = Status::invalid_arithmetic;
};
inline FootprintInput footprint_input(const thread HeroFrozenBSDF &previous,
                                      const thread HeroFrozenBSDF &pivot,
                                      const thread Edge &primary, const thread Edge &connection,
                                      float previous_q, float pivot_out_q,
                                      bool terminal_emission = false) {
    FootprintInput out;
    const auto a = classify(previous), b = classify(pivot);
    if (a.status != Status::ok || (!terminal_emission && b.status != Status::ok)) {
        out.status = a.status != Status::ok ? a.status : b.status;
        return out;
    }
    if (primary.status != Status::ok || connection.status != Status::ok) {
        out.status = primary.status != Status::ok ? primary.status : connection.status;
        return out;
    }
    if (!valid_edge(primary) || !valid_edge(connection) || !el::nonnegative(previous_q) ||
        (!terminal_emission && b.family != fp::Family::diffuse && !el::nonnegative(pivot_out_q)))
        return out;
    out.value.predecessor = a.family;
    out.value.pivot = terminal_emission ? fp::Family::terminal_emission : b.family;
    out.value.predecessor_alpha = a.alpha;
    out.value.primary_distance_squared = primary.distance_squared;
    out.value.primary_cosine = primary.cosine_destination;
    out.value.edge_distance_squared = connection.distance_squared;
    out.value.predecessor_pdf =
        !el::positive(previous_q) ? rm::Scaled{0, 0} : rm::decompose_positive(previous_q);
    out.value.cosine_at_pivot = connection.cosine_destination;
    out.value.pivot_outgoing_pdf =
        terminal_emission || b.family == fp::Family::diffuse || !el::positive(pivot_out_q)
            ? rm::Scaled{0, 0}
            : rm::decompose_positive(pivot_out_q);
    out.value.cosine_at_predecessor = connection.cosine_source;
    out.status = Status::ok;
    return out;
}
} // namespace enhanced_hybrid_geometry
