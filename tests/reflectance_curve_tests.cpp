// SPDX-License-Identifier: GPL-3.0-only
#include "reflectance_curve.hpp"
#include "render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <span>
#include <vector>

namespace {
using namespace rypath;

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

std::vector<SpectralKnot> compile(std::span<const SpectralKnot> raw) {
    auto result = compile_reflectance_curve(raw, 100);
    require(result.error == ReflectanceCurveError::none, "Valid curve failed to compile");
    require(result.knots.capacity() == result.knots.size(),
            "Compiled curve retained storage beyond its charged knot count");
    require(std::ranges::all_of(result.knots,
                                [](SpectralKnot knot) {
                                    return std::isfinite(knot.wavelength) && knot.value >= 0 &&
                                           knot.value <= 1;
                                }),
            "Compiled curve has an invalid knot");
    for (const auto knot : raw) {
        require(std::ranges::binary_search(result.knots, knot.wavelength, {},
                                           &SpectralKnot::wavelength),
                "Compiler lost an authored wavelength");
    }
    const auto exact = compile_reflectance_curve(raw, result.knots.size());
    require(exact.error == ReflectanceCurveError::none, "Exact output limit rejected curve");
    const auto short_limit = compile_reflectance_curve(raw, result.knots.size() - 1);
    require(short_limit.error == ReflectanceCurveError::limit_exceeded && short_limit.knots.empty(),
            "Output limit did not reject the entire curve");
    return result.knots;
}

void check(std::span<const SpectralKnot> knots, float wavelength, double expected) {
    const double actual = sampled_spectrum(knots, wavelength);
    if ((expected == 0 || expected == 1) ? actual != expected
                                         : std::abs(actual - expected) > 1e-6) {
        std::fprintf(stderr, "Reflectance at %.9g: got %.17g, expected %.17g\n", wavelength, actual,
                     expected);
        std::exit(EXIT_FAILURE);
    }
}

void analytic_curves() {
    const auto ordinary = compile(std::array{SpectralKnot{499.25f, -1}, SpectralKnot{500.75f, 2}});
    for (const auto [wavelength, expected] :
         std::array{SpectralKnot{499.25f, 0}, SpectralKnot{499.75f, 0}, SpectralKnot{500, 0.5f},
                    SpectralKnot{500.25f, 1}, SpectralKnot{500.75f, 1}}) {
        check(ordinary, wavelength, expected);
    }
    check(ordinary, std::nextafter(500.75f, 501.f), 0);
    const auto narrow =
        compile(std::array{SpectralKnot{503.25f, 0.5f}, SpectralKnot{503.75f, 0.5f}});
    check(narrow, 503.5f, 0.5);
    check(narrow, 503.f, 0);
    check(narrow, 504.f, 0);

    // Exact dyadic source: cancellation must retain raw(lambda) = lambda / 512.
    const auto wide =
        compile(std::array{SpectralKnot{-0x1p120f, -0x1p111f}, SpectralKnot{0x1p120f, 0x1p111f}});
    for (int i = 0; i <= 1880; ++i) {
        const float wavelength = 360.f + static_cast<float>(i) / 4;
        check(wide, wavelength, std::min(double{wavelength} / 512, 1.));
    }
    constexpr float largest = std::numeric_limits<float>::max();
    const auto identity =
        compile(std::array{SpectralKnot{-largest, -largest}, SpectralKnot{largest, largest}});
    check(identity, 360, 1);
    check(identity, 830, 1);
    const auto steep = compile(std::array{SpectralKnot{511, -largest}, SpectralKnot{513, largest}});
    check(steep, 512, 0);
    check(steep, std::nextafter(512.f, 513.f), 1);
    check(steep, std::nextafter(512.f, 511.f), 0);

    const auto one_sided =
        compile(std::array{SpectralKnot{360, 0}, SpectralKnot{largest, largest}});
    for (const float wavelength : {360.f, std::nextafter(360.f, 361.f), 360.25f, 360.75f, 361.f}) {
        check(one_sided, wavelength, std::min(double{wavelength} - 360, 1.));
    }
    const auto boundary = compile(std::array{SpectralKnot{350, -1}, SpectralKnot{360, 0.5f}});
    check(boundary, 360, 0.5);
    check(boundary, std::nextafter(360.f, 361.f), 0);
    const auto invisible = compile(std::array{SpectralKnot{900, -1}, SpectralKnot{1000, 2}});
    check(invisible, 830, 0);
    const auto zero = compile(std::array{SpectralKnot{360, 0}, SpectralKnot{830, 0}});
    check(zero, 500, 0);
}

void malformed_curves() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto &raw : std::vector<std::vector<SpectralKnot>>{{},
                                                                  {{500, 1}},
                                                                  {{500, 0}, {500, 1}},
                                                                  {{501, 0}, {500, 1}},
                                                                  {{-inf, 0}, {500, 1}},
                                                                  {{360, nan}, {830, 1}},
                                                                  {{360, 0}, {830, inf}}}) {
        const auto result = compile_reflectance_curve(raw, 100);
        require(result.error == ReflectanceCurveError::invalid_knots && result.knots.empty(),
                "Malformed curve did not fail without a partial payload");
    }
}
} // namespace

int main() {
    try {
        analytic_curves();
        malformed_curves();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return EXIT_FAILURE;
    }
}
