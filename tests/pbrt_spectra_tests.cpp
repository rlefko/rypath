// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_spectra.hpp"
#include "rgb_spectrum.hpp"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

using rypath::pbrt_named_spectrum;
using rypath::pbrt_named_spectrum_knots;
using rypath::pbrt_named_spectrum_names;

constexpr std::array<float, 471> previous_d65{
#include "../third_party/pbrt/d65.inc"
};

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

void check_sample(std::string_view name, int wavelength, std::uint32_t expected) {
    const auto *spectrum = pbrt_named_spectrum(name);
    require(spectrum != nullptr, "Missing spectrum with a reference sample");
    const auto index = static_cast<std::size_t>(wavelength - 360);
    if (const auto actual = std::bit_cast<std::uint32_t>((*spectrum)[index]); actual != expected) {
        std::fprintf(stderr, "%.*s at %d nm: expected %08" PRIx32 ", got %08" PRIx32 "\n",
                     static_cast<int>(name.size()), name.data(), wavelength, expected, actual);
        std::exit(EXIT_FAILURE);
    }
}

void check_registry() {
    const auto names = pbrt_named_spectrum_names();
    require(names.size() == 88, "Wrong named spectrum count");
    require(std::ranges::is_sorted(names), "Spectrum names are unsorted");
    require(std::ranges::adjacent_find(names) == names.end(), "Duplicate spectrum name");
    for (const auto name : names) {
        const auto *spectrum = pbrt_named_spectrum(name);
        require(spectrum != nullptr, "Could not look up a listed spectrum");
        require(std::ranges::all_of(
                    *spectrum, [](float value) { return std::isfinite(value) && value >= 0.0f; }),
                "Named spectrum has an invalid sample");
        require(std::ranges::any_of(*spectrum, [](float value) { return value > 0.0f; }),
                "Named spectrum has no positive samples");
        const auto knots = pbrt_named_spectrum_knots(name);
        require(knots.size() >= 2 && knots.front().wavelength <= 360.0f &&
                    knots.back().wavelength >= 830.0f,
                "Prepared knots must cover the visible wavelengths");
        require(std::ranges::all_of(knots,
                                    [](const auto &knot) {
                                        return std::isfinite(knot.wavelength) &&
                                               std::isfinite(knot.value);
                                    }),
                "Named spectrum has a nonfinite knot");
        require(std::ranges::is_sorted(knots, {}, &rypath::SpectralKnot::wavelength) &&
                    std::ranges::adjacent_find(knots, {}, &rypath::SpectralKnot::wavelength) ==
                        knots.end(),
                "Named spectrum wavelengths are not strictly increasing");
    }
    for (const auto name : {"", "STDILLUM-D65", "stdillum-D65.spd", "glass-SF5", "unknown"}) {
        require(pbrt_named_spectrum(name) == nullptr, "Unexpected named spectrum match");
        require(pbrt_named_spectrum_knots(name).empty(), "Unexpected named knot match");
    }
}

void check_knots() {
    const auto d65 = pbrt_named_spectrum_knots("stdillum-D65");
    require(d65.size() == 107 && d65.front().wavelength == 300.0f &&
                d65.front().value == 0x1.6993f6p-12f,
            "D65 prepared knots preserve normalized out-of-band values");

    const auto glass = pbrt_named_spectrum_knots("glass-F5");
    require(glass.size() == 27 && glass.front().wavelength == 359.0f &&
                glass[1].wavelength == 370.0f && glass[2].wavelength == 391.0f &&
                glass.front().value == 0x1.ba8922p+0f && glass.front().value == glass[1].value,
            "Optical knots preserve their nonunit values and PBRT endpoint padding");

    const auto copper = pbrt_named_spectrum_knots("metal-Cu-k");
    require(copper.size() == 56 && copper.front().wavelength == 298.757050f &&
                copper.front().value == 1.662125f,
            "Copper knots preserve the first fractional wavelength and its paired value");
}

void check_samples() {
    // The archived D65 payload preserves every binary32 value used by RGB and default lights.
    const auto *d65 = pbrt_named_spectrum("stdillum-D65");
    require(d65 == &rypath::pbrt_d65(), "D65 lookup does not share the named payload");
    require(std::ranges::equal(*d65, previous_d65,
                               [](float a, float b) {
                                   return std::bit_cast<std::uint32_t>(a) ==
                                          std::bit_cast<std::uint32_t>(b);
                               }),
            "D65 changed from the archived payload");

    // Values captured from scripts/pbrt_named_spectrum_probe.cpp at the pinned revision.
    check_sample("stdillum-F11", 360, 0x3d880ce0);
    check_sample("stdillum-F11", 555, 0x3f0c8914);
    check_sample("stdillum-F11", 830, 0x3bd749d3);
    check_sample("glass-F5", 509, 0x3fd77495);
    check_sample("metal-Cu-k", 509, 0x402602b6);
    check_sample("canon_eos_100d_r", 360, 0x3a879934);
    check_sample("canon_eos_100d_r", 830, 0x3abb0e76);
}

void dump_registry() {
    for (const auto name : pbrt_named_spectrum_names()) {
        const auto *spectrum = pbrt_named_spectrum(name);
        require(spectrum != nullptr, "Could not dump a listed spectrum");
        std::printf("%.*s", static_cast<int>(name.size()), name.data());
        for (const float sample : *spectrum) {
            std::printf(" %08" PRIx32, std::bit_cast<std::uint32_t>(sample));
        }
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--dump") {
        dump_registry();
        return std::ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    require(argc == 1, "Usage: rypath_pbrt_spectra_tests [--dump]");
    check_registry();
    check_samples();
    check_knots();
}
