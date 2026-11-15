// SPDX-License-Identifier: GPL-3.0-only
// Build against pinned binary32 PBRT b0bdf461; no rypath scattering code is included.
#include <iomanip>
#include <iostream>
#include <pbrt/bxdfs.h>
#include <type_traits>
int main() {
    using namespace pbrt;
    static_assert(std::is_same_v<Float, float>);
    std::cout << std::setprecision(9);
    Float eta, k, ax, ay, ox, oy, oz, ix, iy, iz, u, v;
    unsigned remap;
    while (std::cin >> eta >> k >> ax >> ay >> ox >> oy >> oz >> ix >> iy >> iz >> u >> v >>
           remap) {
        if (remap) {
            ax = TrowbridgeReitzDistribution::RoughnessToAlpha(ax);
            ay = TrowbridgeReitzDistribution::RoughnessToAlpha(ay);
        }
        TrowbridgeReitzDistribution distribution(ax, ay);
        ConductorBxDF bxdf(distribution, SampledSpectrum(eta), SampledSpectrum(k));
        Vector3f o(ox, oy, oz), i(ix, iy, iz);
        auto f = bxdf.f(o, i, TransportMode::Radiance);
        auto pdf = bxdf.PDF(o, i, TransportMode::Radiance, BxDFReflTransFlags::All);
        auto sample = bxdf.Sample_f(o, .5f, {u, v}, TransportMode::Radiance);
        std::cout << f[0] << ' ' << pdf << ' '
                  << FrComplex(std::abs(oz), pstd::complex<Float>(eta, k));
        if (sample) {
            auto s = *sample;
            std::cout << ' ' << s.wi.x << ' ' << s.wi.y << ' ' << s.wi.z << ' '
                      << s.f[0] * std::abs(s.wi.z) / s.pdf << ' ' << s.pdf << ' '
                      << (s.pdf > 0 && s.f[0] > 0) << ' ' << distribution.EffectivelySmooth();
        } else
            std::cout << " 0 0 0 0 0 0 " << distribution.EffectivelySmooth();
        std::cout << '\n';
    }
    return std::cin.eof() && !std::cin.bad() ? 0 : 1;
}
