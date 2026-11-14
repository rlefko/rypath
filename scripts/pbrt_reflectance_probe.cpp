// SPDX-License-Identifier: GPL-3.0-only
// Build and link against the pinned CPU PBRT, outside rypath.
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
    for (std::string name; std::cin >> name;) {
        const pbrt::Spectrum spectrum = pbrt::GetNamedSpectrum(name);
        if (!spectrum) {
            std::fprintf(stderr, "PBRT has no named spectrum '%s'\n", name.c_str());
            return 1;
        }
        std::printf("%s", name.c_str());
        for (int quarter_nm = 360 * 4; quarter_nm <= 830 * 4; ++quarter_nm) {
            const auto wavelength = static_cast<pbrt::Float>(quarter_nm) * 0.25f;
            // Use PBRT's post-evaluation clamp from DiffuseMaterial::GetBxDF.
            const float value = pbrt::Clamp(spectrum(wavelength), 0.f, 1.f);
            std::uint32_t bits = 0;
            static_assert(sizeof(bits) == sizeof(value));
            std::memcpy(&bits, &value, sizeof(bits));
            std::printf(" %08" PRIx32, bits);
        }
        std::printf("\n");
    }
    return std::cin.eof() && !std::cin.bad() && !std::ferror(stdout) ? 0 : 1;
}
