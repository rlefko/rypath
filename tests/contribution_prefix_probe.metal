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

// Sixteen additional one-event controls. These retain the original full-event
// Collector as oracle, and use analytic arrival emission when omitted future
// work makes that oracle fail before readout. Still zero NEE and no AS trace.
kernel void rypath_contribution_selected_event(
    constant wire::Parameters &p [[buffer(0)]],
    device const rypath::wavefront_wire::Config &config [[buffer(1)]],
    device const wire::Instance *instances [[buffer(2)]],
    device const wire::Point *positions [[buffer(3)]],
    device const wire::Triangle *triangles [[buffer(4)]],
    device const wire::Material *materials [[buffer(5)]],
    device const wire::AreaEmission *emissions [[buffer(6)]],
    device const float *environment [[buffer(7)]], device const wire::Point *cie [[buffer(8)]],
    device uchar *sidecars [[buffer(9)]], device float4 *collected [[buffer(10)]],
    device uint *out [[buffer(11)]], constant wire::TextureParameters &tp [[buffer(12)]],
    device const wire::TextureFrame *frames [[buffer(13)]],
    device const wire::SpectralKnot *knots [[buffer(14)]], device const uint *data [[buffer(15)]],
    constant wire::TextureGraphParameters &gp [[buffer(16)]],
    device const wire::SurfaceVertex *vertices [[buffer(17)]],
    device const wire::SurfaceTransform *transforms [[buffer(18)]],
    uint n [[thread_position_in_grid]]) {
    using namespace prefix_probe;
    if (n >= 16)
        return;
    const uint fixture = n / 2, j = 2u + (n & 1u), slots = 2u * p.max_depth + 1u;
    auto atoms = collected + n * (slots + 1u);
    for (uint k = 0; k <= slots; ++k)
        atoms[k] = 0;
    Resources r{};
    r.alpha.parameters = &p;
    r.alpha.instances = instances;
    r.alpha.positions = positions;
    r.alpha.triangles = triangles;
    r.alpha.materials = materials;
    r.alpha.material_data = data;
    r.alpha.frames = frames;
    r.alpha.vertices = vertices;
    r.alpha.transforms = transforms;
    r.alpha.graph.parameters = &gp;
    r.alpha.graph.images.parameters = &tp;
    r.knots = knots;
    r.emissions = emissions;
    r.environment = environment;
    r.tables = reinterpret_cast<device const sobol::Tables *>(environment);
    auto source = initial(n, 0, p);
    source.count = 2;           // E1/N1: RR is eligible, and texture context is secondary.
    source.latest.delta = true; // Isolate arrival emission from preceding MIS.
    source.beta = .25f;
    if (fixture == 1)
        source.hero.density.lane[0] = {}; // Preparation succeeds; collapse must fail.
    source.hero.roulette_enabled = fixture == 5 || fixture == 6;
    auto u = replay(100u + fixture);
    bool gate_witness = fixture != 5 && fixture != 6;
    for (uint attempt = 0; !gate_witness && attempt < 128; ++attempt) {
        const auto words = random(r, source.sample, source.index, 5u, &u);
        const bool survives = sample_uniform(p, words.w) >= .75f;
        gate_witness = survives == (fixture == 6);
        if (!gate_witness)
            u.tail_key[0] += 7919u;
    }
    const SurfaceHit h{intersection_type::triangle, fixture, 0, 0, 2, float2(.25f), true,
                       wire::triangle_primitive};
    auto full = source;
    Collector oracle{atoms, cie, slots};
    packet_pt_step_contribution_pretraced(r, full, &u, oracle, h);
    auto actual = source;
    auto pool = reinterpret_cast<device cr::State *>(sidecars);
    const cw::Evaluation request{j, 0xffffffffu, 0.f, 0};
    pool[n] = cr::begin(request, config);
    auto selection = pool[n];
    cr::Sink sink{selection, cie, p, config, n};
    packet_pt_step_contribution_pretraced(r, actual, &u, sink, h);
    cw::Evaluation result{};
    float3 value(0);
    const bool finished = actual.complete && actual.sample.status == wire::success &&
                          sink.finish(request, value, result);
    pool[n] = selection;

    const bool endpoint = (j & 1u) == 0;
    const bool full_bad = fixture == 1 || fixture == 2 || fixture == 4 || fixture == 7;
    const bool prefix_bad =
        fixture == 4 || (fixture == 1 && !endpoint) || (fixture == 7 && endpoint);
    uint bad = 0;
    if (!gate_witness ||
        full.sample.status != (full_bad ? wire::invalid_arithmetic : wire::success) ||
        oracle.failed ||
        actual.sample.status != (prefix_bad ? wire::invalid_arithmetic : wire::success) ||
        finished == prefix_bad || selection.failed)
        bad |= 1;
    if (full.count != (fixture == 4 ? 2u : 3u) || actual.count != (fixture == 4 ? 2u : 3u) ||
        !actual.complete)
        bad |= 2;
    if (finished) {
        // At E1 the previous delta vertex gives unit technique MIS. Emission is
        // the known .125 at each wavelength, and arrival beta is exactly .25.
        Collector expected{atoms, cie, slots + 1u};
        expected.add(slots, endpoint ? float4(.03125f) : float4(0), source.lambda);
        if (expected.failed || !same(value, atoms[slots].xyz) ||
            (!full_bad && !same(value, atoms[j].xyz)))
            bad |= 4;
        if (result.requested_slot != j || result.selected_slot != j || result.ready != 1 ||
            as_type<uint>(result.normalizer) != 0 || selection.next_slot != slots)
            bad |= 8;
    }
    if (fixture == 0) {
        // Both outputs may match even without the cut. These state assertions
        // prove E did not collapse, while N did collapse before stopping.
        if (full.hero.active_mask != 1 || actual.hero.active_mask != (endpoint ? 15u : 1u) ||
            (endpoint ? any(actual.beta != source.beta)
                      : actual.beta[0] != 1.f || any(actual.beta.yzw != 0)) ||
            actual.latest.bsdf.kind != (endpoint ? 0xffffffffu : 5u))
            bad |= 16;
    }
    if (fixture == 2 || fixture == 3) {
        const auto words = random(r, source.sample, source.index, 5u, &u);
        const auto probe = hero_sample_current(
            full.latest.bsdf, full.latest.frame.local(float3(0, 0, 1)), sample_uniform(p, words.x),
            uniform2(p, words.yz), full.latest.seed, Mode::Radiance);
        if (fixture == 2 ? probe.valid || full.latest.bsdf.kind != 1
                         : !probe.valid || probe.accepted || !full.complete)
            bad |= 32;
    }
    if (fixture == 5 && (!full.complete || any(full.beta != source.beta) || atoms[3].w != 0))
        bad |= 64;
    if (fixture == 6 && (full.complete || atoms[3].w != 1 ||
                         (endpoint ? any(actual.beta != source.beta) : any(actual.beta != 1))))
        bad |= 128;
    out[8 * n] = bad;
    out[8 * n + 1] = full.sample.status;
    out[8 * n + 2] = actual.sample.status;
    out[8 * n + 3] = full.hero.active_mask;
    out[8 * n + 4] = actual.hero.active_mask;
    out[8 * n + 5] = full.count;
    out[8 * n + 6] = actual.count;
    out[8 * n + 7] = uint(finished) | (uint(endpoint && finished) << 1) |
                     (uint(full_bad && finished) << 2) | (fixture << 8);
}

// Actual Enhanced sink discovery/selection protocol, with known spectral terms.
// This isolates weighting and observation order; it is not a transport/AS test.
kernel void rypath_enhanced_discovery_witness(constant wire::Parameters &p [[buffer(0)]],
                                              device const rypath::wavefront_wire::Config *configs
                                              [[buffer(1)]],
                                              device const wire::Point *cie [[buffer(2)]],
                                              device uchar *sidecars [[buffer(3)]],
                                              device uint *out [[buffer(4)]],
                                              uint n [[thread_position_in_grid]]) {
    if (n >= 6)
        return;
    namespace er = rypath_enhanced_runtime;
    namespace ep = rypath_enhanced_path;
    namespace cw = rypath::contribution_wire;
    using namespace rypath_bdpt;
    const device auto &config = configs[n];
    const bool imported = n == 4, black = n == 5, unit = n == 3 || black;
    const cw::Evaluation request{imported ? 5u : ep::fresh_slot, ep::fresh_slot, 0.f, 0};
    auto pool = reinterpret_cast<device er::State *>(sidecars);
    pool[n] = er::begin(request, config, n);
    auto state = pool[n];
    er::Sink sink{state, cie, p, config, n};
    Wavelengths lambda{};
    const float wavelengths[4] = {400, 500, 600, 700};
    const float pdfs[4] = {1, 2, 4, 8};
    for (uint k = 0; k < 4; ++k) {
        lambda.lambda[k] = wavelengths[k];
        lambda.pdf[k] = pdfs[k];
    }
    // term[k]/pdf[k] is exactly the slot coefficient. Distinct original sensor
    // PDFs make lost companion projection or an extra collapse factor visible.
    const float4 basis(1, 2, 4, 8);
    const float coefficients[7] = {.0625f, 0, .125f, .25f, .375f, .5f, 1};
    HeroTrack track;
    bool first = true, first_skip = true;
    uint second = 2; // Not executed after a first-gate cutoff.
    if (imported) {
        sink.begin_event();
        first = sink.continue_discovery(float4(.5f), track, 2);
        second = sink.continue_discovery(float4(.25f), track, 3);
        sink.add(5, basis * coefficients[5], lambda);
    } else {
        sink.begin_event();
        sink.add(0, black ? float4(0) : basis * coefficients[0], lambda);
        first_skip = sink.continue_discovery(float4(.5f), track, 1);
        sink.add(1, float4(0), lambda);
        sink.begin_event();
        first = sink.continue_discovery(float4(unit ? 1.f : .5f), track, 2);
        sink.add(2, black ? float4(0) : basis * coefficients[2], lambda);
        if (first) {
            sink.add(3, black ? float4(0) : basis * coefficients[3], lambda);
            sink.begin_event();
            second = sink.continue_discovery(float4(unit ? 1.f : .25f), track, 3);
            sink.add(4, black ? float4(0) : basis * coefficients[4], lambda);
            if (second) {
                sink.add(5, black ? float4(0) : basis * coefficients[5], lambda);
                sink.begin_event();
                sink.add(6, black ? float4(0) : basis * coefficients[6], lambda);
            }
        }
    }
    // A separate dispatch reloads these bytes, preventing thread/device copy
    // forwarding from turning the persistence check into a thread-local test.
    pool[n] = state;
    out[32u * n] = uint(!first_skip);
    out[32u * n + 25u] = uint(first);
    out[32u * n + 26u] = second;
}

kernel void rypath_enhanced_discovery_finish(constant wire::Parameters &p [[buffer(0)]],
                                             device const rypath::wavefront_wire::Config *configs
                                             [[buffer(1)]],
                                             device const wire::Point *cie [[buffer(2)]],
                                             device uchar *sidecars [[buffer(3)]],
                                             device uint *out [[buffer(4)]],
                                             uint n [[thread_position_in_grid]]) {
    if (n >= 6)
        return;
    namespace er = rypath_enhanced_runtime;
    namespace ep = rypath_enhanced_path;
    namespace cw = rypath::contribution_wire;
    const device auto &config = configs[n];
    const cw::Evaluation request{n == 4 ? 5u : ep::fresh_slot, ep::fresh_slot, 0.f, 0};
    auto pool = reinterpret_cast<device er::State *>(sidecars);
    auto persisted = pool[n];
    er::Sink final_sink{persisted, cie, p, config, n};
    cw::Evaluation result{};
    float3 value(0);
    const bool finished = final_sink.finish(request, value, result);
    pool[n] = persisted;
    const uint base = 32u * n;
    out[base] |= uint(!finished || persisted.failed);
    out[base + 1] = result.requested_slot;
    out[base + 2] = result.selected_slot;
    out[base + 3] = result.ready;
    out[base + 4] = as_type<uint>(result.normalizer);
    out[base + 5] = persisted.selection.observed;
    out[base + 6] = persisted.next_slot;
    out[base + 7] = persisted.discovery.gates;
    out[base + 8] = as_type<uint>(persisted.discovery.probability);
    out[base + 9] = as_type<uint>(persisted.arrival_probability);
    out[base + 10] = as_type<uint>(persisted.direct_probability);
    out[base + 11] = persisted.rr_cut;
    out[base + 12] = as_type<uint>(persisted.selection.selected_target);
    out[base + 13] = persisted.selection.selected_slot;
    out[base + 14] = as_type<uint>(value.x);
    out[base + 15] = as_type<uint>(value.y);
    out[base + 16] = as_type<uint>(value.z);
    out[base + 17] = as_type<uint>(persisted.selection.selected_xyz.x);
    out[base + 18] = as_type<uint>(persisted.selection.selected_xyz.y);
    out[base + 19] = as_type<uint>(persisted.selection.selected_xyz.z);
    out[base + 20] = persisted.failed;
    out[base + 21] = persisted.selection.descriptor.slot_count;
    out[base + 22] = uint(persisted.selection.status);
    out[base + 23] = uint(persisted.discovery.status);
    out[base + 24] = uint(final_sink.fresh());
    out[base + 27] = as_type<uint>(persisted.selection.score_sum);
    out[base + 28] = as_type<uint>(persisted.requested_xyz.x);
    out[base + 29] = as_type<uint>(persisted.requested_xyz.y);
    out[base + 30] = as_type<uint>(persisted.requested_xyz.z);
    out[base + 31] = persisted.pixel;
}
