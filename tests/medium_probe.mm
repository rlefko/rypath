// SPDX-License-Identifier: GPL-3.0-only
#include "medium_probe_wire.h"
#include "medium_reference.hpp"
#include "metal_execution.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace {
using Q = rypath::medium_probe::Query;
using R = rypath::medium_probe::Result;
namespace ref = medium_reference;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
void close(double actual, double expected, double absolute, double relative, const char *label,
           std::size_t index) {
    if (!std::isfinite(actual) || !std::isfinite(expected) ||
        std::abs(actual - expected) > absolute + relative * std::abs(expected)) {
        std::ostringstream message;
        message << label << " case " << index << ": " << actual << " expected " << expected;
        throw std::runtime_error(message.str());
    }
}
Q base() { return {0.4f, 0.7f, 2, 1, 0.3f, 0, 0, 1, 1, 0, 0, 0.4f, 0.8f}; }
std::vector<R> dispatch(id<MTLDevice> device, id<MTLComputePipelineState> pipeline,
                        const std::vector<Q> &queries) {
    const auto count = static_cast<std::uint32_t>(queries.size());
    auto input = [device newBufferWithBytes:queries.data()
                                     length:queries.size() * sizeof(Q)
                                    options:MTLResourceStorageModeShared];
    [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
        [device newBufferWithLength:(queries.size() + 1) * sizeof(R)
                            options:MTLResourceStorageModeShared];
    require(input != nil && output != nil, "Medium probe allocation failed");
    auto *data = static_cast<R *>(output.contents);
    data[queries.size()].valid = 0x12345678;
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    require(queue != nil && command != nil && encoder != nil, "Medium probe command allocation");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(rypath::metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    const auto diagnostic = rypath::metal::complete(command, "Medium probe");
    require(diagnostic.empty(), diagnostic);
    require(data[queries.size()].valid == 0x12345678, "Medium probe output guard");
    return {data, data + queries.size()};
}
void segment_checks(id<MTLDevice> device, id<MTLComputePipelineState> pipeline) {
    std::vector<Q> queries;
    for (float a : {0.f, 1e-30f, 1e-8f, 0.01f, 1.f, 100.f, 1e30f})
        for (float s : {0.f, 1e-20f, 0.5f, 1e20f})
            for (float le : {0.f, 1e-20f, 1.f, 1e20f})
                for (float d : {0.f, 1e-30f, 1e-8f, 0.125f, 1.f, 100.f,
                                std::numeric_limits<float>::infinity()}) {
                    auto q = base();
                    q.sigma_a = a;
                    q.sigma_s = s;
                    q.Le = le;
                    q.distance = d;
                    queries.push_back(q);
                }
    auto results = dispatch(device, pipeline, queries);
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto q = queries[i];
        const auto r = results[i];
        const long double t = static_cast<long double>(q.sigma_a) + q.sigma_s;
        const auto tr = t == 0 || q.distance == 0 ? 1.L : std::exp(-t * q.distance);
        const auto e = ref::emission(q.sigma_a, q.sigma_s, q.Le, q.distance);
        if (e > std::numeric_limits<float>::max()) {
            require(!r.valid, "Emission overflow must fail visibly");
            continue;
        }
        require(r.valid, "Valid homogeneous segment rejected");
        close(r.transmission, double(tr), 2e-7, 2e-5, "transmission", i);
        close(r.emission, double(e), 2e-37, 3e-5, "segment emission", i);
        const double expected = t == 0 ? INFINITY : -std::log1p(-double(q.u)) / double(t);
        if (std::isinf(expected))
            require(std::isinf(r.distance), "Vacuum free flight must be infinite");
        else
            close(r.distance, expected, 2e-37, 3e-6, "free flight", i);
    }
    std::cout << "Medium " << queries.size() << " independent Beer/emission/flight cases\n";
}
void phase_checks(id<MTLDevice> device, id<MTLComputePipelineState> pipeline) {
    std::mt19937 random(0x659122);
    auto uniform = [&]() { return std::generate_canonical<float, 24>(random); };
    std::vector<Q> queries;
    for (float g : {-2.f, -0.99f, -0.8f, -0.1f, -1e-6f, 0.f, 1e-6f, 0.1f, 0.8f, 0.99f, 2.f})
        for (int i = 0; i < 4096; ++i) {
            auto q = base();
            q.g = g;
            q.u = i == 0 ? 0.f : i == 1 ? 1.f : i == 2 ? std::nextafter(1.f, 0.f) : uniform();
            q.v = uniform();
            const double z = 2 * double(uniform()) - 1;
            const double phi = 2 * ref::pi * uniform();
            q.ix = float(std::sqrt(1 - z * z) * std::cos(phi));
            q.iy = float(std::sqrt(1 - z * z) * std::sin(phi));
            q.iz = float(z);
            queries.push_back(q);
        }
    const auto results = dispatch(device, pipeline, queries);
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto q = queries[i];
        const auto r = results[i];
        require(std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z),
                "Invalid phase sample");
        close(r.phase, double(ref::phase(q.iz, q.g)), 1e-7, 3e-4, "HG evaluation", i);
        close(r.x * double(r.x) + r.y * double(r.y) + r.z * double(r.z), 1, 4e-7, 0,
              "HG sampled norm", i);
        close(double(ref::cdf(r.z, q.g)), q.u, 0.0015, 0, "Independent HG CDF", i);
        close(r.sample_pdf, double(ref::phase(r.z, q.g)), 1e-7, 3e-4, "HG sampled PDF", i);
    }
    // Deterministic probability quadrature of returned samples verifies mean
    // cosine without reusing an inverse-CDF implementation as the oracle.
    constexpr int n = 65536;
    for (float g : {-0.99f, -0.8f, -0.1f, -1e-6f, 0.f, 1e-6f, 0.1f, 0.8f, 0.99f}) {
        queries.clear();
        for (int i = 0; i < n; ++i) {
            auto q = base();
            q.g = g;
            q.u = float((i + 0.5) / n);
            q.v = float(std::fmod(i * 0.6180339887498949, 1.0));
            queries.push_back(q);
        }
        const auto samples = dispatch(device, pipeline, queries);
        double x = 0, y = 0, z = 0, flight_cdf_error = 0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto r = samples[i];
            x += r.x;
            y += r.y;
            z += r.z;
            const double cdf =
                -std::expm1(-double(queries[i].sigma_a + queries[i].sigma_s) * r.distance);
            flight_cdf_error = std::max(flight_cdf_error, std::abs(cdf - queries[i].u));
        }
        close(z / n, -g, 3e-5, 0, "HG mean outward cosine", 0);
        close(x / n, 0, 5e-5, 0, "HG mean azimuth x", 0);
        close(y / n, 0, 5e-5, 0, "HG mean azimuth y", 0);
        require(flight_cdf_error < 5e-7, "Free-flight inverse distribution");
    }
    // Independent sphere integration, with explicit pole samples and Simpson weights.
    constexpr int intervals = 262144;
    for (float g : {-0.99f, 0.f, 0.99f}) {
        queries.clear();
        for (int i = 0; i <= intervals; ++i) {
            auto q = base();
            q.g = g;
            q.iz = float(-1 + 2.0 * i / intervals);
            q.ix = std::sqrt(std::max(0.f, (1 - q.iz) * (1 + q.iz)));
            queries.push_back(q);
        }
        const auto values = dispatch(device, pipeline, queries);
        double integral = 0;
        for (int i = 0; i <= intervals; ++i)
            integral += (i == 0 || i == intervals ? 1 : i % 2 ? 4 : 2) * double(values[i].phase);
        integral *= 4 * ref::pi / (3 * intervals);
        close(integral, 1, 2e-4, 0, "HG independent sphere normalization", 0);
    }
    // Arbitrary wo and reciprocity, beyond the +z axis fixtures above.
    queries.clear();
    for (int i = 0; i < 4096; ++i) {
        auto q = base();
        q.ox = 0.36f;
        q.oy = 0.48f;
        q.oz = 0.8f;
        q.g = 1.98f * uniform() - 0.99f;
        q.u = uniform();
        q.v = uniform();
        queries.push_back(q);
        std::swap(q.ox, q.ix);
        std::swap(q.oy, q.iy);
        std::swap(q.oz, q.iz);
        queries.push_back(q);
    }
    const auto reciprocal = dispatch(device, pipeline, queries);
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto q = queries[i];
        const auto r = reciprocal[i];
        close(r.phase, reciprocal[i ^ 1].phase, 0, 0, "HG reciprocity", i);
        const double c = double(q.ox) * r.x + double(q.oy) * r.y + double(q.oz) * r.z;
        close(double(ref::cdf(std::clamp(c, -1.0, 1.0), q.g)), q.u, 0.002, 0, "HG rotated CDF", i);
    }
    std::cout << "Medium HG evaluations, inverse-CDF distributions, moments, normalization and "
                 "reciprocity passed\n";
}
void limits(id<MTLDevice> device, id<MTLComputePipelineState> pipeline) {
    std::vector<Q> queries;
    for (float bad :
         {-1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        for (int field = 0; field < 3; ++field) {
            auto q = base();
            if (field == 0)
                q.sigma_a = bad;
            if (field == 1)
                q.sigma_s = bad;
            if (field == 2)
                q.Le = bad;
            queries.push_back(q);
        }
    }
    for (const auto r : dispatch(device, pipeline, queries))
        require(!r.valid, "Invalid medium coefficient accepted");
    queries.clear();
    for (float bad : {-1.f, std::numeric_limits<float>::quiet_NaN()}) {
        auto q = base();
        q.distance = bad;
        queries.push_back(q);
    }
    for (const auto r : dispatch(device, pipeline, queries))
        require(!r.valid, "Invalid medium distance accepted");
    queries.clear();
    for (float u : {-0.01f, 1.01f, std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::quiet_NaN()}) {
        auto q = base();
        q.u = u;
        queries.push_back(q);
    }
    for (const auto r : dispatch(device, pipeline, queries))
        require(std::isnan(r.distance) && std::isnan(r.x), "Invalid variate accepted");
    queries.clear();
    for (float u :
         {0.f, 1e-30f, 1e-8f, 0.125f, std::nextafter(0.125f, 1.f), std::nextafter(1.f, 0.f), 1.f}) {
        auto q = base();
        q.u = u;
        queries.push_back(q);
    }
    const auto values = dispatch(device, pipeline, queries);
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto q = queries[i];
        const double expected = -std::log1p(-double(q.u)) / double(q.sigma_a + q.sigma_s);
        if (std::isinf(expected))
            require(std::isinf(values[i].distance), "Flight endpoint one");
        else
            close(values[i].distance, expected, 2e-37, 3e-6, "Flight endpoint", i);
    }
    auto q = base();
    q.sigma_a = q.sigma_s = std::numeric_limits<float>::max();
    require(!dispatch(device, pipeline, {q})[0].valid, "Extinction overflow accepted");
    q = base();
    q.Le = std::numeric_limits<float>::max();
    q.sigma_s = 0;
    q.distance = INFINITY;
    const auto saturated = dispatch(device, pipeline, {q})[0];
    require(saturated.valid && saturated.emission == q.Le, "Finite source saturation");
    std::cout << "Medium invalid inputs and endpoint limits passed\n";
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            require(argc <= 2, "Expected [METALLIB]");
            auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP Medium GPU probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Medium GPU probe requires a Metal device");
            std::string diagnostic;
            auto library =
                rypath::metal::load_library(device, argc == 2 ? argv[1] : "", diagnostic);
            require(library != nil, diagnostic);
            auto pipeline =
                rypath::metal::load_pipeline(device, library, @"rypath_medium_probe", diagnostic);
            require(pipeline != nil, diagnostic);
            segment_checks(device, pipeline);
            phase_checks(device, pipeline);
            limits(device, pipeline);
        } catch (const std::exception &error) {
            std::cerr << "Medium probe: " << error.what() << '\n';
            return 1;
        }
    }
}
