// SPDX-License-Identifier: GPL-3.0-only
// Standalone CPU probe. Build and link against the pinned PBRT, outside rypath.
#include <pbrt/util/spectrum.h>

#include <cstdio>
#include <filesystem>
#include <type_traits>

int main(int argc, char **argv) {
    static_assert(std::is_same_v<pbrt::Float, float>);
    if (argc < 2) {
        std::fprintf(stderr, "Expected one or more spectrum files\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path path(argv[i]);
        const auto spectrum = pbrt::PiecewiseLinearSpectrum::Read(path.string(), pbrt::Allocator{});
        if (!spectrum) {
            std::fprintf(stderr, "PBRT could not read %s\n", argv[i]);
            return 1;
        }
        std::printf("%s %.17g", path.filename().string().c_str(), spectrum->MaxValue());
        for (pbrt::Float wavelength = 360; wavelength <= 830; ++wavelength) {
            std::printf(" %.17g", (*spectrum)(wavelength));
        }
        std::printf("\n");
    }
    return std::ferror(stdout) ? 1 : 0;
}
