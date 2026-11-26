// SPDX-License-Identifier: GPL-3.0-only AND Apache-2.0
// Incident sampling follows packet_transport.h's PBRT adaptation.
// Copyright (c) 1998–2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt. Owned-measure evaluation is a rypath adaptation.
#pragma once
// Included inside rypath_bdpt after hero_pt_helpers.h. The legacy incident
// sampler/direct estimator are unchanged. Owned terminal NEE uses a separate
// raw-radiance evaluator: it never divides by a source PDF and then undoes it.
namespace enhanced_light {
namespace ew = rypath::enhanced_wire;
namespace ep = rypath_enhanced_path;
namespace rm = rypath_reuse_mis;
struct Sample {
    HeroLightEndpoint endpoint;
    float3 direction = 0;
    float4 radiance = 0;
    float incident_pdf = 0;
    rm::Scaled generation_h{.5f, 1};
    ew::Record record = ew::empty_record();
    bool present = false;
};
inline bool positive(float x) {
    return ep::math::finite(x) && (ep::math::bits(x) & 0x80000000u) == 0 &&
           (ep::math::bits(x) & 0x7fffffffu) != 0;
}
inline bool nonnegative(float x) {
    const uint bits = ep::math::bits(x);
    return ep::math::finite(x) && ((bits & 0x80000000u) == 0 || (bits & 0x7fffffffu) == 0);
}
inline bool valid_spectrum(float4 values) {
    for (uint lane = 0; lane < 4; ++lane)
        if (!nonnegative(values[lane]))
            return false;
    return true;
}
inline bool exact_zero(float3 delta) {
    return all((as_type<uint3>(delta) & uint3(0x7fffffffu)) == uint3(0));
}
inline bool valid_record(const thread ew::Record &v, uint event) {
    if (v.version != ew::record_version || v.tag > ew::environment_omega || v.reserved != 0)
        return false;
    if (v.tag == ew::generic)
        return v.light_id == ew::invalid && v.event_ordinal == ew::invalid &&
               v.instance == ew::invalid && v.primitive == ew::invalid && v.kind == ew::invalid &&
               (v.coordinate_bits[0] | v.coordinate_bits[1] | v.coordinate_bits[2] |
                v.coordinate_bits[3]) == 0;
    if (v.light_id == ew::invalid || v.event_ordinal != event || v.coordinate_bits[3] != 0)
        return false;
    if (v.tag == ew::triangle_area) {
        const float2 uv(as_type<float>(v.coordinate_bits[0]), as_type<float>(v.coordinate_bits[1]));
        // surface_point owns the exact sampler's UV convention, including its
        // rounded shared-edge coordinates. Do not quantize or clamp the record.
        return v.instance != ew::invalid && v.primitive != ew::invalid &&
               v.kind == wire::triangle_primitive && all(isfinite(uv)) && nonnegative(uv.x) &&
               nonnegative(uv.y) && all(uv <= 1) && uv.x + uv.y <= 0x1.000002p0f &&
               v.coordinate_bits[2] == 0;
    }
    if (v.instance != ew::invalid || v.primitive != ew::invalid || v.kind != ew::invalid)
        return false;
    if (v.tag == ew::environment_omega) {
        const float3 wi(as_type<float>(v.coordinate_bits[0]), as_type<float>(v.coordinate_bits[1]),
                        as_type<float>(v.coordinate_bits[2]));
        return all(isfinite(wi)) && abs(dot(wi, wi) - 1.f) <= 0x1p-18f;
    }
    return (v.coordinate_bits[0] | v.coordinate_bits[1] | v.coordinate_bits[2]) == 0;
}
template <typename Receiver>
inline bool geometry(const thread Receiver &from, const thread HeroLightEndpoint &to, uint tag,
                     thread rm::Scaled &factor, thread bool &support) {
    support = true;
    factor = {.5f, 1};
    if (tag != ew::triangle_area && tag != ew::positional_mass)
        return true;
    const float3 delta = to.point.position - from.point.position;
    const float distance2 = dot(delta, delta);
    if (!nonnegative(distance2))
        return false;
    if (!positive(distance2)) {
        if (!exact_zero(delta))
            return false; // Nonzero separation underflow is a range failure, not absent support.
        support = false;
        return true;
    }
    factor = rm::quotient(factor, rm::decompose_positive(distance2));
    if (tag == ew::triangle_area) {
        const float cosine = abs(dot(to.point.geometric_normal, delta * rsqrt(distance2)));
        if (!nonnegative(cosine))
            return false;
        if (!positive(cosine)) {
            support = false;
            return true;
        }
        factor = rm::product(factor, rm::decompose_positive(cosine));
    }
    return true;
}
// The marginal LightID probability returned by the light sampler is retained
// separately from the reverse PMF query. Conditional alias/BVH route labels
// are not coordinates of this owned endpoint measure.
template <typename Receiver>
inline bool sample(thread Resources &r, const thread Receiver &from,
                   const thread Wavelengths &lambda, float4 u, uint event, thread Sample &s) {
    s = {};
    const constant auto &p = *r.alpha.parameters;
    const auto choice = rypath_light_sampling::sample(
        p, r.alpha.material_data, {from.point.position, from.point.shading_normal}, u.x);
    if (!nonnegative(choice.pmf))
        return false;
    if (!positive(choice.pmf))
        return choice.light ==
               ew::invalid; // A selected positive branch must not underflow its PMF.
    if (choice.light >= p.light_count ||
        !sample_light_point(r, choice.light, u.yz, s.endpoint, false))
        return false;
    thread auto &v = s.endpoint;
    uint tag = ew::generic;
    float conditional_pdf = 1;
    if (choice.light < p.emitter_count) {
        const auto emitter = r.emitters[choice.light];
        if (!rypath_light_geometry::sample(r.alpha, emitter, from.point.position,
                                           from.point.shading_normal, u.yz, v.uv, v.point,
                                           conditional_pdf))
            return false;
        if (v.point.inverse_area_pdf == 0)
            return true;
        const float3 delta = v.point.position - from.point.position;
        const float distance2 = dot(delta, delta);
        if (!nonnegative(distance2))
            return false;
        if (!positive(distance2))
            return exact_zero(delta);
        s.direction = delta * rsqrt(distance2);
        if (!emitted(r, v, -s.direction, lambda, s.radiance))
            return false;
        tag = emitter.kind == wire::triangle_primitive ? ew::triangle_area : ew::generic;
    } else if (choice.light < p.emitter_count + p.positional_count) {
        const uint id = choice.light - p.emitter_count;
        const device auto *light = positional_record(r.alpha.material_data, p, id);
        if (!light)
            return false;
        v.delta_light = true;
        v.point.position = unpack(light->position);
        const float3 delta = v.point.position - from.point.position;
        const float distance2 = dot(delta, delta);
        if (!nonnegative(distance2))
            return false;
        if (!positive(distance2))
            return exact_zero(delta);
        s.direction = delta * rsqrt(distance2);
        s.radiance = positional_radiance(r, id, s.direction, lambda);
        tag = ew::positional_mass;
    } else {
        v.type = environment_vertex;
        if (choice.light < p.emitter_count + p.positional_count + p.distant_count) {
            const uint id = choice.light - p.emitter_count - p.positional_count;
            const device auto *light = distant_record(r.alpha.material_data, p, id);
            if (!light)
                return false;
            s.direction = unpack(light->direction);
            s.radiance = distant_radiance(r, id, lambda);
            v.delta_light = true;
            v.connectible = false;
            tag = ew::distant_mass;
        } else {
            if (!sample_environment(r, u.yz, s.direction, conditional_pdf))
                return false;
            s.radiance = environment_radiance(r, s.direction, lambda);
            tag = ew::environment_omega;
        }
        v.point.position = from.point.position + 2 * p.scene_radius * s.direction;
        v.point.geometric_normal = v.point.shading_normal = -s.direction;
    }
    if (!nonnegative(conditional_pdf) || !valid_spectrum(s.radiance) || !all(isfinite(s.direction)))
        return false;
    if (!positive(conditional_pdf)) {
        if (tag != ew::triangle_area)
            return true;
        rm::Scaled geometric;
        bool support;
        if (!geometry(from, v, tag, geometric, support))
            return false;
        return !support; // Coplanar/grazing zero is real; positive geometry with PDF0 is a failure.
    }
    s.present = true;
    s.incident_pdf = conditional_pdf * choice.pmf;
    if (!nonnegative(s.incident_pdf))
        return false;
    if (tag == ew::generic)
        return true;
    rm::Scaled geometric;
    bool support;
    if (!geometry(from, v, tag, geometric, support))
        return false;
    if (!support) {
        s.present = false;
        return true;
    }
    s.generation_h =
        rm::product(rm::decompose_positive(choice.pmf), rm::decompose_positive(conditional_pdf));
    if (tag == ew::triangle_area)
        s.generation_h = rm::product(s.generation_h, geometric);
    s.record = {ew::record_version, tag, choice.light, event, ew::invalid, ew::invalid,
                ew::invalid,        0,   {0, 0, 0, 0}};
    if (tag == ew::triangle_area) {
        s.record.instance = v.instance;
        s.record.primitive = v.primitive;
        s.record.kind = v.kind;
        s.record.coordinate_bits[0] = as_type<uint>(v.uv.x);
        s.record.coordinate_bits[1] = as_type<uint>(v.uv.y);
    } else if (tag == ew::environment_omega) {
        for (uint i = 0; i < 3; ++i)
            s.record.coordinate_bits[i] = as_type<uint>(s.direction[i]);
    }
    return valid_record(s.record, event);
}
template <typename Receiver>
inline bool reconstruct(thread Resources &r, const thread Receiver &from,
                        const thread Wavelengths &lambda, const thread ew::Record &record,
                        uint event, thread Sample &s) {
    s = {};
    const constant auto &p = *r.alpha.parameters;
    if (!valid_record(record, event) || record.tag == ew::generic ||
        record.light_id >= p.light_count)
        return false;
    const uint id = record.light_id;
    if (!sample_light_point(r, id, float2(0), s.endpoint, false))
        return false;
    thread auto &v = s.endpoint;
    s.record = record;
    if (record.tag == ew::triangle_area) {
        if (id >= p.emitter_count || v.instance != record.instance ||
            v.primitive != record.primitive || v.kind != record.kind)
            return false;
        v.uv = float2(as_type<float>(record.coordinate_bits[0]),
                      as_type<float>(record.coordinate_bits[1]));
        if (!surface(r, v.uv, record.instance, record.primitive, record.kind,
                     PointSource::area_sample, v.point))
            return false;
        const float3 delta = v.point.position - from.point.position;
        const float d2 = dot(delta, delta);
        if (!nonnegative(d2))
            return false;
        if (!positive(d2))
            return exact_zero(delta);
        s.direction = delta * rsqrt(d2);
        if (!emitted(r, v, -s.direction, lambda, s.radiance))
            return false;
    } else if (record.tag == ew::positional_mass) {
        if (id < p.emitter_count || id >= p.emitter_count + p.positional_count)
            return false;
        const uint index = id - p.emitter_count;
        const device auto *light = positional_record(r.alpha.material_data, p, index);
        if (!light)
            return false;
        v.delta_light = true;
        v.point.position = unpack(light->position);
        const float3 delta = v.point.position - from.point.position;
        const float d2 = dot(delta, delta);
        if (!nonnegative(d2))
            return false;
        if (!positive(d2))
            return exact_zero(delta);
        s.direction = delta * rsqrt(d2);
        s.radiance = positional_radiance(r, index, s.direction, lambda);
    } else {
        v.type = environment_vertex;
        const uint begin = p.emitter_count + p.positional_count;
        if (record.tag == ew::distant_mass) {
            if (id < begin || id >= begin + p.distant_count)
                return false;
            const device auto *light = distant_record(r.alpha.material_data, p, id - begin);
            if (!light)
                return false;
            s.direction = unpack(light->direction);
            s.radiance = distant_radiance(r, id - begin, lambda);
            v.delta_light = true;
            v.connectible = false;
        } else {
            if (id != begin + p.distant_count)
                return false;
            s.direction = float3(as_type<float>(record.coordinate_bits[0]),
                                 as_type<float>(record.coordinate_bits[1]),
                                 as_type<float>(record.coordinate_bits[2]));
            s.radiance = environment_radiance(r, s.direction, lambda);
        }
        v.point.position = from.point.position + 2 * p.scene_radius * s.direction;
        v.point.geometric_normal = v.point.shading_normal = -s.direction;
    }
    s.present = true;
    return valid_spectrum(s.radiance) && all(isfinite(s.direction));
}
inline bool selection_density(constant wire::Parameters &p, const device uint *data,
                              rypath_light_sampling::Context context, uint id,
                              thread rm::Scaled &density, thread bool &support) {
    support = false;
    if (id >= p.light_count)
        return false;
    if (p.light_sampler == 1 || p.light_sampler == 2) {
        const float value = rypath_light_sampling::pmf(p, data, context, id);
        if (!nonnegative(value))
            return false;
        support = positive(value);
        if (support)
            density = rm::decompose_positive(value);
        return true;
    }
    const device auto *records =
        reinterpret_cast<const device wire::LightRecord *>(data + p.light_records_begin);
    if (records[id].flags & 1) {
        const float value = 1.f / float(p.infinite_light_count + (p.light_node_count ? 1 : 0));
        if (!positive(value))
            return false;
        density = rm::decompose_positive(value);
        support = true;
        return true;
    }
    uint node = records[id].leaf;
    if (node >= p.light_node_count)
        return false;
    const device auto *tree = rypath_light_sampling::nodes(p, data);
    const float leaf = rypath_light_sampling::importance(tree[node], context);
    if (!nonnegative(leaf))
        return false;
    if (!positive(leaf))
        return true; // The represented light-bound proposal has zero support here.
    const float root = 1.f - float(p.infinite_light_count) / float(p.infinite_light_count + 1u);
    if (!positive(root))
        return false;
    density = rm::decompose_positive(root);
    for (uint steps = 0; steps < p.light_node_count; ++steps) {
        const uint parent = tree[node].parent;
        if (parent == ew::invalid) {
            support = true;
            return true;
        }
        if (parent + 1u >= p.light_node_count || tree[parent].child >= p.light_node_count)
            return false;
        const float a = rypath_light_sampling::importance(tree[parent + 1u], context);
        const float b = rypath_light_sampling::importance(tree[tree[parent].child], context);
        const float sum = a + b;
        if (!nonnegative(a) || !nonnegative(b) || !nonnegative(sum))
            return false;
        if (!positive(sum))
            return true;
        const float numerator = node == parent + 1u ? a : b;
        if (!positive(numerator))
            return true;
        const float factor = numerator / sum;
        if (!positive(factor))
            return false; // Positive division underflow must not remove the domain.
        density = rm::product(density, rm::decompose_positive(factor));
        node = parent;
    }
    return false;
}
inline bool environment_density(constant wire::TextureGraphParameters &p,
                                const device float *environment_cdf, float3 direction,
                                bool compensated, thread rm::Scaled &density,
                                thread bool &support) {
    support = false;
    uint2 cell;
    if (!rypath_image_light::valid_cdf(p, compensated) ||
        !rypath_image_light::environment_cell(p, direction, cell))
        return false;
    const uint width = p.environment_width;
    const device float *cdf =
        environment_cdf + (compensated ? p.environment_compensated_offset : 0u);
    const device float *row = cdf + (cell.y + 1u) * (width + 1u);
    const auto marginal = rm::signed_sum(cdf[cell.y + 1u], -cdf[cell.y]);
    const auto conditional = rm::signed_sum(row[cell.x + 1u], -row[cell.x]);
    if (marginal.status != ep::math::Status::ok || conditional.status != ep::math::Status::ok ||
        !nonnegative(marginal.value) || marginal.value > 1 || !nonnegative(conditional.value) ||
        conditional.value > 1)
        return false;
    const auto sphere = rm::decompose_positive(rypath_image_light::sphere_density);
    if (positive(marginal.value) && positive(conditional.value)) {
        const auto size = rm::decompose_positive(float(width));
        density = rm::product(rm::product(size, size),
                              rm::product(rm::decompose_positive(marginal.value),
                                          rm::decompose_positive(conditional.value)));
        if (!compensated)
            density = rm::sum(
                rm::decompose_positive(rypath_image_light::uniform_probability),
                rm::product(rm::decompose_positive(1.f - rypath_image_light::uniform_probability),
                            density));
        density = rm::product(density, sphere);
        support = true;
    } else if (!compensated) {
        density =
            rm::product(rm::decompose_positive(rypath_image_light::uniform_probability), sphere);
        support = true;
    }
    return true;
}
template <typename Receiver>
inline bool query(thread Resources &r, const thread Receiver &from, const thread Sample &s,
                  thread rm::Scaled &h, thread rm::Scaled &geometric, thread bool &support) {
    h = geometric = {.5f, 1};
    support = false;
    rm::Scaled selection{.5f, 1};
    if (!selection_density(*r.alpha.parameters, r.alpha.material_data,
                           {from.point.position, from.point.shading_normal}, s.record.light_id,
                           selection, support))
        return false;
    if (!support || !s.present)
        return true;
    if (!geometry(from, s.endpoint, s.record.tag, geometric, support))
        return false;
    if (!support)
        return true;
    float conditional = 1;
    if (s.record.tag == ew::triangle_area) {
        const wire::Emitter emitter{s.record.instance, s.record.primitive, s.record.kind};
        conditional =
            rypath_light_geometry::pdf(r.alpha, emitter, from.point.position,
                                       from.point.shading_normal, s.endpoint.uv, s.endpoint.point);
    } else if (s.record.tag == ew::environment_omega) {
        rm::Scaled environment{.5f, 1};
        if (!environment_image_enabled ||
            !environment_density(*r.alpha.graph.parameters, r.environment_cdf, s.direction,
                                 !packet_bidirectional_enabled(*r.alpha.parameters), environment,
                                 support))
            return false;
        if (support)
            h = rm::product(selection, environment);
        return true;
    }
    if (!nonnegative(conditional))
        return false;
    if (!positive(conditional)) {
        if (s.record.tag == ew::triangle_area)
            return false; // A nondegenerate triangle proposal has strictly positive density.
        support = false;
        return true;
    }
    h = rm::product(selection, rm::decompose_positive(conditional));
    if (s.record.tag == ew::triangle_area)
        h = rm::product(h, geometric);
    return true;
}
inline bool physical(float4 beta, float4 f, float4 radiance, float cosine, rm::Scaled geometric,
                     thread float4 &out) {
    out = 0;
    if (!valid_spectrum(beta) || !valid_spectrum(f) || !valid_spectrum(radiance) ||
        !nonnegative(cosine))
        return false;
    if (!positive(cosine))
        return true;
    const auto geometry_cosine = rm::product(geometric, rm::decompose_positive(cosine));
    for (uint lane = 0; lane < 4; ++lane) {
        if (!positive(beta[lane]) || !positive(f[lane]) || !positive(radiance[lane]))
            continue;
        const auto product = rm::product(rm::product(rm::product(rm::decompose_positive(beta[lane]),
                                                                 rm::decompose_positive(f[lane])),
                                                     rm::decompose_positive(radiance[lane])),
                                         geometry_cosine);
        const auto value = rm::materialize_contribution(product);
        if (value.status != ep::math::Status::ok)
            return false;
        out[lane] = value.value;
    }
    return true;
}
} // namespace enhanced_light

template <typename Current, typename Previous, typename Sink>
inline float4 hero_direct_enhanced(thread Resources &r, thread wire::Sample &sample,
                                   const thread Wavelengths &lambda, ulong index,
                                   const thread Current &pt, const thread Previous &before, uint t,
                                   float4 beta, const thread rypath_hero::Density &prefix,
                                   thread const rypath_replay::ReplayState *replay,
                                   thread Sink &sink) {
    namespace el = enhanced_light;
    namespace ew = rypath::enhanced_wire;
    sink.reset_atom();
    if (pt.type != surface_vertex || !pt.connectible)
        return 0;
    const uint event = t - 2u;
    const bool imported_terminal = sink.mapping_import(event);
    const auto imported = imported_terminal ? sink.imported_record() : ew::empty_record();
    if (sink.state.failed)
        return 0;
    el::Sample light;
    if (imported_terminal && imported.tag != ew::generic) {
        if (!el::reconstruct(r, pt, lambda, imported, event, light)) {
            sample.status = wire::invalid_arithmetic;
            return 0;
        }
    } else {
        const constant auto &p = *r.alpha.parameters;
        if (!el::sample(r, pt, lambda, uniforms4(p, random(r, sample, index, 133 + event, replay)),
                        event, light)) {
            sample.status = wire::invalid_arithmetic;
            return 0;
        }
        if (imported_terminal && light.record.tag != ew::generic)
            return 0; // Generic and owned terminal charts are disjoint domains.
    }
    if (!light.present)
        return 0;
    const bool owned = light.record.tag != ew::generic;
    el::rm::Scaled query_h{.5f, 1}, geometric{.5f, 1};
    if (owned) {
        bool support;
        if (!el::query(r, pt, light, query_h, geometric, support)) {
            sample.status = wire::invalid_arithmetic;
            return 0;
        }
        if (!support) {
            if (!imported_terminal)
                sample.status = wire::invalid_arithmetic;
            return 0;
        }
        sink.set_atom(light.record, query_h, light.generation_h);
    }
    float4 incident = light.radiance;
    if (!owned) {
        if (!(light.incident_pdf > 0))
            return 0;
        incident /= light.incident_pdf;
        if (!finite_nonnegative(incident)) {
            sample.status = wire::invalid_arithmetic;
            return 0;
        }
    }
    if (!nonzero(incident))
        return 0;
    const auto evaluated =
        hero_evaluate_current(pt.bsdf, pt.pair_valid ? pt.sampled_wo_local : pt.frame.local(pt.wo),
                              pt.frame.local(light.direction), pt.seed);
    float4 value;
    if (!evaluated.valid ||
        (owned &&
         !el::physical(beta, evaluated.f, incident,
                       abs(dot(pt.point.shading_normal, light.direction)), geometric, value))) {
        sample.status = wire::invalid_arithmetic;
        return 0;
    }
    if (!owned)
        value = beta * evaluated.f * incident * abs(dot(pt.point.shading_normal, light.direction));
    if (!nonzero(value) || !visible(r, pt, light.endpoint, sample))
        return 0;
    if (light.endpoint.delta_light)
        return value;
    // This is the original represented four-lane technique MIS. It is separate
    // from the owned chart's reference density and the actual fresh proposal.
    const float light_pdf = direct_pdf(r, pt, light.endpoint);
    const float4 bsdf_pdf = hero_represented_pdf(pt, before, light.endpoint);
    return hero_weight(sample, prefix, value, float4(light_pdf), bsdf_pdf);
}
