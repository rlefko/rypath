// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "packet_layered.h"
#include "scattering.h"
// Renderer specialization and all-family probes require distinct symbols.
// Separate namespaces prevent weak-ODR coalescing when their AIR shares a metallib.
#if defined(RYPATH_PACKET_RENDER)
namespace rypath_packet_renderer {
#else
namespace rypath_packet {
#endif
enum class TransportMode { Radiance, Importance };
inline TransportMode opposite(TransportMode m) {
    return m == TransportMode::Radiance ? TransportMode::Importance : TransportMode::Radiance;
}
struct Prepared {
    rypath_scattering::Prepared lane[4];
    uint active_mask;
};
struct Evaluation {
    float4 f;
    float pdf, reverse_pdf;
    bool valid;
};
struct Sample {
    float3 wi;
    float4 weight;
    float pdf, reverse_pdf, eta;
    bool delta, transmission, pdf_is_proportional, connectible, accepted, valid;
};
// Renderer builds specialize material families with their existing function
// constants. Standalone probes keep every family available.
inline bool has_conductor() {
#if defined(RYPATH_PACKET_RENDER)
    return conductor_enabled;
#else
    return true;
#endif
}
inline bool has_dielectric() {
#if defined(RYPATH_PACKET_RENDER)
    return dielectric_enabled;
#else
    return true;
#endif
}
inline bool has_surface() {
#if defined(RYPATH_PACKET_RENDER)
    return surface_materials_enabled;
#else
    return true;
#endif
}
inline bool has_gltf() {
#if defined(RYPATH_PACKET_RENDER)
    return gltf_enabled;
#else
    return true;
#endif
}
inline bool has_coated() {
#if defined(RYPATH_PACKET_RENDER)
    return packet_coated_enabled;
#else
    return true;
#endif
}
inline bool has_thin_dielectric() {
#if defined(RYPATH_PACKET_RENDER)
    return packet_thin_dielectric_enabled;
#else
    return true;
#endif
}
inline bool has_diffuse_transmission() {
#if defined(RYPATH_PACKET_RENDER)
    return packet_diffuse_transmission_enabled;
#else
    return true;
#endif
}
inline bool has_measured() {
#if defined(RYPATH_PACKET_RENDER)
    return packet_measured_enabled;
#else
    return true;
#endif
}
inline bool has_fourier() {
#if defined(RYPATH_PACKET_RENDER)
    return packet_fourier_enabled;
#else
    return true;
#endif
}
// Keep specialized table dispatch in the renderer namespace: shared scalar and
// probe AIR must retain their unspecialized table symbols.
inline TableBsdfEvaluation table_evaluate(const device uint *table, float3 wo, float3 wi,
                                          float wavelength) {
#if defined(RYPATH_PACKET_RENDER)
    if (has_fourier() && table[0] == RYPATH_TABLE_FOURIER)
        return tb_fourier_eval(table, wo, wi);
    if (has_measured() && table[0] == RYPATH_TABLE_MEASURED)
        return tb_measured_eval(table, wo, wi, wavelength);
    return {float3(0), 0, false, false};
#else
    return table_bsdf_evaluate(table, wo, wi, wavelength);
#endif
}
inline TableBsdfSample table_sample(const device uint *table, float3 wo, float2 u,
                                    float wavelength) {
#if defined(RYPATH_PACKET_RENDER)
    if (has_fourier() && table[0] == RYPATH_TABLE_FOURIER)
        return tb_fourier_sample(table, wo, u);
    if (has_measured() && table[0] == RYPATH_TABLE_MEASURED)
        return tb_measured_sample(table, wo, u, wavelength);
    return {float3(0), float3(0), 0, false, false, false};
#else
    return table_bsdf_sample(table, wo, u, wavelength);
#endif
}
inline rypath_scattering::Evaluation scalar_evaluate(const thread rypath_scattering::Prepared &p,
                                                     float3 wo, float3 wi) {
    if (p.kind == 0)
        return rypath_transmission::diffuse_evaluate(p.reflectance, 0, wo, wi);
    if (has_conductor() && p.kind == 1)
        return rypath_conductor::evaluate(p.conductor, wo, wi);
    if (has_dielectric() && p.kind == 2)
        return rypath_dielectric::evaluate(p.dielectric, wo, wi);
    if (has_gltf() && p.kind == 11)
        return rypath_gltf::evaluate(p.gltf, wo, wi);
    if (has_surface()) {
        if (has_thin_dielectric() && p.kind == 3)
            return rypath_transmission::thin_evaluate(p.dielectric.eta, wo, wi);
        if (has_diffuse_transmission() && p.kind == 4)
            return rypath_transmission::diffuse_evaluate(p.reflectance, p.transmittance, wo, wi);
        if (p.kind == 7 || p.kind == 8) {
            auto e = table_evaluate(p.table, wo, wi, p.wavelength);
            float f = rypath_scattering::spectral_value(e.value, e.rgb, p);
            return {f, e.pdf, e.valid && isfinite(f) && f >= 0};
        }
    }
    return {0, 0, false};
}
inline rypath_scattering::Sample scalar_sample(const thread rypath_scattering::Prepared &p,
                                               float3 wo, float uc, float2 u) {
    if (p.kind == 0)
        return rypath_transmission::diffuse_sample(p.reflectance, 0, wo, uc, u);
    if (has_conductor() && p.kind == 1)
        return rypath_conductor::sample(p.conductor, wo, u);
    if (has_dielectric() && p.kind == 2)
        return rypath_dielectric::sample(p.dielectric, wo, uc, u);
    if (has_gltf() && p.kind == 11)
        return rypath_gltf::sample(p.gltf, wo, uc, u);
    if (has_surface()) {
        if (has_thin_dielectric() && p.kind == 3)
            return rypath_transmission::thin_sample(p.dielectric.eta, wo, uc, u);
        if (has_diffuse_transmission() && p.kind == 4)
            return rypath_transmission::diffuse_sample(p.reflectance, p.transmittance, wo, uc, u);
        if (p.kind == 7 || p.kind == 8) {
            // Luminance selects the shared table direction; a black hero does
            // not reject support belonging to another packet wavelength.
            auto e = table_sample(p.table, wo, u, p.wavelength);
            return {e.wi, 1, e.pdf, false, e.accepted, e.valid};
        }
    }
    return {float3(0), 0, 0, false, false, false};
}
// Direction-shaping parameters must be shared. Dispersive refraction requires
// wavelength termination before preparation; inactive lanes do not constrain it.
inline bool compatible(const thread Prepared &p) {
    const thread auto &a = p.lane[0];
    if ((p.active_mask & 15) == 0 || (p.active_mask & ~15u) != 0)
        return false;
    if (a.kind != 0 && a.kind != 1 && a.kind != 2 && a.kind != 3 && a.kind != 4 && a.kind != 5 &&
        a.kind != 6 && a.kind != 7 && a.kind != 8 && a.kind != 11)
        return false;
    for (uint i = 0; i < 4; ++i)
        if (p.active_mask & (1u << i)) {
            const thread auto &b = p.lane[i];
            if (a.kind != b.kind)
                return false;
            if (a.kind == 1 && (a.conductor.alpha_x != b.conductor.alpha_x ||
                                a.conductor.alpha_y != b.conductor.alpha_y ||
                                a.conductor.smooth != b.conductor.smooth))
                return false;
            if (a.kind == 2 && (a.dielectric.eta != b.dielectric.eta ||
                                a.dielectric.alpha_x != b.dielectric.alpha_x ||
                                a.dielectric.alpha_y != b.dielectric.alpha_y ||
                                a.dielectric.smooth != b.dielectric.smooth))
                return false;
            if (a.kind == 11 && a.gltf.alpha != b.gltf.alpha)
                return false;
            if ((a.kind == 7 || a.kind == 8) && a.table != b.table)
                return false;
            if (a.kind == 5 || a.kind == 6) {
                const thread auto &x = a.layered;
                const thread auto &y = b.layered;
                if (x.interface.eta != y.interface.eta ||
                    x.interface.alpha_x != y.interface.alpha_x ||
                    x.interface.alpha_y != y.interface.alpha_y ||
                    x.interface.smooth != y.interface.smooth ||
                    x.conductor_base != y.conductor_base || x.thickness != y.thickness ||
                    x.g != y.g || x.max_depth != y.max_depth || x.samples != y.samples)
                    return false;
                if (x.conductor_base && (x.conductor.alpha_x != y.conductor.alpha_x ||
                                         x.conductor.alpha_y != y.conductor.alpha_y ||
                                         x.conductor.smooth != y.conductor.smooth))
                    return false;
                if (!rypath_layered::valid_parameters(y))
                    return false;
            }
        }
    return true;
}
inline float maximum(float4 v) { return rypath_packet_layered::maximum(v); }
inline rypath_packet_layered::Parameters layered(const thread Prepared &p) {
    rypath_packet_layered::Parameters q;
    q.active_mask = p.active_mask;
    for (uint i = 0; i < 4; ++i)
        q.lane[i] = p.lane[i].layered;
    return q;
}
// Spectral mixtures use packet-average coefficients, never hero-only support.
inline rypath_scattering::Prepared proposal(const thread Prepared &p) {
    auto q = p.lane[0];
    float r = 0, t = 0, n = 0;
    for (uint i = 0; i < 4; ++i)
        if (p.active_mask & (1u << i)) {
            r += p.lane[i].reflectance;
            t += p.lane[i].transmittance;
            n += 1;
        }
    q.reflectance = n > 0 ? r / n : 0;
    q.transmittance = n > 0 ? t / n : 0;
    if (q.kind == 0)
        q.reflectance = 1;
    if (q.kind == 1) {
        q.conductor.eta = 1;
        q.conductor.k = 1;
    }
    if (q.kind == 11) {
        q.gltf.specular_probability = .5f;
        q.gltf.color = 1;
        q.gltf.metallic = 0;
        bool diffuse = false;
        for (uint i = 0; i < 4; ++i)
            if (p.active_mask & (1u << i))
                diffuse = diffuse || (p.lane[i].gltf.metallic < 1 && p.lane[i].gltf.color > 0);
        if (!diffuse)
            q.gltf.specular_probability = 1;
    }
    return q;
}
inline bool delta_only(const thread Prepared &p) {
    auto q = proposal(p);
    return rypath_scattering::delta_only(q);
}
inline bool both_hemispheres(const thread Prepared &p) {
    return rypath_scattering::both_hemispheres(p.lane[0]);
}
inline float mode_scale(const thread rypath_scattering::Prepared &p, float3 wo, float3 wi,
                        TransportMode mode) {
    if (mode == TransportMode::Radiance || wo.z * wi.z >= 0)
        return 1;
    if (p.kind == 2)
        return wo.z > 0 ? p.dielectric.eta * p.dielectric.eta
                        : 1 / (p.dielectric.eta * p.dielectric.eta);
    if (has_fourier() && (p.kind == 7 || p.kind == 8) && p.table[0] == RYPATH_TABLE_FOURIER) {
        float eta = tb_f(p.table, 4);
        return wi.z < 0 ? eta * eta : 1 / (eta * eta);
    }
    return 1;
}
inline float pdf(const thread Prepared &p, float3 wo, float3 wi,
                 TransportMode mode = TransportMode::Radiance) {
    (void)mode;
    auto q = proposal(p);
    if (has_surface() && has_coated() && (q.kind == 5 || q.kind == 6))
        return rypath_packet_layered::directional_pdf(layered(p), wo, wi).pdf;
    return scalar_evaluate(q, wo, wi).pdf;
}
[[clang::noinline]] inline Evaluation evaluate(const thread Prepared &p, float3 wo, float3 wi,
                                               uint seed,
                                               TransportMode mode = TransportMode::Radiance) {
    if (!compatible(p))
        return {float4(0), 0, 0, false};
    Evaluation e{float4(0), pdf(p, wo, wi, mode), pdf(p, wi, wo, opposite(mode)), true};
    if (has_surface() && has_coated() && (p.lane[0].kind == 5 || p.lane[0].kind == 6)) {
        auto q = layered(p);
        auto a =
            rypath_packet_layered::evaluate(q, wo, wi, seed, mode == TransportMode::Importance);
        e.f = a.f;
        e.valid = a.valid;
        return e;
    }
    for (uint i = 0; i < 4; ++i)
        if (p.active_mask & (1u << i)) {
            auto a = scalar_evaluate(p.lane[i], wo, wi);
            e.f[i] = a.f * mode_scale(p.lane[i], wo, wi, mode);
            e.valid = e.valid && a.valid;
        }
    e.valid = e.valid && all(isfinite(e.f)) && isfinite(e.pdf) && isfinite(e.reverse_pdf);
    return e;
}
[[clang::noinline]] inline Sample sample(const thread Prepared &p, float3 wo, float uc, float2 u,
                                         uint seed, TransportMode mode = TransportMode::Radiance) {
    Sample out{float3(0), float4(0), 0, 0, 1, false, false, false, false, false, true};
    if (!compatible(p)) {
        out.valid = false;
        return out;
    }
    auto q = proposal(p);
    if (has_surface() && has_coated() && (q.kind == 5 || q.kind == 6)) {
        auto a = rypath_packet_layered::sample(layered(p), wo, uc, u, seed,
                                               mode == TransportMode::Importance);
        out.wi = a.wi;
        out.weight = a.weight;
        out.pdf = a.pdf;
        out.delta = a.delta;
        out.accepted = a.accepted;
        out.valid = a.valid;
        out.pdf_is_proportional = true;
    } else {
        auto a = scalar_sample(q, wo, uc, u);
        if (has_surface() && has_thin_dielectric() && q.kind == 3 && a.valid && wo.z != 0) {
            float R = 0, n = 0;
            for (uint i = 0; i < 4; ++i)
                if (p.active_mask & (1u << i)) {
                    float f = 0;
                    a.valid = a.valid &&
                              rypath_dielectric::fresnel(abs(wo.z), p.lane[i].dielectric.eta, f);
                    R += 2 * f / (1 + f);
                    n += 1;
                }
            R = n > 0 ? R / n : 0;
            bool reflection = uc < R;
            a.wi = reflection ? float3(-wo.x, -wo.y, wo.z) : -wo;
            a.pdf = reflection ? R : 1 - R;
            a.accepted = a.pdf > 0;
        }
        out.wi = a.wi;
        out.pdf = a.pdf;
        out.delta = a.delta;
        out.valid = a.valid;
        if (!a.valid || !a.accepted)
            return out;
        if (a.delta) {
            for (uint i = 0; i < 4; ++i)
                if (p.active_mask & (1u << i)) {
                    const thread auto &v = p.lane[i];
                    float f = 0;
                    if (has_conductor() && q.kind == 1)
                        out.valid = out.valid && rypath_conductor::fresnel(
                                                     abs(wo.z), v.conductor.eta, v.conductor.k, f);
                    else if (has_dielectric() && q.kind == 2)
                        f = a.weight * a.pdf * mode_scale(v, wo, a.wi, mode);
                    else if (has_surface() && has_thin_dielectric() && q.kind == 3) {
                        float R = 0;
                        out.valid =
                            out.valid && rypath_dielectric::fresnel(abs(wo.z), v.dielectric.eta, R);
                        R = 2 * R / (1 + R);
                        f = wo.z * a.wi.z > 0 ? R : 1 - R;
                    } else if (has_gltf() && q.kind == 11)
                        f = rypath_gltf::fresnel(v.gltf, wo.z);
                    out.weight[i] = f / a.pdf;
                }
        } else {
            auto e = evaluate(p, wo, a.wi, seed, mode);
            out.valid = out.valid && e.valid;
            out.weight = e.f * (abs(a.wi.z) / a.pdf);
        }
        out.accepted = out.valid && maximum(out.weight) > 0;
    }
    out.transmission = wo.z * out.wi.z < 0;
    if (out.transmission && q.kind == 2)
        out.eta = wo.z > 0 ? q.dielectric.eta : 1 / q.dielectric.eta;
    if (has_fourier() && out.transmission && (q.kind == 7 || q.kind == 8) &&
        q.table[0] == RYPATH_TABLE_FOURIER) {
        float eta = tb_f(q.table, 4);
        out.eta = wo.z > 0 ? eta : 1 / eta;
    }
    out.reverse_pdf = out.delta ? out.pdf : pdf(p, out.wi, wo, opposite(mode));
    out.connectible = !out.delta;
    out.valid = out.valid && all(isfinite(out.weight)) && all(out.weight >= 0);
    out.accepted = out.accepted && out.valid && maximum(out.weight) > 0;
    return out;
}
} // namespace packet implementation
#if defined(RYPATH_PACKET_RENDER)
namespace rypath_packet = rypath_packet_renderer;
#endif
