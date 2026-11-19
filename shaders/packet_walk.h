// SPDX-License-Identifier: Apache-2.0
// Packet random walk adapted from PBRT v4 b0bdf461 RandomWalk.
// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// See third_party/pbrt/LICENSE.txt.
#pragma once
// Included inside rypath_bdpt after Resources, Vertex and density helpers.

inline uint4 material_words(const thread Resources &r, const thread wire::Sample &sample,
                            uint material, uint bounce, Mode mode) {
    const constant auto &p = *r.alpha.parameters;
    const uint role = mode == Mode::Radiance ? 0x63616d31u : 0x6c697431u;
    return philox(uint4(sample.pixel, sample.sample_low, sample.sample_high,
                        rypath::sampling_contract::material_local_block(bounce)),
                  uint2(p.seed_low ^ material,
                        p.seed_high ^ role ^ rypath::sampling_contract::material_stream_v1));
}

inline bool packet_reflectance(const thread Resources &r, const device wire::Material &material,
                               const thread graph_wire::Context &context, float wavelength,
                               thread float &rho) {
    const constant auto &p = *r.alpha.parameters;
    const thread auto &images = r.alpha.graph.images;
    if (material.scattering != 0 && material.scattering != 4 && material.scattering != 5) {
        rho = 1;
    } else if (material.kind == wire::sampled_material && material.knot_count >= 2 &&
               material.knot_begin <= p.knot_count &&
               material.knot_count <= p.knot_count - material.knot_begin) {
        rho = pbrt_reflectance(r.knots + material.knot_begin, material.knot_count, wavelength);
    } else if (material.kind == wire::constant_material) {
        rho = material.scalar_value;
    } else if (material.kind == wire::blackbody_material) {
        rho = blackbody_reflectance(material.scalar_value, wavelength);
    } else if (material.kind == wire::rgb_sigmoid_material) {
        rho = sigmoid_reflectance(material, wavelength);
    } else if (material.kind == wire::image_material &&
               material.image_texture < images.parameters->texture_count) {
        const device auto &texture = images.textures[material.image_texture];
        float3 rgb = rypath_image::lookup(texture, images.levels, images.texels, images.transfer,
                                          *images.parameters, float2(context.u, context.v),
                                          float2(context.dudx, context.dvdx),
                                          float2(context.dudy, context.dvdy));
        rgb *= texture.scale;
        if (texture.invert != 0)
            rgb = 1 - rgb;
        rho = rypath_image::spectrum(rgb, wavelength, images.spectra);
    } else if (material.kind == wire::graph_material) {
        if (!r.alpha.graph.evaluate(material.texture_root, context, wavelength, rho))
            return false;
        if (material.scattering != 4)
            rho = clamp(rho, 0.0f, 1.0f);
    } else {
        return false;
    }
    return isfinite(rho) && (material.scattering == 4 || (rho >= 0 && rho <= 1));
}

inline bool prepare_packet(const thread Resources &r, const device wire::Material &material,
                           graph_wire::Context context, thread Wavelengths &lambda,
                           thread float4 &beta, thread rypath_packet::Prepared &out) {
    const constant auto &p = *r.alpha.parameters;
    if (material.scattering != 0 &&
        !(conductor_enabled || dielectric_enabled || surface_materials_enabled || gltf_enabled))
        return false;
    const device auto *aux =
        material_auxiliary(material, r.alpha.material_data, p.material_data_words);
    out = {};
    out.active_mask = lambda.secondary_terminated() ? 1u : 15u;
    for (uint lane = 0; lane < 4; ++lane) {
        if (lane > 0 && out.active_mask == 1) {
            out.lane[lane] = out.lane[0];
            continue;
        }
        float rho;
        if (!packet_reflectance(r, material, context, lambda.lambda[lane], rho))
            return false;
        if (material.scattering == 0) {
            out.lane[lane].kind = 0;
            out.lane[lane].reflectance = rho;
            out.lane[lane].wavelength = lambda.lambda[lane];
            out.lane[lane].rgb_table = r.alpha.graph.images.spectra;
        } else if (!prepare_scattering(material, aux, rho, r.knots, p, r.alpha.graph, context,
                                       lambda.lambda[lane], r.alpha.material_data,
                                       r.alpha.graph.images.spectra, out.lane[lane])) {
            return false;
        }
    }
    // Refraction must terminate secondary wavelengths before selecting a shared
    // direction. Complex conductor eta and thin-sheet eta do not shape directions.
    bool dispersive = false;
    if (out.active_mask != 1) {
        for (uint lane = 1; lane < 4; ++lane) {
            if (material.scattering == 2)
                dispersive |= out.lane[lane].dielectric.eta != out.lane[0].dielectric.eta;
            if (material.scattering == 5 || material.scattering == 6)
                dispersive |=
                    out.lane[lane].layered.interface.eta != out.lane[0].layered.interface.eta;
        }
    }
    if (dispersive) {
        lambda.terminate_secondary();
        out.active_mask = 1;
    }
    if (lambda.secondary_terminated())
        beta *= float4(1, 0, 0, 0);
    return rypath_packet::compatible(out);
}

[[clang::noinline]] inline uint random_walk(thread Resources &r, thread wire::Sample &sample,
                                            thread Wavelengths &lambda, ray incoming, float4 beta,
                                            float pdf, device uchar *path, uint count,
                                            uint capacity, Mode mode, ulong index) {
    const constant auto &p = *r.alpha.parameters;
    if (count == 0 || count > capacity || !finite_nonnegative(beta) || !isfinite(pdf) || pdf < 0) {
        sample.status = wire::invalid_arithmetic;
        return count;
    }
    MixedIntersector closest;
    closest.set_triangle_cull_mode(triangle_cull_mode::none);
    uint crossings = 0;
    r.alpha.sample = &sample;
    sample.wavelength = lambda.lambda[0];
    while (count < capacity && nonzero(beta)) {
        if (!all(isfinite(incoming.origin)) || !all(isfinite(incoming.direction)) ||
            !finite_nonnegative(beta)) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        r.alpha.incoming = incoming;
        const auto hit =
            p.instance_count == 0 ? SurfaceHit{intersection_type::none, 0, 0, 0, 0, float2(0), true}
            : alpha_enabled
                ? trace_masked_surface(incoming, r.scene, all_visible, r.alpha)
                : trace_surface(closest, incoming, r.scene, all_visible, r.intersection_table);
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
                v.pdf_fwd = pdf;
                v.connectible = true;
                path_vertex(path, count++) = v;
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
            continue;
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
            continue;
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
        const auto words = material_words(r, sample, material_index, count - 1, mode);
        v.seed = words.w;
        v.beta = beta;
        v.connectible = !rypath_packet::delta_only(v.bsdf);
        const Vertex prev = path_vertex(path, count - 1);
        v.pdf_fwd = convert_density(pdf, prev, v);
        if (!isfinite(v.pdf_fwd) || v.pdf_fwd < 0) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        path_vertex(path, count++) = v;
        crossings = 0;
        if (count == capacity)
            break;
        if (count - 2 >= p.emergency_limit) {
            sample.status = wire::emergency_depth;
            break;
        }
        const auto draws =
            random(r, sample, index, (mode == Mode::Radiance ? 4u : 69u) + count - 2);
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
        // Retain the exact ordered local floats used by the directional PDF.
        // Reconstructing a world-space edge and returning to the frame can
        // change one ULP and therefore select an unrelated PBRT hash stream.
        device auto &stored = path_vertex(path, count - 1);
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
        pdf = continuation.pdf;
        float reverse_pdf = continuation.reverse_pdf;
        if (continuation.delta) {
            path_vertex(path, count - 1).delta = true;
            pdf = reverse_pdf = 0;
        }
        const float reverse_area_pdf = convert_density(reverse_pdf, v, prev);
        if (!isfinite(reverse_area_pdf) || reverse_area_pdf < 0) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        path_vertex(path, count - 2).pdf_rev = reverse_area_pdf;
        const float3 origin = offset(v, wi);
        if (!all(isfinite(origin))) {
            sample.status = wire::invalid_arithmetic;
            break;
        }
        incoming = ray(origin, wi);
    }
    return count;
}
