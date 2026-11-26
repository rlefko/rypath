// SPDX-License-Identifier: GPL-3.0-only
#include "../shaders/render.metal"
namespace current_probe {
using namespace rypath_bdpt;
namespace h = rypath_hero_scattering;
inline bool same(float a, float b) {
    return as_type<uint>(a) == as_type<uint>(b) || (isnan(a) && isnan(b));
}
inline bool same(float3 a, float3 b) { return same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z); }
inline bool same(float4 a, float4 b) {
    return same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z) && same(a.w, b.w);
}
inline bool same_conductor(rypath_conductor::Parameters a, rypath_conductor::Parameters b) {
    return same(a.eta, b.eta) && same(a.k, b.k) && same(a.alpha_x, b.alpha_x) &&
           same(a.alpha_y, b.alpha_y) && a.smooth == b.smooth;
}
inline bool same_dielectric(rypath_dielectric::Parameters a, rypath_dielectric::Parameters b) {
    return same(a.eta, b.eta) && same(a.alpha_x, b.alpha_x) && same(a.alpha_y, b.alpha_y) &&
           a.smooth == b.smooth;
}
// Storage equivalence compares only defined family fields. NaNs retain their
// invalid-input semantics, while comparing as NaNs rather than with float ==.
inline bool current_storage_matches(const thread rypath_packet::Prepared &p,
                                    const thread HeroFrozenBSDF &f) {
    for (uint k = 0; k < 4; ++k) {
        if (!(p.active_mask & (1u << k)))
            continue;
        const thread auto &s = p.lane[k];
        switch (f.kind) {
        case 0:
            if (!same(s.reflectance, hero_frozen_diffuse(f, k)))
                return false;
            break;
        case 1:
            if (!same_conductor(s.conductor, hero_frozen_conductor(f, k)))
                return false;
            break;
        case 2:
            if (!same_dielectric(s.dielectric, hero_frozen_dielectric(f, k)))
                return false;
            break;
        case 3:
            if (!same(s.dielectric.eta, hero_frozen_thin_eta(f, k)))
                return false;
            break;
        case 4: {
            const auto t = hero_frozen_transmission(f, k);
            if (!same(s.reflectance, t.reflectance) || !same(s.transmittance, t.transmittance))
                return false;
            break;
        }
        case 5:
        case 6: {
            const auto t = hero_frozen_coated(f);
            if (!same_dielectric(s.layered.interface, t.interface) ||
                !same_conductor(s.layered.conductor, t.conductor) ||
                s.layered.conductor_base != t.conductor_base ||
                !same(s.layered.reflectance, t.reflectance) ||
                !same(s.layered.thickness, t.thickness) || !same(s.layered.albedo, t.albedo) ||
                !same(s.layered.g, t.g) || s.layered.max_depth != t.max_depth ||
                s.layered.samples != t.samples || s.layered.material_seed != t.material_seed)
                return false;
            break;
        }
        case 7:
        case 8: {
            const auto t = hero_frozen_table(f, k);
            if (s.table != t.table || s.rgb_table != t.rgb_table ||
                !same(s.wavelength, t.wavelength))
                return false;
            break;
        }
        case 11: {
            const auto t = hero_frozen_gltf(f, k);
            if (!same(s.gltf.color, t.color) || !same(s.gltf.metallic, t.metallic) ||
                !same(s.gltf.alpha, t.alpha) ||
                !same(s.gltf.specular_probability, t.specular_probability))
                return false;
            break;
        }
        }
    }
    return true;
}
inline uint compare(const thread rypath_packet::Prepared &reference,
                    const thread HeroFrozenBSDF &compact, float3 wo, float3 wi, float uc, float2 u,
                    uint seed, Mode mode) {
    uint failures = 0;
    if (h::valid_structure(reference) != hero_current_structure(compact))
        failures |= 1;
    if (h::requires_single_hero(reference) != hero_current_requires_single(compact))
        failures |= 2;
    if (hero_connectible(reference) != hero_current_connectible(compact))
        failures |= 4;
    const auto a = h::evaluate(reference, wo, wi, seed, mode);
    const auto b = hero_current_evaluate(compact, wo, wi, seed, mode);
    if (a.valid != b.valid)
        failures |= 8;
    if (!same(a.f, b.f) || !same(a.pdf, b.pdf))
        failures |= 16;
    const auto x = h::sample(reference, wo, uc, u, seed, mode);
    const auto y = hero_current_sample(compact, wo, uc, u, seed, mode);
    if (x.valid != y.valid || x.accepted != y.accepted || x.delta != y.delta ||
        x.transmission != y.transmission || x.fallback != y.fallback)
        failures |= 32;
    if (!same(x.wi, y.wi))
        failures |= 64;
    if (!same(x.numerator, y.numerator))
        failures |= 128;
    if (!same(x.pdf, y.pdf))
        failures |= 256;
    if (!same(x.eta, y.eta))
        failures |= 512;
    if (!same(x.fallback_weight, y.fallback_weight) || !same(x.fallback_pdf, y.fallback_pdf))
        failures |= 1024;
    Vertex full{};
    HeroCurrentVertex current{};
    full.bsdf = reference;
    current.bsdf = compact;
    full.type = current.type = surface_vertex;
    full.seed = current.seed = seed;
    full.path_id = current.path_id = 31;
    full.delta = current.delta = true;
    full.point.position = current.point.position = float3(1, 2, 3);
    full.point.geometric_normal = current.point.geometric_normal = float3(0, 0, 1);
    full.point.shading_normal = current.point.shading_normal = float3(.1f, .2f, .9f);
    full.frame = current.frame = {float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
    const auto old_history = hero_freeze_previous(full);
    const auto new_history = hero_freeze_previous(current);
    if (old_history.bsdf.kind != new_history.bsdf.kind ||
        old_history.bsdf.active_mask != new_history.bsdf.active_mask ||
        old_history.type != new_history.type || old_history.seed != new_history.seed ||
        old_history.path_id != new_history.path_id || old_history.delta != new_history.delta ||
        !same(old_history.point.position, new_history.point.position) ||
        !same(old_history.point.geometric_normal, new_history.point.geometric_normal) ||
        !same(old_history.point.shading_normal, new_history.point.shading_normal) ||
        !same(old_history.frame.x, new_history.frame.x) ||
        !same(old_history.frame.y, new_history.frame.y) ||
        !same(old_history.frame.z, new_history.frame.z))
        failures |= 2048;
    if (new_history.bsdf.kind == 0xffffffffu) {
        // The invalid history sentinel must not retain current coating data.
        for (uint k = 0; k < 24; ++k)
            if (new_history.bsdf.payload[k] != 0)
                failures |= 2048;
    } else if (!current_storage_matches(reference, new_history.bsdf)) {
        failures |= 2048;
    }
    return failures;
}
} // namespace current_probe
kernel void rypath_hero_current_differential(device uint *out [[buffer(0)]],
                                             device const uint *measured [[buffer(1)]],
                                             device const uint *fourier [[buffer(2)]],
                                             device const float *rgb [[buffer(3)]],
                                             uint index [[thread_position_in_grid]]) {
    using namespace current_probe;
    if (index >= 4224)
        return;
    const uint kinds[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 11, 9};
    const uint masks[6] = {0, 1, 3, 15, 2, 17};
    const uint kind = kinds[index / 384], mask = masks[(index / 64) % 6];
    const uint variant = index % 32;
    const Mode mode = (index / 32) % 2 ? Mode::Importance : Mode::Radiance;
    rypath_packet::Prepared original{};
    original.active_mask = mask;
    HeroFrozenBSDF compact{};
    compact.kind = kind;
    compact.active_mask = mask;
    for (uint k = 0; k < 4; ++k) {
        thread auto &p = original.lane[k];
        p.kind = kind;
        p.reflectance = .125f * float(k + 1);
        p.transmittance = .0625f * float(4 - k);
        p.wavelength = 410 + 80 * k;
        p.rgb_table = rgb;
        p.table = kind == 7 ? measured : fourier;
        p.conductor = {1.25f + float(k), 2.5f + float(k), .2f + .03f * float(k), .3f,
                       variant == 1 || variant == 2 || variant == 9};
        p.dielectric = {variant == 3 ? 1.f : 1.4f + .1f * float(k), .2f, .3f,
                        variant == 1 || variant == 2 || variant == 4};
        p.layered = {{1.5f, .2f, .3f, variant == 1},
                     {1.5f, 2.5f, .2f, .3f, variant == 1},
                     kind == 6,
                     .125f * float(k + 1),
                     variant == 12 ? .3f : .1f,
                     variant == 12 ? .5f : 0.f,
                     .125f,
                     3,
                     2,
                     79};
        p.gltf = {.125f * float(k + 1), .25f,
                  variant == 1 || variant == 2 || variant == 9 ? 0.f : .2f, .5f};
        if (variant == 9 && k == 0) {
            p.reflectance = 0;
            p.conductor.eta = 1;
            p.conductor.k = 0;
            p.gltf.color = 0;
        }
        if (variant == 11) {
            p.layered.interface.alpha_x = NAN;
            p.dielectric.alpha_x = NAN;
            p.conductor.alpha_x = NAN;
        }
        if (variant == 13)
            p.layered.conductor.alpha_y = NAN;
        if (variant == 18 && k == 1) {
            p.reflectance = NAN;
            p.gltf.color = NAN;
        }
        if (variant == 20)
            p.layered.interface.eta = NAN;
        if (variant == 21)
            p.layered.max_depth = 0;
        if (variant == 22)
            p.gltf.specular_probability = 0;
        // Connectibility is an any-active-lane property, independent of the
        // hero proposal's own delta classification. Inactive rough companions
        // must not make an otherwise all-delta active set connectible.
        if (variant >= 24) {
            const bool smooth = variant == 24   ? k == 0
                                : variant == 25 ? k != 0
                                : variant == 26 ? k != 3
                                : variant == 30 ? k != 2
                                : variant == 31 ? k != 1
                                                : true;
            p.conductor.smooth = p.dielectric.smooth = smooth;
            p.gltf.alpha = smooth ? 0.f : .2f;
            p.gltf.specular_probability = 1;
            if (variant == 27)
                p.dielectric.eta = k == 0 ? 1.f : 1.5f;
            if (variant == 28)
                p.gltf.specular_probability = k == 0 ? 1.f : .5f;
            if (variant == 29)
                p.gltf.specular_probability = k == 0 ? 0.f : 1.f;
        }
        hero_current_store_lane(compact, k, p);
    }
    float3 wo = normalize(float3(.3f, .2f, 1)), wi = normalize(float3(-.2f, .1f, 1));
    float uc = .01f;
    float2 u(.15f, .25f);
    if (variant == 2 || variant == 3 || variant == 12)
        uc = .99f;
    if (variant == 4) {
        wo = float3(sqrt(1.f - .75f * .75f), 0, -.75f);
        uc = .99f;
    }
    if (variant == 5 || variant == 11)
        wo = float3(1, 0, 0);
    if (variant == 6)
        wo.x = NAN;
    if (variant == 7)
        uc = 1;
    if (variant == 8)
        u.x = NAN;
    if (variant == 10) {
        wo.z = -wo.z;
        wi.z = -wi.z;
        uc = .99f;
    }
    if (variant == 14)
        uc = 0;
    if (variant == 15)
        u = float2(0, 0);
    if (variant == 16)
        u = float2(0x1.fffffep-1f);
    if (variant == 17) {
        wo = normalize(float3(1, .1f, 0x1p-20f));
        wi = -wo;
    }
    if (variant == 19)
        wi.z = -wi.z;
    if (variant == 23) {
        u = float2(.5f);
        uc = .5f;
    }
    out[index] = compare(original, compact, wo, wi, uc, u, 0xfedcba98u + index, mode);
}
kernel void rypath_hero_current_preparation(
    device uint *out [[buffer(0)]], device const wire::Material *materials [[buffer(1)]],
    device const uint *data [[buffer(2)]], device const wire::SpectralKnot *knots [[buffer(3)]],
    device const float *rgb [[buffer(4)]], constant wire::Parameters &p [[buffer(5)]],
    constant wire::TextureParameters &tp [[buffer(6)]],
    device const wire::ImageTexture *textures [[buffer(7)]],
    device const wire::TextureLevel *levels [[buffer(8)]], device const uint *texels [[buffer(9)]],
    device const graph_wire::Node *nodes [[buffer(10)]],
    constant wire::TextureGraphParameters &gp [[buffer(11)]],
    uint index [[thread_position_in_grid]]) {
    using namespace current_probe;
    if (index >= 84)
        return;
    const uint masks[6] = {0, 1, 3, 15, 2, 17};
    const uint material_index = index / 6, mask = masks[index % 6];
    Resources r{};
    r.alpha.parameters = &p;
    r.alpha.material_data = data;
    r.alpha.graph.parameters = &gp;
    r.alpha.graph.nodes = nodes;
    r.alpha.graph.knots = knots;
    r.alpha.graph.images.parameters = &tp;
    r.alpha.graph.images.spectra = rgb;
    r.alpha.graph.images.textures = textures;
    r.alpha.graph.images.levels = levels;
    r.alpha.graph.images.texels = texels;
    r.alpha.graph.images.transfer = rgb; // Linear FLOAT fixture never reads a transfer table.
    r.knots = knots;
    Wavelengths lambda{};
    for (uint k = 0; k < 4; ++k) {
        lambda.lambda[k] = 410 + 80 * k;
        lambda.pdf[k] = 1.f / 470.f;
    }
    float4 beta(1);
    rypath_packet::Prepared original{};
    HeroFrozenBSDF compact{};
    graph_wire::Context context{};
    context.u = .375f;
    context.v = .625f;
    context.dudx = .25f;
    context.dvdy = .25f;
    const bool a =
        prepare_packet(r, materials[material_index], context, lambda, beta, original, mask);
    const bool b =
        prepare_hero_current(r, materials[material_index], context, lambda, compact, mask);
    uint failures = a != b ? 1u << 16 : 0;
    if (material_index == 12) {
        const device auto &material = materials[material_index];
        for (uint k = 0; k < 4; ++k) {
            const float eta = optical_value(knots, material.eta_begin, material.eta_count,
                                            p.knot_count, true, lambda.lambda[k]);
            const float expected_eta = k == 0 ? 1.5f : -1.f;
            if (as_type<uint>(eta) != as_type<uint>(expected_eta))
                failures |= 1u << 18;
            rypath_dielectric::Parameters lane;
            const bool accepted = rypath_dielectric::prepare(eta, .2f, .3f, false, lane);
            if (accepted != (k == 0))
                failures |= 1u << 19;
        }
    }
    // Last two cases are valid hero0 but invalid active companions: a compact
    // coating implementation must not stop preparation after retaining hero0.
    const bool expected_valid =
        (mask == 1 || mask == 3 || mask == 15) && !(material_index >= 12 && mask != 1);
    if (a != expected_valid)
        failures |= 1u << 17;
    if (a && b) {
        failures |=
            compare(original, compact, normalize(float3(.3f, .2f, 1)),
                    normalize(float3(-.2f, .1f, 1)), .99f, float2(.15f, .25f), 719, Mode::Radiance);
        HeroTrack old_track, new_track;
        old_track.active_mask = new_track.active_mask = mask;
        for (uint k = 0; k < 4; ++k) {
            const auto density = (mask & (1u << k)) ? rypath_hero::number(.125f * float(k + 1))
                                                    : rypath_hero::Number{};
            old_track.density.lane[k] = new_track.density.lane[k] = density;
        }
        float4 old_beta(1, .25f, .5f, .75f), new_beta = old_beta;
        const bool old_future = hero_prepare_future(old_track, old_beta, original);
        const bool new_future = hero_prepare_future(new_track, new_beta, compact);
        if (old_future != new_future || !same(old_beta, new_beta) ||
            old_track.active_mask != new_track.active_mask ||
            original.active_mask != compact.active_mask)
            failures |= 1u << 20;
        for (uint k = 0; k < 4; ++k)
            if (!same(old_track.density.lane[k].fraction, new_track.density.lane[k].fraction) ||
                old_track.density.lane[k].exponent != new_track.density.lane[k].exponent)
                failures |= 1u << 20;
        if (old_future && new_future)
            failures |= compare(original, compact, normalize(float3(.3f, .2f, 1)),
                                normalize(float3(-.2f, .1f, 1)), .99f, float2(.15f, .25f), 719,
                                Mode::Radiance);
    }
    if (failures)
        failures |= (uint(a) << 24) | (uint(b) << 25) | (uint(expected_valid) << 26);
    out[index] = failures;
}
