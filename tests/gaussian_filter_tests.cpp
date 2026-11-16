// SPDX-License-Identifier: GPL-3.0-only
// To regenerate the pinned oracle digests, compile this file as C++17 with
// RYPATH_PBRT_GAUSSIAN_ORACLE and link PBRT b0bdf461 in Float mode, with
// -ffp-contract=off. This optional branch uses upstream GaussianFilter,
// PiecewiseConstant1D, and the production GaussianFilter::Sample directly.
#if defined(RYPATH_PBRT_GAUSSIAN_ORACLE)
#include <pbrt/filters.h>
#else
#include "gaussian_filter.hpp"
#include "render.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

struct Configuration {
    float x, y, sigma;
};
constexpr std::array configurations{Configuration{1.5f, 1.5f, .5f}, Configuration{1.5f, .75f, .5f},
                                    Configuration{.1f, .16f, .04f}};

void hash_float(std::uint64_t &hash, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (bits >> shift) & 255;
        hash *= 1099511628211ULL;
    }
}

#if defined(RYPATH_PBRT_GAUSSIAN_ORACLE)
int oracle() {
    for (const auto config : configurations) {
        pbrt::GaussianFilter filter({config.x, config.y}, config.sigma);
        const int nx = int(32 * config.x), ny = int(32 * config.y);
        std::vector<float> row(nx), marginal;
        std::uint64_t cdf_hash = 14695981039346656037ULL;
        for (int y = 0; y < ny; ++y) {
            const float py = pbrt::Lerp((static_cast<float>(y) + .5f) / static_cast<float>(ny),
                                        -config.y, config.y);
            for (int x = 0; x < nx; ++x) {
                const float px = pbrt::Lerp((static_cast<float>(x) + .5f) / static_cast<float>(nx),
                                            -config.x, config.x);
                row[x] = filter.Evaluate({px, py});
            }
            pbrt::PiecewiseConstant1D conditional(row, -config.x, config.x);
            marginal.push_back(conditional.Integral());
            for (float value : conditional.cdf)
                hash_float(cdf_hash, value);
        }
        pbrt::PiecewiseConstant1D marginal_distribution(marginal, -config.y, config.y);
        for (float value : marginal_distribution.cdf)
            hash_float(cdf_hash, value);
        std::uint64_t sample_hash = 14695981039346656037ULL;
        for (int y = 0; y <= 32; ++y)
            for (int x = 0; x <= 32; ++x) {
                const auto sample =
                    filter.Sample({std::min(static_cast<float>(x) / 32.f, 0x1.fffffep-1f),
                                   std::min(static_cast<float>(y) / 32.f, 0x1.fffffep-1f)});
                hash_float(sample_hash, sample.p.x);
                hash_float(sample_hash, sample.p.y);
            }
        std::cout << "0x" << std::hex << cdf_hash << "ULL, 0x" << sample_hash << "ULL\n";
    }
    return 0;
}
#else
using namespace rypath;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

GaussianFilterDistribution compile(Configuration config) {
    auto result =
        compile_gaussian_filter({PixelFilterKind::gaussian, config.x, config.y, config.sigma});
    if (!result.diagnostic.empty())
        throw std::runtime_error(result.diagnostic);
    return std::move(result.distribution);
}

void pinned_oracle() {
    // All CDF entries and 1089 production sample pairs per configuration,
    // obtained independently from the pinned PBRT-linked branch above.
    constexpr std::array<std::array<std::uint64_t, 2>, 3> expected{
        {{0xc3f9ee5098fe6b9aULL, 0xee5857e52f638c40ULL},
         {0x1e3b50fa53faba43ULL, 0xe21428c7748daa49ULL},
         {0x64d7a87137843fa9ULL, 0xfadf77a69a345d53ULL}}};
    for (std::size_t i = 0; i < configurations.size(); ++i) {
        const auto distribution = compile(configurations[i]);
        std::uint64_t cdf_hash = 14695981039346656037ULL;
        for (const float value : distribution.conditional_cdf)
            hash_float(cdf_hash, value);
        for (const float value : distribution.marginal_cdf)
            hash_float(cdf_hash, value);
        std::uint64_t sample_hash = 14695981039346656037ULL;
        for (int y = 0; y <= 32; ++y)
            for (int x = 0; x <= 32; ++x) {
                const auto sample = sample_gaussian_filter(
                    distribution, std::min(static_cast<float>(x) / 32.f, 0x1.fffffep-1f),
                    std::min(static_cast<float>(y) / 32.f, 0x1.fffffep-1f));
                hash_float(sample_hash, sample[0]);
                hash_float(sample_hash, sample[1]);
            }
        if (cdf_hash != expected[i][0] || sample_hash != expected[i][1]) {
            std::cerr << "configuration " << i << ": 0x" << std::hex << cdf_hash << " 0x"
                      << sample_hash << std::dec << '\n';
            throw std::runtime_error("Gaussian CDF or samples differ from pinned PBRT");
        }
    }
}

void normalized_model() {
    const auto distribution = compile({.0625f, .0625f, .01f});
    require(distribution.width == 2 && distribution.height == 2,
            "Gaussian grid must use floor(32*radius)");
    // This normalized table is uniform. A continuous Gaussian instead has
    // P[x > .03125] ~= .000889021 and must fail this contract.
    unsigned positive = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        const float u = (static_cast<float>(i) + .5f) / 4096;
        const auto sample = sample_gaussian_filter(distribution, u, 1 - u);
        require(std::abs(sample[0] - (.125f * u - .0625f)) <= 0x1p-24f,
                "Two-cell Gaussian must sample its uniform midpoint table");
        positive += sample[0] > .03125f;
    }
    require(positive == 1024, "Tabulated Gaussian edge probability is incorrect");

    const auto plateau = compile({.25f, .25f, .006f});
    require(std::count(plateau.marginal_cdf.begin(), plateau.marginal_cdf.end(), 0.f) > 1,
            "Plateau test must exercise leading zero-mass cells");
    for (const float u : {0.f, 0x1p-24f, .5f, 0x1.fffffep-1f, 1.f}) {
        const auto sample = sample_gaussian_filter(plateau, u, u);
        require(std::isfinite(sample[0]) && std::isfinite(sample[1]) &&
                    std::abs(sample[0]) <= .0625f && std::abs(sample[1]) <= .0625f,
                "Zero-mass CDF plateaus must not select an inactive cell");
    }
}

void independent_density() {
    for (const auto config : configurations) {
        const auto distribution = compile(config);
        std::vector<double> cdf(distribution.width + 1);
        const auto gaussian = [&](double x) {
            return std::exp(-x * x / (2 * config.sigma * config.sigma));
        };
        for (std::uint32_t x = 0; x < distribution.width; ++x) {
            const double p = -config.x + (x + .5) * 2 * config.x / distribution.width;
            cdf[x + 1] = cdf[x] + std::max(0., gaussian(p) - gaussian(config.x));
        }
        const double total = cdf.back();
        for (auto &value : cdf)
            value /= total;
        for (std::uint32_t x = 0; x <= distribution.width; ++x)
            require(std::abs(cdf[x] - distribution.conditional_cdf[x]) < .00015,
                    "Gaussian CDF disagrees with independent exponential midpoint quadrature");
    }
}

void invalid_parameters() {
    const auto rejects = [](PixelFilter filter) {
        const auto result = compile_gaussian_filter(filter);
        require(!result.diagnostic.empty() && result.distribution.conditional_cdf.empty() &&
                    result.distribution.marginal_cdf.empty(),
                "Invalid Gaussian must diagnose and withhold its distribution");
    };
    rejects({});
    for (const float value :
         {0.f, -1.f, std::numeric_limits<float>::denorm_min(),
          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        rejects({PixelFilterKind::gaussian, value, 1.5f, .5f});
        rejects({PixelFilterKind::gaussian, 1.5f, value, .5f});
        rejects({PixelFilterKind::gaussian, 1.5f, 1.5f, value});
    }
    rejects({PixelFilterKind::gaussian, .02f, 1.5f, .5f});
    rejects({PixelFilterKind::gaussian, 129, 1.5f, .5f});
    rejects({PixelFilterKind::gaussian, 128, 128, .5f});
    rejects({PixelFilterKind::gaussian, .0625f, .0625f, .001f});
    rejects({PixelFilterKind::gaussian, 1.5f, 1.5f, 1e30f});
    rejects({PixelFilterKind::gaussian, 1.5f, 1.5f, std::numeric_limits<float>::min()});
    const auto one_cell = compile({.03125f, .03125f, .5f});
    require(one_cell.width == 1 && one_cell.height == 1,
            "The smallest nonempty Gaussian table must remain supported");
    const auto max_axis = compile({128.f, .03125f, 20.f});
    require(max_axis.width == gaussian_filter_max_axis && max_axis.height == 1,
            "The inclusive Gaussian axis limit must remain supported");
    const auto max_cells = compile({32.f, 32.f, 10.f});
    require(std::uint64_t{max_cells.width} * max_cells.height == gaussian_filter_max_cells,
            "The inclusive Gaussian cell limit must remain supported");
    const auto distribution = compile({1.5f, 1.5f, .5f});
    const auto invalid =
        sample_gaussian_filter(distribution, std::numeric_limits<float>::quiet_NaN(), 0);
    require(std::isnan(invalid[0]) && std::isnan(invalid[1]),
            "Nonfinite uniforms must stay invalid");
}
#endif
} // namespace

int main() {
#if defined(RYPATH_PBRT_GAUSSIAN_ORACLE)
    return oracle();
#else
    try {
        pinned_oracle();
        normalized_model();
        independent_density();
        invalid_parameters();
        std::cout
            << "Gaussian pinned CDF/sample, normalized model, plateau and bounds checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
#endif
}
