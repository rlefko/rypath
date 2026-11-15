// SPDX-License-Identifier: GPL-3.0-only
#include "dielectric_probe_wire.h"
#include "dielectric_reference.hpp"
#include "metal_execution.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace {
namespace ref = dielectric_reference;
using namespace rypath;
using Q = dielectric_probe::Query;
using R = dielectric_probe::Result;
void require(bool b, const std::string &s) {
    if (!b)
        throw std::runtime_error(s);
}
void close(double a, double b, double absolute, double relative, const char *label, std::size_t n) {
    if (!std::isfinite(a) || !std::isfinite(b) ||
        std::abs(a - b) > absolute + relative * std::abs(b)) {
        std::ostringstream s;
        s << label << " case " << n << ": " << a << " expected " << b;
        throw std::runtime_error(s.str());
    }
}
Q query(float eta, float ax, float ay, ref::V o, ref::V i, float uc, float u, float v,
        bool remap = false) {
    return {eta,        ax,         ay, float(o.x), float(o.y), float(o.z),     float(i.x),
            float(i.y), float(i.z), uc, u,          v,          unsigned(remap)};
}
std::vector<R> dispatch(id<MTLDevice> device, id<MTLComputePipelineState> pipeline,
                        const std::vector<Q> &q) {
    const auto count = static_cast<std::uint32_t>(q.size());
    auto input = [device newBufferWithBytes:q.data()
                                     length:q.size() * sizeof(Q)
                                    options:MTLResourceStorageModeShared];
    [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
        [device newBufferWithLength:(q.size() + 1) * sizeof(R)
                            options:MTLResourceStorageModeShared];
    require(input != nil && output != nil, "Dielectric allocation failed");
    auto *data = static_cast<R *>(output.contents);
    data[q.size()].prepared = 0x12345678;
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    require(queue != nil && command != nil && encoder != nil,
            "Dielectric command allocation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    auto width = metal::group_width(device, pipeline);
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
    [encoder endEncoding];
    require(metal::complete(command, "Dielectric probe").empty(),
            "Dielectric GPU execution failed");
    require(data[q.size()].prepared == 0x12345678, "Dielectric guard overwritten");
    return {data, data + q.size()};
}
void pinned_checks(const std::string &executable, const std::vector<Q> &queries,
                   const std::vector<R> &results) {
    if (executable.empty())
        return;
    char pattern[] = "/tmp/rypath-dielectric-pbrt-XXXXXX";
    auto directory = mkdtemp(pattern);
    require(directory != nullptr, "PBRT oracle temporary directory");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    auto input = cleanup.path / "input", output = cleanup.path / "output";
    {
        std::ofstream stream(input);
        stream << std::setprecision(9);
        // Ordinary random cases only; special grazing/optical-limit adaptations are
        // independently checked against double equations, not upstream float cancellation.
        for (std::size_t n = 0; n < 4096; ++n) {
            auto q = queries[n];
            stream << q.eta << ' ' << q.u_roughness << ' ' << q.v_roughness << ' ' << q.ox << ' '
                   << q.oy << ' ' << q.oz << ' ' << q.ix << ' ' << q.iy << ' ' << q.iz << ' '
                   << q.uc << ' ' << q.u << ' ' << q.v << ' ' << q.remap << '\n';
        }
        require(bool(stream), "PBRT oracle input write");
    }
    std::ofstream(output).close();
    auto task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:executable.c_str()]];
    auto in =
        [NSFileHandle fileHandleForReadingAtPath:[NSString stringWithUTF8String:input.c_str()]];
    auto out =
        [NSFileHandle fileHandleForWritingAtPath:[NSString stringWithUTF8String:output.c_str()]];
    task.standardInput = in;
    task.standardOutput = out;
    NSError *error = nil;
    require([task launchAndReturnError:&error], "PBRT oracle launch failed");
    [task waitUntilExit];
    [in closeFile];
    [out closeFile];
    require(task.terminationStatus == 0, "PBRT oracle exit failed");
    std::ifstream stream(output);
    for (std::size_t n = 0; n < 4096; ++n) {
        double f, pdf, F, x, y, z, weight, sample_pdf;
        unsigned accepted, delta;
        require(bool(stream >> f >> pdf >> F >> x >> y >> z >> weight >> sample_pdf >> accepted >>
                     delta),
                "PBRT oracle row parse");
        auto r = results[n];
        close(r.f, f, 2e-6, 3e-4, "pinned f", n);
        close(r.pdf, pdf, 2e-6, 3e-4, "pinned pdf", n);
        close(r.fresnel, F, 2e-6, 2e-5, "pinned Fresnel", n);
        require(r.accepted == accepted && r.delta == delta, "Pinned sample flags");
        if (accepted) {
            close(r.ix, x, 3e-5, 0, "pinned sample x", n);
            close(r.iy, y, 3e-5, 0, "pinned sample y", n);
            close(r.iz, z, 3e-5, 0, "pinned sample z", n);
            close(r.weight, weight, 2e-6, 3e-4, "pinned weight", n);
            close(r.sample_pdf, sample_pdf, 2e-6, 3e-4, "pinned sample pdf", n);
        }
    }
    std::string extra;
    require(!(stream >> extra), "Extra PBRT oracle output");
    std::cout << "Dielectric 4096 pinned PBRT binary32 comparisons\n";
}
void point_checks(id<MTLDevice> d, id<MTLComputePipelineState> p, const std::string &oracle) {
    std::vector<Q> cases;
    std::mt19937 random(812724);
    auto uniform = [&]() { return std::generate_canonical<float, 24>(random); };
    auto direction = [&]() {
        double z = .02 + .98 * uniform(), phi = 2 * ref::pi * uniform();
        return ref::V{std::sqrt(1 - z * z) * std::cos(phi), std::sqrt(1 - z * z) * std::sin(phi),
                      z};
    };
    for (int n = 0; n < 4096; ++n) {
        auto o = direction(), i = direction();
        if (n % 2)
            o = -o;
        if (n % 3)
            i = -i;
        // Keep ordinary source-float comparison away from the singular eta=1 limit.
        float eta = n % 2 ? .4f + .4f * uniform() : 1.2f + 1.8f * uniform();
        cases.push_back(query(eta, .08f + 1.5f * uniform(), .08f + 1.5f * uniform(), o, i,
                              uniform(), uniform(), uniform(), n % 2));
    }
    for (float eta : {0.f, .5f, 1.f, 1.5f, 2.f})
        for (float ax : {0.f, .001f, .3f, 2.f})
            for (float ay : {0.f, .7f})
                for (float z : {.0001f, .01f, .5f, 1.f})
                    for (float sign : {-1.f, 1.f})
                        for (float u : {0.f, .5f, std::nextafter(1.f, 0.f)})
                            cases.push_back(query(eta, ax, ay,
                                                  {std::sqrt(1 - double(z) * z), 0, sign * z},
                                                  {0, 0, -sign}, .5f, u, .375f));
    // Critical angles and near-unity indices: smooth direction/Fresnel limits.
    for (float eta : {.5f, std::nextafter(1.f, 0.f), std::nextafter(1.f, 2.f), 1.5f}) {
        double e = eta < 1 ? eta : 1 / double(eta);
        float critical = float(std::sqrt(1 - e * e)), sign = eta < 1 ? 1.f : -1.f;
        for (float z :
             {std::nextafter(critical, 0.f), critical, std::nextafter(critical, 1.f), .0001f, 1.f})
            for (float uc : {0.f, .5f, std::nextafter(1.f, 0.f)})
                cases.push_back(query(eta, 0, 0, {std::sqrt(1 - double(z) * z), 0, sign * z},
                                      {0, 0, 1}, uc, .5f, .5f));
    }
    for (float eta : {std::nextafter(1.f, 0.f), std::nextafter(1.f, 2.f)})
        for (float ax : {.1f, 1.f})
            for (float sign : {-1.f, 1.f})
                for (float uc : {0.f, .5f, std::nextafter(1.f, 0.f)})
                    cases.push_back(
                        query(eta, ax, .7f, {.6f, 0, sign * .8f}, {0, 0, -sign}, uc, .25f, .625f));
    for (float eta : {std::nextafter(1.f, 0.f), std::nextafter(1.f, 2.f)})
        for (float sign : {-1.f, 1.f}) {
            ref::V o{.6f, 0, sign * .8f};
            // Offset one tangent component by a binary32 step; the independent
            // oracle evaluates these exact authored directions, not sample output.
            ref::V i{-std::nextafter(.6f, 1.f), 0, -sign * .8f};
            cases.push_back(query(eta, .3f, .7f, o, i, .5f, .25f, .625f));
        }
    auto results = dispatch(d, p, cases);
    pinned_checks(oracle, cases, results);
    for (std::size_t n = 0; n < cases.size(); ++n) {
        auto q = cases[n];
        auto r = results[n];
        auto par = ref::prepare(q.eta, q.u_roughness, q.v_roughness, q.remap);
        require(r.prepared && bool(r.smooth) == par.smooth,
                "Preparation case " + std::to_string(n));
        par.ax = r.alpha_x;
        par.ay = r.alpha_y;
        ref::V o{q.ox, q.oy, q.oz}, i{q.ix, q.iy, q.iz};
        auto e = ref::evaluate(par, o, i);
        auto s = ref::sample(par, o, q.uc, q.u, q.v);
        require(r.evaluation_valid && r.fresnel_valid && r.valid,
                "Numeric validity case " + std::to_string(n));
        close(r.fresnel, ref::fresnel(o.z, par.eta), 2e-6, 2e-5, "Fresnel", n);
        close(r.f, e.f, 2e-6, 3e-4, "f", n);
        close(r.pdf, e.pdf, 2e-6, 3e-4, "pdf", n);
        require(bool(r.accepted) == s.accepted && bool(r.delta) == par.smooth,
                "Sample flags case " + std::to_string(n));
        close(r.ix, s.i.x, 3e-5, 0, "sample x", n);
        close(r.iy, s.i.y, 3e-5, 0, "sample y", n);
        close(r.iz, s.i.z, 3e-5, 0, "sample z", n);
        if (s.accepted) {
            close(r.weight, s.weight, 2e-6, 3e-4, "weight", n);
            close(r.sample_pdf, s.pdf, 2e-6, 3e-4, "sample pdf", n);
        }
    }
    std::vector<Q> paired, consistency;
    std::vector<std::size_t> indices;
    for (std::size_t n = 0; n < 128; ++n) {
        auto q = cases[n];
        std::swap(q.ox, q.ix);
        std::swap(q.oy, q.iy);
        std::swap(q.oz, q.iz);
        paired.push_back(q);
        q = cases[n];
        std::swap(q.u_roughness, q.v_roughness);
        std::swap(q.ox, q.oy);
        std::swap(q.ix, q.iy);
        paired.push_back(q);
        if (results[n].accepted) {
            q = cases[n];
            q.ix = results[n].ix;
            q.iy = results[n].iy;
            q.iz = results[n].iz;
            consistency.push_back(q);
            indices.push_back(n);
        }
    }
    auto pairs = dispatch(d, p, paired), evaluated = dispatch(d, p, consistency);
    for (std::size_t n = 0; n < 128; ++n) {
        auto q = cases[n];
        bool transmission = (q.oz > 0) != (q.iz > 0);
        double ep = q.oz > 0 ? q.eta : 1 / double(q.eta);
        // Radiance adjoint reciprocity: f(o,i)*ep² == f(i,o).
        close(pairs[2 * n].f, results[n].f * (transmission ? ep * ep : 1), 2e-6, 3e-4,
              "adjoint reciprocity", n);
        close(pairs[2 * n + 1].f, results[n].f, 2e-6, 3e-4, "axis swap", n);
    }
    for (std::size_t n = 0; n < evaluated.size(); ++n) {
        auto r = results[indices[n]];
        close(evaluated[n].pdf, r.sample_pdf, 2e-6, 3e-4, "sample/evaluate pdf", n);
        close(evaluated[n].f * std::abs(r.iz) / evaluated[n].pdf, r.weight, 2e-6, 3e-4,
              "sample/evaluate weight", n);
    }
    std::cout << "Dielectric " << cases.size()
              << " independent point/sample cases; 128 adjoint reciprocity/axis pairs\n";
}
void energy_checks(id<MTLDevice> d, id<MTLComputePipelineState> p) {
    constexpr unsigned count = 131072;
    std::mt19937 random(170424);
    for (auto axes : std::array<std::array<float, 2>, 3>{{{.3f, .3f}, {.2f, .7f}, {1.f, 1.f}}}) {
        ref::V o = ref::unit({.6, 0, 1});
        auto par = ref::prepare(1.5f, axes[0], axes[1], false);
        auto coarse = ref::integrate(par, o, 2048), fine = ref::integrate(par, o, 4096);
        require(std::abs(coarse.f - fine.f) < 2e-5 && std::abs(coarse.pdf - fine.pdf) < 2e-5,
                "Quadrature refinement");
        require(fine.f <= 1 + 2e-4, "Radiance energy bound");
        std::vector<Q> queries;
        queries.reserve(count);
        for (unsigned n = 0; n < count; ++n)
            queries.push_back(query(1.5f, axes[0], axes[1], o, {0, 0, 1},
                                    std::generate_canonical<float, 24>(random),
                                    std::generate_canonical<float, 24>(random),
                                    std::generate_canonical<float, 24>(random)));
        auto results = dispatch(d, p, queries);
        double sum = 0, square = 0, accepted = 0;
        for (auto r : results) {
            require(r.valid, "Energy invalid arithmetic");
            double w = r.accepted ? r.weight : 0;
            sum += w;
            square += w * w;
            accepted += r.accepted;
        }
        double mean = sum / count, variance = (square - count * mean * mean) / (count - 1),
               se = std::sqrt(variance / count), mass = accepted / count;
        require(std::abs(mean - fine.f) <= 5 * se + 2e-4, "Energy quadrature agreement");
        require(std::abs(mass - fine.pdf) <=
                    5 * std::sqrt(fine.pdf * (1 - fine.pdf) / count) + 2e-4,
                "Accepted/null mass agreement");
        std::cout << "Dielectric alpha " << axes[0] << ',' << axes[1] << " energy " << mean
                  << " quadrature " << fine.f << " SE " << se << " null " << 1 - mass
                  << " reference " << 1 - fine.pdf << '\n';
    }
}
void invalid_checks(id<MTLDevice> d, id<MTLComputePipelineState> p) {
    auto base = query(1.5f, .3f, .4f, {0, 0, 1}, {0, 0, -1}, .5f, .5f, .5f);
    std::vector<Q> cases;
    for (float v :
         {-1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto q = base;
        q.eta = v;
        cases.push_back(q);
        q = base;
        q.u_roughness = v;
        cases.push_back(q);
        q = base;
        q.v_roughness = v;
        cases.push_back(q);
    }
    for (auto r : dispatch(d, p, cases))
        require(!r.prepared, "Invalid parameter accepted");
    cases.clear();
    auto q = base;
    q.uc = 1;
    cases.push_back(q);
    q = base;
    q.u = -.1f;
    cases.push_back(q);
    q = base;
    q.v = 1;
    cases.push_back(q);
    q = base;
    q.oz = NAN;
    cases.push_back(q);
    for (auto r : dispatch(d, p, cases))
        require(r.prepared && !r.valid, "Invalid sample accepted");
    q = base;
    q.oz = 0;
    q.ox = 1;
    for (auto r : dispatch(d, p, {q}))
        require(r.valid && !r.accepted, "Tangent null");
    q = base;
    q.eta = std::numeric_limits<float>::max();
    for (auto r : dispatch(d, p, {q}))
        require(r.prepared && !r.valid && !r.fresnel_valid, "Overflow must fail visibly");
    q = base;
    q.u_roughness = q.v_roughness = std::numeric_limits<float>::max();
    q.u = 0;
    for (auto r : dispatch(d, p, {q}))
        require(r.prepared && !r.valid, "Unrepresentable GGX density must fail visibly");
    q = query(1.5f, 1, 1, {1, 0, 1e-5}, {0, 0, 1}, 0, .9999f, .625f);
    for (auto r : dispatch(d, p, {q}))
        require(r.valid && !r.accepted && r.sample_pdf == 0 && r.weight == 0,
                "Pinned grazing distribution cutoff null");
    for (float rough : {std::nextafter(1e-6f, 0.f), 1e-6f, std::nextafter(1e-6f, 1.f)}) {
        q = base;
        q.u_roughness = q.v_roughness = rough;
        q.remap = 1;
        for (auto r : dispatch(d, p, {q}))
            require(r.prepared && bool(r.smooth) == (std::sqrt(rough) < 1e-3f),
                    "Remapped smooth threshold");
    }
    for (float eta : {0.f, 1.f})
        for (float rough : {0.f, 2.f}) {
            q = base;
            q.eta = eta;
            q.u_roughness = q.v_roughness = rough;
            for (auto r : dispatch(d, p, {q}))
                require(r.valid && r.accepted && r.delta && r.weight == 1 && r.sample_pdf == 1 &&
                            r.iz == -1,
                        "Matched index delta identity");
        }
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP Dielectric GPU probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Dielectric probe requires Metal");
            std::string diagnostic;
            std::filesystem::path library_path;
            std::string oracle;
            bool extended = false;
            for (int argument = 1; argument < argc; ++argument) {
                std::string value = argv[argument];
                if (value == "--extended")
                    extended = true;
                else if (value == "--pbrt-probe") {
                    require(argument + 1 < argc, "--pbrt-probe requires an executable");
                    oracle = argv[++argument];
                } else {
                    require(library_path.empty(),
                            "Expected [METALLIB] [--extended] [--pbrt-probe EXECUTABLE]");
                    library_path = value;
                }
            }
            auto library = metal::load_library(device, library_path, diagnostic);
            require(library != nil, diagnostic);
            auto pipeline =
                metal::load_pipeline(device, library, @"rypath_dielectric_probe", diagnostic);
            require(pipeline != nil, diagnostic);
            point_checks(device, pipeline, oracle);
            invalid_checks(device, pipeline);
            if (extended)
                energy_checks(device, pipeline);
        } catch (const std::exception &e) {
            std::cerr << "Dielectric probe: " << e.what() << '\n';
            return 1;
        }
    }
}
