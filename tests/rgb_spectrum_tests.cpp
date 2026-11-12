// SPDX-License-Identifier: GPL-3.0-only

#include "rgb_spectrum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

// Samples from the pinned upstream RGBToSpectrumTable and RGBSigmoidPolynomial
// using its native Float build. See third_party/pbrt/README.md.
constexpr std::array wavelengths{360.0f, 400.125f, 475.25f, 555.0f, 612.75f, 700.0f, 830.0f};
constexpr float tolerance = 1e-5f;

struct Reference {
    std::array<float, 3> rgb;
    std::array<float, wavelengths.size()> values;
};

constexpr Reference references[]{
    {{1.0f, 0.0f, 0.0f},
     {0.993458152f, 0.467036664f, 0.00707635283f, 0.0162791014f, 0.973684549f, 0.999466777f,
      0.999946356f}},
    {{0.0f, 1.0f, 0.0f},
     {0.000340610743f, 0.00106203556f, 0.107176632f, 0.957678437f, 0.0365299284f, 0.000550985336f,
      4.40776348e-05f}},
    {{0.0f, 0.0f, 1.0f},
     {0.998519957f, 0.996137619f, 0.832085252f, 0.0189266801f, 0.0108638704f, 0.0213832259f,
      0.993077934f}},
    {{1.0f, 1.0f, 0.0f},
     {0.00147947669f, 0.00386232138f, 0.168149352f, 0.981050491f, 0.989101768f, 0.978364468f,
      0.00680810213f}},
    {{1.0f, 0.0f, 1.0f},
     {0.999647975f, 0.998902559f, 0.890475869f, 0.0435749888f, 0.962355852f, 0.999430358f,
      0.999954462f}},
    {{0.0f, 1.0f, 1.0f},
     {0.00654619932f, 0.536227107f, 0.992945731f, 0.983757377f, 0.0262466371f, 0.000532001257f,
      5.3524971e-05f}},
    {{0.62f, 0.17f, 0.31f},
     {0.90882051f, 0.685571253f, 0.211925358f, 0.205820531f, 0.497585714f, 0.957403779f,
      0.996389627f}},
    {{0.31f, 0.62f, 0.17f},
     {0.0161775351f, 0.0422106981f, 0.328716904f, 0.612538397f, 0.374300122f, 0.0365359485f,
      0.00302916765f}},
    {{0.17f, 0.31f, 0.62f},
     {0.85565865f, 0.764200509f, 0.506154299f, 0.289192945f, 0.212982684f, 0.169007003f,
      0.196176261f}},
    {{0.3f, 0.3f, 0.18f},
     {0.080599755f, 0.120709091f, 0.223466784f, 0.304717362f, 0.299298584f, 0.197936207f,
      0.0574654341f}},
    {{0.3f, 0.18f, 0.3f},
     {0.741298199f, 0.505654752f, 0.233401388f, 0.191239268f, 0.256063968f, 0.65319109f,
      0.971571207f}},
    {{0.18f, 0.3f, 0.3f},
     {0.186399519f, 0.243058681f, 0.31235218f, 0.286305249f, 0.213822782f, 0.101576447f,
      0.0278332829f}},
    {{0.482147723f, 0.0f, 0.0f},
     {0.00623247027f, 0.00650209188f, 0.00940865278f, 0.0290119052f, 0.307253689f, 0.987404823f,
      0.998754144f}},
    {{0.0f, 0.000399308716f, 0.0f},
     {0.000130265951f, 0.000305831432f, 0.0024561882f, 0.0113705397f, 0.00257691741f,
      0.000240296125f, 2.59578228e-05f}},
    {{0.0f, 0.0f, 0.999998331f},
     {0.998519897f, 0.9961375f, 0.832081676f, 0.0189267993f, 0.0108636916f, 0.0213809311f,
      0.993076503f}},
    {{0.01f, 0.008f, 0.007f},
     {0.00669935346f, 0.00674527884f, 0.00711664557f, 0.00802218914f, 0.00916874409f, 0.0123385191f,
      0.0268892348f}},
    {{1e-20f, 0.0f, 0.0f},
     {3.72231007e-05f, 5.76972961e-05f, 0.000150948763f, 0.000554561615f, 0.00179868937f,
      0.0102370679f, 0.00201389194f}},
    {{std::numeric_limits<float>::denorm_min(), 0.0f, 0.0f},
     {3.72231007e-05f, 5.76972961e-05f, 0.000150948763f, 0.000554561615f, 0.00179868937f,
      0.0102370679f, 0.00201389194f}},
};

bool match_reconstruction() {
    float max_error = 0.0f;
    for (const auto &reference : references) {
        const auto spectrum = rypath::reconstruct_srgb(reference.rgb);
        if (!spectrum || spectrum->is_constant) {
            std::cerr << "Chromatic RGB did not reconstruct a polynomial\n";
            return false;
        }
        for (std::size_t i = 0; i < wavelengths.size(); ++i) {
            const float actual = rypath::evaluate_srgb(*spectrum, wavelengths[i]);
            const float error = std::abs(actual - reference.values[i]);
            if (!std::isfinite(actual) || error > tolerance) {
                std::cerr << "PBRT reconstruction mismatch for RGB " << reference.rgb[0] << ' '
                          << reference.rgb[1] << ' ' << reference.rgb[2] << " at " << wavelengths[i]
                          << " nm: " << actual << " versus " << reference.values[i] << '\n';
                return false;
            }
            max_error = std::max(max_error, error);
        }
        for (int quarter_nm = 360 * 4; quarter_nm <= 830 * 4; ++quarter_nm) {
            const float nm = static_cast<float>(quarter_nm) * 0.25f;
            const float actual = rypath::evaluate_srgb(*spectrum, nm);
            if (!std::isfinite(actual) || actual < 0.0f || actual > 1.0f) {
                std::cerr << "Reconstructed spectrum outside [0, 1]\n";
                return false;
            }
        }
    }
    std::cout << "Maximum PBRT RGB sample error: " << max_error << '\n';
    return true;
}

bool check_neutrals() {
    constexpr auto tiny = std::numeric_limits<float>::denorm_min();
    for (float level : {0.0f, tiny, 0.18f, 0.5f, std::nextafter(1.0f, 0.0f), 1.0f}) {
        const auto spectrum = rypath::reconstruct_srgb({level, level, level});
        if (!spectrum || !spectrum->is_constant || spectrum->constant != level) {
            std::cerr << "Neutral RGB did not reconstruct the exact constant\n";
            return false;
        }
        for (float wavelength : wavelengths) {
            if (rypath::evaluate_srgb(*spectrum, wavelength) != level) {
                std::cerr << "Neutral RGB evaluation did not preserve the exact constant\n";
                return false;
            }
        }
    }
    return true;
}

bool check_boundaries() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    for (float invalid : {-1.0f, 1.1f, -inf, inf, nan}) {
        for (int channel = 0; channel < 3; ++channel) {
            std::array rgb{0.25f, 0.25f, 0.25f};
            rgb[channel] = invalid;
            if (rypath::reconstruct_srgb(rgb)) {
                std::cerr << "Accepted out-of-domain RGB input\n";
                return false;
            }
        }
    }
    constexpr float large = std::numeric_limits<float>::max();
    const rypath::RgbSpectrum positive{.coefficients = {0, 0, large}};
    const rypath::RgbSpectrum negative{.coefficients = {0, 0, -large}};
    if (rypath::evaluate_srgb(positive, 555.0f) != 1.0f ||
        rypath::evaluate_srgb(negative, 555.0f) != 0.0f) {
        std::cerr << "Large polynomial values failed to saturate the sigmoid\n";
        return false;
    }
    const rypath::RgbSpectrum overflow{.coefficients = {large, 0, 0}};
    if (!std::isnan(rypath::evaluate_srgb(overflow, 555.0f))) {
        std::cerr << "Polynomial overflow did not produce a non-finite result\n";
        return false;
    }
    return true;
}

bool check_d65() {
    const auto &d65 = rypath::pbrt_d65();
    // Endpoints, PBRT's 5 nm knots, and interpolated samples.
    constexpr std::array indices{360, 361, 375, 421, 550, 555, 556, 560, 601, 705, 829, 830};
    constexpr std::array values{0x1.e2efd8p-2f, 0x1.e894c8p-2f, 0x1.0837dep-1f, 0x1.e03ef0p-1f,
                                0x1.0d58fap+0f, 0x1.081c4ep+0f, 0x1.07102cp+0f, 0x1.02dfa0p+0f,
                                0x1.d1cb2cp-1f, 0x1.79d8bcp-1f, 0x1.36c77cp-1f, 0x1.384426p-1f};
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (d65[indices[i] - 360] != values[i]) {
            std::cerr << "PBRT D65 mismatch at " << indices[i] << " nm\n";
            return false;
        }
    }
    return std::all_of(d65.begin(), d65.end(),
                       [](float value) { return std::isfinite(value) && value > 0.0f; });
}

} // namespace

int main() {
    return match_reconstruction() && check_neutrals() && check_boundaries() && check_d65() ? 0 : 1;
}
