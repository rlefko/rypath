// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "hero_spectrum_shared.h"
#if defined(__METAL_VERSION__)
#define RYPATH_SUFFIX_THREAD thread
#else
#define RYPATH_SUFFIX_THREAD
#endif

// Fixed, already validated spectral response coefficients. This helper neither
// proves wavelength/context/geometry compatibility nor performs a shift map.
// No OUTER path RR belongs in N/D; coating INTERNAL RR stays in its sampled N.
namespace rypath_enhanced_suffix {
namespace hero = rypath_hero;
using Number = hero::Number;
using Density = hero::Density;
using Vector = hero::Vector;
enum : unsigned { all_lanes = 15, maximum_exponent = 1u << 20 };
enum class Status : unsigned { ok, cache_miss, outside_support, invalid_state, invalid_arithmetic };
enum class Role : unsigned { atom, mis };
inline bool zero(Number n) {
    return (hero::bits(n.fraction) & 0x7fffffffu) == 0 && n.exponent == 0;
}
inline bool valid_number(Number n) {
    return zero(n) ||
           (hero::nonnegative_finite(n.fraction) && n.fraction >= .5f && n.fraction < 1 &&
            n.exponent >= -int(maximum_exponent) && n.exponent <= int(maximum_exponent));
}
inline bool positive(float x) { return (hero::bits(x) & 0x7fffffffu) != 0; }
struct Checked {
    Number value;
    Status status = Status::ok;
};
inline Checked multiply(Number a, Number b) {
    if (!valid_number(a) || !valid_number(b))
        return {{}, Status::invalid_arithmetic};
    // Valid input exponents are <=2^20, so the primitive's int addition and
    // normalization shift cannot overflow even before the result range check.
    const auto out = hero::multiply(a, b);
    return valid_number(out) ? Checked{out, Status::ok} : Checked{{}, Status::invalid_arithmetic};
}
inline Checked add(Number a, Number b) {
    if (!valid_number(a) || !valid_number(b))
        return {{}, Status::invalid_arithmetic};
    const auto out = hero::add(a, b);
    return valid_number(out) ? Checked{out, Status::ok} : Checked{{}, Status::invalid_arithmetic};
}
inline Checked divide(Number a, Number b) {
    if (!valid_number(a) || !valid_number(b) || zero(b))
        return {{}, Status::invalid_arithmetic};
    const auto out = hero::divide(a, b);
    return valid_number(out) ? Checked{out, Status::ok} : Checked{{}, Status::invalid_arithmetic};
}
inline Checked total(RYPATH_SUFFIX_THREAD const Density &p) {
    const auto a = rypath_enhanced_suffix::add(p.lane[0], p.lane[1]),
               b = rypath_enhanced_suffix::add(p.lane[2], p.lane[3]);
    if (a.status != Status::ok || b.status != Status::ok)
        return {{}, Status::invalid_arithmetic};
    return rypath_enhanced_suffix::add(a.value,
                                       b.value); // Preserve the retained hero pairwise association.
}
struct State {
    Number N[4];
    Density D_to_atom, D_to_MIS;
    unsigned N_coverage = 0, D_atom_coverage = 0, D_MIS_coverage = 0, hero_only = 0;
    Status status = Status::ok;
};
inline State begin(unsigned N_coverage = all_lanes, unsigned D_coverage = all_lanes) {
    State s{};
    if ((N_coverage | D_coverage) & ~all_lanes) {
        s.status = Status::invalid_state;
        return s;
    }
    s.N_coverage = N_coverage;
    s.D_atom_coverage = s.D_MIS_coverage = D_coverage;
    for (unsigned k = 0; k < 4; ++k) {
        if (N_coverage & (1u << k))
            s.N[k] = hero::number(1);
        if (D_coverage & (1u << k))
            s.D_to_atom.lane[k] = s.D_to_MIS.lane[k] = hero::number(1);
    }
    return s;
}
inline bool valid_state(RYPATH_SUFFIX_THREAD const State &s) {
    if (s.status != Status::ok || s.hero_only > 1 ||
        ((s.N_coverage | s.D_atom_coverage | s.D_MIS_coverage) & ~all_lanes))
        return false;
    for (unsigned k = 0; k < 4; ++k) {
        const unsigned bit = 1u << k;
        if (((s.N_coverage & bit) && !valid_number(s.N[k])) ||
            ((s.D_atom_coverage & bit) && !valid_number(s.D_to_atom.lane[k])) ||
            ((s.D_MIS_coverage & bit) && !valid_number(s.D_to_MIS.lane[k])))
            return false;
        if (s.hero_only && k &&
            (!(s.N_coverage & bit) || !(s.D_atom_coverage & bit) || !zero(s.N[k]) ||
             !zero(s.D_to_atom.lane[k])))
            return false;
    }
    return true;
}
inline Status append(RYPATH_SUFFIX_THREAD State &s, Vector numerator, Vector pdf,
                     unsigned N_coverage = all_lanes, unsigned D_coverage = all_lanes) {
    if (s.status != Status::ok)
        return s.status;
    if (!valid_state(s) || ((N_coverage | D_coverage) & ~all_lanes))
        return s.status = Status::invalid_state;
    for (unsigned k = 0; k < 4; ++k) {
        if (s.hero_only && k)
            continue; // Known killed channels, never read their undefined future material values.
        const unsigned bit = 1u << k;
        if (((N_coverage & bit) && !hero::nonnegative_finite(numerator[k])) ||
            ((D_coverage & bit) && !hero::nonnegative_finite(pdf[k])))
            return s.status = Status::invalid_arithmetic;
        if ((s.N_coverage & N_coverage & bit) != 0) {
            const auto n = rypath_enhanced_suffix::multiply(s.N[k], hero::number(numerator[k]));
            if (n.status != Status::ok)
                return s.status = n.status;
            s.N[k] = n.value;
        } else {
            s.N_coverage &= ~bit;
            s.N[k] = {};
        }
        if ((s.D_atom_coverage & D_coverage & bit) != 0) {
            const auto d =
                rypath_enhanced_suffix::multiply(s.D_to_atom.lane[k], hero::number(pdf[k]));
            if (d.status != Status::ok)
                return s.status = d.status;
            s.D_to_atom.lane[k] = d.value;
        } else {
            s.D_atom_coverage &= ~bit;
            s.D_to_atom.lane[k] = {};
        }
    }
    return Status::ok;
}
inline Status snapshot_mis(RYPATH_SUFFIX_THREAD State &s) {
    if (s.status != Status::ok)
        return s.status;
    if (!valid_state(s))
        return s.status = Status::invalid_state;
    s.D_to_MIS = s.D_to_atom;
    s.D_MIS_coverage = s.D_atom_coverage;
    return Status::ok;
}
inline Status collapse(RYPATH_SUFFIX_THREAD State &s) {
    if (s.status != Status::ok)
        return s.status;
    if (!valid_state(s))
        return s.status = Status::invalid_state;
    for (unsigned k = 1; k < 4; ++k) {
        s.N[k] = {};
        s.D_to_atom.lane[k] = {};
    }
    s.N_coverage |= 14u;
    s.D_atom_coverage |= 14u;
    s.hero_only = 1;
    // The old pre-collapse MIS snapshot is deliberately unchanged.
    return Status::ok;
}
inline Status coated(RYPATH_SUFFIX_THREAD State &s, float hidden_walk_weight) {
    if (s.status != Status::ok)
        return s.status;
    if (!valid_state(s) || !s.hero_only)
        return s.status = Status::invalid_state;
    if (!hero::nonnegative_finite(hidden_walk_weight))
        return s.status = Status::invalid_arithmetic;
    if (s.N_coverage & 1u) {
        const auto out = rypath_enhanced_suffix::multiply(s.N[0], hero::number(hidden_walk_weight));
        if (out.status != Status::ok)
            return s.status = out.status;
        s.N[0] = out.value;
    }
    return Status::ok; // D0*=1. Never multiply by the coating directional proxy.
}
struct DensityResult {
    Density value;
    Status status = Status::ok;
};
inline DensityResult density(RYPATH_SUFFIX_THREAD const State &s,
                             RYPATH_SUFFIX_THREAD const Density &prefix, Role role) {
    DensityResult out{};
    if (!valid_state(s) || (role != Role::atom && role != Role::mis)) {
        out.status = s.status != Status::ok ? s.status : Status::invalid_state;
        return out;
    }
    const unsigned coverage = role == Role::atom ? s.D_atom_coverage : s.D_MIS_coverage;
    for (unsigned k = 0; k < 4; ++k)
        if (!valid_number(prefix.lane[k])) {
            out.status = Status::invalid_arithmetic;
            return out;
        }
    for (unsigned k = 0; k < 4; ++k) {
        if (zero(prefix.lane[k]))
            continue;
        if (!(coverage & (1u << k))) {
            out.status = Status::cache_miss;
            return out;
        }
        const auto d = rypath_enhanced_suffix::multiply(
            prefix.lane[k], role == Role::atom ? s.D_to_atom.lane[k] : s.D_to_MIS.lane[k]);
        if (d.status != Status::ok) {
            out.status = d.status;
            return out;
        }
        out.value.lane[k] = d.value;
    }
    const auto sum = rypath_enhanced_suffix::total(out.value);
    out.status = sum.status != Status::ok ? sum.status
                 : zero(sum.value)        ? Status::outside_support
                                          : Status::ok;
    return out;
}
struct Recombined {
    Vector beta{};
    Density to_atom, to_MIS;
    Status status = Status::ok;
};
inline Recombined recombine(RYPATH_SUFFIX_THREAD const State &s,
                            RYPATH_SUFFIX_THREAD const Density &prefix, Vector beta,
                            bool include_MIS = true) {
    Recombined out{};
    if (!valid_state(s)) {
        out.status = s.status != Status::ok ? s.status : Status::invalid_state;
        return out;
    }
    for (unsigned k = 0; k < 4; ++k) {
        if (!hero::nonnegative_finite(beta[k])) {
            out.status = Status::invalid_arithmetic;
            return out;
        }
    }
    for (unsigned k = 0; k < 4; ++k) {
        if (positive(beta[k]) && !(s.N_coverage & (1u << k))) {
            out.status = Status::cache_miss;
            return out;
        }
    }
    const auto atom = density(s, prefix, Role::atom);
    if (atom.status != Status::ok) {
        out.status = atom.status;
        return out;
    }
    out.to_atom = atom.value;
    if (include_MIS) {
        const auto mis = density(s, prefix, Role::mis);
        if (mis.status != Status::ok) {
            out.status = mis.status;
            return out;
        }
        out.to_MIS = mis.value;
    }
    const auto before = rypath_enhanced_suffix::total(prefix),
               after = rypath_enhanced_suffix::total(out.to_atom);
    if (before.status != Status::ok || after.status != Status::ok || zero(before.value)) {
        out.status = Status::invalid_arithmetic;
        return out;
    }
    const auto ratio = rypath_enhanced_suffix::divide(before.value, after.value);
    if (ratio.status != Status::ok) {
        out.status = ratio.status;
        return out;
    }
    for (unsigned k = 0; k < 4; ++k) {
        if (!positive(beta[k]))
            continue;
        const auto numerator = rypath_enhanced_suffix::multiply(hero::number(beta[k]), s.N[k]);
        const auto result = numerator.status == Status::ok
                                ? rypath_enhanced_suffix::multiply(numerator.value, ratio.value)
                                : Checked{{}, numerator.status};
        if (result.status != Status::ok) {
            out.status = result.status;
            return out;
        }
        const auto materialized = hero::materialize(result.value);
        if (!materialized.valid) {
            out.status = Status::invalid_arithmetic;
            return out;
        }
        out.beta[k] = materialized.value;
    }
    return out;
}
static_assert(sizeof(State) == 116 && alignof(State) == 4,
              "Spectral suffix N/D, MIS snapshot and coverage word ABI");
} // namespace rypath_enhanced_suffix
#undef RYPATH_SUFFIX_THREAD
