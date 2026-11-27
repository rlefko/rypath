// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_suffix_tape_shared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
namespace t = rypath_enhanced_suffix_tape;
namespace h = rypath_hero;
using V = std::array<long double, 4>;
unsigned checks = 0;
void need(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
h::Density density(h::Vector p) {
    h::Density out{};
    for (unsigned k = 0; k < 4; ++k)
        out.lane[k] = h::number(p[k]);
    return out;
}
long double number(h::Number p) {
    return std::ldexp(static_cast<long double>(p.fraction), p.exponent);
}
long double sum(V v) { return (v[0] + v[1]) + (v[2] + v[3]); }
V independent_density(const h::Density &p) {
    V out{};
    for (unsigned k = 0; k < 4; ++k)
        out[k] = number(p.lane[k]);
    return out;
}
struct Oracle {
    V value{}, beta{}, P{}, mis{};
};
// Independent rational/long-double prefix arithmetic, with an explicit binary32
// beta materialization at each event. Terminal products preserve FLOAT ordering;
// the final balance weight is evaluated in long double, not with the tape helper.
Oracle oracle(const t::Entry &e, const t::Input &input) {
    Oracle out{};
    out.P = independent_density(input.density);
    for (unsigned k = 0; k < 4; ++k)
        out.beta[k] = input.beta[k];
    const auto count = e.flags & t::operation_count_mask;
    for (unsigned op = 0; op < count; ++op) {
        bool positive = false;
        for (auto b : out.beta)
            positive |= b > 0;
        if (!positive)
            return out;
        out.mis = out.P;
        const auto before = sum(out.P);
        for (unsigned k = 0; k < 4; ++k)
            out.P[k] *= e.operations[op].sampled_likelihood[k];
        const auto after = sum(out.P);
        need(after > 0, "Independent oracle has positive sampled likelihood");
        for (unsigned k = 0; k < 4; ++k)
            out.beta[k] =
                static_cast<float>(out.beta[k] * e.operations[op].numerator[k] * before / after);
    }
    const bool direct = (e.flags & t::nee) != 0;
    const bool bypass = (e.flags & t::mis_bypass) != 0;
    const bool immediate = !direct && count == 0;
    if (direct)
        out.mis = out.P;
    else if (immediate)
        out.mis = independent_density(input.pre_pivot_density);
    for (unsigned k = 0; k < 4; ++k) {
        float value = static_cast<float>(out.beta[k]) * e.factor_a[k];
        if (direct) {
            value = value * e.factor_b[k];
            value = value * e.cosine;
        }
        out.value[k] = value;
    }
    if (bypass)
        return out;
    long double sampled = 0, other = 0;
    for (unsigned k = 0; k < 4; ++k) {
        const auto q = immediate ? input.immediate_bsdf_pdf[k] : e.bsdf_pdf[k];
        sampled += out.mis[k] * (direct ? e.light_pdf : q);
        other += out.mis[k] * (direct ? q : e.light_pdf);
    }
    if (other != 0)
        for (auto &v : out.value)
            v *= sampled / (sampled + other);
    return out;
}
bool agrees(h::Vector actual, V expected) {
    for (unsigned k = 0; k < 4; ++k) {
        const long double a = actual[k], b = expected[k];
        const auto tolerance =
            64 * std::numeric_limits<float>::epsilon() * std::max(std::abs(a), std::abs(b)) +
            std::numeric_limits<float>::denorm_min();
        if (!std::isfinite(a) || std::abs(a - b) > tolerance)
            return false;
    }
    return true;
}
t::Input input() {
    t::Input x{};
    x.beta = {.5f, 1.5f, 2, 3};
    x.density = density({1, 2, 3, 4});
    x.pre_pivot_density = density({4, 3, 2, 1});
    x.immediate_bsdf_pdf = {.125f, .75f, .25f, 1};
    x.immediate_pdf_ready = 1;
    x.immediate_pdf_coverage = t::all_lanes;
    x.pivot_event = 1;
    x.context_low = 0x12345678u;
    x.context_high = 0x9abcdef0u;
    return x;
}
t::Entry entry(unsigned count, bool direct, bool bypass = false) {
    t::Entry e{};
    e.version = t::version;
    e.flags = count | t::required_capture_flags | (direct ? t::nee : 0) |
              (bypass ? t::mis_bypass | t::delta_light : 0);
    e.coverage = t::known_coverage;
    e.pivot_event = 1;
    e.selected_slot = 2 * (e.pivot_event + count + 1) + unsigned(direct);
    e.context_low = input().context_low;
    e.context_high = input().context_high;
    const h::Vector n[2]{{.5f, 1.25f, 2, 3}, {1.5f, .75f, .25f, 2}};
    const h::Vector p[2]{{.25f, .5f, .75f, 1}, {.5f, 1, .125f, .25f}};
    const h::Vector a{1, 2, .5f, 3}, b{.25f, 1.5f, 2, .75f}, q{.25f, .5f, .75f, 1};
    for (unsigned k = 0; k < 4; ++k) {
        for (unsigned op = 0; op < 2; ++op) {
            e.operations[op].numerator[k] = n[op][k];
            e.operations[op].sampled_likelihood[k] = p[op][k];
        }
        e.factor_a[k] = a[k];
        e.factor_b[k] = direct ? b[k] : 0;
        e.bsdf_pdf[k] = q[k];
    }
    e.cosine = direct ? .5f : 0;
    e.light_pdf = .3f;
    e.immediate_direction[2] = 1;
    return e;
}
t::Input for_entry(const t::Entry &e) {
    auto x = input();
    x.selected_slot = e.selected_slot;
    return x;
}
void ordinary_oracles() {
    for (unsigned count = 0; count <= 2; ++count)
        for (bool direct : {false, true}) {
            const auto e = entry(count, direct);
            const auto x = for_entry(e);
            const auto result = t::readout(e, x);
            const auto expected = oracle(e, x);
            need(result.status == t::Status::ok && result.operations_applied == count &&
                     !result.early_zero,
                 "Zero/one/two sequential ordinary operations are supported");
            need(agrees(result.value, expected.value),
                 "Independent long-double terminal MIS oracle");
            need(agrees(result.beta, expected.beta), "Independent sequential FLOAT beta oracle");
            const auto P = independent_density(result.density),
                       M = independent_density(result.mis_density);
            for (unsigned k = 0; k < 4; ++k) {
                need(std::abs(P[k] - expected.P[k]) < 1e-12L,
                     "Dyadic suffix prefix exact rational oracle");
                need(std::abs(M[k] - expected.mis[k]) < 1e-12L,
                     "E before-last versus N post-ops prefix");
            }
        }
}
void headers_and_identity() {
    auto e = entry(1, false);
    auto x = for_entry(e);
    auto absent = e;
    absent.version = 0;
    absent.flags = absent.coverage = ~0u;
    absent.operations[0].numerator[0] = std::numeric_limits<float>::quiet_NaN();
    need(t::readout(absent, x).status == t::Status::cache_miss,
         "Version0 ignores all poisoned payload");
    for (unsigned bad = 0; bad < 9; ++bad) {
        auto malformed = e;
        switch (bad) {
        case 0:
            malformed.version = 2;
            break;
        case 1:
            malformed.flags |= 1u << 31;
            break;
        case 2:
            malformed.flags &= ~t::complete;
            break;
        case 3:
            malformed.flags = (malformed.flags & ~3u) | 3u;
            break;
        case 4:
            malformed.selected_slot = 131;
            break;
        case 5:
            malformed.pivot_event = 0;
            break;
        case 6:
            malformed.flags |= t::mis_bypass | t::delta_light;
            break;
        case 7:
            malformed.flags &= ~t::visible;
            break;
        default:
            malformed.coverage |= 1u << 31;
            break;
        }
        need(t::readout(malformed, x).status == t::Status::invalid_header,
             "Malformed complete capture/header fails visibly");
    }
    auto changed = x;
    ++changed.selected_slot;
    need(t::readout(e, changed).status == t::Status::invalid_header,
         "Owned selected-slot mismatch is corruption");
    changed = x;
    ++changed.pivot_event;
    need(t::readout(e, changed).status == t::Status::invalid_header,
         "Owned pivot mismatch is corruption");
    changed = x;
    ++changed.context_high;
    need(t::readout(e, changed).status == t::Status::cache_miss,
         "Full64-bit context high word controls hit");
    changed = x;
    ++changed.context_low;
    need(t::readout(e, changed).status == t::Status::cache_miss,
         "Context low word mismatch replays");
    auto too_long = entry(3, true);
    need(t::readout(too_long, for_entry(too_long)).status == t::Status::invalid_header,
         "A complete third-operation tape is unsupported, never indexed");
    auto boundary = entry(0, false);
    boundary.pivot_event = 64;
    boundary.selected_slot = 130;
    auto bx = for_entry(boundary);
    bx.pivot_event = 64;
    need(t::readout(boundary, bx).status == t::Status::ok,
         "Largest finite-depth selected slot130 with consistent pivot is supported");
    e.flags = (e.flags & ~3u) | 2u;
    need(t::readout(e, x).status == t::Status::invalid_header,
         "Count must match j/2-pivot-1 exactly");
    auto n = entry(0, true, true);
    auto nx = for_entry(n);
    n.flags &= ~t::delta_light;
    need(t::readout(n, nx).status == t::Status::invalid_header,
         "N bypass requires delta light certificate");
}
void immediate_boundary() {
    auto e = entry(0, false);
    auto x = for_entry(e);
    // PRE-pivot lane0 needs the live query but neither CURRENT atom-P nor
    // beta uses it. It therefore needs no terminal-event preparation receipt.
    x.beta[0] = 0;
    x.density.lane[0] = {};
    e.coverage &= ~((1u << t::terminal_prepared_shift) | (1u << t::terminal_response_shift));
    e.coverage &= ~(15u << t::terminal_pdf_shift);
    for (float &q : e.bsdf_pdf)
        q = std::numeric_limits<float>::quiet_NaN();
    auto result = t::readout(e, x);
    need(result.status == t::Status::ok && agrees(result.value, oracle(e, x).value),
         "Immediate E uses ready live PDF and PRE-pivot P without cached q coverage");
    auto wrong = x;
    wrong.pre_pivot_density = x.density;
    need(!agrees(result.value, oracle(e, wrong).value),
         "Wrong POST-pivot P must fail immediate-E oracle");
    x.immediate_pdf_ready = 0;
    need(t::readout(e, x).status == t::Status::needs_immediate_pdf,
         "Only a nonzero immediate term requests a deferred live query");
    x.immediate_pdf_ready = 1;
    x.immediate_pdf_coverage = 7;
    need(t::readout(e, x).status == t::Status::cache_miss,
         "Immediate query needs all live PRE-pivot P lanes");
    x.immediate_pdf_coverage = 15;
    x.immediate_bsdf_pdf[0] = -1;
    need(t::readout(e, x).status == t::Status::invalid_arithmetic,
         "Consumed negative live q is an error");
    x = for_entry(e);
    e.coverage |= (15u << t::terminal_prepared_shift) | (15u << t::terminal_response_shift);
    e.light_pdf = std::numeric_limits<float>::infinity();
    need(t::readout(e, x).status == t::Status::invalid_arithmetic,
         "Immediate E still validates cached scalar light PDF");
}
void coverage_and_lane_semantics() {
    auto e = entry(1, true);
    auto x = for_entry(e);
    x.density = density({1, 0, 2, 0});
    x.beta = {0, 3, 1, 0};
    // Lane1 has zero likelihood but positive throughput; lane0 the converse.
    for (unsigned missing : {0u, 1u, 2u}) {
        auto uncovered = e;
        uncovered.coverage &= ~(1u << (t::operation_prepared_shift(0) + missing));
        need(t::readout(uncovered, x).status == t::Status::cache_miss,
             "Op coverage includes likelihood OR throughput lanes");
        uncovered = e;
        uncovered.coverage &= ~(1u << (t::operation_response_shift(0) + missing));
        need(t::readout(uncovered, x).status == t::Status::cache_miss,
             "Missing numerator response is a miss");
    }
    e.operations[0].numerator[3] = e.operations[0].sampled_likelihood[3] =
        std::numeric_limits<float>::quiet_NaN();
    e.factor_a[3] = e.factor_b[3] = e.bsdf_pdf[3] = std::numeric_limits<float>::quiet_NaN();
    need(t::readout(e, x).status == t::Status::ok,
         "Unneeded inactive lane fields are not evaluated");
    auto missing_pdf = e;
    missing_pdf.coverage &= ~(1u << t::terminal_pdf_shift);
    need(t::readout(missing_pdf, x).status == t::Status::cache_miss,
         "Zero-beta positive-P lane still needs terminal PDF coverage");
    e.bsdf_pdf[0] = std::numeric_limits<float>::quiet_NaN();
    need(t::readout(e, x).status == t::Status::invalid_arithmetic,
         "Zero-beta positive-P lane still validates consumed terminal PDF");
    e = entry(0, true);
    x = for_entry(e);
    x.density = {};
    need(t::readout(e, x).status == t::Status::invalid_arithmetic,
         "All-zero likelihood with positive beta is invalid");
    for (unsigned shift :
         {t::terminal_prepared_shift, t::terminal_response_shift, t::terminal_pdf_shift}) {
        auto missing = entry(0, true);
        missing.coverage &= ~(1u << shift);
        need(t::readout(missing, for_entry(missing)).status == t::Status::cache_miss,
             "Missing terminal preparation/response/PDF coverage replays");
    }
}
void zero_termination_and_order() {
    auto e = entry(2, true, true);
    auto x = for_entry(e);
    x.beta = {};
    x.density.lane[0].fraction = std::numeric_limits<float>::quiet_NaN();
    e.coverage = 0;
    for (auto &op : e.operations)
        for (unsigned k = 0; k < 4; ++k)
            op.numerator[k] = op.sampled_likelihood[k] = std::numeric_limits<float>::quiet_NaN();
    e.factor_a[0] = e.cosine = std::numeric_limits<float>::quiet_NaN();
    auto result = t::readout(e, x);
    need(result.status == t::Status::ok && result.early_zero && result.operations_applied == 0,
         "Zero throughput stops before future density/coverage/terminal reads");
    e = entry(2, true, true);
    x = for_entry(e);
    for (unsigned k = 0; k < 4; ++k) {
        x.beta[k] = std::numeric_limits<float>::min();
        e.operations[0].numerator[k] = 1e-20f;
        e.operations[1].numerator[k] = 1e20f;
        e.operations[0].sampled_likelihood[k] = e.operations[1].sampled_likelihood[k] = 1;
        e.factor_a[k] = e.factor_b[k] = 1;
    }
    e.cosine = 1;
    result = t::readout(e, x);
    need(result.status == t::Status::ok && result.early_zero && result.operations_applied == 1 &&
             agrees(result.value, oracle(e, x).value),
         "Ordinary FLOAT underflow terminates before a later bright operation");
    auto reversed = e;
    reversed.operations[0] = e.operations[1];
    reversed.operations[1] = e.operations[0];
    need(!agrees(t::readout(reversed, x).value, oracle(e, x).value),
         "Wrong operation order must fail independent sequential-rounding oracle");
    e.operations[1].numerator[0] = std::numeric_limits<float>::infinity();
    e.coverage &= ~(255u << 8);
    e.coverage &= 0xffffu;
    need(t::readout(e, x).status == t::Status::ok,
         "Zero after first op does not read missing/poisoned later receipt");
}
void zero_terminal_branch_order() {
    const float poison = std::numeric_limits<float>::quiet_NaN();
    for (unsigned count = 0; count <= 2; ++count)
        for (bool direct : {false, true}) {
            auto e = entry(count, direct);
            auto x = for_entry(e);
            // Valid ordinary operations still run; the observed terminal value
            // vanishes before any terminal MIS density/PDF query is executed.
            for (unsigned k = 0; k < 4; ++k) {
                e.factor_a[k] = 0;
                e.bsdf_pdf[k] = x.immediate_bsdf_pdf[k] = poison;
                x.pre_pivot_density.lane[k].fraction = poison;
            }
            e.light_pdf = poison;
            e.coverage &= ~(15u << t::terminal_pdf_shift);
            x.immediate_pdf_ready = x.immediate_pdf_coverage = ~0u;
            const auto zero = t::readout(e, x);
            need(zero.status == t::Status::ok && zero.early_zero &&
                     zero.operations_applied == count && zero.value == h::Vector{},
                 "Zero terminal E/NEE skips poisoned MIS/PDF/ready receipts after ordinary "
                 "operations");
        }
    auto e = entry(0, false);
    auto x = for_entry(e);
    x.beta = {1, 0, 0, 0};
    x.density = density({1, 0, 0, 0});
    for (auto &n : x.pre_pivot_density.lane)
        n.fraction = poison;
    e.factor_a[0] = 0;
    // No later-MIS-only prepared lanes are required by a zero terminal term.
    e.coverage = (1u << t::terminal_prepared_shift) | (1u << t::terminal_response_shift);
    e.light_pdf = poison;
    x.immediate_pdf_ready = 2;
    x.immediate_pdf_coverage = 1u << 31;
    const auto zero = t::readout(e, x);
    need(zero.status == t::Status::ok && zero.value == h::Vector{},
         "Zero Le needs CURRENT atom-P/beta preparation, not PRE-pivot MIS preparation");
    e = entry(0, true);
    x = for_entry(e);
    for (float &b : e.factor_b)
        b = 0;
    for (float &q : e.bsdf_pdf)
        q = poison;
    e.light_pdf = poison;
    need(t::readout(e, x).status == t::Status::ok,
         "Zero N value skips unexecuted technique PDF receipts");
    e = entry(0, false);
    x = for_entry(e);
    x.beta[0] = 0; // Positive CURRENT P lane0 still participates in event preparation.
    e.coverage &= ~(1u << t::terminal_prepared_shift);
    for (float &a : e.factor_a)
        a = 0;
    need(t::readout(e, x).status == t::Status::cache_miss,
         "Even zero E requires CURRENT atom-P preparation before terminal evaluation");
    e = entry(0, false);
    x = for_entry(e);
    x.immediate_pdf_ready = 0;
    x.immediate_pdf_coverage = ~0u; // Not a receipt until the deferred query is ready.
    auto deferred = t::readout(e, x);
    need(deferred.status == t::Status::needs_immediate_pdf && !deferred.early_zero,
         "Nonzero E requests boundary work without consuming unready receipt fields");
    x.immediate_pdf_ready = 1;
    x.immediate_pdf_coverage = 15;
    deferred = t::readout(e, x);
    need(deferred.status == t::Status::ok && agrees(deferred.value, oracle(e, x).value),
         "Resubmitting the deferred query preserves the independent MIS oracle");
}
bool exact_vector(h::Vector a, h::Vector b) {
    for (unsigned k = 0; k < 4; ++k)
        if (h::bits(a[k]) != h::bits(b[k]))
            return false;
    return true;
}
bool exact_density(const h::Density &a, const h::Density &b) {
    for (unsigned k = 0; k < 4; ++k)
        if (h::bits(a.lane[k].fraction) != h::bits(b.lane[k].fraction) ||
            a.lane[k].exponent != b.lane[k].exponent)
            return false;
    return true;
}
void deferred_completion() {
    const float poison = std::numeric_limits<float>::quiet_NaN();
    for (unsigned variant = 0; variant < 6; ++variant) {
        auto e = entry(0, false);
        auto x = for_entry(e);
        if (variant == 1 || variant == 2) {
            e.light_pdf = 0;
            if (variant == 2)
                x.immediate_bsdf_pdf = {};
        } else if (variant == 3) {
            // MIS P needs lane0 even though CURRENT P and beta do not.
            x.beta[0] = 0;
            x.density.lane[0] = {};
            x.pre_pivot_density.lane[3] = {};
            x.immediate_bsdf_pdf[3] = poison; // Unused q is never consumed.
            x.immediate_pdf_coverage = 7;
            e.coverage &=
                ~((1u << t::terminal_prepared_shift) | (1u << t::terminal_response_shift));
        } else if (variant == 4) {
            for (unsigned k = 0; k < 4; ++k) {
                x.pre_pivot_density.lane[k].exponent = k & 1 ? -900000 : 900000;
                x.beta[k] = std::numeric_limits<float>::denorm_min();
                e.factor_a[k] = 1;
            }
        } else if (variant == 5) {
            x.beta = {1, 1, 1, 1};
            x.immediate_bsdf_pdf.fill(std::numeric_limits<float>::min());
            e.light_pdf = std::numeric_limits<float>::max();
            for (auto &a : e.factor_a)
                a = std::numeric_limits<float>::max();
        }
        const auto e_before = e;
        const auto x_before = x;
        const auto ready = t::readout(e, x);
        x.immediate_pdf_ready = 0;
        const auto unready_before = x;
        const auto pending = t::readout(e, x);
        const auto pending_before = pending;
        const auto resumed =
            t::complete_immediate(e, pending, x.immediate_bsdf_pdf, 1, x.immediate_pdf_coverage);
        need(ready.status == t::Status::ok && pending.status == t::Status::needs_immediate_pdf &&
                 resumed.status == t::Status::ok,
             "Ready and deferred immediate E both finish successfully");
        need(exact_vector(ready.value, resumed.value) && exact_vector(ready.beta, resumed.beta) &&
                 exact_density(ready.density, resumed.density) &&
                 exact_density(ready.mis_density, resumed.mis_density) &&
                 ready.operations_applied == resumed.operations_applied &&
                 ready.early_zero == resumed.early_zero,
             "Deferred MIS tail exactly matches ready readout, including extreme FLOAT cases");
        need(exact_vector(resumed.beta, pending.beta) &&
                 exact_density(resumed.density, pending.density) &&
                 exact_density(resumed.mis_density, pending.mis_density),
             "Completion preserves the first readout throughput and both densities");
        need(std::memcmp(&e, &e_before, sizeof(e)) == 0 &&
                 std::memcmp(&x, &unready_before, sizeof(x)) == 0 &&
                 std::memcmp(&pending, &pending_before, sizeof(pending)) == 0,
             "Readout and completion leave entry, input, and pending receipt unchanged");
        if (variant < 4)
            need(agrees(resumed.value, oracle(e, x_before).value),
                 "Deferred result retains independent long-double MIS agreement");
    }
    auto e = entry(0, false);
    auto x = for_entry(e);
    x.immediate_pdf_ready = 0;
    const auto pending = t::readout(e, x);
    for (float bad_q : {-1.f, poison, std::numeric_limits<float>::infinity()}) {
        auto pdf = x.immediate_bsdf_pdf;
        pdf[0] = bad_q;
        need(t::complete_immediate(e, pending, pdf, 1, 15).status == t::Status::invalid_arithmetic,
             "Completion rejects every invalid consumed live q");
    }
    need(t::complete_immediate(e, pending, x.immediate_bsdf_pdf, 1, 7).status ==
             t::Status::cache_miss,
         "Completion misses on missing PRE-pivot P PDF coverage");
    for (unsigned ready : {0u, 2u})
        need(t::complete_immediate(e, pending, x.immediate_bsdf_pdf, ready, 15).status ==
                 t::Status::invalid_arithmetic,
             "Completion requires a ready live PDF receipt");
    need(t::complete_immediate(e, pending, x.immediate_bsdf_pdf, 1, 31).status ==
             t::Status::invalid_arithmetic,
         "Completion rejects unknown coverage bits");
    for (unsigned variant = 0; variant < 8; ++variant) {
        auto bad = pending;
        switch (variant) {
        case 0:
            bad.status = t::Status::ok;
            break;
        case 1:
            bad.status = t::Status::cache_miss;
            break;
        case 2:
            bad.operations_applied = 1;
            break;
        case 3:
            bad.early_zero = 1;
            break;
        case 4:
            bad.mis_density = {};
            break;
        case 5:
            bad.value = {};
            break;
        case 6:
            bad.value[0] = poison;
            break;
        case 7:
            bad.mis_density.lane[0].fraction = poison;
            break;
        }
        const auto result = t::complete_immediate(e, bad, x.immediate_bsdf_pdf, 1, 15);
        need(result.status ==
                 (variant < 4 ? t::Status::invalid_header : t::Status::invalid_arithmetic),
             "Completion rejects a malformed or nonpending result");
    }
    for (const auto &malformed : {entry(1, false), entry(0, true), entry(0, true, true)})
        need(t::complete_immediate(malformed, pending, x.immediate_bsdf_pdf, 1, 15).status ==
                 t::Status::invalid_header,
             "Only an immediate zero-operation E can complete a pending result");
    e.flags |= 1u << 31;
    need(t::complete_immediate(e, pending, x.immediate_bsdf_pdf, 1, 15).status ==
             t::Status::invalid_header,
         "Completion validates the immutable entry header");
    e = entry(0, false);
    e.light_pdf = poison;
    need(t::complete_immediate(e, pending, x.immediate_bsdf_pdf, 1, 15).status ==
             t::Status::invalid_arithmetic,
         "Completion rejects invalid scalar light PDF");
}
void terminal_mis_and_failures() {
    for (bool direct : {false, true}) {
        auto e = entry(1, direct);
        auto x = for_entry(e);
        for (float &q : e.bsdf_pdf)
            q = 0;
        e.light_pdf = 0;
        auto result = t::readout(e, x);
        need(result.status == t::Status::ok && agrees(result.value, oracle(e, x).value),
             "Zero competitor including both-zero techniques returns the original value");
        if (!direct)
            e.light_pdf = 1;
        else
            for (float &q : e.bsdf_pdf)
                q = 1;
        result = t::readout(e, x);
        need(result.status == t::Status::ok && result.value == h::Vector{},
             "Zero sampled mixture with positive competitor gives zero weighted contribution");
    }
    for (bool direct : {false, true}) {
        auto scaled = entry(1, direct);
        auto live = for_entry(scaled);
        live.beta = {1, 1, 1, 1};
        for (unsigned k = 0; k < 4; ++k) {
            scaled.operations[0].numerator[k] = scaled.operations[0].sampled_likelihood[k] = 1;
            scaled.factor_a[k] = std::numeric_limits<float>::max();
            scaled.factor_b[k] = direct ? 1 : 0;
            scaled.bsdf_pdf[k] =
                direct ? std::numeric_limits<float>::max() : std::numeric_limits<float>::min();
        }
        scaled.cosine = direct ? 1 : 0;
        scaled.light_pdf =
            direct ? std::numeric_limits<float>::min() : std::numeric_limits<float>::max();
        const auto weighted = t::readout(scaled, live);
        need(weighted.status == t::Status::ok && weighted.value[0] > 0 &&
                 agrees(weighted.value, oracle(scaled, live).value),
             "Sub-FLOAT MIS weight is applied before FLOAT materialization of a bright term");
    }
    auto e = entry(0, true, true);
    auto x = for_entry(e);
    e.coverage &= ~(15u << t::terminal_pdf_shift);
    e.light_pdf = std::numeric_limits<float>::quiet_NaN();
    for (float &q : e.bsdf_pdf)
        q = std::numeric_limits<float>::quiet_NaN();
    need(t::readout(e, x).status == t::Status::ok,
         "Visible N delta bypass does not read unused PDFs");
    e.factor_a[0] = std::numeric_limits<float>::max();
    x.beta[0] = 2;
    e.factor_b[0] = 0;
    need(t::readout(e, x).status == t::Status::invalid_arithmetic,
         "Terminal intermediate overflow fails before a later zero can conceal it");
    e = entry(0, true, true);
    x = for_entry(e);
    x.beta = {std::numeric_limits<float>::min(), 0, 0, 0};
    e.factor_a[0] = 1e-20f;
    e.factor_b[0] = 1e20f;
    e.cosine = 1;
    const auto ordered = t::readout(e, x);
    need(ordered.status == t::Status::ok && agrees(ordered.value, oracle(e, x).value),
         "Terminal N retains beta*A materialization before B and cosine");
    V folded{};
    folded[0] = x.beta[0] * (e.factor_a[0] * e.factor_b[0]);
    need(!agrees(ordered.value, folded),
         "Reassociated terminal product must fail sequential oracle");
    e = entry(1, true);
    x = for_entry(e);
    e.operations[0].sampled_likelihood[0] = -1;
    need(t::readout(e, x).status == t::Status::invalid_arithmetic,
         "Consumed negative ordinary likelihood fails visibly");
    e.operations[0].sampled_likelihood[0] = 1;
    e.operations[0].numerator[0] = std::numeric_limits<float>::infinity();
    need(t::readout(e, x).status == t::Status::invalid_arithmetic,
         "Consumed nonfinite ordinary numerator fails visibly");
    e = entry(0, true);
    x = for_entry(e);
    e.factor_b[0] = -1;
    need(t::readout(e, x).status == t::Status::invalid_arithmetic,
         "Consumed negative terminal factor is not clamped");
    e = entry(0, false);
    x = for_entry(e);
    e.factor_b[0] = e.cosine = std::numeric_limits<float>::quiet_NaN();
    e.operations[1].numerator[0] = std::numeric_limits<float>::quiet_NaN();
    need(t::readout(e, x).status == t::Status::ok,
         "Unused E factors and future operation slots are ignored");
}
} // namespace
int main() {
    try {
        ordinary_oracles();
        headers_and_identity();
        immediate_boundary();
        coverage_and_lane_semantics();
        zero_termination_and_order();
        zero_terminal_branch_order();
        terminal_mis_and_failures();
        deferred_completion();
        std::cout << "Enhanced suffix sequential tape passed " << checks << " CPU checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
