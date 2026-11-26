// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "contribution_path_shared.h"
#if defined(__METAL_VERSION__)
#define RYPATH_ENHANCED_THREAD thread
#else
#define RYPATH_ENHANCED_THREAD
#endif

namespace rypath_enhanced_path {
namespace cp = rypath_contribution_path;
namespace math = rypath_reservoir;
namespace reuse = rypath_reuse_mis;
using Word = cp::Word;
using XYZ = cp::XYZ;
using Descriptor = cp::Descriptor;
using Status = cp::Status;
enum : Word { null_slot = 0xfffffffeu, fresh_slot = 0xffffffffu, law_version = 1 };
inline bool nonnegative(float value) {
    return (math::bits(value) & 0x7fffffffu) == 0 || math::positive_normal(value);
}
// Positive normalized densities remain scaled; do not materialize h. Bounds
// keep every subsequent exponent sum/difference safely inside signed int.
inline bool valid_density(reuse::Scaled h) {
    return math::positive_normal(h.fraction) && h.fraction >= .5f && h.fraction < 1 &&
           h.exponent >= -1024 && h.exponent <= 1024;
}
inline bool unit_density(reuse::Scaled h) {
    return math::bits(h.fraction) == math::bits(.5f) && h.exponent == 1;
}
inline bool same_density(reuse::Scaled a, reuse::Scaled b) {
    return math::bits(a.fraction) == math::bits(b.fraction) && a.exponent == b.exponent;
}
// Exact zero is decided from represented F_chart, not a rounded magnitude sum.
// Nonzero target = quarterL1(F_chart) + h_eval*epsilon(scale). Actual fresh
// generation density h_gen is separate and enters ONLY the fresh WRS score.
inline math::ScalarResult target(XYZ xyz, float scale, reuse::Scaled h_eval = {.5f, 1}) {
    if (!cp::finite(xyz) || !math::positive_normal(scale) || !valid_density(h_eval))
        return {0, math::Status::invalid_target};
    if (cp::zero(xyz))
        return {0, math::Status::ok};
    if (unit_density(h_eval))
        return reuse::importance_target(xyz.x, xyz.y, xyz.z, scale);
    const reuse::Scaled minimum{.5f, -125};
    auto floor = reuse::product(reuse::decompose_positive(scale), reuse::decompose_positive(1e-6f));
    if (reuse::less(floor, minimum))
        floor = minimum;
    floor = reuse::product(floor, h_eval);
    bool have = false;
    reuse::Scaled energy{.5f, -125};
    const float values[3] = {xyz.x, xyz.y, xyz.z};
    for (unsigned i = 0; i < 3; ++i) {
        const float magnitude = math::magnitude(values[i]);
        if (math::bits(magnitude) == 0)
            continue;
        auto part = reuse::decompose_positive(magnitude);
        part.exponent -= 2;
        energy = have ? reuse::sum(energy, part) : part;
        have = true;
    }
    return reuse::materialize(reuse::sum(floor, energy));
}
struct State {
    Descriptor descriptor;
    float target_scale = 0, score_sum = 0, selected_target = 0;
    Word observed = 0, selected_slot = null_slot;
    XYZ selected_xyz;
    Status status = Status::ok;
    reuse::Scaled selected_h{.5f, 1};
};
inline State begin(Descriptor descriptor, float scale) {
    State s;
    s.descriptor = descriptor;
    s.target_scale = scale;
    if (!cp::valid_descriptor(descriptor) || descriptor.maximum_depth > 65)
        s.status = Status::invalid_descriptor;
    else if (!math::positive_normal(scale))
        s.status = Status::invalid_value;
    return s;
}
inline bool valid_state(RYPATH_ENHANCED_THREAD const State &s) {
    if (s.status != Status::ok || !cp::valid_descriptor(s.descriptor) ||
        s.descriptor.maximum_depth > 65 || !math::positive_normal(s.target_scale) ||
        s.observed > s.descriptor.slot_count)
        return false;
    if (s.selected_slot == null_slot)
        return math::bits(s.score_sum) == 0 && math::bits(s.selected_target) == 0 &&
               cp::zero(s.selected_xyz) && unit_density(s.selected_h);
    if (!s.observed || s.selected_slot >= s.observed || !math::positive_normal(s.score_sum) ||
        !math::positive_normal(s.selected_target) || !valid_density(s.selected_h))
        return false;
    const auto t = target(s.selected_xyz, s.target_scale, s.selected_h);
    return t.status == math::Status::ok && math::bits(t.value) == math::bits(s.selected_target);
}
inline bool same_state(RYPATH_ENHANCED_THREAD const State &a,
                       RYPATH_ENHANCED_THREAD const State &b) {
    return a.descriptor.version == b.descriptor.version &&
           a.descriptor.depth_mode == b.descriptor.depth_mode &&
           a.descriptor.maximum_depth == b.descriptor.maximum_depth &&
           a.descriptor.slot_count == b.descriptor.slot_count && a.observed == b.observed &&
           a.selected_slot == b.selected_slot && a.status == b.status &&
           math::bits(a.target_scale) == math::bits(b.target_scale) &&
           math::bits(a.score_sum) == math::bits(b.score_sum) &&
           math::bits(a.selected_target) == math::bits(b.selected_target) &&
           math::bits(a.selected_xyz.x) == math::bits(b.selected_xyz.x) &&
           math::bits(a.selected_xyz.y) == math::bits(b.selected_xyz.y) &&
           math::bits(a.selected_xyz.z) == math::bits(b.selected_xyz.z) &&
           same_density(a.selected_h, b.selected_h);
}
inline math::State selector(RYPATH_ENHANCED_THREAD const State &s) {
    return s.selected_slot == null_slot ? math::State{}
                                        : math::State{s.score_sum, 1.f, 1, 0, math::Status::ok};
}
struct PreparedUpdate {
    State source;
    math::PreparedUpdate decision;
    XYZ xyz;
    float alpha = 0, probability = 0;
    Word index = fresh_slot;
    Status status = Status::ok;
    reuse::Scaled h_eval{.5f, 1}, h_gen{.5f, 1};
};
inline PreparedUpdate prepare_update(RYPATH_ENHANCED_THREAD const State &s, Word index, XYZ xyz,
                                     float probability, reuse::Scaled h_eval = {.5f, 1},
                                     reuse::Scaled h_gen = {.5f, 1}) {
    PreparedUpdate p;
    p.source = s;
    p.xyz = xyz;
    p.index = index;
    p.probability = probability;
    p.h_eval = h_eval;
    p.h_gen = h_gen;
    if (s.status != Status::ok)
        p.status = s.status;
    else if (!valid_state(s))
        p.status = Status::invalid_state;
    else if (index != s.observed || index >= s.descriptor.slot_count)
        p.status = Status::invalid_slot;
    else if (!math::positive_normal(probability) || probability > 1 || !valid_density(h_eval) ||
             !valid_density(h_gen))
        p.status = Status::invalid_value;
    else {
        const auto t = target(xyz, s.target_scale, h_eval);
        p.status = cp::numeric_status(t.status);
        p.alpha = t.value;
        if (p.status == Status::ok && p.alpha == 0) {
            p.xyz = {}; // Canonical null/zero observation, no selector entropy.
            p.decision.complete = true;
        } else if (p.status == Status::ok) {
            const auto a = reuse::decompose_positive(probability);
            const auto proposal = unit_density(h_gen) ? a : reuse::product(h_gen, a);
            const auto score =
                reuse::materialize(reuse::quotient(reuse::decompose_positive(p.alpha), proposal));
            p.status = cp::numeric_status(score.status);
            if (p.status == Status::ok) {
                p.decision = math::prepare_update(selector(s), score.value, 1.f);
                p.status = cp::numeric_status(p.decision.status);
            }
        }
    }
    return p;
}
inline Status feed_random_word(RYPATH_ENHANCED_THREAD PreparedUpdate &p, Word word) {
    if (p.status == Status::ok && p.alpha != 0)
        p.status = cp::numeric_status(math::feed_random_word(p.decision, word));
    return p.status;
}
using UpdateResult = cp::UpdateResult;
inline UpdateResult commit_update(RYPATH_ENHANCED_THREAD State &s,
                                  RYPATH_ENHANCED_THREAD const PreparedUpdate &p) {
    if (s.status != Status::ok)
        return {false, s.status};
    if (p.status != Status::ok) {
        s.status = p.status;
        return {false, s.status};
    }
    const auto checked = prepare_update(s, p.index, p.xyz, p.probability, p.h_eval, p.h_gen);
    if (!valid_state(s) || !same_state(s, p.source) || checked.status != Status::ok ||
        math::bits(checked.alpha) != math::bits(p.alpha)) {
        s.status = Status::invalid_state;
        return {false, s.status};
    }
    bool selected = false;
    if (p.alpha != 0) {
        // Validate the deterministic selector fields as well as its entropy state.
        const RYPATH_ENHANCED_THREAD auto &a = checked.decision;
        const RYPATH_ENHANCED_THREAD auto &b = p.decision;
        if (math::bits(a.score_sum) != math::bits(b.score_sum) ||
            math::bits(a.target) != math::bits(b.target) ||
            a.rare_probability.significand != b.rare_probability.significand ||
            a.rare_probability.bit_count != b.rare_probability.bit_count ||
            a.select_new_on_success != b.select_new_on_success) {
            s.status = Status::invalid_state;
            return {false, s.status};
        }
        auto local = selector(s);
        const auto result = math::commit_update(local, p.decision);
        if (result.status != math::Status::ok) {
            s.status = cp::numeric_status(result.status);
            return {false, s.status};
        }
        s.score_sum = local.score_sum;
        selected = result.selected;
        if (selected) {
            s.selected_slot = p.index;
            s.selected_target = p.alpha; // Canonical alpha, NOT alpha/a.
            s.selected_xyz = p.xyz;
            s.selected_h = p.h_eval;
        }
    } else if (p.decision.status != math::Status::ok || !p.decision.complete ||
               p.decision.words_used != 0 || p.decision.compared_bits != 0 || p.decision.selected ||
               p.decision.rare_probability.significand != 0 ||
               p.decision.rare_probability.bit_count != 0) {
        s.status = Status::invalid_state;
        return {false, s.status};
    }
    ++s.observed;
    return {selected, Status::ok};
}
enum class StopReason : Word { physical_end = 0, rr_cut = 1, depth_end = 2 };
struct Reservoir {
    Descriptor descriptor;
    Word selected_slot = null_slot;
    float normalizer = 0, target = 0;
    XYZ xyz;
    Status status = Status::ok;
    reuse::Scaled selected_h{.5f, 1};
};
inline Reservoir finalize(RYPATH_ENHANCED_THREAD const State &s, StopReason stop) {
    Reservoir r;
    r.descriptor = s.descriptor;
    if (s.status != Status::ok)
        r.status = s.status;
    else if (!valid_state(s))
        r.status = Status::invalid_state;
    else if (s.observed == 0 ||
             (stop == StopReason::depth_end && s.observed != s.descriptor.slot_count) ||
             (stop == StopReason::rr_cut &&
              ((s.observed & 1u) == 0 || s.observed >= s.descriptor.slot_count)) ||
             (stop != StopReason::physical_end && stop != StopReason::rr_cut &&
              stop != StopReason::depth_end))
        r.status = Status::incomplete;
    else {
        r.selected_slot = s.selected_slot;
        r.normalizer = s.score_sum;
        r.target = s.selected_target;
        r.xyz = s.selected_slot == null_slot ? XYZ{} : s.selected_xyz;
        r.selected_h = s.selected_h;
    }
    return r;
}
using XYZResult = cp::XYZResult;
inline XYZResult readout(float normalizer, XYZ xyz, float alpha) {
    if (!cp::finite(xyz) || !nonnegative(normalizer) || !nonnegative(alpha))
        return {{}, Status::invalid_value};
    if (normalizer > 0 && alpha > 0 && cp::zero(xyz))
        return {{}, Status::invalid_state};
    if (normalizer == 0 || alpha == 0) {
        if (normalizer == 0 && alpha == 0 && cp::zero(xyz))
            return {{}, Status::ok};
        return {{}, Status::invalid_state};
    }
    return cp::readout(normalizer, xyz, alpha);
}
inline XYZResult readout(RYPATH_ENHANCED_THREAD const Reservoir &r) {
    if (r.status != Status::ok)
        return {{}, r.status};
    if (!cp::valid_descriptor(r.descriptor) || r.descriptor.maximum_depth > 65 ||
        !valid_density(r.selected_h) ||
        (r.selected_slot == null_slot && !unit_density(r.selected_h)) ||
        (r.selected_slot != null_slot && r.selected_slot >= r.descriptor.slot_count) ||
        (r.selected_slot == null_slot) != (r.normalizer == 0))
        return {{}, Status::invalid_state};
    return rypath_enhanced_path::readout(r.normalizer, r.xyz, r.target);
}
struct PairInput {
    float source_Z, source_target, destination_target;
    float source_coefficient, destination_coefficient;
    float pair_weight = 1;
    bool source_present = true, destination_present = true;
};
// Structural presence is independent of a particular null reservoir draw.
// source_target==0 is legal for a complementary inverse target, including Z>0.
inline math::ScalarResult pair_score(PairInput p) {
    if (!nonnegative(p.source_Z) || !nonnegative(p.source_target) ||
        !nonnegative(p.destination_target) || !math::positive_normal(p.source_coefficient) ||
        !math::positive_normal(p.destination_coefficient) || !math::positive_normal(p.pair_weight))
        return {0, math::Status::invalid_candidate};
    if (!p.source_present || p.source_Z == 0 || p.destination_target == 0)
        return {0, math::Status::ok};
    const auto numerator =
        reuse::product(reuse::product(reuse::decompose_positive(p.source_Z),
                                      reuse::decompose_positive(p.source_coefficient)),
                       reuse::decompose_positive(p.destination_target));
    bool have = p.source_target > 0;
    reuse::Scaled denominator{.5f, 0};
    if (have)
        denominator = reuse::product(reuse::decompose_positive(p.source_coefficient),
                                     reuse::decompose_positive(p.source_target));
    if (p.destination_present) {
        const auto other = reuse::product(reuse::decompose_positive(p.destination_coefficient),
                                          reuse::decompose_positive(p.destination_target));
        denominator = have ? reuse::sum(denominator, other) : other;
        have = true;
    }
    if (!have)
        return {0, math::Status::invalid_target};
    return reuse::materialize(reuse::product(reuse::quotient(numerator, denominator),
                                             reuse::decompose_positive(p.pair_weight)));
}
// Canonical complement at its OWN point; inverse target zero means full share.
inline math::ScalarResult canonical_score(float Z, float current_target, float inverse_target,
                                          float current_coefficient, float other_coefficient,
                                          float pair_weight = 1) {
    if (!nonnegative(Z) || !nonnegative(current_target) || !nonnegative(inverse_target) ||
        !math::positive_normal(current_coefficient) || !math::positive_normal(other_coefficient) ||
        !math::positive_normal(pair_weight))
        return {0, math::Status::invalid_candidate};
    if (Z == 0)
        return {0, math::Status::ok};
    if (current_target == 0)
        return {0, math::Status::invalid_target};
    const auto current = reuse::product(reuse::decompose_positive(current_coefficient),
                                        reuse::decompose_positive(current_target));
    auto denominator = current;
    if (inverse_target > 0)
        denominator =
            reuse::sum(current, reuse::product(reuse::decompose_positive(other_coefficient),
                                               reuse::decompose_positive(inverse_target)));
    return reuse::materialize(reuse::product(
        reuse::quotient(reuse::product(reuse::decompose_positive(Z), current), denominator),
        reuse::decompose_positive(pair_weight)));
}
inline math::ScalarResult add_scores(float a, float b) {
    if (!nonnegative(a) || !nonnegative(b))
        return {0, math::Status::invalid_candidate};
    if (a == 0)
        return {b == 0 ? 0.f : b, math::Status::ok};
    if (b == 0)
        return {a, math::Status::ok};
    return reuse::add_scores(a, b);
}
// Law1: actual q=T/2^24, never the unrounded desired probability. The caller
// freezes the arrival probability BEFORE this gate for its E_d observation.
// Actual q is dyadic, but multiplying up to64 arbitrary24-bit q values rounds
// the stored FLOAT A. Relative error is bounded by gamma64=64u/(1-64u), u=2^-24;
// this is the declared floating approximation, not an exact finite-word HT law.
struct DiscoveryState {
    float probability = 1;
    Word gates = 0;
    Status status = Status::ok;
};
struct PreparedGate {
    DiscoveryState source;
    Word threshold = 0;
    float probability = 0;
    Status status = Status::ok;
};
inline PreparedGate prepare_gate(DiscoveryState s, float desired) {
    PreparedGate g;
    g.source = s;
    if (s.status != Status::ok)
        g.status = s.status;
    else if (!math::positive_normal(s.probability) || s.probability > 1 ||
             s.probability < 0x1p-32f || s.gates >= 64)
        g.status = Status::invalid_state;
    else if (!math::positive_normal(desired) || desired > 1)
        g.status = Status::invalid_value;
    else {
        const float units = desired * 0x1p24f;
        g.threshold = Word(units);
        if (float(g.threshold) < units)
            ++g.threshold;
        if (g.threshold == 0)
            g.threshold = 1;
        const float q = float(g.threshold) * 0x1p-24f;
        g.probability = s.probability * q;
        // gamma64 < 2^-17. This is a PRE-DRAW proposal choice, not clipping A.
        if (g.probability < (1.f + 0x1p-17f) * 0x1p-32f) {
            g.threshold = 1u << 24;
            g.probability = s.probability;
        }
    }
    return g;
}
struct GateResult {
    bool survived = false;
    float probability = 0;
    Status status = Status::ok;
};
inline GateResult commit_gate(RYPATH_ENHANCED_THREAD DiscoveryState &s,
                              RYPATH_ENHANCED_THREAD const PreparedGate &g, Word word) {
    if (s.status != Status::ok)
        return {false, 0, s.status};
    if (g.status != Status::ok) {
        s.status = g.status;
        return {false, 0, s.status};
    }
    if (!math::positive_normal(s.probability) || s.probability < 0x1p-32f || s.probability > 1 ||
        s.gates >= 64 || s.gates != g.source.gates ||
        math::bits(s.probability) != math::bits(g.source.probability) ||
        g.source.status != s.status || g.threshold == 0 || g.threshold > (1u << 24)) {
        s.status = Status::invalid_state;
        return {false, 0, s.status};
    }
    const float next = s.probability * (float(g.threshold) * 0x1p-24f);
    if (math::bits(next) != math::bits(g.probability) ||
        (g.threshold != (1u << 24) && next < (1.f + 0x1p-17f) * 0x1p-32f)) {
        s.status = Status::invalid_state;
        return {false, 0, s.status};
    }
    s.probability = next;
    ++s.gates;
    return {(word >> 8) < g.threshold, next, Status::ok};
}
static_assert(sizeof(State) == 60 && alignof(State) == 4 && sizeof(Reservoir) == 52,
              "Enhanced chart-density state CPU/Metal layout");
} // namespace rypath_enhanced_path
#undef RYPATH_ENHANCED_THREAD
