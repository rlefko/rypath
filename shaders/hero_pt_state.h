// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included inside rypath_bdpt after PacketPTHot.
// Hero storage retains only fields consumed after the current event. Active
// material values are frozen verbatim; never resolve textures/mix/bump again.
struct HeroPrevious {
    HeroFrozenBSDF bsdf;
    ScatteringFrame frame;
    struct Point {
        float3 position, geometric_normal, shading_normal;
    } point;
    uint type, seed, path_id;
    bool delta;
};
inline HeroPrevious hero_freeze_previous(const thread Vertex &v) {
    return {hero_freeze_bsdf(v.bsdf),
            v.frame,
            {v.point.position, v.point.geometric_normal, v.point.shading_normal},
            v.type,
            v.seed,
            v.path_id,
            v.delta};
}
inline HeroPrevious hero_freeze_previous(const thread HeroCurrentVertex &v) {
    HeroFrozenBSDF frozen{};
    frozen.kind = v.bsdf.kind;
    frozen.active_mask = v.bsdf.active_mask;
    // Terminal coating events can precede collapse. Match history's invalid
    // sentinel AND zero payload, rather than persisting a queryable coating.
    if (!hero_current_structure(v.bsdf) ||
        ((v.bsdf.kind == 5 || v.bsdf.kind == 6) &&
         (v.bsdf.active_mask != 1 || !hero_current_coated_compatible(v.bsdf))))
        frozen.kind = 0xffffffffu;
    else
        frozen = v.bsdf;
    return {frozen, v.frame, {v.point.position, v.point.geometric_normal, v.point.shading_normal},
            v.type, v.seed,  v.path_id,
            v.delta};
}
using HeroWalkState = PTWalkState<HeroPrevious, true>;
inline void hero_store_previous(thread Vertex &out, const thread Vertex &v) { out = v; }
inline void hero_store_previous(thread HeroPrevious &out, const thread Vertex &v) {
    out = hero_freeze_previous(v);
}
inline void hero_store_previous(thread HeroPrevious &out, const thread HeroCurrentVertex &v) {
    out = hero_freeze_previous(v);
}
inline bool infinite(const thread HeroPrevious &v) { return v.type == environment_vertex; }
template <typename From, typename To>
inline float hero_convert_density(float pdf, const thread From &from, const thread To &to) {
    if (!isfinite(pdf) || pdf < 0)
        return NAN;
    if (infinite(to))
        return pdf;
    const float3 d = to.point.position - from.point.position;
    const float d2 = dot(d, d);
    if (!all(isfinite(d)) || !isfinite(d2))
        return NAN;
    if (!(d2 > 0))
        return 0;
    if (any(to.point.geometric_normal != 0))
        pdf *= abs(dot(to.point.geometric_normal, d * rsqrt(d2)));
    return pdf / d2;
}
inline float convert_density(float pdf, const thread HeroPrevious &from, const thread Vertex &to) {
    return hero_convert_density(pdf, from, to);
}
inline float convert_density(float pdf, const thread Vertex &from, const thread HeroPrevious &to) {
    return hero_convert_density(pdf, from, to);
}
struct HeroPTState {
    HeroPrevious latest;
    PacketPTHot hot;
};
static_assert(sizeof(HeroPrevious) == 224 && sizeof(HeroPrevious) < sizeof(Vertex) &&
                  sizeof(HeroPTState) < sizeof(PacketPTState) && alignof(HeroPrevious) <= 16 &&
                  alignof(HeroPTState) <= 16 &&
                  sizeof(HeroPTState) == sizeof(HeroPrevious) + sizeof(PacketPTHot),
              "Hero history must reduce persistent state without padding surprises");
