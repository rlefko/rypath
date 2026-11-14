// SPDX-License-Identifier: GPL-3.0-only
// Standalone CPU probe. Build and link against the pinned PBRT, outside rypath.
#include <pbrt/util/spectrum.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>

int main() {
    static_assert(std::is_same_v<pbrt::Float, float>);
    pbrt::Spectra::Init(pbrt::Allocator{});
    for (std::string name; std::getline(std::cin, name);) {
        const pbrt::Spectrum spectrum = pbrt::GetNamedSpectrum(name);
        if (!spectrum) {
            std::fprintf(stderr, "PBRT has no named spectrum '%s'\n", name.c_str());
            return 1;
        }
        std::printf("%s", name.c_str());
        for (pbrt::Float wavelength = 360; wavelength <= 830; ++wavelength) {
            const float sample = spectrum(wavelength);
            std::uint32_t bits;
            static_assert(sizeof(bits) == sizeof(sample));
            std::memcpy(&bits, &sample, sizeof(bits));
            std::printf(" %08" PRIx32, bits);
        }
        std::printf("\n");
    }
    return std::cin.eof() && !std::ferror(stdout) ? 0 : 1;
}
