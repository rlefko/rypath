// SPDX-License-Identifier: GPL-3.0-only
// Standalone oracle linked to unmodified pinned PBRT b0bdf461 binary32.
#include <iomanip>
#include <iostream>
#include <pbrt/bxdfs.h>
#include <pbrt/options.h>
#include <type_traits>
int main() {
    using namespace pbrt;
    static_assert(std::is_same_v<Float, float>);
    std::cout << std::setprecision(9);
    float eta, ax, ay, be, bk, bx, by, R, t, a, g, ox, oy, oz, ix, iy, iz, uc, u, v;
    unsigned conductor, depth, samples, seed, operation;
    PBRTOptions opts;
    Options = &opts;
    while (std::cin >> eta >> ax >> ay >> be >> bk >> bx >> by >> R >> t >> a >> g >> ox >> oy >>
           oz >> ix >> iy >> iz >> uc >> u >> v >> conductor >> depth >> samples >> seed >>
           operation) {
        opts.seed = seed;
        Vector3f o(ox, oy, oz), i(ix, iy, iz);
        auto test = [&](auto &b) {
            auto f = b.f(o, i, TransportMode::Radiance);
            auto pdf = b.PDF(o, i, TransportMode::Radiance, BxDFReflTransFlags::All);
            auto s = b.Sample_f(o, uc, {u, v}, TransportMode::Radiance, BxDFReflTransFlags::All);
            std::cout << f[0] << ' ' << pdf << ' ';
            if (s)
                std::cout << s->f[0] * std::abs(s->wi.z) / s->pdf << ' ' << s->pdf << ' ' << s->wi.x
                          << ' ' << s->wi.y << ' ' << s->wi.z << ' ' << s->pdfIsProportional;
            else
                std::cout << "0 0 0 0 0 0";
            std::cout << '\n';
        };
        DielectricBxDF top(eta, TrowbridgeReitzDistribution(ax, ay));
        if (operation == 1) {
            ThinDielectricBxDF b(eta);
            test(b);
        } else if (operation == 2) {
            DiffuseTransmissionBxDF b{SampledSpectrum(R), SampledSpectrum(a)};
            test(b);
        } else if (conductor) {
            CoatedConductorBxDF b(top,
                                  ConductorBxDF(TrowbridgeReitzDistribution(bx, by),
                                                SampledSpectrum(be), SampledSpectrum(bk)),
                                  t, SampledSpectrum(a), g, depth, samples);
            test(b);
        } else {
            CoatedDiffuseBxDF b(top, DiffuseBxDF(SampledSpectrum(R)), t, SampledSpectrum(a), g,
                                depth, samples);
            test(b);
        }
    }
    Options = nullptr;
    return std::cin.eof() && !std::cin.bad() ? 0 : 1;
}
