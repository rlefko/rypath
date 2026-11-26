// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "reuse_mis_shared.h"
#if defined(__METAL_VERSION__)
#define RYPATH_FOOTPRINT_THREAD thread
#else
#define RYPATH_FOOTPRINT_THREAD
#endif

// Lin et al., ReSTIR PT Enhanced, main Eq5 PDFp7/article13:7; section4.2
// footnote6 PDFp8; alpha=.2 stated in evaluation PDFp14/article13:14.
// Supplemental Eq5/6 PDFp2 and section3 PDFpp4-5. The displayed denominator
// is cos_primary/(4*pi), NOT cos_primary*4*pi. Hence RHS=c/100*4*pi*r^2/cos.
// https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/
namespace rypath_enhanced_footprint {
namespace math = rypath_reservoir;
namespace scaled = rypath_reuse_mis;
using Word = rypath_replay::Word;
using Number = scaled::Scaled;
enum : Word { law_version = 1, no_pivot = 0xffffffffu, maximum_pivot = 66 };
enum class Family : Word {
    diffuse = 0,
    rough_conductor = 1,
    terminal_emission = 2,
    unsupported = 3
};
enum class Status : Word { ok, invalid_input, invalid_state };
enum class Decision : Word { eligible, ineligible, unsupported, zero_support };
inline bool valid_positive(Number v) {
    return math::positive_normal(v.fraction) && v.fraction >= .5f && v.fraction < 1 &&
           v.exponent >= -1024 && v.exponent <= 1024;
}
inline bool zero(Number v) {
    return (math::bits(v.fraction) & 0x7fffffffu) == 0 && v.exponent == 0;
}
inline bool valid_number(Number v) { return valid_positive(v) || zero(v); }
inline bool cosine_zero(float c) { return (math::bits(c) & 0x7fffffffu) == 0; }
inline bool valid_cosine(float c) {
    const auto word = math::bits(c), magnitude = word & 0x7fffffffu;
    return magnitude <= 0x3f800000u && ((word & 0x80000000u) == 0 || magnitude == 0);
}
inline bool known_family(Family f) {
    return f == Family::diffuse || f == Family::rough_conductor || f == Family::terminal_emission ||
           f == Family::unsupported;
}
struct Threshold {
    Number area{0, 0};
    Decision decision = Decision::zero_support;
    Status status = Status::ok;
};
// r^2 uses WORLD LENGTH SQUARED; cosine is abs(n_primary dot direction_to_camera).
// p are marginal CONTINUOUS directional PDFs [sr^-1], never cumulative hero
// likelihoods, area PDFs, discrete masses or joint direction/lobe chart densities.
// Geometry conversion uses the DESTINATION geometric-normal cosine / edge r^2.
inline Threshold primary_threshold(Number primary_distance_squared, float primary_cosine) {
    Threshold t;
    if (!valid_number(primary_distance_squared) || !valid_cosine(primary_cosine)) {
        t.status = Status::invalid_input;
        return t;
    }
    if (zero(primary_distance_squared) || cosine_zero(primary_cosine))
        return t;
    const auto factor = scaled::product(scaled::decompose_positive(.0002f),
                                        scaled::decompose_positive(12.56637061435917295385f));
    t.area = scaled::quotient(scaled::product(factor, primary_distance_squared),
                              scaled::decompose_positive(primary_cosine));
    t.decision = Decision::eligible;
    return t;
}
struct Input {
    Family predecessor = Family::unsupported, pivot = Family::unsupported;
    // Alpha is the actual NDF width of the sampled predecessor lobe. For an
    // anisotropic conductor the caller's declared conservative policy may use
    // min(alpha_x,alpha_y); do not pass PBRT's pre-remap u/v roughness by accident.
    float predecessor_alpha = 0;
    Number primary_distance_squared{0, 0};
    float primary_cosine = 0;
    Number edge_distance_squared{0, 0};
    Number predecessor_pdf{0, 0};
    float cosine_at_pivot = 0;
    // This is p_k(omega_k), the ACTUAL OUTGOING-direction marginal PDF at pivot;
    // G goes BACK toward predecessor. It is NOT the PDF toward predecessor or
    // the geometry factor toward the suffix endpoint x_(k+1).
    Number pivot_outgoing_pdf{0, 0};
    float cosine_at_predecessor = 0;
};
struct Result {
    Decision decision = Decision::ineligible;
    Status status = Status::ok;
    bool forward_pass = false, inverse_pass = false;
};
inline Result classify(Input v) {
    Result result;
    if (!known_family(v.predecessor) || !known_family(v.pivot)) {
        result.status = Status::invalid_input;
        return result;
    }
    // A narrow supported chart, not an implicit replacement BSDF or general
    // multi-lobe/Dirac/coating connectability claim. Terminal emission means an
    // endpoint atom; an emissive material with a nonterminal suffix is NOT it.
    if (v.predecessor == Family::unsupported || v.predecessor == Family::terminal_emission ||
        v.pivot == Family::unsupported) {
        result.decision = Decision::unsupported;
        return result;
    }
    const bool inverse = v.pivot == Family::rough_conductor;
    if (!valid_number(v.edge_distance_squared) || !valid_number(v.predecessor_pdf) ||
        !valid_cosine(v.cosine_at_pivot) ||
        (inverse &&
         (!valid_number(v.pivot_outgoing_pdf) || !valid_cosine(v.cosine_at_predecessor))) ||
        (v.predecessor == Family::rough_conductor &&
         (!math::finite(v.predecessor_alpha) ||
          ((math::bits(v.predecessor_alpha) & 0x80000000u) != 0 &&
           (math::bits(v.predecessor_alpha) & 0x7fffffffu) != 0)))) {
        result.status = Status::invalid_input;
        return result;
    }
    const auto threshold = primary_threshold(v.primary_distance_squared, v.primary_cosine);
    result.status = threshold.status;
    if (result.status != Status::ok)
        return result;
    if (threshold.decision == Decision::zero_support || zero(v.edge_distance_squared) ||
        zero(v.predecessor_pdf) || cosine_zero(v.cosine_at_pivot) ||
        (inverse && (zero(v.pivot_outgoing_pdf) || cosine_zero(v.cosine_at_predecessor)))) {
        result.decision = Decision::zero_support;
        return result;
    }
    if (v.predecessor == Family::rough_conductor && v.predecessor_alpha < .2f)
        return result;
    const auto forward = scaled::quotient(
        v.edge_distance_squared,
        scaled::product(v.predecessor_pdf, scaled::decompose_positive(v.cosine_at_pivot)));
    // Inclusive >= as displayed Eq5. Comparisons are of represented scaled
    // FLOAT operations, not a certified exact-real geometric bound. Never use
    // an epsilon selected from observed source/destination outcomes.
    result.forward_pass = !scaled::less(forward, threshold.area);
    result.inverse_pass = !inverse;
    if (inverse) {
        const auto backward =
            scaled::quotient(v.edge_distance_squared,
                             scaled::product(v.pivot_outgoing_pdf,
                                             scaled::decompose_positive(v.cosine_at_predecessor)));
        result.inverse_pass = !scaled::less(backward, threshold.area);
    }
    if (result.forward_pass && result.inverse_pass)
        result.decision = Decision::eligible;
    return result;
}
struct FirstEligible {
    Word next = 2, selected = no_pivot;
    Status status = Status::ok;
};
inline bool valid_first(FirstEligible s) {
    return s.status == Status::ok && s.next >= 2 && s.next <= maximum_pivot + 1 &&
           (s.selected == no_pivot || (s.selected >= 2 && s.selected < s.next));
}
// Camera=x0, E_d ends at x_(d+1); D65 permits a terminal pivot66. Ordinary
// owned-z,w candidates need a real suffix endpoint and cannot invent one there.
// Invoke once in path order, AFTER required outgoing PDF is available. Stop
// after selection. No path state, seed or vertices are retained by this helper.
inline Result consider(RYPATH_FOOTPRINT_THREAD FirstEligible &s, Word k, Input v) {
    if (s.status != Status::ok)
        return {Decision::ineligible, s.status, false, false};
    if (!valid_first(s) || s.selected != no_pivot || k != s.next || k > maximum_pivot) {
        s.status = Status::invalid_state;
        return {Decision::ineligible, s.status, false, false};
    }
    auto result = classify(v);
    if (result.status != Status::ok) {
        s.status = result.status;
        return result;
    }
    ++s.next;
    if (result.decision == Decision::eligible)
        s.selected = k;
    return result;
}
// Necessary label agreement only; geometry/BSDF support and the actual shift
// inverse/Jacobian remain the caller's independent obligations.
inline bool reciprocal_first(FirstEligible source, FirstEligible destination) {
    return valid_first(source) && valid_first(destination) && source.selected != no_pivot &&
           source.selected == destination.selected;
}
static_assert(sizeof(Input) == 56 && sizeof(FirstEligible) == 12,
              "Footprint classifier has only CPU/Metal scalar-word inputs");
} // namespace rypath_enhanced_footprint
#undef RYPATH_FOOTPRINT_THREAD
