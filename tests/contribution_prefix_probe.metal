// SPDX-License-Identifier: GPL-3.0-only
#include "../shaders/render.metal"

// Pretraced event differential, not an intersection or positive-NEE test.
// Every fixed slot is compared with a full event collector that never requests
// prefix termination and does not use the production Sink's XYZ projection.
namespace prefix_probe {
using namespace rypath_bdpt;
namespace cr = rypath_contribution_runtime;
namespace cw = rypath::contribution_wire;
namespace wf = rypath::wavefront_wire;
constant uint variants = 16;
constant uint null_instance = 66, invisible_instance = 67;
inline bool same(float3 a, float3 b) { return all(as_type<uint3>(a) == as_type<uint3>(b)); }

struct Collector {
    device float4 *atoms;
    device const wire::Point *table;
    uint slots, failed = 0;
    bool wanted(uint) const thread { return true; }
    bool stop_after(uint) const thread { return false; }
    template <typename Wavelengths>
    void add(uint slot, float4 term, const thread Wavelengths &lambda) thread {
        if (failed)
            return;
        if (slot >= slots || atoms[slot].w != 0 || !all(isfinite(term)) || any(term < 0)) {
            failed = 1;
            return;
        }
        float3 xyz(0);
        for (uint k = 0; k < 4; ++k) {
            const float w = lambda.lambda[k], pdf = lambda.pdf[k];
            if (!(w >= 360 && w <= 830 && isfinite(pdf) && pdf > 0)) {
                failed = 1;
                return;
            }
            // Independent table indexing/reduction; no frame_sensor or Sink call.
            const uint index = uint(floor(w + .5f)) - 360u;
            const auto c = table[index];
            xyz += float3(c.x, c.y, c.z) * (term[k] / pdf / 4.f);
        }
        if (!all(isfinite(xyz))) {
            failed = 1;
            return;
        }
        atoms[slot] = float4(xyz, 1);
    }
};
inline rypath_replay::ReplayState replay(uint n) {
    return {{0x1234567u + n * 7919u, 0xabcdef01u}, {1, 2}, {3, 4}, 5, rypath_replay::contract};
}
inline HeroWalkState initial(uint n, uint variant, constant wire::Parameters &p) {
    wire::Sample sample{};
    sample.pixel = n;
    sample.sample_low = 29;
    sample.sample_high = 1;
    HeroWalkState s{};
    auto lambda = rypath_hero::sample_uniform(float(n % 31) / 31.f);
    packet_pt_initialize(s, sample, lambda, ray(float3(0, 0, 2), float3(0, 0, -1)), 1,
                         0x10000001dul, p);
    // Exercise the entire prescribed finite chain without stochastic RR ending
    // it early. This changes the fixture, not the production event implementation.
    s.hero.roulette_enabled = false;
    if (variant == 1 || variant == 2) {
        const uint mask = variant == 1 ? 1u : 3u;
        s.hero.active_mask = mask;
        for (uint k = 0; k < 4; ++k)
            if (!(mask & (1u << k))) {
                s.beta[k] = 0;
                s.hero.density.lane[k] = {};
            }
    }
    if (variant == 7)
        s.ray_direction.x = NAN;
    if (variant == 13)
        s.pdf = NAN;
    if (variant == 14)
        s.beta.x = 0;
    if (variant == 15)
        s.lambda.pdf[1] = 0;
    if (variant == variants)
        for (uint k = 0; k < 4; ++k)
            s.lambda.pdf[k] = 1; // Finite atom sensor arithmetic despite sum overflow.
    return s;
}
inline SurfaceHit hit(uint variant, uint j, uint step, const thread HeroWalkState &s,
                      constant wire::Parameters &p) {
    const uint depth = s.count - 1u;
    SurfaceHit h{intersection_type::triangle, 0, 0, 0, 1, float2(.25f, .25f), true,
                 wire::triangle_primitive};
    h.instance_id = depth;
    if (variant == 3 || (variant == 11 && depth == 1))
        h.type = intersection_type::none;
    if ((variant == 5 || variant == 6) && step == 0)
        h.instance_id = variant == 5 ? null_instance : invisible_instance;
    if (variant == 12)
        h.instance_id = null_instance;
    if (variant == 4 && depth == 0)
        h.instance_id = 68; // Black BSDF, positive emission, successful termination.
    if ((variant == 8 && depth == j / 2u) || (variant == 9 && depth == p.max_depth))
        h.valid = false;
    if (variant == 10 && depth == p.max_depth)
        h.instance_id = 69; // Invalid terminal emission; earlier atoms remain defined.
    if (variant == variants)
        h.instance_id = 70 + depth;
    return h;
}
} // namespace prefix_probe

kernel void rypath_contribution_prefix_layout(device uint *out [[buffer(0)]]) {
    out[0] = 0x43504631u;
    out[1] = sizeof(rypath_contribution_runtime::State);
    out[2] = alignof(rypath_contribution_runtime::State);
    out[3] = sizeof(rypath::contribution_wire::Evaluation);
}

kernel void rypath_contribution_prefix_differential(
    constant wire::Parameters &p [[buffer(0)]],
    device const rypath::wavefront_wire::Config &config [[buffer(1)]],
    device const wire::Instance *instances [[buffer(2)]],
    device const wire::Point *positions [[buffer(3)]],
    device const wire::Triangle *triangles [[buffer(4)]],
    device const wire::Material *materials [[buffer(5)]],
    device const wire::AreaEmission *emissions [[buffer(6)]],
    device const float *environment [[buffer(7)]], device const wire::Point *cie [[buffer(8)]],
    device uchar *sidecars [[buffer(9)]], device float4 *collected [[buffer(10)]],
    device uint *out [[buffer(11)]], uint n [[thread_position_in_grid]]) {
    using namespace prefix_probe;
    const uint slots = 2u * p.max_depth + 1u;
    if (n >= variants * slots + 1u)
        return;
    const uint variant = n / slots, j = variant == variants ? 0u : n % slots;
    auto atoms = collected + n * slots;
    for (uint k = 0; k < slots; ++k)
        atoms[k] = 0;
    Resources r{};
    r.alpha.parameters = &p;
    r.alpha.instances = instances;
    r.alpha.positions = positions;
    r.alpha.triangles = triangles;
    r.alpha.materials = materials;
    r.emissions = emissions;
    r.environment = environment;
    // All transport draws use ReplayState; no table dereference or AS trace.
    r.tables = reinterpret_cast<device const sobol::Tables *>(environment);
    const auto u = replay(n);
    auto full = initial(n, variant, p);
    Collector oracle{atoms, cie, slots};
    uint full_steps = 0;
    for (; !full.complete && full_steps < p.max_depth + 6u; ++full_steps) {
        const auto h = hit(variant, j, full_steps, full, p);
        packet_pt_step_contribution_pretraced(r, full, &u, oracle, h);
    }
    const bool full_failed = full.sample.status != wire::success || oracle.failed;
    auto actual = initial(n, variant, p);
    const cw::Evaluation request{j, 0xffffffffu, 0.f, 0};
    auto pool = reinterpret_cast<device cr::State *>(sidecars);
    pool[n] = cr::begin(request, config);
    uint prefix_steps = 0;
    for (; !actual.complete && prefix_steps < p.max_depth + 6u;) {
        // Exercise genuine thread/device sidecar serialization at every event.
        auto selection = pool[n];
        cr::Sink sink{selection, cie, p, config, n};
        const auto h = hit(variant, j, prefix_steps, actual, p);
        packet_pt_step_contribution_pretraced(r, actual, &u, sink, h);
        ++prefix_steps;
        pool[n] = selection;
        if (selection.failed)
            break;
    }
    auto selection = pool[n];
    cr::Sink sink{selection, cie, p, config, n};
    cw::Evaluation result{};
    float3 value(0);
    const bool finished = actual.complete && actual.sample.status == wire::success &&
                          sink.finish(request, value, result);
    pool[n] = selection;
    const bool prefix_failed = !finished;
    const bool always_bad =
        variant == 7 || variant == 8 || variant == 12 || variant == 13 || variant == 15;
    const bool terminal_bad = variant == 9 || variant == 10;
    const bool overflow = variant == variants && p.max_depth > 0;
    const bool expect_full_bad = always_bad || terminal_bad || overflow;
    const bool expect_prefix_bad = always_bad || (terminal_bad && j / 2u == p.max_depth);
    const uint full_status = variant == 8 || variant == 9 ? wire::invalid_hit
                             : variant == 12              ? wire::boundary_exhaustion
                             : variant == 7 || variant == 10 || variant == 13 || overflow
                                 ? wire::invalid_arithmetic
                                 : wire::success;
    const uint prefix_status =
        variant == 8 || (variant == 9 && j / 2u == p.max_depth) ? wire::invalid_hit
        : variant == 12                                         ? wire::boundary_exhaustion
        : variant == 7 || variant == 13 || (variant == 10 && j / 2u == p.max_depth)
            ? wire::invalid_arithmetic
            : wire::success;
    uint bad = 0;
    if (full_failed != expect_full_bad || prefix_failed != expect_prefix_bad)
        bad |= 1;
    if (!full.complete || (!actual.complete && !selection.failed) || prefix_steps > full_steps)
        bad |= 2;
    if (full.sample.status != full_status || actual.sample.status != prefix_status ||
        bool(oracle.failed) != (variant == 15) || bool(selection.failed) != (variant == 15))
        bad |= 256;
    if (finished) {
        if (!same(value, atoms[j].xyz))
            bad |= 4;
        if (result.requested_slot != j || result.selected_slot != j || result.ready != 1 ||
            as_type<uint>(result.normalizer) != 0 || selection.next_slot != slots)
            bad |= 8;
        const bool chain =
            variant <= 2 || variant == 5 || variant == 6 || variant == 14 || variant == variants;
        if (chain && ((j & 1u) ? any(value != 0) : !all(value > 0)))
            bad |= 16;
        if (chain && j / 2u < p.max_depth && !(prefix_steps < full_steps))
            bad |= 32;
        if (chain && (actual.count != j / 2u + 2u ||
                      prefix_steps != j / 2u + 1u + uint(variant == 5 || variant == 6)))
            bad |= 512;
        if ((variant == 3 && j > 0) || (variant == 4 && j > 0) || (variant == 11 && j > 2))
            if (any(value != 0))
                bad |= 64;
    }
    // Before/selected failures must remain visible, rather than publishing zero.
    if (expect_prefix_bad && finished)
        bad |= 128;
    out[8 * n] = bad;
    out[8 * n + 1] = full.sample.status;
    out[8 * n + 2] = actual.sample.status;
    out[8 * n + 3] = full_steps;
    out[8 * n + 4] = prefix_steps;
    out[8 * n + 5] = full.count;
    out[8 * n + 6] = actual.count;
    out[8 * n + 7] = (uint(full_failed) << 0) | (uint(prefix_failed) << 1) |
                     (uint(atoms[j].w != 0) << 2) | (variant << 8);
}
