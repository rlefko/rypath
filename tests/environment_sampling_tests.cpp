// SPDX-License-Identifier: GPL-3.0-only
#include "environment_sampling.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

ImagePyramid image(std::uint32_t width, const std::vector<float> &weights) {
    require(weights.size() == std::size_t(width) * width, "Invalid test image");
    ImagePyramid result{.encoding = ImageEncoding::linear,
                        .wrap = ImageWrap::clamp,
                        .levels = {{width, width, 0}},
                        .texels = {},
                        .storage = ImageStorage::linear_float,
                        .native_base = true};
    for (float weight : weights)
        result.texels.insert(result.texels.end(), {std::bit_cast<std::uint32_t>(weight),
                                                   std::bit_cast<std::uint32_t>(weight),
                                                   std::bit_cast<std::uint32_t>(weight), 0});
    return result;
}

EnvironmentSampling build(const ImagePyramid &source) {
    std::string diagnostic;
    auto result = build_environment_sampling(source, image_retained_ceiling, diagnostic);
    if (!result)
        throw std::runtime_error(diagnostic);
    return std::move(*result);
}

double probability(const EnvironmentSampling &table, unsigned x, unsigned y, bool compensated) {
    const auto offset = compensated ? table.compensated_offset : 0;
    const auto &cdf = table.cdf;
    const auto row = offset + (y + 1) * (table.width + 1);
    return (double(cdf[offset + y + 1]) - cdf[offset + y]) *
           (double(cdf[row + x + 1]) - cdf[row + x]);
}

void validate(const EnvironmentSampling &table) {
    const auto width = table.width;
    require(table.compensated_offset == (width + 1) * (width + 1), "Compensated table offset");
    require(table.cdf.size() == 2 * std::size_t(table.compensated_offset), "Total table count");
    for (bool compensated : {false, true}) {
        const auto offset = compensated ? table.compensated_offset : 0;
        for (unsigned row = 0; row <= width; ++row) {
            const auto base = offset + row * (width + 1);
            require(table.cdf[base] == 0 && table.cdf[base + width] == 1, "CDF endpoints");
            for (unsigned x = 0; x < width; ++x)
                require(std::isfinite(table.cdf[base + x]) &&
                            table.cdf[base + x] <= table.cdf[base + x + 1],
                        "CDF must be finite and monotonic");
        }
        double sum = 0;
        for (unsigned y = 0; y < width; ++y)
            for (unsigned x = 0; x < width; ++x)
                sum += probability(table, x, y, compensated);
        require(std::abs(sum - 1) < 1e-12, "Stored interval probabilities normalize");
    }
}

void reference_weights() {
    // Non-power-of-two native resolution and zero-mass rows/columns.
    const std::vector<float> weights{0, 1, 3, 0, 0, 0, 1, 2, 11};
    const auto table = build(image(3, weights));
    validate(table);
    constexpr double total = 18, average = total / 9;
    double compensated_total = 0;
    for (float weight : weights)
        compensated_total += std::max(double(weight) - average, 0.);
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 3; ++x) {
            const auto weight = weights[y * 3 + x];
            require(std::abs(probability(table, x, y, false) - weight / total) < 6e-8,
                    "Full RGB-average mass differs from independent reference");
            require(std::abs(probability(table, x, y, true) -
                             std::max(double(weight) - average, 0.) / compensated_total) < 6e-8,
                    "Compensated mass differs from independent reference");
        }
    require(build(image(3, weights)).cdf == table.cdf, "Build must be deterministic");
}

void fallbacks_and_support() {
    for (float value : {0.f, 3.f}) {
        const auto table = build(image(7, std::vector<float>(49, value)));
        validate(table);
        for (unsigned y = 0; y < 7; ++y)
            for (unsigned x = 0; x < 7; ++x)
                for (bool compensated : {false, true})
                    require(std::abs(probability(table, x, y, compensated) - 1. / 49) < 2e-8,
                            "Uniform full/constant compensated fallback");
    }
    auto singleton = build(image(1, {2.f}));
    validate(singleton);
    require(probability(singleton, 0, 0, true) == 1, "One-texel fallback");
    const auto extreme = build(image(3, {1e38f, 1e-38f, 0, 0, 1e-38f, 0, 0, 0, 0}));
    validate(extreme);
    const double erased = probability(extreme, 1, 0, false);
    require(erased == 0, "Extreme fixture must exercise quantized-away mass");
    const double full_mixture_mass = 1. / (16 * 9) + 15. / 16 * erased;
    require(full_mixture_mass > 0, "Uniform full proposal must retain erased texel support");
    require(probability(extreme, 1, 0, true) == 0, "Compensation intentionally omits dim mass");
    const auto clamped = build(image(1, {-1.f}));
    validate(clamped);
}

void resource_limits() {
    require(!environment_sampling_bytes(0) && !environment_sampling_bytes(8193) &&
                !environment_sampling_bytes(UINT32_MAX),
            "Invalid widths must fail before allocation");
    const auto large = environment_sampling_bytes(8192).value_or(0);
    require(large == std::size_t(8193) * 8193 * 8,
            "8192 profile must budget both native tables without a default-limit cap");
    require(large > ImageLimits{}.retained_bytes && large < image_retained_ceiling,
            "Large table must require the selected large-scene budget");
    std::string diagnostic;
    const auto source = image(3, std::vector<float>(9, 1.f));
    const auto bytes = environment_sampling_bytes(3).value_or(0);
    require(bytes != 0, "Small table resource estimate must exist");
    require(!build_environment_sampling(source, bytes - 1, diagnostic) && !diagnostic.empty(),
            "Insufficient budget must fail visibly");
    require(build_environment_sampling(source, bytes, diagnostic).has_value() && diagnostic.empty(),
            "Exact budget must succeed and clear old diagnostics");
    auto invalid = source;
    invalid.levels[0].height = 2;
    require(!build_environment_sampling(invalid, bytes, diagnostic), "Non-square map rejected");
    invalid = source;
    invalid.texels[0] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    require(!build_environment_sampling(invalid, bytes, diagnostic), "Nonfinite mass rejected");
    invalid = source;
    invalid.texels.clear();
    require(!build_environment_sampling(invalid, bytes, diagnostic), "Truncated image rejected");
}
} // namespace

int main() {
    try {
        reference_weights();
        fallbacks_and_support();
        resource_limits();
        std::cout << "Environment sampling: native/compensated CDFs, quantized support and budgets "
                     "passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
