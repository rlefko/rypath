// SPDX-License-Identifier: GPL-3.0-only
// Build and link against the pinned CPU PBRT, outside rypath.
#include <pbrt/util/spectrum.h>

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>

int main() {
    static_assert(std::is_same_v<pbrt::Float, float>);
    for (std::string label; std::cin >> label;) {
        pbrt::Float temperature = 0;
        if (!(std::cin >> temperature) || !std::isfinite(temperature) || temperature <= 0) {
            std::fprintf(stderr, "Invalid temperature for %s\n", label.c_str());
            return 1;
        }
        const pbrt::BlackbodySpectrum spectrum(temperature);
        std::printf("%s", label.c_str());
        for (int quarter_nm = 360 * 4; quarter_nm <= 830 * 4; ++quarter_nm) {
            const auto wavelength = static_cast<pbrt::Float>(quarter_nm) * 0.25f;
            // DiffuseMaterial::GetBxDF clamps each value after evaluation.
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
