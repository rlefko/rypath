// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../src/render_wire.h"
namespace rypath {
namespace light_geometry_probe {
struct Query {
    render_wire::Point reference;
    render_wire::Point normal;
    render_wire::Point2 sample;
    render_wire::Word emitter;
};
struct Result {
    render_wire::Point position;
    render_wire::Point normal;
    render_wire::Point2 uv;
    float sampled_pdf;
    float queried_pdf;
    float area_pdf;
    float far_root_pdf;
    render_wire::Word valid;
    render_wire::Word cone;
};
static_assert(sizeof(Query) == 36 && sizeof(Result) == 56, "Geometry proposal probe ABI");
// Actual BDPT density conversion, observed through this existing full-render TU.
struct DensityQuery {
    render_wire::Point from, to, normal;
    render_wire::Word pdf_bits, infinite;
};
struct DensityResult {
    render_wire::Word pdf_bits, distance_squared_bits, nonzero_edge, magic;
};
static_assert(sizeof(DensityQuery) == 44 && sizeof(DensityResult) == 16,
              "Density conversion probe ABI");
struct OffsetQuery {
    render_wire::Word bits, axis, negative, move;
};
struct OffsetResult {
    render_wire::Word rounded[3], scalar_bits, scalar_valid, magic;
};
static_assert(sizeof(OffsetQuery) == 16 && sizeof(OffsetResult) == 24, "Offset rounding probe ABI");
// Same eight represented camera/coating/environment contexts, without exposing
// production Vertex or Prepared layout to the host.
enum : render_wire::Word {
    canonical_contexts = 8,
    canonical_helper_rows = 11,
    canonical_scratch_slots = 5
};
struct CanonicalQuery {
    float frame[9], wo[3], wi[3];
    render_wire::Word frame_id, angle_id, zero_base, material_seed;
};
struct CanonicalStrategy {
    float C, S, O, L, weight, expected_S;
    render_wire::Point canonical_wo, canonical_wi;
    render_wire::Word s, t, ids[3], delta[3], status;
};
// Scalar fields only: equality checks typed beta/locals/history/material data,
// never private Vertex padding or GPU pointer addresses.
struct CanonicalProtected {
    float beta[4];
    render_wire::Point sampled_wo, sampled_wi;
    float rho[4], top_rho[4];
    render_wire::Word path_id, prev_id, next_id, pair_valid, delta, active_mask, kind[4];
};
struct CanonicalPartition {
    render_wire::Word magic, context, status, vertex_bytes, vertex_alignment, renderer;
    render_wire::Word active_mask, compatible;
    float rho[4], top_rho[4];
    CanonicalStrategy strategy[3];
    CanonicalProtected stored_camera, stored_light;
};
struct CanonicalJoint {
    render_wire::Point wi;
    float weight[4], pdf, reverse_pdf, eta;
    render_wire::Word valid, accepted, delta;
};
struct CanonicalFinalizer {
    render_wire::Word magic, index, context, control, status, called, valid;
    render_wire::Word vertex_bytes, vertex_alignment, renderer, count, mode;
    CanonicalJoint joint;
    CanonicalProtected before, after;
    float next_beta[4], f_cos_over_proxy[4];
    render_wire::Word evaluation_valid;
    float forward, reverse, expected_forward, expected_reverse, prior_forward, prior_reverse;
};
static_assert(sizeof(CanonicalQuery) == 76 && sizeof(CanonicalStrategy) == 84 &&
                  sizeof(CanonicalPartition) == 540 && sizeof(CanonicalJoint) == 52 &&
                  sizeof(CanonicalProtected) == 112 && sizeof(CanonicalFinalizer) == 384,
              "Canonical MIS scalar probe ABI");

} // namespace light_geometry_probe
} // namespace rypath

// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#if !defined(__METAL_VERSION__)
#include <cstdint>
#endif
namespace strategy_context21 {
#if defined(__METAL_VERSION__)
using Word = uint;
#else
using Word = std::uint32_t;
#endif
enum : Word {
    cases = 8,
    strategy_words = 80,
    summary_words = 128,
    result_words = summary_words + 3 * strategy_words,
    magic = 0x53433231,
    unwritten = 0xffffffffu
};
// Plain scalar arrays deliberately avoid native float3 ABI assumptions.
struct Query {
    float frame[9], wo[3], wi[3];
    Word frame_id, angle_id, zero_base, material_seed;
};
struct Result {
    Word words[result_words];
};
static_assert(sizeof(Query) == 76 && sizeof(Result) == result_words * 4,
              "strategy context scalar wire layout");
// Summary words:
// 0 magic;1 index;2 status;3 schema;4..6 frame/angle/zero;7 material seed;
// 8 Vertex bytes;9 align;10 Parameters bytes;11 actual Parameters renderer;
// 12..15 PT(qS,qL,wS,wL);16..19 f Radiance;20..23 importance_f_cos;
// 24..26 original ordered wo;27..29 original ordered wi;
// 30..32 world C direction;33..35 world E direction;36..44 frame;
// 45..48 C.xyz/distance2;49..52 camera_sample.xyz/beta;
// 53..56 joint weight;57 joint pdf;58 joint accepted;59 joint valid;
// 60..63 joint sampled wi.xyz / reverse pdf;64..67 f*cos/proxy for that sample;
// 68..71 actual camera_connection(s=0,t=3);72 its status;
// 73..76 actual camera_connection(s=1,t=2);77 its status;
// 78..81 independently repeated sample_li sampled direction/pdf;82 success;
// 83..86 actual NEE f;87 NEE proxy pdf;88..91 sampled env beta;
// 92..95 sky values;96..99 sampled IDs/types;100..103 physical rho lanes;
// 104 surface seed;105 sample pixel;106..107 sample index;108 selected env ID.
// Each strategy record:
// 0 s;1 t;2 status;3 reserved;4 actual mis_weight;
// 5..8 actual C,S,O,L (different context values are NOT reconciled);
// 9..14 camera->environment ordered local pair;
// 15..20 actual outgoing/incident locals used by importance f;
// 21..24 path IDs C/P/E plus P.seed;25..27 sampled prev/next/pair_valid;
// 28..31 f Radiance;32..35 importance_f_cos;36..39 camera_connection(0,3);
// 40..43 P.pdf_fwd,E.pdf_fwd,P.pdf_rev,E.pdf_rev;
// 44..49 raw independent forward/reverse proxy query locals;
// 50..53 explicit proxy forward/reverse and converted forward/reverse;
// 54..57 auxiliary next/previous IDs/delta flags;58 renderer status;
// Remaining words are reserved zero, checked by the offline auditor.
} // namespace strategy_context21

namespace strategy_context21 {
// Scalar semantic readback, no private Vertex layout/padding exposed to CPU.
enum : Word {
    vertex_fields = 220,
    prepared_begin = 45,
    lane_fields = 36,
    beta_begin = 190,
    wo_begin = 194,
    pdf_begin = 199,
    identity_begin = 201,
    sampled_begin = 211,
    readback_magic = 0x43534631u
};
struct SemanticVertex {
    Word magic, context, slot, field_count, vertex_bytes, vertex_alignment;
    Word fields[vertex_fields];
};
static_assert(sizeof(SemanticVertex) == (6 + vertex_fields) * 4, "Full context semantic wire ABI");
} // namespace strategy_context21
