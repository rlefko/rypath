// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "hero_spectrum_shared.h"
#if defined(__METAL_VERSION__)
#define RYPATH_TAPE_THREAD thread
#else
#define RYPATH_TAPE_THREAD
#endif

// Bounded execution cache for the existing sequential FLOAT hero law. Geometry,
// tuple ownership, wavelength and exact evaluator-context identity are established
// by the caller; this tape neither samples nor performs a shift or visibility query.
namespace rypath_enhanced_suffix_tape {
namespace hero = rypath_hero;
using Word = unsigned;
using Vector = hero::Vector;
using Density = hero::Density;
enum : Word {
    version = 1,
    maximum_operations = 2,
    maximum_slot = 130,
    all_lanes = 15,
    maximum_density_exponent = 1u << 20,
    operation_count_mask = 3,
    nee = 1u << 2,
    mis_bypass = 1u << 3,
    visibility_known = 1u << 4,
    visible = 1u << 5,
    complete = 1u << 6,
    positive_capture = 1u << 7,
    delta_light = 1u << 8,
    known_flags = 0x1ff,
    terminal_prepared_shift = 16,
    terminal_response_shift = 20,
    terminal_pdf_shift = 24,
    known_coverage = 0x0fffffff,
    required_capture_flags = visibility_known | visible | complete | positive_capture
};
struct Operation {
    float numerator[4]{};
    float sampled_likelihood[4]{};
};
struct Entry {
    Operation operations[2];
    float factor_a[4]{}, factor_b[4]{}, cosine = 0;
    float bsdf_pdf[4]{}, light_pdf = 0;
    // Original represented endpoint direction. Query these exact bits only
    // after needs_immediate_pdf, then complete the pending result with its live PDF receipt.
    float immediate_direction[3]{};
    Word version = 0, flags = 0, coverage = 0, selected_slot = 0, pivot_event = 0;
    Word context_low = 0, context_high = 0;
};
static_assert(sizeof(Word) == 4 && sizeof(Operation) == 32 && sizeof(Entry) == 160 &&
                  alignof(Entry) == 4 && __builtin_offsetof(Entry, version) == 132,
              "Two-operation suffix tape scalar CPU/Metal ABI");
enum class Status : Word {
    ok,
    cache_miss,
    invalid_header,
    invalid_arithmetic,
    needs_immediate_pdf
};
struct Input {
    Vector beta{};
    Density density{}, pre_pivot_density{};
    Vector immediate_bsdf_pdf{};
    // Unready fields are not receipts; zero terminal values never request them.
    Word immediate_pdf_ready = 0, immediate_pdf_coverage = 0;
    Word selected_slot = 0, pivot_event = 0, context_low = 0, context_high = 0;
};
struct Result {
    // value is a completed spectral contribution only when status == ok.
    // Only needs_immediate_pdf retains the validated, unweighted terminal value.
    Vector value{}, beta{};
    Density density{}, mis_density{};
    Status status = Status::cache_miss;
    // early_zero covers throughput or terminal-value termination before MIS.
    Word operations_applied = 0, early_zero = 0;
};
inline Word operation_prepared_shift(Word operation) { return operation * 8; }
inline Word operation_response_shift(Word operation) { return operation * 8 + 4; }
inline Word lanes(Word coverage, Word shift) { return (coverage >> shift) & all_lanes; }
inline bool covered(Word coverage, Word shift, Word needed) {
    return (lanes(coverage, shift) & needed) == needed;
}
inline bool nonzero(float x) { return (hero::bits(x) & 0x7fffffffu) != 0; }
inline bool valid_density(RYPATH_TAPE_THREAD const Density &p) {
    bool positive = false;
    for (Word k = 0; k < 4; ++k) {
        const auto n = p.lane[k];
        const bool zero = hero::bits(n.fraction) == 0 && n.exponent == 0;
        if (!zero && !(hero::nonnegative_finite(n.fraction) && n.fraction >= .5f &&
                       n.fraction < 1 && n.exponent >= -int(maximum_density_exponent) &&
                       n.exponent <= int(maximum_density_exponent)))
            return false;
        positive |= !zero;
    }
    // Canonical lanes are nonnegative: their scaled sum is positive exactly
    // when at least one lane is positive. Validation needs no summation.
    return positive;
}
inline Word density_lanes(RYPATH_TAPE_THREAD const Density &p) {
    Word mask = 0;
    for (Word k = 0; k < 4; ++k)
        if (p.lane[k].fraction > 0)
            mask |= 1u << k;
    return mask;
}
inline Word beta_lanes(Vector beta) {
    Word mask = 0;
    for (Word k = 0; k < 4; ++k)
        if (nonzero(beta[k]))
            mask |= 1u << k;
    return mask;
}
inline Status header(RYPATH_TAPE_THREAD const Entry &entry, RYPATH_TAPE_THREAD const Input &input) {
    if (entry.version == 0)
        return Status::cache_miss; // Unpublished/unsupported capture: no other bytes are receipts.
    const Word count = entry.flags & operation_count_mask;
    const bool direct = (entry.flags & nee) != 0;
    const bool bypass = (entry.flags & mis_bypass) != 0;
    const bool delta = (entry.flags & delta_light) != 0;
    if (entry.version != version || (entry.flags & ~known_flags) ||
        (entry.coverage & ~known_coverage) ||
        (entry.flags & required_capture_flags) != required_capture_flags ||
        count > maximum_operations || entry.selected_slot > maximum_slot || entry.pivot_event < 1 ||
        entry.selected_slot / 2 <= entry.pivot_event ||
        count != entry.selected_slot / 2 - entry.pivot_event - 1 ||
        direct != bool(entry.selected_slot & 1u) || bypass != delta || (delta && !direct) ||
        entry.selected_slot != input.selected_slot || entry.pivot_event != input.pivot_event)
        return Status::invalid_header;
    if (entry.context_low != input.context_low || entry.context_high != input.context_high)
        return Status::cache_miss;
    return Status::ok;
}
// Both ready readout and deferred completion enter with validated MIS density,
// light PDF, and coverage. Keep the original hero_weight arithmetic in one place.
inline Result finish_mis(RYPATH_TAPE_THREAD const Entry &entry, Result out, Vector bsdf_pdf) {
    const bool direct = (entry.flags & nee) != 0;
    const Word pdf_needed = density_lanes(out.mis_density);
    Vector sampled{}, other{};
    for (Word k = 0; k < 4; ++k)
        if (pdf_needed & (1u << k)) {
            const float q = bsdf_pdf[k];
            if (!hero::nonnegative_finite(q)) {
                out.status = Status::invalid_arithmetic;
                return out;
            }
            sampled[k] = direct ? entry.light_pdf : q;
            other[k] = direct ? q : entry.light_pdf;
        }
    // Match production hero_weight, including its represented zero-competitor
    // convention. Do not evaluate 0/0 or first materialize a small MIS weight.
    if (hero::mixture(out.mis_density, other).fraction == 0)
        return out;
    const auto weighted = hero::balance_contribution(out.mis_density, out.value, sampled, other);
    if (!weighted.valid) {
        out.status = Status::invalid_arithmetic;
        return out;
    }
    out.value = weighted.value;
    return out;
}
// Full inlining fails the independent GPU state/MIS probe on the validated
// Metal 32023.883 toolchain. Keep this arithmetic boundary; the CPU law and
// the shader operation order remain identical.
#if defined(__METAL_VERSION__)
[[clang::noinline]]
#endif
inline Result readout(RYPATH_TAPE_THREAD const Entry &entry,
                      RYPATH_TAPE_THREAD const Input &input) {
    Result out{};
    out.status = header(entry, input);
    if (out.status != Status::ok)
        return out;
    out.beta = input.beta;
    out.density = input.density;
    for (Word k = 0; k < 4; ++k)
        if (!hero::nonnegative_finite(out.beta[k])) {
            out.status = Status::invalid_arithmetic;
            return out;
        }
    const Word count = entry.flags & operation_count_mask;
    for (Word op = 0; op <= count; ++op) {
        // Preserve physical zero-throughput termination before looking at the
        // next operation, its missing coverage, or any terminal/PDF coefficient.
        if (beta_lanes(out.beta) == 0) {
            out.early_zero = 1;
            return out;
        }
        if (op == count)
            break;
        if (!valid_density(out.density)) {
            out.status = Status::invalid_arithmetic;
            return out;
        }
        const Word needed = density_lanes(out.density) | beta_lanes(out.beta);
        if (!covered(entry.coverage, operation_prepared_shift(op), needed) ||
            !covered(entry.coverage, operation_response_shift(op), needed)) {
            out.status = Status::cache_miss;
            return out;
        }
        Vector numerator{}, pdf{};
        for (Word k = 0; k < 4; ++k)
            if (needed & (1u << k)) {
                numerator[k] = entry.operations[op].numerator[k];
                pdf[k] = entry.operations[op].sampled_likelihood[k];
                if (!hero::nonnegative_finite(numerator[k]) || !hero::nonnegative_finite(pdf[k])) {
                    out.status = Status::invalid_arithmetic;
                    return out;
                }
            }
        out.mis_density = out.density; // E uses P BEFORE the last ordinary update.
        const auto advanced = hero::advance(out.density, out.beta, numerator, pdf);
        if (!advanced.valid) {
            out.status = Status::invalid_arithmetic;
            return out;
        }
        out.beta = advanced.beta;
        out.density = advanced.density;
        ++out.operations_applied;
    }
    const bool direct = (entry.flags & nee) != 0;
    const bool bypass = (entry.flags & mis_bypass) != 0;
    const bool immediate = !direct && count == 0;
    const Word response = beta_lanes(out.beta);
    // prepare_event_bsdf precedes E/N, even when terminal value is zero. It
    // requires CURRENT atom-P OR beta lanes, independently of E's MIS prefix.
    if (!valid_density(out.density)) {
        out.status = Status::invalid_arithmetic;
        return out;
    }
    const Word prepared = density_lanes(out.density) | response;
    if (!covered(entry.coverage, terminal_prepared_shift, prepared) ||
        !covered(entry.coverage, terminal_response_shift, response)) {
        out.status = Status::cache_miss;
        return out;
    }
    if (direct && !hero::nonnegative_finite(entry.cosine)) {
        out.status = Status::invalid_arithmetic;
        return out;
    }
    for (Word k = 0; k < 4; ++k)
        if (response & (1u << k)) {
            const float a = entry.factor_a[k];
            float value = out.beta[k] * a;
            if (!hero::nonnegative_finite(a) || !hero::nonnegative_finite(value)) {
                out.status = Status::invalid_arithmetic;
                return out;
            }
            if (direct) {
                const float b = entry.factor_b[k];
                value = value * b;
                if (!hero::nonnegative_finite(b) || !hero::nonnegative_finite(value)) {
                    out.status = Status::invalid_arithmetic;
                    return out;
                }
                value = value * entry.cosine;
                if (!hero::nonnegative_finite(value)) {
                    out.status = Status::invalid_arithmetic;
                    return out;
                }
            }
            out.value[k] = value;
        }
    // Match endpoint/direct's physical zero-value branch before MIS, including
    // any unused live PRE-pivot P or not-yet-requested boundary PDF receipt.
    if (beta_lanes(out.value) == 0) {
        out.early_zero = 1;
        return out;
    }
    if (direct)
        out.mis_density = out.density;
    else if (immediate)
        out.mis_density = input.pre_pivot_density;
    if (bypass)
        return out; // Only a captured, visible N delta light can reach this branch.
    if (!valid_density(out.mis_density)) {
        out.status = Status::invalid_arithmetic;
        return out;
    }
    const Word pdf_needed = density_lanes(out.mis_density);
    if (!immediate && !covered(entry.coverage, terminal_pdf_shift, pdf_needed)) {
        out.status = Status::cache_miss;
        return out;
    }
    if (!hero::nonnegative_finite(entry.light_pdf)) {
        out.status = Status::invalid_arithmetic;
        return out;
    }
    if (immediate) {
        if (input.immediate_pdf_ready == 0) {
            // The caller evaluates the live pivot boundary query only on this
            // request, then completes the pending result with a covered receipt.
            out.status = Status::needs_immediate_pdf;
            return out;
        }
        if (input.immediate_pdf_ready != 1 || (input.immediate_pdf_coverage & ~all_lanes)) {
            out.status = Status::invalid_arithmetic;
            return out;
        }
        if ((input.immediate_pdf_coverage & pdf_needed) != pdf_needed) {
            out.status = Status::cache_miss;
            return out;
        }
    }
    Vector bsdf_pdf{};
    for (Word k = 0; k < 4; ++k)
        if (pdf_needed & (1u << k))
            bsdf_pdf[k] = immediate ? input.immediate_bsdf_pdf[k] : entry.bsdf_pdf[k];
    return finish_mis(entry, out, bsdf_pdf);
}
// Complete only the requested immediate-E MIS tail. The caller must retain the
// SAME immutable entry and pending result from readout: Result carries no tuple
// identity. No terminal products, throughput updates, or prefix reconstruction
// are repeated here; beta, density, and mis_density are retained unchanged.
inline Result complete_immediate(RYPATH_TAPE_THREAD const Entry &entry,
                                 RYPATH_TAPE_THREAD const Result &pending, Vector live_pdf,
                                 Word ready, Word coverage) {
    Result out = pending;
    Input identity{};
    identity.selected_slot = entry.selected_slot;
    identity.pivot_event = entry.pivot_event;
    identity.context_low = entry.context_low;
    identity.context_high = entry.context_high;
    out.status = header(entry, identity);
    if (out.status != Status::ok)
        return out;
    if ((entry.flags & (operation_count_mask | nee | mis_bypass)) != 0 ||
        pending.status != Status::needs_immediate_pdf || pending.operations_applied != 0 ||
        pending.early_zero != 0) {
        out.status = Status::invalid_header;
        return out;
    }
    if (!valid_density(out.mis_density) || !hero::nonnegative_finite(entry.light_pdf) ||
        beta_lanes(out.value) == 0) {
        out.status = Status::invalid_arithmetic;
        return out;
    }
    for (Word k = 0; k < 4; ++k)
        if (!hero::nonnegative_finite(out.value[k])) {
            out.status = Status::invalid_arithmetic;
            return out;
        }
    if (ready != 1 || (coverage & ~all_lanes)) {
        out.status = Status::invalid_arithmetic;
        return out;
    }
    const Word pdf_needed = density_lanes(out.mis_density);
    if ((coverage & pdf_needed) != pdf_needed) {
        out.status = Status::cache_miss;
        return out;
    }
    return finish_mis(entry, out, live_pdf);
}

} // namespace rypath_enhanced_suffix_tape
#undef RYPATH_TAPE_THREAD
