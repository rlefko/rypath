// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "reuse_mis_shared.h"

#if defined(__METAL_VERSION__)
#define RYPATH_CONTRIBUTION_THREAD thread
#else
#define RYPATH_CONTRIBUTION_THREAD
#endif

namespace rypath_contribution_path {
namespace math = rypath_reservoir;
namespace reuse = rypath_reuse_mis;
using Word = rypath_replay::Word;
#if defined(__METAL_VERSION__)
constant constexpr Word contract = 1;
#else
inline constexpr Word contract = 1;
#endif
enum class Status : Word {
    ok,
    invalid_descriptor,
    unsupported_depth,
    count_overflow,
    invalid_state,
    invalid_slot,
    invalid_value,
    arithmetic_failure,
    incomplete
};
enum class DepthMode : Word { finite = 1, unbounded = 2 };
enum class Kind : Word { endpoint = 0, direct = 1 };

// Counting measure over fixed slots, not uniform probability over those slots.
// E_d=2*d includes terminal environment/hit emission at scatter ordinal d;
// N_d=2*d+1 is direct lighting. E_0..E_D and N_0..N_(D-1) give 2*D+1 slots.
// N_D is absent. D=0 is one emission/environment atom, never unbounded depth.
// For a complete original replay U, q(j|U)=t_j/sum(t); the selected sample has
// UCW=sum(t)/t_j on uniform-U x counting-j. Thus E[UCW*F_j|U]=sum(F_j), with
// no slot-count divisor. F_j already includes the original technique/hero MIS.
// This is the real-arithmetic contract; binary32 WRS/readout have rounding error.
struct Descriptor {
    Word version = 0;
    DepthMode depth_mode = DepthMode::finite;
    Word maximum_depth = 0, slot_count = 0;
};
struct DescriptorResult {
    Descriptor value;
    Status status = Status::ok;
};
struct SlotResult {
    Word index = math::no_selection;
    Kind kind = Kind::endpoint;
    Word depth = 0;
    Status status = Status::ok;
};
struct XYZ {
    float x = 0, y = 0, z = 0;
};
inline DescriptorResult make_descriptor(DepthMode mode, Word depth) {
    if (mode == DepthMode::unbounded)
        return {{}, Status::unsupported_depth};
    if (mode != DepthMode::finite)
        return {{}, Status::invalid_descriptor};
    if (depth > (Word(0xffffffffu) - 1u) / 2u)
        return {{}, Status::count_overflow};
    return {{contract, mode, depth, 2u * depth + 1u}, Status::ok};
}
inline bool valid_descriptor(RYPATH_CONTRIBUTION_THREAD const Descriptor &d) {
    const auto expected = make_descriptor(d.depth_mode, d.maximum_depth);
    return expected.status == Status::ok && d.version == contract &&
           d.slot_count == expected.value.slot_count;
}
inline SlotResult slot(RYPATH_CONTRIBUTION_THREAD const Descriptor &d, Kind kind, Word depth) {
    if (!valid_descriptor(d))
        return {math::no_selection, kind, depth, Status::invalid_descriptor};
    if ((kind != Kind::endpoint && kind != Kind::direct) || depth > d.maximum_depth ||
        (kind == Kind::direct && depth == d.maximum_depth))
        return {math::no_selection, kind, depth, Status::invalid_slot};
    return {2u * depth + Word(kind), kind, depth, Status::ok};
}
inline SlotResult slot(RYPATH_CONTRIBUTION_THREAD const Descriptor &d, Word index) {
    if (!valid_descriptor(d))
        return {index, Kind::endpoint, 0, Status::invalid_descriptor};
    if (index >= d.slot_count)
        return {index, Kind::endpoint, 0, Status::invalid_slot};
    return {index, Kind(index & 1u), index / 2u, Status::ok};
}
inline Status numeric_status(math::Status status) {
    if (status == math::Status::ok)
        return Status::ok;
    if (status == math::Status::arithmetic_failure)
        return Status::arithmetic_failure;
    if (status == math::Status::count_overflow)
        return Status::count_overflow;
    if (status == math::Status::incomplete || status == math::Status::empty)
        return Status::incomplete;
    if (status == math::Status::invalid_state)
        return Status::invalid_state;
    return Status::invalid_value;
}
inline bool finite(XYZ xyz) {
    return math::finite(xyz.x) && math::finite(xyz.y) && math::finite(xyz.z);
}
inline bool zero(XYZ xyz) {
    return (math::bits(xyz.x) & 0x7fffffffu) == 0 && (math::bits(xyz.y) & 0x7fffffffu) == 0 &&
           (math::bits(xyz.z) & 0x7fffffffu) == 0;
}

struct State {
    Descriptor descriptor;
    float target_scale = 0;
    float score_sum = 0, selected_target = 0;
    Word processed = 0, selected_slot = math::no_selection;
    XYZ selected_xyz;
    Status status = Status::ok;
};
inline State begin(RYPATH_CONTRIBUTION_THREAD const Descriptor &d, float target_scale) {
    State state;
    state.descriptor = d;
    state.target_scale = target_scale;
    if (!valid_descriptor(d))
        state.status = Status::invalid_descriptor;
    else if (!math::positive_normal(target_scale))
        state.status = Status::invalid_value;
    return state;
}
inline bool valid_state(RYPATH_CONTRIBUTION_THREAD const State &s) {
    if (s.status != Status::ok || !valid_descriptor(s.descriptor) ||
        !math::positive_normal(s.target_scale) || s.processed > s.descriptor.slot_count)
        return false;
    if (s.processed == 0)
        return s.score_sum == 0 && s.selected_target == 0 &&
               s.selected_slot == math::no_selection && zero(s.selected_xyz);
    if (!math::positive_normal(s.score_sum) || !math::positive_normal(s.selected_target) ||
        s.selected_slot >= s.processed || !finite(s.selected_xyz))
        return false;
    const auto target = reuse::importance_target(s.selected_xyz.x, s.selected_xyz.y,
                                                 s.selected_xyz.z, s.target_scale);
    return target.status == math::Status::ok &&
           math::bits(target.value) == math::bits(s.selected_target);
}
inline bool same_state(RYPATH_CONTRIBUTION_THREAD const State &a,
                       RYPATH_CONTRIBUTION_THREAD const State &b) {
    return a.descriptor.version == b.descriptor.version &&
           a.descriptor.depth_mode == b.descriptor.depth_mode &&
           a.descriptor.maximum_depth == b.descriptor.maximum_depth &&
           a.descriptor.slot_count == b.descriptor.slot_count && a.processed == b.processed &&
           a.selected_slot == b.selected_slot && a.status == b.status &&
           math::bits(a.target_scale) == math::bits(b.target_scale) &&
           math::bits(a.score_sum) == math::bits(b.score_sum) &&
           math::bits(a.selected_target) == math::bits(b.selected_target) &&
           math::bits(a.selected_xyz.x) == math::bits(b.selected_xyz.x) &&
           math::bits(a.selected_xyz.y) == math::bits(b.selected_xyz.y) &&
           math::bits(a.selected_xyz.z) == math::bits(b.selected_xyz.z);
}

// A prior categorical selection of score S and a new score t form a two-input
// WRS merge. Rebase only this local selector; processed retains the TRUE slot
// count. This is not a way to import >32 inputs into the legacy reservoir proof:
// rounding accumulates over every contribution slot and needs its own envelope.
inline math::State selector(RYPATH_CONTRIBUTION_THREAD const State &s) {
    if (s.processed == 0)
        return {};
    return {s.score_sum, 1.f, 1, 0, math::Status::ok};
}
struct PreparedUpdate {
    State source;
    math::PreparedUpdate decision;
    XYZ xyz;
    float target = 0;
    Word index = math::no_selection;
    Status status = Status::ok;
};
inline PreparedUpdate prepare_update(RYPATH_CONTRIBUTION_THREAD const State &s, Word index, XYZ xyz,
                                     Word active) {
    PreparedUpdate out;
    out.source = s;
    out.xyz = xyz;
    out.index = index;
    if (s.status != Status::ok) {
        out.status = s.status;
        return out;
    }
    if (!valid_state(s)) {
        out.status = Status::invalid_state;
        return out;
    }
    // Exact stream order enforces accounting for inactive/black slots. No
    // omissions, duplicates, reordering or adaptive stopping are permitted.
    if (index != s.processed || index >= s.descriptor.slot_count) {
        out.status = Status::invalid_slot;
        return out;
    }
    if (active > 1 || !finite(xyz) || (active == 0 && !zero(xyz))) {
        out.status = Status::invalid_value;
        return out;
    }
    // Inactive slots still receive the actual positive importance floor.
    const auto target = reuse::importance_target(xyz.x, xyz.y, xyz.z, s.target_scale);
    out.status = numeric_status(target.status);
    if (out.status != Status::ok)
        return out;
    out.target = target.value;
    out.decision = math::prepare_update(selector(s), out.target, 1.f);
    out.status = numeric_status(out.decision.status);
    return out;
}
// Entropy is external and independent of U. A renderer must use a separate RNG
// domain keyed by immutable sample/submission identity and fixed slot, never
// queue position or payload. Reusing a transport draw does not meet this rule.
inline Status feed_random_word(RYPATH_CONTRIBUTION_THREAD PreparedUpdate &pending, Word word) {
    if (pending.status != Status::ok)
        return pending.status;
    pending.status = numeric_status(math::feed_random_word(pending.decision, word));
    return pending.status;
}
struct UpdateResult {
    bool selected = false;
    Status status = Status::ok;
};
inline UpdateResult commit_update(RYPATH_CONTRIBUTION_THREAD State &state,
                                  RYPATH_CONTRIBUTION_THREAD const PreparedUpdate &pending) {
    if (state.status != Status::ok)
        return {false, state.status};
    if (pending.status != Status::ok) {
        state.status = pending.status;
        return {false, state.status};
    }
    if (!valid_state(state) || !same_state(state, pending.source) ||
        pending.index != state.processed || pending.index >= state.descriptor.slot_count ||
        !finite(pending.xyz) || math::bits(pending.decision.target) != math::bits(1.f)) {
        state.status = Status::invalid_state;
        return {false, state.status};
    }
    const auto target =
        reuse::importance_target(pending.xyz.x, pending.xyz.y, pending.xyz.z, state.target_scale);
    if (target.status != math::Status::ok ||
        math::bits(target.value) != math::bits(pending.target)) {
        state.status = Status::invalid_state;
        return {false, state.status};
    }
    auto local = selector(state);
    const auto result = math::commit_update(local, pending.decision);
    if (result.status != math::Status::ok) {
        state.status = numeric_status(result.status);
        return {false, state.status};
    }
    state.score_sum = local.score_sum;
    ++state.processed; // descriptor bounds prove this cannot wrap.
    if (result.selected) {
        state.selected_slot = pending.index;
        state.selected_target = pending.target;
        state.selected_xyz = pending.xyz;
    }
    return {result.selected, Status::ok};
}
struct Reservoir {
    Descriptor descriptor;
    Word selected_slot = math::no_selection;
    float normalizer = 0, target = 0;
    XYZ xyz;
    Status status = Status::ok;
};
inline Reservoir finalize(RYPATH_CONTRIBUTION_THREAD const State &s) {
    if (s.status != Status::ok)
        return {{}, math::no_selection, 0, 0, {}, s.status};
    if (!valid_state(s))
        return {{}, math::no_selection, 0, 0, {}, Status::invalid_state};
    if (s.processed != s.descriptor.slot_count)
        return {{}, math::no_selection, 0, 0, {}, Status::incomplete};
    // Counting-slot normalizer Z=sum(t), UCW=Z/t_selected. NEVER legacy
    // reservoir::finalize(), /slot_count, or a count of the surviving leaves.
    return {s.descriptor,      s.selected_slot, s.score_sum,
            s.selected_target, s.selected_xyz,  Status::ok};
}
struct XYZResult {
    XYZ value;
    Status status = Status::ok;
};
inline XYZResult readout(float normalizer, XYZ xyz, float target) {
    const auto x = reuse::signed_contribution(normalizer, xyz.x, target);
    const auto y = reuse::signed_contribution(normalizer, xyz.y, target);
    const auto z = reuse::signed_contribution(normalizer, xyz.z, target);
    if (x.status != math::Status::ok)
        return {{}, numeric_status(x.status)};
    if (y.status != math::Status::ok)
        return {{}, numeric_status(y.status)};
    if (z.status != math::Status::ok)
        return {{}, numeric_status(z.status)};
    return {{x.value, y.value, z.value}, Status::ok};
}
inline XYZResult readout(RYPATH_CONTRIBUTION_THREAD const Reservoir &r) {
    if (r.status != Status::ok)
        return {{}, r.status};
    if (!valid_descriptor(r.descriptor) || r.selected_slot >= r.descriptor.slot_count)
        return {{}, Status::invalid_state};
    return readout(r.normalizer, r.xyz, r.target);
}

// This schema is metadata for (U,j). Future mutation/replay integration MUST
// retain j while mutating U unless a separate slot proposal is derived; replay
// must evaluate that same j, not draw another local contribution. No such
// renderer integration or hybrid/invertible shift chart is implemented here.
static_assert(sizeof(Descriptor) == 16 && sizeof(XYZ) == 12, "Contribution slot ABI");
} // namespace rypath_contribution_path
#undef RYPATH_CONTRIBUTION_THREAD
