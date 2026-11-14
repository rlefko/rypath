// SPDX-License-Identifier: GPL-3.0-only

#include "metal_execution.hpp"
#include "render.hpp"

#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace rypath;

constexpr double planck_h = 6.62606957e-34;
constexpr double light_speed = 299792458;
constexpr double boltzmann = 1.3806488e-23;
constexpr double wien_m_kelvin = 2.8977721e-3;

struct Query {
    float temperature;
    float wavelength;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

double absolute_planck(double temperature, double wavelength_m) {
    const double exponent = planck_h * light_speed / (wavelength_m * boltzmann * temperature);
    // Above 700 the normalized visible result is already below binary32 range.
    if (exponent > 700) {
        return 0;
    }
    return 2 * planck_h * light_speed * light_speed /
           (std::pow(wavelength_m, 5) * std::expm1(exponent));
}

double expected_reflectance(Query query) {
    if (query.wavelength < 360 || query.wavelength > 830) {
        return 0;
    }
    const double radiance = absolute_planck(query.temperature, query.wavelength * 1e-9);
    const double peak = absolute_planck(query.temperature, wien_m_kelvin / query.temperature);
    return std::clamp(radiance / peak, 0., 1.);
}

std::vector<Query> queries() {
    using Limits = std::numeric_limits<float>;
    const std::array wavelengths{std::nextafter(360.0f, 0.0f),
                                 360.0f,
                                 std::nextafter(360.0f, 830.0f),
                                 447.125f,
                                 512.0f,
                                 579.55442f,
                                 829.875f,
                                 std::nextafter(830.0f, 360.0f),
                                 830.0f,
                                 std::nextafter(830.0f, Limits::max())};
    // Peaks above, within, and below the visible interval, and both binary32 limits.
    constexpr std::array temperatures{
        Limits::denorm_min(), 0.25f, 3000.0f, 5000.0f, 15000.0f, 1e12f, Limits::max()};
    std::vector<Query> result;
    for (const float temperature : temperatures) {
        for (const float wavelength : wavelengths) {
            result.push_back({temperature, wavelength});
        }
    }
    // Sweep fractional wavelengths on both sides of the peak and into the cold tail.
    for (const float temperature : {175.0f, 3000.0f, 5000.0f, 15000.0f, 1e12f}) {
        for (int quarter_nm = 4 * 360; quarter_nm <= 4 * 830; ++quarter_nm) {
            result.push_back({temperature, 0.25f * static_cast<float>(quarter_nm)});
        }
    }

    constexpr float wavelength = 512;
    constexpr double exponent_nm_kelvin = planck_h * light_speed * 1e9 / boltzmann;
    const auto boundary_temperature = static_cast<float>(exponent_nm_kelvin / (wavelength / 16.0));
    const float colder = std::nextafter(std::nextafter(boundary_temperature, 0.0f), 0.0f);
    const float warmer = std::nextafter(boundary_temperature, Limits::max());
    constexpr float boundary = 1.0f / 16.0f;
    require(blackbody_exponent_factor(colder) / wavelength > boundary &&
                blackbody_exponent_factor(boundary_temperature) / wavelength == boundary &&
                blackbody_exponent_factor(warmer) / wavelength < boundary,
            "Blackbody probe must bracket the small-exponent branch");
    result.push_back({colder, wavelength});
    result.push_back({boundary_temperature, wavelength});
    result.push_back({warmer, wavelength});

    // x=100 tests a normal reflectance that loses intermediate exp(x_peak-x).
    // The transition around x=108 straddles FLT_MIN. Metal may flush subnormals;
    // the relative guarantee starts at 2*FLT_MIN to leave room for rounding.
    for (const double exponent : {100.0, 106.9, 107.0, 107.5, 108.0, 110.0, 127.99, 128.01}) {
        result.push_back(
            {static_cast<float>(exponent_nm_kelvin / (wavelength * exponent)), wavelength});
    }
    constexpr float fractional_wavelength = 512.375f;
    result.push_back({static_cast<float>(exponent_nm_kelvin / (fractional_wavelength * 100.0)),
                      fractional_wavelength});
    return result;
}

void check(std::span<const Query> cases, std::span<const float> results) {
    constexpr double absolute_limit = 2e-5;
    constexpr double relative_limit = 1e-4;
    constexpr double normal_gate = 2.0 * std::numeric_limits<float>::min();
    constexpr double subnormal_step = std::numeric_limits<float>::denorm_min();
    double maximum_absolute = 0;
    double maximum_relative = 0;
    std::size_t relative_count = 0;
    std::size_t exact_zero_count = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const float actual = results[i];
        const double expected = expected_reflectance(cases[i]);
        const double error = std::abs(actual - expected);
        const bool relative = expected >= normal_gate;
        const bool exact_zero = expected <= 0.25 * subnormal_step;
        // Allow flushing subnormals in the transition band and bound spurious light.
        const bool agrees =
            (!relative || (actual > 0 && error <= relative_limit * expected)) &&
            (!exact_zero || actual == 0) &&
            (relative || actual <= (1 + relative_limit) * expected + subnormal_step);
        if (!(std::isfinite(actual) && actual >= 0 && actual <= 1 && error <= absolute_limit &&
              agrees)) {
            std::ostringstream message;
            message << std::setprecision(10) << cases[i].temperature << " K at "
                    << cases[i].wavelength << " nm expected " << expected << ", got " << actual;
            throw std::runtime_error(message.str());
        }
        maximum_absolute = std::max(maximum_absolute, error);
        if (relative) {
            maximum_relative = std::max(maximum_relative, error / expected);
            ++relative_count;
        }
        exact_zero_count += exact_zero;
    }
    std::cout << "Blackbody reflectance " << cases.size() << " cases (" << relative_count
              << " relative checks, " << exact_zero_count
              << " exact zeros), maximum absolute error " << maximum_absolute
              << ", maximum relative error " << maximum_relative << '\n';
}

void run_probe(id<MTLDevice> device) {
    std::string diagnostic;
    const auto library = metal::load_library(device, {}, diagnostic);
    require(library != nil, diagnostic);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_probe_blackbody", diagnostic);
    require(pipeline != nil, diagnostic);

    const auto cases = queries();
    const auto count = static_cast<std::uint32_t>(cases.size());
    const NSUInteger group_width = metal::group_width(device, pipeline);
    require(group_width > 0, "Blackbody probe requires a positive threadgroup width");
    const NSUInteger group_count = (count + group_width - 1) / group_width;
    const NSUInteger slots = group_count * group_width + 1;
    constexpr float sentinel = -123.0f;
    // Pad input too so an incorrectly unguarded lane writes a detectable value.
    std::vector<simd_float2> data(slots, simd_float2{32.0f, 512.0f});
    for (std::size_t i = 0; i < cases.size(); ++i) {
        data[i] = simd_float2{blackbody_exponent_factor(cases[i].temperature), cases[i].wavelength};
    }
    const auto input = [device newBufferWithBytes:data.data()
                                           length:data.size() * sizeof(simd_float2)
                                          options:MTLResourceStorageModeShared];
    [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
        [device newBufferWithLength:slots * sizeof(float) options:MTLResourceStorageModeShared];
    auto *output_data = static_cast<float *>(output.contents);
    require(input != nil && output != nil && output_data != nullptr,
            "Blackbody probe buffer allocation failed");
    const std::span results{output_data, slots};
    std::ranges::fill(results, sentinel);

    const auto queue = [device newCommandQueue];
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(queue != nil && command != nil && encoder != nil, "Blackbody probe command failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(group_count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Blackbody reflectance probe");
    require(diagnostic.empty(), diagnostic);
    require(std::ranges::all_of(results.subspan(count), [](float v) { return v == sentinel; }),
            "Blackbody probe output guard mismatch");
    check(cases, results.first(count));
}

} // namespace

int main() {
    @autoreleasepool {
        try {
            const id<MTLDevice> device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP Blackbody GPU probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Blackbody GPU probe requires a Metal device");
            run_probe(device);
        } catch (const std::exception &error) {
            std::cerr << "Blackbody reflectance probe: " << error.what() << '\n';
            return 1;
        }
    }
}
