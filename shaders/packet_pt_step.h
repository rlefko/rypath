// SPDX-License-Identifier: Apache-2.0
// Resumable vacuum PT scheduling adapted from packet_walk.h's PBRT random walk.
// Copyright (c) 1998–2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt.
#pragma once
// Included inside rypath_bdpt after the shared PT connection helpers.

// GPU-only opaque state: host code allocates sizeof(PacketPTState) bytes per
// active lane. Kernels copy device state into thread storage before calling these
// helpers and write it back afterward. Resources/SurfaceAlpha are reconstructed
// per dispatch; they are never stored here. Vertex Prepared retains immutable
// scene table pointers, which must remain valid through command completion.
struct PacketPTState {
    // PT retains only its latest frozen BSDF/frame. The older endpoint enters
    // represented_scattering_pdf only through type and position: a continuing
    // walk contains camera/surface history, never a terminal environment vertex.
    Vertex latest;
    float3 preceding_position;
    uint preceding_type;
    wire::Sample sample;
    Wavelengths lambda;
    float4 radiance, beta;
    float3 ray_origin, ray_direction;
    float pdf, ray_min_distance, ray_max_distance;
    rypath_packet_roulette::EtaScale roulette_eta;
    ulong index;
    uint count, crossings;
    bool complete;
};
// Hot tracing/publication state. The full frozen Vertex lives in a separate cold pool.
struct PacketPTHot {
    float3 preceding_position;
    uint preceding_type;
    wire::Sample sample;
    Wavelengths lambda;
    float4 radiance, beta;
    float3 ray_origin, ray_direction;
    float pdf, ray_min_distance, ray_max_distance;
    rypath_packet_roulette::EtaScale roulette_eta;
    ulong index;
    uint count, crossings;
    bool complete;
};
struct PacketPTHit {
    SurfaceHit hit;
    uint ready;
};

static_assert(alignof(PacketPTState) <= 16 && alignof(PacketPTHot) <= 16 &&
                  alignof(PacketPTHit) <= 16 &&
                  sizeof(PacketPTHot) + sizeof(Vertex) == sizeof(PacketPTState),
              "Packet PT split-state ABI/alignment");
inline void packet_pt_initialize(thread PacketPTState &state, const thread wire::Sample &sample,
                                 const thread Wavelengths &lambda, ray incoming, float pdf,
                                 ulong index, constant wire::Parameters &p) {
    state = {};
    state.sample = sample;
    state.lambda = lambda;
    state.index = index;
    state.ray_origin = incoming.origin;
    state.ray_direction = incoming.direction;
    state.pdf = pdf;
    state.ray_min_distance = incoming.min_distance;
    state.ray_max_distance = incoming.max_distance;
    state.beta = 1;
    state.roulette_eta = {};
    state.count = 1;
    thread auto &endpoint = state.latest;
    endpoint.type = camera_vertex;
    endpoint.path_id = 0;
    endpoint.point.position = incoming.origin;
    endpoint.point.geometric_normal = unpack(p.camera_normal);
    endpoint.point.shading_normal = unpack(p.camera_normal);
    endpoint.beta = 1;
    endpoint.connectible = true;
    if (sample.status != wire::success || packet_bidirectional_enabled(p) ||
        p.camera_medium != 0xffffffffu || !all(isfinite(incoming.origin)) ||
        !all(isfinite(incoming.direction)) || !isfinite(pdf) || pdf < 0) {
        state.sample.status = wire::invalid_arithmetic;
        state.complete = true;
    }
}

inline SurfaceHit packet_pt_trace(thread Resources &r, thread wire::Sample &sample,
                                  const thread Wavelengths &lambda, ray incoming) {
    const constant auto &p = *r.alpha.parameters;
    MixedIntersector closest;
    closest.set_triangle_cull_mode(triangle_cull_mode::none);
    r.alpha.sample = &sample;
    r.alpha.incoming = incoming;
    sample.wavelength = lambda.lambda[0];
    return p.instance_count == 0 ? SurfaceHit{intersection_type::none, 0, 0, 0, 0, float2(0), true}
           : (native_triangle_hint_mode & 2u)
               ? trace_packet_seeded_closest(incoming, r.scene, all_visible, r.intersection_table,
                                             r.alpha, alpha_enabled)
           : alpha_enabled
               ? trace_masked_surface(incoming, r.scene, all_visible, r.alpha)
               : trace_surface(closest, incoming, r.scene, all_visible, r.intersection_table);
}

// The common numerical body is force-inlined into two distinct noinline entry
// functions. The ordinary event keeps the original three-argument interface;
// pretraced shading cannot retain a nullable-hit branch or event-trace fallback.
struct PacketTraceEvent {};
struct PacketPretracedEvent {
    thread const SurfaceHit &hit;
};
[[clang::always_inline]] inline SurfaceHit
packet_event_hit(thread Resources &r, thread wire::Sample &sample, const thread Wavelengths &lambda,
                 ray incoming, const thread PacketTraceEvent &) {
    return packet_pt_trace(r, sample, lambda, incoming);
}
[[clang::always_inline]] inline SurfaceHit
packet_event_hit(thread Resources &, thread wire::Sample &, const thread Wavelengths &, ray,
                 const thread PacketPretracedEvent &source) {
    return source.hit;
}

// One traced/shaded event, including at most one transparent boundary. The
// numerical body deliberately mirrors random_walk as an independent initial
// execution control. Preserve sampled and represented-density checks, exact local
// pairs, offsets, material streams and eta roulette when maintaining either path.
template <typename HitSource>
[[clang::always_inline]] inline void
packet_pt_event_body(thread Resources &r, thread PacketPTState &state,
                     thread const rypath_replay::ReplayState *replay,
                     const thread HitSource &hit_source) {
    if (state.complete)
        return;
    const constant auto &p = *r.alpha.parameters;
    thread auto &sample = state.sample;
    thread auto &lambda = state.lambda;
    thread auto &beta = state.beta;
    thread auto &pdf = state.pdf;
    thread auto &count = state.count;
    thread auto &crossings = state.crossings;
    thread auto &roulette_eta = state.roulette_eta;
    const ulong index = state.index;
    const uint capacity = p.max_depth + 2;
    const Mode mode = Mode::Radiance;
    ray incoming(state.ray_origin, state.ray_direction, state.ray_min_distance,
                 state.ray_max_distance);

    r.alpha.sample = &sample;
    sample.wavelength = lambda.lambda[0];
    const uint old_count = count;
    bool terminated = true;
    // Keep the current event local until its endpoint/direct contributions have
    // consumed both historical endpoints. It need not survive another dispatch.
    Vertex current{};
    do {
        if (count >= capacity || !nonzero(beta))
            break;
        if (!all(isfinite(incoming.origin)) || !all(isfinite(incoming.direction)) ||
            !finite_nonnegative(beta)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const auto hit = packet_event_hit(r, sample, lambda, incoming, hit_source);
        if (!hit.valid) {
            sample.status = wire::invalid_hit;
            break;
        }
        if (hit.type == intersection_type::none) {
            if (mode == Mode::Radiance) {
                Vertex v{};
                v.type = environment_vertex;
                v.path_id = (mode == Mode::Importance ? 0x80000000u : 0u) | count;
                v.light = p.environment_light;
                v.point.position = incoming.origin + incoming.direction;
                v.point.geometric_normal = -incoming.direction;
                v.point.shading_normal = -incoming.direction;
                v.wo = -incoming.direction;
                v.beta = beta;
                // PT does not consume stored BDPT MIS densities.
                v.connectible = true;
                current = v;
                ++count;
            }
            break;
        }
        if ((hit.type != intersection_type::triangle &&
             hit.type != intersection_type::bounding_box) ||
            hit.instance_id >= p.instance_count || hit.geometry_id != 0 ||
            !isfinite(hit.distance) || hit.distance < 0 || !all(isfinite(hit.coordinates))) {
            sample.status = wire::invalid_hit;
            break;
        }
        const device auto &instance = r.alpha.instances[hit.instance_id];
        const uint kind = hit.primitive_kind;
        if (hit.primitive_id >= primitive_count(instance, kind) ||
            instance.material >= p.material_count) {
            sample.status = wire::invalid_hit;
            break;
        }
        Vertex v{};
        v.type = surface_vertex;
        v.path_id = (mode == Mode::Importance ? 0x80000000u : 0u) | count;
        v.instance = hit.instance_id;
        v.primitive = hit.primitive_id;
        v.kind = kind;
        v.uv = hit.coordinates;
        v.light =
            instance.emission == wire::no_emission
                ? 0xffffffffu
                : instance.emitter_begin +
                      (kind == wire::triangle_primitive ? hit.primitive_id
                       : kind == wire::patch_primitive
                           ? instance.triangle_count + hit.primitive_id
                           : instance.triangle_count + instance.patch_count + hit.primitive_id);
        v.wo = -incoming.direction;
        if (!surface_point(hit.coordinates, kind, instance, hit.primitive_id, r.alpha.positions,
                           r.alpha.triangles, r.alpha.patches, r.alpha.normals, p.normal_count,
                           PointSource::native_hit, v.point, r.alpha.material_data)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        if (instance.invisible_emitter != 0 ||
            r.alpha.materials[instance.material].scattering == 10) {
            if (crossings++ >= p.boundary_limit) {
                sample.status = wire::boundary_exhaustion;
                break;
            }
            float3 origin;
            if (!spawn_boundary(incoming, hit.distance, v.point, incoming.direction, origin)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            incoming = ray(origin, incoming.direction);
            terminated = false;
            break;
        }
        const device auto &root_material = r.alpha.materials[instance.material];
        const bool context_material =
            root_material.scattering != 0 || root_material.kind == wire::image_material ||
            root_material.kind == wire::graph_material || root_material.bump_root != 0xffffffffu ||
            root_material.normal_map != 0xffffffffu;
        wire::Sample texture_sample = sample;
        texture_sample.scatterings = mode == Mode::Radiance ? count - 1 : max(1u, count - 1);
        graph_wire::Context context{};
        float3 dpdu, dpdv;
        if (context_material &&
            !surface_texture_context(instance, hit.primitive_id, kind, hit.coordinates, v.point,
                                     r.alpha.positions, r.alpha.triangles, r.alpha.patches,
                                     r.alpha.uvs, r.alpha.frames, *r.alpha.graph.images.parameters,
                                     p, texture_sample, incoming.direction, true, context, dpdu,
                                     dpdv)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        uint material_index = instance.material;
        for (uint depth = 0; depth <= 64; ++depth) {
            const device auto &node = r.alpha.materials[material_index];
            if (node.scattering != 9)
                break;
            const device auto *aux =
                material_auxiliary(node, r.alpha.material_data, p.material_data_words);
            float amount;
            if (!aux || depth == 64 ||
                !parameter_value(aux->mix_amount_root, aux->mix_amount, r.alpha.graph, context,
                                 lambda.lambda[0], amount)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            const float selector = alpha_uniform(v.point.position, -incoming.direction,
                                                 as_type<float>(material_index));
            material_index = amount <= 0         ? aux->first_material
                             : amount >= 1       ? aux->second_material
                             : selector < amount ? aux->second_material
                                                 : aux->first_material;
            if (material_index >= p.material_count) {
                sample.status = wire::invalid_hit;
                break;
            }
        }
        if (sample.status != wire::success)
            break;
        const device auto &material = r.alpha.materials[material_index];
        if (material.scattering == 10) {
            if (crossings++ >= p.boundary_limit) {
                sample.status = wire::boundary_exhaustion;
                break;
            }
            float3 origin;
            if (!spawn_boundary(incoming, hit.distance, v.point, incoming.direction, origin)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            incoming = ray(origin, incoming.direction);
            terminated = false;
            break;
        }
        rypath_surface_detail::Frame frame{};
        frame.geometric_normal = v.point.geometric_normal;
        frame.n = v.point.shading_normal;
        rypath_surface_detail::coordinate_system(frame.n, frame.dpdu, frame.dpdv);
        const bool detail = surface_detail_enabled && (material.bump_root != 0xffffffffu ||
                                                       material.normal_map != 0xffffffffu);
        const bool framed = (conductor_enabled || dielectric_enabled || surface_materials_enabled ||
                             gltf_enabled) &&
                            material.scattering != 0;
        if (((detail || framed) &&
             !apply_surface_detail(
                 hit.instance_id, instance, hit.primitive_id, kind, hit.coordinates,
                 r.alpha.triangles, r.alpha.patches, r.alpha.uvs, r.alpha.vertices,
                 r.alpha.transforms, material, r.alpha.graph, context, lambda.lambda[0], v.point,
                 frame, true,
                 material_auxiliary(material, r.alpha.material_data, p.material_data_words),
                 incoming.direction)) ||
            !scattering_frame(frame, v.frame) ||
            !prepare_packet(r, material, context, lambda, beta, v.bsdf)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        float3 ray_position = fma(float3(hit.distance), incoming.direction, incoming.origin);
        float3 ray_error =
            gamma_bound(2) * (abs(hit.distance * incoming.direction) + abs(incoming.origin));
        if (v.point.patch) {
            for (uint axis = 0; axis < 3; ++axis) {
                const auto x =
                    BilinearNumber(hit.distance) * incoming.direction[axis] + incoming.origin[axis];
                ray_position[axis] = x.high;
                ray_error[axis] = abs(x.low) +
                                  0x1p-40f * (abs(hit.distance * incoming.direction[axis]) +
                                              abs(incoming.origin[axis])) +
                                  128 * FLT_MIN;
            }
        }
        v.point.error += abs(v.point.position - ray_position) + ray_error;
        if (!all(isfinite(v.point.error))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const auto words = material_words(r, sample, material_index, count - 1, mode, replay);
        v.seed = words.w;
        v.beta = beta;
        v.connectible = !rypath_packet::delta_only(v.bsdf);
        const Vertex prev = state.latest;
        const float sampled_area_pdf = convert_density(pdf, prev, v);
        // Retain sampled-area validation even when PT skips unused BDPT storage.
        if (!isfinite(sampled_area_pdf) || sampled_area_pdf < 0) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        current = v;
        ++count;
        crossings = 0;
        if (count == capacity)
            break;
        if (count - 2 >= p.emergency_limit) {
            sample.status = wire::emergency_depth;
            break;
        }
        const auto draws =
            random(r, sample, index, (mode == Mode::Radiance ? 4u : 69u) + count - 2, replay);
        const float3 sampled_wo_local = v.frame.local(v.wo);
        const auto continuation =
            rypath_packet::sample(v.bsdf, sampled_wo_local, sample_uniform(p, draws.x),
                                  uniform2(p, draws.yz), v.seed, mode);
        if (!continuation.valid) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        if (!continuation.accepted)
            break;
        // Retain exact sampled locals for throughput and directional-f queries.
        // MIS densities instead use actual neighboring geometry after tracing.
        thread auto &stored = current;
        stored.sampled_wo_local = sampled_wo_local;
        stored.sampled_wi_local = continuation.wi;
        stored.sampled_prev_id = prev.path_id;
        stored.sampled_next_id = (mode == Mode::Importance ? 0x80000000u : 0u) | count;
        stored.pair_valid = true;
        float3 wi = v.frame.world(continuation.wi);
        if (!rypath_surface_detail::unit_vector(wi, wi)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        // Preserve the sampled joint coating-walk ratio. Its directional MIS
        // proxy is a separate density and must never replace this weight.
        if (mode == Mode::Importance) {
            // Use the exact local cosines used by sample(), not a world/local
            // round trip that can erase a nonzero grazing sampled direction.
            const auto correction = rypath_shading_normal::correction(
                sampled_wo_local.z, continuation.wi.z, dot(v.wo, v.point.geometric_normal),
                dot(wi, v.point.geometric_normal));
            for (uint lane = 0; lane < 4; ++lane)
                beta[lane] = rypath_shading_normal::multiply(beta[lane], continuation.weight[lane],
                                                             correction);
        } else {
            beta *= continuation.weight;
        }
        if (!finite_nonnegative(beta)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        // Keep original sampled forward/reverse validation. Canonical queries
        // must not conceal an invalid sampled proxy or its area conversion.
        pdf = continuation.pdf;
        float reverse_pdf = continuation.reverse_pdf;
        if (continuation.delta) {
            current.delta = true;
            pdf = reverse_pdf = 0;
        }
        const float reverse_area_pdf = convert_density(reverse_pdf, v, prev);
        if (!isfinite(reverse_area_pdf) || reverse_area_pdf < 0) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const float3 origin = offset(v, wi);
        if (!all(isfinite(origin))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        incoming = ray(origin, wi);
        if (!packet_bidirectional_enabled(p) && mode == Mode::Radiance) {
            rypath_packet_roulette::account_transmission(roulette_eta, continuation.eta,
                                                         continuation.transmission);
            if (nonzero(beta)) {
                // Reserved fourth continuation component: no dimension renumbering
                // or reuse of the independent material/coating random streams.
                const float u_rr = sample_uniform(p, draws.w);
                if (!rypath_packet_roulette::continue_path(beta, roulette_eta, count - 1, u_rr))
                    break;
            }
        }
        terminated = false;

    } while (false);
    state.ray_origin = incoming.origin;
    state.ray_direction = incoming.direction;
    state.ray_min_distance = incoming.min_distance;
    state.ray_max_distance = incoming.max_distance;
    state.complete =
        terminated || sample.status != wire::success || count >= capacity || !nonzero(beta);
    if (count != old_count && sample.status == wire::success) {
        const Vertex pt = current;
        const Vertex previous = state.latest;
        Vertex before{};
        if (count > 2) {
            before.type = state.preceding_type;
            before.point.position = state.preceding_position;
        }
        // Match offline t-major, s=0 then s=1 accumulation. Sampled-local pairs
        // have been finalized before f() is queried, including rejected events.
        state.radiance += packet_pt_endpoint(r, sample, lambda, pt, previous, before, count);
        if (sample.status == wire::success && count < capacity)
            state.radiance +=
                packet_pt_direct(r, sample, lambda, index, pt, previous, count, replay);
        if (!finite_nonnegative(state.radiance))
            sample.status = wire::invalid_arithmetic;
        if (sample.status != wire::success)
            state.complete = true;
    }
    if (count != old_count) {
        // Null/invisible boundaries leave count unchanged and preserve history.
        // Environment endpoints are evaluated above and mark the walk complete;
        // their normals are therefore never needed as a later preceding endpoint.
        state.preceding_position = state.latest.point.position;
        state.preceding_type = state.latest.type;
        state.latest = current;
    }
}
[[clang::noinline]] inline void
packet_pt_step(thread Resources &r, thread PacketPTState &state,
               thread const rypath_replay::ReplayState *replay = nullptr) {
    const PacketTraceEvent source{};
    packet_pt_event_body(r, state, replay, source);
}
[[clang::noinline]] inline void
packet_pt_step_pretraced(thread Resources &r, thread PacketPTState &state,
                         thread const rypath_replay::ReplayState *replay,
                         thread const SurfaceHit &hit) {
    const PacketPretracedEvent source{hit};
    packet_pt_event_body(r, state, replay, source);
}
inline storage::Result packet_pt_result(const thread PacketPTState &state,
                                        constant wire::Parameters &p) {
    storage::Result result{};
    result.status = state.complete ? state.sample.status : wire::invalid_arithmetic;
    result.pixel = state.sample.pixel;
    result.sample_low = state.sample.sample_low;
    result.sample_high = state.sample.sample_high;
    result.filter_weight = p.packet_filter_weight;
    for (uint lane = 0; lane < 4; ++lane) {
        result.wavelengths[lane] = state.lambda.lambda[lane];
        result.wavelength_pdfs[lane] = state.lambda.pdf[lane];
        result.radiance[lane] = state.radiance[lane];
    }
    return result;
}
