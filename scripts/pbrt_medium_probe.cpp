// SPDX-License-Identifier: GPL-3.0-only
// Standalone oracle linked to unmodified pinned PBRT b0bdf461 (binary32).
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <pbrt/media.h>
#include <pbrt/util/colorspace.h>
#include <pbrt/util/scattering.h>
#include <pbrt/util/spectrum.h>
#include <string>
#include <type_traits>
namespace {
void word(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    std::cout << ' ' << std::hex << std::setfill('0') << std::setw(8) << bits << std::dec;
}
} // namespace
int main(int argc, char **argv) {
    using namespace pbrt;
    static_assert(std::is_same_v<Float, float>);
    if (argc != 2) {
        std::cerr << "Expected --presets or --phase or --spectra or --blackbody\n";
        return 1;
    }
    Allocator alloc;
    RGBToSpectrumTable::Init(alloc);
    Spectra::Init(alloc);
    RGBColorSpace::Init(alloc);
    const std::string mode = argv[1];
    std::cout << std::setprecision(9);
    if (mode == "--presets") {
        for (std::string name; std::getline(std::cin, name);) {
            Spectrum a, s;
            if (!GetMediumScatteringProperties(name, &a, &s, alloc)) {
                std::cerr << "Unknown PBRT medium preset: " << name << '\n';
                return 1;
            }
            std::cout << name;
            // Use the actual homogeneous dense preparation, rather than merely
            // evaluating RGBUnboundedSpectrum at comparison wavelengths.
            const HomogeneousMedium medium(a, s, 1, alloc.new_object<ConstantSpectrum>(0), 1, 0,
                                           alloc);
            for (int which = 0; which < 2; ++which)
                for (int wavelength = 360; wavelength <= 830; ++wavelength) {
                    SampledWavelengths lambda;
                    for (int i = 0; i < NSpectrumSamples; ++i)
                        lambda[i] = Float(wavelength);
                    const auto properties = medium.SamplePoint(Point3f(0, 0, 0), lambda);
                    word(which == 0 ? properties.sigma_a[0] : properties.sigma_s[0]);
                }
            std::cout << '\n';
        }
    } else if (mode == "--phase") {
        Float ox, oy, oz, ix, iy, iz, g;
        while (std::cin >> ox >> oy >> oz >> ix >> iy >> iz >> g)
            std::cout << HenyeyGreenstein(Dot(Vector3f(ox, oy, oz), Vector3f(ix, iy, iz)), g)
                      << '\n';
    } else if (mode == "--blackbody") {
        Float temperature, scale, lescale;
        while (std::cin >> temperature >> scale >> lescale) {
            Spectrum spectrum = alloc.new_object<BlackbodySpectrum>(temperature);
            const HomogeneousMedium medium(spectrum, spectrum, scale, spectrum,
                                           lescale / SpectrumToPhotometric(spectrum), 0, alloc);
            for (int which = 0; which < 3; ++which)
                for (int wavelength = 360; wavelength <= 830; ++wavelength) {
                    SampledWavelengths lambda;
                    for (int i = 0; i < NSpectrumSamples; ++i)
                        lambda[i] = Float(wavelength);
                    const auto p = medium.SamplePoint(Point3f(0, 0, 0), lambda);
                    word(which == 0 ? p.sigma_a[0] : which == 1 ? p.sigma_s[0] : p.Le[0]);
                }
            std::cout << '\n';
        }
    } else if (mode == "--spectra") {
        // Standalone constant/RGB homogeneous preparation including illuminant
        // photometric normalization and independent extinction/emission scales.
        Float ar, ag, ab, sr, sg, sb, lr, lg, lb, scale, lescale;
        while (std::cin >> ar >> ag >> ab >> sr >> sg >> sb >> lr >> lg >> lb >> scale >> lescale) {
            Spectrum a =
                alloc.new_object<RGBUnboundedSpectrum>(*RGBColorSpace::sRGB, RGB(ar, ag, ab));
            Spectrum s =
                alloc.new_object<RGBUnboundedSpectrum>(*RGBColorSpace::sRGB, RGB(sr, sg, sb));
            Spectrum le =
                alloc.new_object<RGBIlluminantSpectrum>(*RGBColorSpace::sRGB, RGB(lr, lg, lb));
            if (le.MaxValue() != 0)
                lescale /= SpectrumToPhotometric(le);
            const HomogeneousMedium medium(a, s, scale, le, lescale, 0, alloc);
            for (int which = 0; which < 3; ++which)
                for (int wavelength = 360; wavelength <= 830; ++wavelength) {
                    SampledWavelengths lambda;
                    for (int i = 0; i < NSpectrumSamples; ++i)
                        lambda[i] = Float(wavelength);
                    const auto p = medium.SamplePoint(Point3f(0, 0, 0), lambda);
                    word(which == 0 ? p.sigma_a[0] : which == 1 ? p.sigma_s[0] : p.Le[0]);
                }
            std::cout << '\n';
        }
    } else {
        std::cerr << "Unknown probe mode\n";
        return 1;
    }
    return std::cin.eof() && !std::cin.bad() && bool(std::cout) ? 0 : 1;
}
