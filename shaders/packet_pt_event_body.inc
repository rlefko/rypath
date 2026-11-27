// SPDX-License-Identifier: Apache-2.0
// Shared original packet-event body; intentionally included inside each function.
// Copyright (c) 1998–2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt. Do not add an include guard.
#ifndef RYPATH_PACKET_EVENT_PRETRACED
#error "Select the packet event hit source before including this body"
#endif
#ifndef RYPATH_PACKET_EVENT_REUSE_CURRENT
#error "Select whether the compact hero event reuses current storage"
#endif
    if (state.complete)
        return;
#if RYPATH_PACKET_EVENT_ENHANCED
    sink.begin_event();
#endif
    const constant auto &p = *r.alpha.parameters;
    thread auto &sample = state.sample;
    thread auto &lambda = state.lambda;
    thread auto &beta = state.beta;
    thread auto &pdf = state.pdf;
    thread auto &count = state.count;
    thread auto &crossings = state.crossings;
    thread auto &roulette_eta = state.roulette_eta;
    const bool hero = hero_enabled(p);
    const auto arrival_density = state.hero.density;
    const auto previous_density = state.hero.previous;
    auto direct_density = arrival_density;
    float4 direct_beta = beta;
    bool direct_allowed = true;
    const ulong index = state.index;
    const uint capacity = p.max_depth + 2;
    const Mode mode = Mode::Radiance;
    ray incoming(state.ray_origin, state.ray_direction, state.ray_min_distance,
                 state.ray_max_distance);
#if !RYPATH_PACKET_EVENT_PRETRACED
    MixedIntersector closest;
    closest.set_triangle_cull_mode(triangle_cull_mode::none);
#endif
    r.alpha.sample = &sample;
    sample.wavelength = lambda.lambda[0];
    const uint old_count = count;
    bool terminated = true;
#if RYPATH_PACKET_EVENT_ENHANCED
    namespace hybrid_runtime = enhanced_hybrid_runtime;
    namespace hybrid_geometry = enhanced_hybrid_geometry;
    namespace hybrid_chart = ::rypath_enhanced_chart;
    const float incoming_generation_q = pdf;
    hybrid_runtime::Pending hybrid_pending;
    const auto hybrid_import = sink.support_record();
    const bool forced_pivot = !sink.has_interior() &&
                              hybrid_import.tag == hybrid_chart::interior_zomega &&
                              count == hybrid_import.event_ordinal + 1u;
    bool forced_continuation = false;
    float3 forced_omega = 0;
    if (sink.state.failed) {
        sample.status = wire::invalid_arithmetic;
        state.complete = true;
        return;
    }
#endif
    // Keep the current event local until its endpoint/direct contributions have
    // consumed both historical endpoints. It need not survive another dispatch.
    typename State::Current current{};
    do {
        if (count >= capacity || !nonzero(beta))
            break;
        if (!all(isfinite(incoming.origin)) || !all(isfinite(incoming.direction)) ||
            !finite_nonnegative(beta)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        r.alpha.incoming = incoming;
#if RYPATH_PACKET_EVENT_PRETRACED
        auto hit = pretraced;
#else
        auto hit =
            p.instance_count == 0 ? SurfaceHit{intersection_type::none, 0, 0, 0, 0, float2(0), true}
            : (native_triangle_hint_mode & 2u)
                ? trace_packet_seeded_closest(incoming, r.scene, all_visible, r.intersection_table,
                                              r.alpha, alpha_enabled)
            : alpha_enabled
                ? trace_masked_surface(incoming, r.scene, all_visible, r.alpha)
                : trace_surface(closest, incoming, r.scene, all_visible, r.intersection_table);
#endif
        if (!hit.valid) {
            sample.status = wire::invalid_hit;
            break;
        }
#if RYPATH_PACKET_EVENT_ENHANCED
        if (forced_pivot) {
            if (hit.type != intersection_type::triangle || hit.geometry_id != 0 ||
                hit.instance_id != hybrid_import.instance ||
                hit.primitive_id != hybrid_import.primitive ||
                hit.primitive_kind != wire::triangle_primitive) {
                hybrid_runtime::outside(state, sink);
                break;
            }
            // Traversal certifies identity. The chart owns UV, so an offset
            // ray's returned coordinates can never change the fixed endpoint.
            hit.coordinates = float2(as_type<float>(hybrid_import.coordinate_bits[0]),
                                     as_type<float>(hybrid_import.coordinate_bits[1]));
        }
#endif
        if (hit.type == intersection_type::none) {
            if (mode == Mode::Radiance) {
#if RYPATH_PACKET_EVENT_REUSE_CURRENT
                thread typename State::Current &v = current;
#else
                typename State::Current v{};
#endif
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
#if !RYPATH_PACKET_EVENT_REUSE_CURRENT
                current = v;
#endif
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
#if RYPATH_PACKET_EVENT_REUSE_CURRENT
        thread typename State::Current &v = current;
#else
        typename State::Current v{};
#endif
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
#if RYPATH_PACKET_EVENT_ENHANCED
        if (forced_pivot) {
            const auto support = hybrid_runtime::owned_point(
                r, hybrid_import, sink.state.requested_slot, p.max_depth, v.point);
            if (support == hybrid_geometry::Status::invalid_arithmetic) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            if (support != hybrid_geometry::Status::ok) {
                hybrid_runtime::outside(state, sink);
                break;
            }
        } else
#endif
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
            !prepare_event_bsdf(r, material, context, lambda, beta, v.bsdf,
                                hero ? state.hero.active_mask : 15u)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
#if RYPATH_PACKET_EVENT_ENHANCED
        // Canonical v4 uses one incoming-direction law for every admitted
        // secondary event, including footprint rejects. Material preparation
        // above is incoming-independent for these direct PBRT families.
        if (count == 1) {
            if (enhanced_hybrid_runtime::prepare_primary(sink.state.hybrid,
                                                         state.latest.point.position, v.point) !=
                enhanced_hybrid_geometry::Status::ok) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
        } else if (enhanced_hybrid_runtime::direct_root(r, v.instance, v.primitive, v.kind)) {
            const auto family = enhanced_hybrid_geometry::classify(v.bsdf);
            if (family.status == enhanced_hybrid_geometry::Status::invalid_arithmetic) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            if (family.status == enhanced_hybrid_geometry::Status::ok) {
                const auto direction = enhanced_hybrid_geometry::physical_direction(
                    v.point.position, state.latest.point.position);
                if (direction.status != enhanced_hybrid_geometry::Status::ok) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                v.wo = direction.direction;
            }
        }
#endif
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
#if RYPATH_PACKET_EVENT_ENHANCED
        if (!forced_pivot)
#endif
            v.point.error += abs(v.point.position - ray_position) + ray_error;
        if (!all(isfinite(v.point.error))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        const auto words = material_words(r, sample, material_index, count - 1, mode, replay);
        v.seed = words.w;
        v.beta = beta;
        if constexpr (State::compact_hero)
            v.connectible = hero_connectible(v.bsdf);
        else
            v.connectible = hero ? hero_connectible(v.bsdf) : !rypath_packet::delta_only(v.bsdf);
        const thread auto &prev = state.latest;
        const float sampled_area_pdf = convert_density(pdf, prev, v);
        // Retain sampled-area validation even when PT skips unused BDPT storage.
        if (!isfinite(sampled_area_pdf) || sampled_area_pdf < 0) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
#if !RYPATH_PACKET_EVENT_REUSE_CURRENT
        current = v;
#endif
        ++count;
        crossings = 0;
        if (count == capacity)
            break;
        if (count - 2 >= p.emergency_limit) {
            sample.status = wire::emergency_depth;
            break;
        }
#if RYPATH_PACKET_EVENT_CONTRIBUTION
        // A selected arrival endpoint does not depend on this surface's future
        // collapse, roulette or direction sample. Keep full/fresh collectors on
        // their original path, including collectors that request both atoms.
        const uint selected_event_slot = 2u * (count - 2u);
        const bool selected_event_only =
            sink.stop_after(count - 2u) &&
            (sink.wanted(selected_event_slot) != sink.wanted(selected_event_slot + 1u));
        if (selected_event_only && sink.wanted(selected_event_slot))
            break;
#endif
        auto draws =
            random(r, sample, index, (mode == Mode::Radiance ? 4u : 69u) + count - 2, replay);
#if RYPATH_PACKET_EVENT_ENHANCED && defined(RYPATH_ENHANCED_DIRECTION_DRAW_CONTROL)
        // Validation-only poisoning of direction coordinates removed by the
        // owned chart. Production has no hook, branch or alternate RNG address.
        RYPATH_ENHANCED_DIRECTION_DRAW_CONTROL(draws, hybrid_import, count - 2u, sample, p);
#endif
        const float3 sampled_wo_local = v.frame.local(v.wo);
        rypath_packet::Sample continuation{};
        if (hero) {
            // Arrival emission retains v.beta and the pre-collapse wavelength
            // channels. Only future NEE/continuation use the conditional state.
            if (!hero_prepare_future(state.hero, beta, current.bsdf)) {
                sample.status = wire::invalid_arithmetic;
                break;
            }
            direct_density = state.hero.density;
#if RYPATH_PACKET_EVENT_ENHANCED
            direct_allowed = sink.continue_discovery(beta, state.hero, count - 1);
#else
            direct_allowed = hero_continue(beta, state.hero, count - 1, sample_uniform(p, draws.w));
#endif
            direct_beta = beta;
            current.connectible = hero_connectible(current.bsdf);
            if (!direct_allowed)
                break;
#if RYPATH_PACKET_EVENT_CONTRIBUTION
            // Direct lighting consumes the conditional beta/density and current
            // incoming local direction, but never the unused continuation.
            if (selected_event_only && sink.wanted(selected_event_slot + 1u))
                break;
#endif
            rypath_hero_scattering::Sample proposed;
#if RYPATH_PACKET_EVENT_ENHANCED
            const uint event = count - 2u;
            forced_continuation = !sink.has_interior() &&
                                  hybrid_import.tag == hybrid_chart::interior_zomega &&
                                  (event + 1u == hybrid_import.event_ordinal || forced_pivot);
            if (forced_continuation) {
                const auto admission = hybrid_runtime::admission_status(r, current);
                if (admission == hybrid_geometry::Status::invalid_arithmetic) {
                    sample.status = wire::invalid_arithmetic;
                    break;
                }
                if (admission != hybrid_geometry::Status::ok) {
                    hybrid_runtime::outside(state, sink);
                    break;
                }
                forced_omega = hybrid_runtime::world_omega(hybrid_import);
                if (!forced_pivot) {
                    SurfacePoint target;
                    const auto support = hybrid_runtime::owned_point(
                        r, hybrid_import, sink.state.requested_slot, p.max_depth, target);
                    if (support == hybrid_geometry::Status::invalid_arithmetic) {
                        sample.status = wire::invalid_arithmetic;
                        break;
                    }
                    if (support != hybrid_geometry::Status::ok) {
                        hybrid_runtime::outside(state, sink);
                        break;
                    }
                    const auto direction = hybrid_geometry::physical_direction(
                        current.point.position, target.position);
                    if (direction.status == hybrid_geometry::Status::invalid_arithmetic) {
                        sample.status = wire::invalid_arithmetic;
                        break;
                    }
                    if (direction.status != hybrid_geometry::Status::ok) {
                        hybrid_runtime::outside(state, sink);
                        break;
                    }
                    forced_omega = direction.direction;
                }
                proposed = hybrid_runtime::forced_sample(current.bsdf, current.frame, current.wo,
                                                         forced_omega, current.seed);
                if (proposed.valid && !proposed.accepted) {
                    hybrid_runtime::outside(state, sink);
                    break;
                }
            } else
#endif
                proposed =
                    hero_sample_current(current.bsdf, sampled_wo_local, sample_uniform(p, draws.x),
                                        uniform2(p, draws.yz), v.seed, mode);
            continuation.wi = proposed.wi;
            continuation.pdf = proposed.fallback ? proposed.fallback_pdf : proposed.pdf[0];
            continuation.reverse_pdf = continuation.pdf;
            continuation.delta = proposed.delta;
            continuation.transmission = proposed.transmission;
            continuation.accepted = proposed.accepted;
            continuation.valid = proposed.valid;
            if (proposed.valid && proposed.accepted) {
                state.hero.previous = direct_density;
                if (proposed.fallback) {
                    // The scalar coated sampler returns a hidden-walk ratio.
                    // Its directional MIS proxy is NOT a cumulative likelihood.
                    beta *= proposed.fallback_weight;
                } else {
                    const auto advanced = rypath_hero::advance(direct_density, beta,
                                                               proposed.numerator, proposed.pdf);
                    continuation.valid = advanced.valid;
                    if (advanced.valid) {
                        beta = advanced.beta;
                        state.hero.density = advanced.density;
                    }
                }
                hero_account_eta(state.hero, proposed.eta, proposed.transmission);
                state.hero.active_mask = 1;
                for (uint k = 1; k < 4; ++k)
                    if (state.hero.density.lane[k].fraction > 0 ||
                        (rypath_hero::bits(beta[k]) & 0x7fffffffu))
                        state.hero.active_mask |= 1u << k;
            }
        } else if constexpr (!State::compact_hero) {
            continuation =
                rypath_packet::sample(v.bsdf, sampled_wo_local, sample_uniform(p, draws.x),
                                      uniform2(p, draws.yz), v.seed, mode);
        }
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
#if RYPATH_PACKET_EVENT_ENHANCED
        if (forced_continuation)
            wi = forced_omega;
        else
#endif
            if (!rypath_surface_detail::unit_vector(wi, wi)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        // Preserve the sampled joint coating-walk ratio. Its directional MIS
        // proxy is a separate density and must never replace this weight.
        if (!hero && mode == Mode::Importance) {
            // Use the exact local cosines used by sample(), not a world/local
            // round trip that can erase a nonzero grazing sampled direction.
            const auto correction = rypath_shading_normal::correction(
                sampled_wo_local.z, continuation.wi.z, dot(v.wo, v.point.geometric_normal),
                dot(wi, v.point.geometric_normal));
            for (uint lane = 0; lane < 4; ++lane)
                beta[lane] = rypath_shading_normal::multiply(beta[lane], continuation.weight[lane],
                                                             correction);
        } else if (!hero) {
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
#if RYPATH_PACKET_EVENT_ENHANCED
        const auto candidate_status = hybrid_runtime::candidate(
            r, state, current, material, context, previous_density, incoming_generation_q,
            continuation.pdf, wi, incoming, sink, hybrid_pending);
        if (candidate_status != hybrid_geometry::Status::ok) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        if (sink.state.hybrid.outside)
            break;
#endif
        if (!hero && !packet_bidirectional_enabled(p) && mode == Mode::Radiance) {
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
#if RYPATH_PACKET_EVENT_ENHANCED
    if (sink.state.hybrid.outside) {
        state.complete = true;
        return; // A partial chart map is exactly zero, with no substituted suffix.
    }
    if (count == old_count && !state.complete) {
        // The next physical surface follows an intervening null/invisible
        // boundary; it cannot own a direct predecessor-to-pivot connection.
        sink.state.hybrid.predecessor_valid = 0;
        sink.state.hybrid.predecessor = {};
        sink.state.hybrid.certificate = {};
    }
#endif
    if (count != old_count && sample.status == wire::success) {
        const thread auto &pt = current;
        const thread auto &previous = state.latest;
        typename State::Neighbor before{};
        if (count > 2) {
            before.type = state.preceding_type;
            before.point.position = state.preceding_position;
        }
        // Match offline t-major, s=0 then s=1 accumulation. Sampled-local pairs
        // have been finalized before f() is queried, including rejected events.
        if (hero) {
#if RYPATH_PACKET_EVENT_CONTRIBUTION
            const uint endpoint_slot = 2u * (count - 2u);
            if (sink.wanted(endpoint_slot)) {
                const auto endpoint =
                    hero_endpoint(r, sample, lambda, pt, previous, before, count, previous_density);
#if !RYPATH_PACKET_EVENT_ENHANCED
                state.radiance += endpoint;
#endif
                if (sample.status == wire::success)
                    sink.add(endpoint_slot, endpoint, lambda);
            }
            if (sample.status == wire::success && count < capacity && direct_allowed &&
                sink.wanted(endpoint_slot + 1u)) {
#if RYPATH_PACKET_EVENT_ENHANCED
                const auto direct =
                    sink.has_interior()
                        ? hero_direct(r, sample, lambda, index, pt, previous, count, direct_beta,
                                      direct_density, replay)
                        : hero_direct_enhanced(r, sample, lambda, index, pt, previous, count,
                                               direct_beta, direct_density, replay, sink);
#else
                const auto direct = hero_direct(r, sample, lambda, index, pt, previous, count,
                                                direct_beta, direct_density, replay);
#endif
#if !RYPATH_PACKET_EVENT_ENHANCED
                state.radiance += direct;
#endif
                if (sample.status == wire::success)
                    sink.add(endpoint_slot + 1u, direct, lambda);
            }
            // A fixed atom needs no later transport. Fresh WRS and the
            // full-replay oracle never take this early stop.
            if (sink.stop_after(count - 2u))
                state.complete = true;
#else
            state.radiance +=
                hero_endpoint(r, sample, lambda, pt, previous, before, count, previous_density);
            if (sample.status == wire::success && count < capacity && direct_allowed)
                state.radiance += hero_direct(r, sample, lambda, index, pt, previous, count,
                                              direct_beta, direct_density, replay);
#endif
        } else if constexpr (!State::compact_hero) {
            state.radiance += packet_pt_endpoint(r, sample, lambda, pt, previous, before, count);
            if (sample.status == wire::success && count < capacity)
                state.radiance +=
                    packet_pt_direct(r, sample, lambda, index, pt, previous, count, replay);
        }
        if (!finite_nonnegative(state.radiance))
            sample.status = wire::invalid_arithmetic;
        if (sample.status != wire::success)
            state.complete = true;
#if RYPATH_PACKET_EVENT_ENHANCED
        if (sample.status == wire::success && !sink.state.failed &&
            !hybrid_runtime::commit_candidate(r, state, current, direct_beta, hybrid_pending,
                                              sink)) {
            sample.status = wire::invalid_arithmetic;
            state.complete = true;
        }
#endif
    }
    if (count != old_count) {
        // Null/invisible boundaries leave count unchanged and preserve history.
        // Environment endpoints are evaluated above and mark the walk complete;
        // their normals are therefore never needed as a later preceding endpoint.
        state.preceding_position = state.latest.point.position;
        state.preceding_type = state.latest.type;
        hero_store_previous(state.latest, current);
    }
