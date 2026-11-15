// SPDX-License-Identifier: GPL-3.0-only
// Standalone oracle for the Release/binary32 PBRT build at
// b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f. See compare_pbrt_texture_graph.py.

#include "texture_graph_probe_inputs.hpp"

#include <pbrt/options.h>
#include <pbrt/textures.h>
#include <pbrt/util/parallel.h>

#include <memory>
#include <string>

using namespace pbrt;
namespace probe = graph_probe;

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Expected RGB PNG, grayscale PNG, and spectrum file paths\n";
        return 2;
    }
    Options = new PBRTOptions;
    ParallelInit(1);
    const Allocator alloc{};
    ColorEncoding::Init(alloc);
    RGBToSpectrumTable::Init(alloc);
    Spectra::Init(alloc);
    RGBColorSpace::Init(alloc);
    const auto linear = ColorEncoding::Get("linear", alloc);
    const auto spectrum_file = PiecewiseLinearSpectrum::Read(argv[3], alloc);
    if (!spectrum_file) {
        std::cerr << "PBRT could not read the probe spectrum file\n";
        return 2;
    }

    using probe::a;
    using probe::b;
    using probe::c;
    using probe::d;
    const Transform inverse =
        Translate({probe::translate[0], probe::translate[1], probe::translate[2]}) *
        Scale(probe::stretch[0], probe::stretch[1], probe::stretch[2]);
    UVMapping uv(probe::uv[0], probe::uv[1], probe::uv[2], probe::uv[3]);
    PlanarMapping planar(inverse, {probe::vs[0], probe::vs[1], probe::vs[2]},
                         {probe::vt[0], probe::vt[1], probe::vt[2]}, probe::planar_ds,
                         probe::planar_dt);
    SphericalMapping spherical(inverse);
    CylindricalMapping cylindrical(inverse);
    PointTransformMapping spatial(inverse);
    const std::array<TextureMapping2D, 4> mappings{&uv, &planar, &spherical, &cylindrical};
    constexpr std::array names{"map-uv", "map-planar", "map-spherical", "map-cylindrical"};

    FloatConstantTexture fa(a), fb(b), fc(c), fd(d), zero(0), one(1);
    ConstantSpectrum ca(a), cb(b), cc(c), cd(d);
    SpectrumConstantTexture sca(&ca), scb(&cb), scc(&cc), scd(&cd);
    BlackbodySpectrum planck(probe::temperature);
    RGBAlbedoSpectrum rgb(*RGBColorSpace::sRGB,
                          {probe::color[0], probe::color[1], probe::color[2]});
    SpectrumConstantTexture named(GetNamedSpectrum(std::string(probe::named_spectrum)));
    SpectrumConstantTexture file(*spectrum_file);
    SpectrumConstantTexture rgb_leaf(&rgb), blackbody_leaf(&planck);
    FloatScaledTexture fscale(&fb, &fc);
    SpectrumScaledTexture sscale(&sca, &fd);
    FloatMixTexture fmix(&fa, &fb, &fc);
    SpectrumMixTexture smix(&file, &named, &fd);
    SpectrumMixTexture select_low(&rgb_leaf, &blackbody_leaf, &zero);
    SpectrumMixTexture select_high(&rgb_leaf, &blackbody_leaf, &one);
    const Vector3f direction(probe::direction[0], probe::direction[1], probe::direction[2]);
    FloatDirectionMixTexture fdir(&fa, &fb, direction);
    SpectrumDirectionMixTexture sdir(&sca, &scb, direction);
    FloatBilerpTexture fbilerp(&planar, a, b, c, d);
    SpectrumBilerpTexture sbilerp(&uv, &ca, &cb, &cc, &cd);
    FloatCheckerboardTexture fcheck(&spherical, nullptr, &fa, &fb);
    SpectrumCheckerboardTexture scheck(nullptr, &spatial, &sca, &scb);
    FloatDotsTexture fdots(&uv, &fa, &fb);
    SpectrumDotsTexture sdots(&cylindrical, &sca, &scb);
    FBmTexture fbm(&spatial, probe::octaves, probe::roughness);
    WrinkledTexture wrinkled(&spatial, probe::octaves, probe::roughness);
    WindyTexture windy(&spatial);
    MarbleTexture marble(&spatial, probe::octaves, probe::roughness, probe::marble_scale,
                         probe::marble_variation);
    std::array<std::unique_ptr<FloatImageTexture>, 4> fimages;
    std::array<std::unique_ptr<SpectrumImageTexture>, 4> simages;
    for (std::size_t i = 0; i < fimages.size(); ++i) {
        const MIPMapFilterOptions opts{*ParseFilter(probe::filter_names[i]), 8};
        fimages[i] = std::make_unique<FloatImageTexture>(
            mappings[i], argv[1], opts, WrapMode::Repeat, 1.4f, true, linear, alloc);
        simages[i] =
            std::make_unique<SpectrumImageTexture>(mappings[i], argv[1], opts, WrapMode::Repeat,
                                                   .9f, false, linear, SpectrumType::Albedo, alloc);
    }
    const MIPMapFilterOptions point{FilterFunction::Point, 8};
    SpectrumImageTexture gray(&uv, argv[2], point, WrapMode::Repeat, 2.5f, false, linear,
                              SpectrumType::Albedo, alloc);
    SpectrumScaledTexture gray_scaled(&gray, &fa);
    std::cout << std::setprecision(9);
    for (int i = 0; i < probe::query_count; ++i) {
        const auto q = probe::query(i);
        const auto &p = q.p;
        const auto &x = q.dx;
        const auto &y = q.dy;
        const auto &n = q.n;
        const TextureEvalContext ctx({p[0], p[1], p[2]}, {x[0], x[1], x[2]}, {y[0], y[1], y[2]},
                                     {n[0], n[1], n[2]}, {q.u, q.v}, q.dudx, q.dudy, q.dvdx, q.dvdy,
                                     0);
        auto lambda = SampledWavelengths::SampleUniform(.5f);
        lambda[0] = q.wavelength;
        for (std::size_t j = 0; j < mappings.size(); ++j) {
            const auto m = mappings[j].Map(ctx);
            probe::row(i, names[j], m.st[0], m.st[1], m.dsdx, m.dtdx, m.dsdy, m.dtdy);
        }
        const auto m = spatial.Map(ctx);
        probe::row(i, "map-spatial", m.p.x, m.p.y, m.p.z, m.dpdx.x, m.dpdx.y, m.dpdx.z, m.dpdy.x,
                   m.dpdy.y, m.dpdy.z);
        const auto scalar = [&](std::string_view name, FloatTexture tex) {
            probe::row(i, name, tex.Evaluate(ctx));
        };
        const auto spectrum = [&](std::string_view name, SpectrumTexture tex) {
            probe::row(i, name, tex.Evaluate(ctx, lambda)[0]);
        };
        scalar("scalar-constant", &fa);
        scalar("scalar-scale", &fscale);
        scalar("scalar-mix", &fmix);
        scalar("scalar-directionmix", &fdir);
        scalar("scalar-bilerp", &fbilerp);
        scalar("scalar-checkerboard", &fcheck);
        scalar("scalar-dots", &fdots);
        scalar("scalar-fbm", &fbm);
        scalar("scalar-wrinkled", &wrinkled);
        scalar("scalar-windy", &windy);
        spectrum("spectrum-constant", &scd);
        spectrum("spectrum-named", &named);
        spectrum("spectrum-file", &file);
        spectrum("spectrum-rgb", &rgb_leaf);
        spectrum("spectrum-blackbody", &blackbody_leaf);
        spectrum("spectrum-scale", &sscale);
        spectrum("spectrum-mix", &smix);
        spectrum("spectrum-mix-0", &select_low);
        spectrum("spectrum-mix-1", &select_high);
        spectrum("spectrum-directionmix", &sdir);
        spectrum("spectrum-bilerp", &sbilerp);
        spectrum("spectrum-checkerboard", &scheck);
        spectrum("spectrum-dots", &sdots);
        spectrum("spectrum-marble", &marble);
        spectrum("spectrum-image-gray", &gray);
        spectrum("spectrum-scale-image-gray", &gray_scaled);
        for (std::size_t j = 0; j < fimages.size(); ++j) {
            scalar(std::string("scalar-image-") + probe::filter_names[j], fimages[j].get());
            spectrum(std::string("spectrum-image-") + probe::filter_names[j], simages[j].get());
        }
    }
    SphericalMapping sphere_axis(Transform{});
    CylindricalMapping cylinder_axis(Transform{});
    FloatBilerpTexture sphere_bilerp(&sphere_axis, a, b, c, d);
    FloatBilerpTexture cylinder_bilerp(&cylinder_axis, a, b, c, d);
    FloatDotsTexture sphere_dots(&sphere_axis, &fa, &fb);
    FloatDotsTexture cylinder_dots(&cylinder_axis, &fa, &fb);
    for (const float z : {.25f, -.25f}) {
        TextureEvalContext ctx;
        ctx.p = {0, 0, z};
        ctx.dpdx = {1, 0, 0};
        ctx.dpdy = {0, 1, 0};
        probe::row(z > 0 ? 0 : 1, "angular-axes", sphere_bilerp.Evaluate(ctx),
                   cylinder_bilerp.Evaluate(ctx), sphere_dots.Evaluate(ctx),
                   cylinder_dots.Evaluate(ctx));
    }
    ParallelCleanup();
}
