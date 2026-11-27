// SPDX-License-Identifier: GPL-3.0-only
// Test-only perturbation: manual pinhole ray and box filter never consume the
// Gaussian-width selector or lens words as physical inputs. Replay/NEE/seeds and
// draws.w remain identical. The bitmask proves both removed sites were reached.
#define RYPATH_ENHANCED_DIRECTION_DRAW_CONTROL(draws, imported, event, sample, p)                  \
    do {                                                                                           \
        (sample).random_words[0] = (draws).x;                                                      \
        (sample).random_words[1] = (draws).y;                                                      \
        (sample).random_words[2] = (draws).z;                                                      \
        (sample).random_words[3] = (draws).w;                                                      \
        if ((p).gaussian_width == 1u && (imported).tag == 5u &&                                    \
            ((event) + 1u == (imported).event_ordinal || (event) == (imported).event_ordinal)) {   \
            (draws).x ^= 0x87654321u;                                                              \
            (draws).y ^= 0xa5a5a5a5u;                                                              \
            (draws).z ^= 0x13579bdfu;                                                              \
            (sample).lens_words[0] |= 1u << (event);                                               \
        }                                                                                          \
    } while (false)
#include "../shaders/render.metal"
#undef RYPATH_ENHANCED_DIRECTION_DRAW_CONTROL
namespace hybrid_runtime_probe {
using namespace rypath_bdpt;
namespace er = rypath_enhanced_runtime;
namespace ew = rypath::enhanced_wire;
namespace ep = rypath_enhanced_path;
namespace rt = enhanced_hybrid_runtime;
constant uint words = 512, atom_base = 64, atom_words = 48, numeric = 448;
inline void store_record(device uint *out, const thread ew::Record &record) {
    const auto raw = reinterpret_cast<const thread uint *>(&record);
    for (uint k = 0; k < 12; ++k)
        out[k] = raw[k];
}
struct Observer {
    thread er::Sink &inner;
    thread er::State &state;
    thread HeroWalkState &walk;
    thread Resources &r;
    device uint *out;
    rypath_hero::Density entry_previous;
    float incoming_generation_q = 0;
    bool fresh() const thread { return inner.fresh(); }
    bool has_interior() const thread { return inner.has_interior(); }
    bool wanted(uint j) const thread { return inner.wanted(j); }
    bool stop_after(uint d) const thread { return inner.stop_after(d); }
    bool mapping_import(uint d) const thread { return inner.mapping_import(d); }
    void reset_atom() thread { inner.reset_atom(); }
    void begin_event() thread {
        entry_previous = walk.hero.previous;
        incoming_generation_q = walk.pdf;
        inner.begin_event();
    }
    ew::Record support_record() thread { return inner.support_record(); }
    ew::Record imported_record() thread { return inner.imported_record(); }
    void set_atom(const thread ew::Record &v, ep::reuse::Scaled h, ep::reuse::Scaled g) thread {
        inner.set_atom(v, h, g);
    }
    bool continue_discovery(float4 b, const thread HeroTrack &t, uint d) thread {
        return inner.continue_discovery(b, t, d);
    }
    bool activate_hybrid(const thread ew::Record &v, ep::reuse::Scaled h,
                         ep::reuse::Scaled g) thread {
        if (!inner.activate_hybrid(v, h, g))
            return false;
        auto ptr = out + numeric;
        out[41] = as_type<uint>(incoming_generation_q);
        for (uint k = 0; k < 4; ++k) {
            ptr[2 * k] = as_type<uint>(entry_previous.lane[k].fraction);
            ptr[2 * k + 1] = as_type<uint>(entry_previous.lane[k].exponent);
            ptr[8 + k] = as_type<uint>(state.hybrid.predecessor.beta[k]);
        }
        SurfacePoint owned{};
        const auto point_status = rt::owned_point(r, v, 2u * (v.event_ordinal + 1u),
                                                  r.alpha.parameters->max_depth, owned);
        const auto edge = enhanced_hybrid_geometry::edge(walk.latest.point.position,
                                                         walk.latest.point.geometric_normal,
                                                         owned.position, owned.geometric_normal);
        const auto incoming = enhanced_hybrid_geometry::forced_direction(
            walk.latest.bsdf, walk.latest.frame, rt::snapshot_wo(state.hybrid.predecessor),
            edge.direction, walk.latest.seed);
        out[43] = uint(point_status != enhanced_hybrid_geometry::Status::ok ||
                       edge.status != enhanced_hybrid_geometry::Status::ok ||
                       incoming.status != enhanced_hybrid_geometry::Status::ok);
        for (uint k = 0; k < 4; ++k) {
            ptr[12 + k] = as_type<uint>(incoming.numerator[k]);
            ptr[16 + k] = as_type<uint>(incoming.q[k]);
        }
        for (uint k = 0; k < 3; ++k) {
            ptr[48 + k] = as_type<uint>(walk.latest.point.position[k]);
            ptr[51 + k] = as_type<uint>(owned.position[k]);
        }
        ptr[54] = as_type<uint>(edge.cosine_destination);
        for (uint k = 0; k < 3; ++k) {
            ptr[57 + k] = as_type<uint>(owned.geometric_normal[k]);
            ptr[60 + k] = as_type<uint>(walk.latest.point.geometric_normal[k]);
        }
        ptr[55] = as_type<uint>(edge.distance_squared.fraction);
        ptr[56] = as_type<uint>(edge.distance_squared.exponent);
        return true;
    }
    template <typename Lambda> void add(uint j, float4 term, const thread Lambda &lambda) thread {
        auto ptr = out + atom_base + j * atom_words;
        ptr[0]++;
        const bool active = inner.has_interior();
        const auto record = active     ? state.hybrid.active_record
                            : (j & 1u) ? inner.atom_record
                                       : ew::empty_record();
        const auto h = active     ? state.hybrid.active_h
                       : (j & 1u) ? inner.atom_h
                                  : ep::reuse::Scaled{.5f, 1};
        const auto g = active     ? state.hybrid.active_generation_h
                       : (j & 1u) ? inner.atom_generation_h
                                  : ep::reuse::Scaled{.5f, 1};
        ptr[1] = record.tag;
        ptr[2] = as_type<uint>(h.fraction);
        ptr[3] = as_type<uint>(h.exponent);
        ptr[4] = as_type<uint>(g.fraction);
        ptr[5] = as_type<uint>(g.exponent);
        for (uint k = 0; k < 4; ++k) {
            ptr[6 + k] = as_type<uint>(term[k]);
            ptr[10 + k] = as_type<uint>(lambda.pdf[k]);
            ptr[14 + k] = as_type<uint>(lambda.lambda[k]);
        }
        store_record(ptr + 20, record);
        inner.add(j, term, lambda);
    }
};
} // namespace hybrid_runtime_probe
struct HybridRuntimeFixture {
    uint mode, selected_slot, count, reserved;
};
struct HybridRuntimeArguments {
    device const rypath::wavefront_wire::Config *config [[id(0)]];
    device const wire::Point *cie [[id(1)]];
    device rypath::enhanced_wire::Record *records [[id(2)]];
    device rypath::enhanced_wire::Evaluation *evaluations [[id(3)]];
    device uint *output [[id(4)]];
    device const rypath_replay::ReplayState *replays [[id(5)]];
    device const HybridRuntimeFixture *fixture [[id(6)]];
};
#define HYBRID_PROBE_PARAMETERS                                                                    \
    constant wire::Parameters &p [[buffer(0)]], constant HybridRuntimeArguments &a [[buffer(1)]],  \
        instance_acceleration_structure scene [[buffer(2)]],                                       \
        const device wire::Instance *instances [[buffer(3)]],                                      \
        const device wire::Point *positions [[buffer(4)]],                                         \
        const device wire::Triangle *triangles [[buffer(5)]],                                      \
        const device wire::Material *materials [[buffer(6)]],                                      \
        const device float *environment [[buffer(7)]],                                             \
        const device wire::SpectralKnot *knots [[buffer(8)]],                                      \
        const device wire::AreaEmission *emissions [[buffer(9)]],                                  \
        const device wire::Emitter *emitters [[buffer(10)]],                                       \
        const device wire::Point *normals [[buffer(11)]],                                          \
        const device sobol::Tables &tables [[buffer(12)]],                                         \
        const device wire::Patch *patches [[buffer(13)]],                                          \
        MixedIntersectionTable intersection_table [[buffer(14)]],                                  \
        const device wire::Point2 *texture_uvs [[buffer(15)]],                                     \
        const device wire::TextureFrame *texture_frames [[buffer(16)]],                            \
        const device wire::ImageTexture *image_textures [[buffer(17)]],                            \
        const device wire::TextureLevel *image_levels [[buffer(18)]],                              \
        const device wire::Word *image_texels [[buffer(19)]],                                      \
        const device float *image_transfer [[buffer(20)]],                                         \
        const device float *image_spectra [[buffer(21)]],                                          \
        constant wire::TextureParameters &texture_p [[buffer(22)]],                                \
        const device graph_wire::Node *graph_nodes [[buffer(23)]],                                 \
        const device wire::SpectralKnot *graph_knots [[buffer(24)]],                               \
        constant wire::TextureGraphParameters &graph_p [[buffer(25)]],                             \
        const device wire::SurfaceVertex *surface_vertices [[buffer(26)]],                         \
        const device wire::SurfaceTransform *surface_transforms [[buffer(27)]],                    \
        const device float *environment_cdf [[buffer(28)]],                                        \
        const device float *illuminant [[buffer(29)]],                                             \
        const device uint *material_data [[buffer(30)]], uint lane [[thread_position_in_grid]]

kernel void rypath_enhanced_hybrid_runtime_probe(HYBRID_PROBE_PARAMETERS) {
    using namespace hybrid_runtime_probe;
    if (lane >= a.fixture->count)
        return;
    auto out = a.output + lane * words;
    for (uint k = 0; k < words; ++k)
        out[k] = 0;
    const auto replay = a.replays[lane];
    wire::Sample sample{};
    sample.pixel = lane;
    sample.sample_low = 29;
    sample.sample_high = 1;
    HeroWalkState walk{};
    const auto lambda = rypath_hero::sample_uniform((float(lane) + .5f) / 16.f);
    float3 direction;
    rypath_conductor::unit(float3(.137f, .271f, 1), direction);
    packet_pt_initialize(walk, sample, lambda, ray(float3(0), direction), 1, 0x10000001dul, p);
    walk.hero.roulette_enabled = false; // Fixture discoveryq1; not a physical-mean oracle.
    const GraphResources graph{
        graph_nodes,
        graph_knots,
        &graph_p,
        {image_textures, image_levels, image_texels, image_transfer, image_spectra, &texture_p}};
    const ray incoming(walk.ray_origin, walk.ray_direction);
    SurfaceAlpha alpha{
        materials,    material_data, instances,      positions,        triangles,          patches,
        normals,      texture_uvs,   texture_frames, surface_vertices, surface_transforms, &p,
        &walk.sample, graph,         incoming};
    const rypath_image_light::Images images{image_textures, image_levels,  image_texels,
                                            image_transfer, image_spectra, illuminant,
                                            &texture_p};
    Resources r{alpha,           knots,  emissions, emitters, environment,
                environment_cdf, images, &tables,   scene,    {}};
    const bool fresh = a.fixture->mode == 0;
    uint selected_slot = a.fixture->selected_slot;
    if (!fresh) {
        const auto source = a.records[lane]; // Test capture always uses history bank0.
        if (source.tag == rypath_enhanced_chart::interior_zomega && source.event_ordinal >= 1 &&
            source.event_ordinal < a.config->contribution_depth) {
            // The second requested kind is N only when that atom exists. A
            // last admissible pivot has an E-only future at the finite boundary.
            selected_slot = min(2u * (source.event_ordinal + 1u) + (selected_slot & 1u),
                                2u * a.config->contribution_depth);
        }
    }
    const rypath::contribution_wire::Evaluation request{fresh ? ep::fresh_slot : selected_slot,
                                                        ep::fresh_slot, 0, 0};
    const auto owned = a.evaluations[lane];
    auto state = er::begin(request, *a.config, lane, owned);
    const auto owned_words = reinterpret_cast<const thread uint *>(&owned);
    for (uint k = 0; k < 8; ++k)
        out[24 + k] = owned_words[k];
    out[32] = state.failed;
    out[33] = request.requested_slot;
    out[34] = request.selected_slot;
    out[35] = request.ready;
    er::Sink inner{state, a.cie, p, *a.config, lane, a.records, a.evaluations};
    Observer sink{inner, state, walk, r, out, {}};
    uint steps = 0;
    for (; !walk.complete && steps < 10; ++steps) {
        const bool active = sink.has_interior();
        packet_pt_step_enhanced(r, walk, &replay, sink);
        out[412 + 2 * steps] = walk.sample.status;
        out[413 + 2 * steps] = (walk.count & 255u) | (state.failed << 8) |
                               (uint(walk.complete) << 9) | (state.hybrid.outside << 10) |
                               (uint(sink.has_interior()) << 12);
        if (!active && sink.has_interior()) {
            auto ptr = out + numeric;
            const auto outgoing = enhanced_hybrid_geometry::forced_direction(
                walk.latest.bsdf, walk.latest.frame, rt::snapshot_wo(state.hybrid.predecessor),
                rt::world_omega(state.hybrid.active_record), walk.latest.seed);
            out[43] |= uint(outgoing.status != enhanced_hybrid_geometry::Status::ok);
            if (fresh) {
                const thread auto &raw = walk.sample.random_words;
                const auto sampled = hero_sample_current(
                    walk.latest.bsdf,
                    walk.latest.frame.local(rt::snapshot_wo(state.hybrid.predecessor)),
                    sample_uniform(p, raw[0]), uniform2(p, uint2(raw[1], raw[2])), walk.latest.seed,
                    Mode::Radiance);
                out[40] = uint(sampled.valid && sampled.accepted && !sampled.fallback);
                out[42] = as_type<uint>(sampled.pdf[0]);
            }
            for (uint k = 0; k < 3; ++k)
                out[36 + k] = as_type<uint>(walk.ray_direction[k]);
            for (uint k = 0; k < 4; ++k) {
                ptr[20 + k] = as_type<uint>(outgoing.numerator[k]);
                ptr[24 + k] = as_type<uint>(outgoing.q[k]);
                ptr[28 + k] = as_type<uint>(walk.beta[k]);
                ptr[32 + 2 * k] = as_type<uint>(walk.hero.density.lane[k].fraction);
                ptr[33 + 2 * k] = as_type<uint>(walk.hero.density.lane[k].exponent);
                ptr[40 + 2 * k] = as_type<uint>(walk.hero.previous.lane[k].fraction);
                ptr[41 + 2 * k] = as_type<uint>(walk.hero.previous.lane[k].exponent);
            }
        }
    }
    float3 xyz(0);
    rypath::contribution_wire::Evaluation result{};
    const bool finished = inner.finish(request, xyz, result);
    out[0] = state.failed;
    out[1] = walk.sample.status;
    out[2] = steps;
    out[3] = walk.complete;
    out[4] = sink.has_interior();
    out[5] = state.hybrid.active_record.event_ordinal;
    out[6] = fresh;
    out[7] = walk.sample.lens_words[0];
    store_record(out + 8, state.hybrid.active_record);
    out[20] = as_type<uint>(state.hybrid.active_h.fraction);
    out[21] = as_type<uint>(state.hybrid.active_h.exponent);
    out[22] = as_type<uint>(state.hybrid.active_generation_h.fraction);
    out[23] = as_type<uint>(state.hybrid.active_generation_h.exponent);
    out[48] = finished;
    out[49] = result.selected_slot;
    out[50] = as_type<uint>(result.normalizer);
    out[51] = as_type<uint>(a.evaluations[lane].normalizer_exponent);
    out[52] = as_type<uint>(xyz.x);
    out[53] = as_type<uint>(xyz.y);
    out[54] = as_type<uint>(xyz.z);
    out[55] = result.ready;
    out[56] = as_type<uint>(state.requested_h.fraction);
    out[57] = as_type<uint>(state.requested_h.exponent);
    out[58] = sizeof(er::State);
    out[59] = state.hybrid.outside;
    out[60] = state.hybrid.first.next;
    out[61] = state.hybrid.first.selected;
    out[62] = uint(state.hybrid.first.status);
    out[44] = state.hybrid.primary_ready;
    out[45] = as_type<uint>(state.hybrid.primary_distance_squared.fraction);
    out[46] = as_type<uint>(state.hybrid.primary_distance_squared.exponent);
    out[47] = as_type<uint>(state.hybrid.primary_cosine);
    const auto published = a.evaluations[lane];
    const auto published_words = reinterpret_cast<const thread uint *>(&published);
    for (uint k = 0; k < 8; ++k)
        out[432 + k] = published_words[k];
    if (fresh) {
        // Diagnostic capture uses the independently visible twelve raw words,
        // avoiding the conditional aggregate assignment under investigation.
        // Atomic word readback forces a device observation rather than a
        // compiler-forwarded thread temporary. No other lane owns this record.
        auto raw = reinterpret_cast<device atomic_uint *>(a.records + lane);
        for (uint k = 0; k < 12; ++k)
            atomic_store_explicit(raw + k, out[8 + k], memory_order_relaxed);
        for (uint k = 0; k < 12; ++k)
            out[400 + k] = atomic_load_explicit(raw + k, memory_order_relaxed);
    }
}
#undef HYBRID_PROBE_PARAMETERS
