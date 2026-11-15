// SPDX-License-Identifier: GPL-3.0-only
#include "conductor_probe_wire.h"
#include "conductor_reference.hpp"
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
namespace ref = conductor_reference;
using namespace rypath;
using Q = conductor_probe::Query;
using R = conductor_probe::Result;
void require(bool b, const std::string &s) {
    if (!b)
        throw std::runtime_error(s);
}
void close(double a, double b, double absolute, double relative, const char *label, std::size_t n) {
    if (!std::isfinite(a) || std::abs(a - b) > absolute + relative * std::abs(b)) {
        std::ostringstream s;
        s << label << " case " << n << ": " << a << " expected " << b;
        throw std::runtime_error(s.str());
    }
}
Q query(float eta, float k, float ax, float ay, ref::V o, ref::V i, float u, float v,
        bool remap = false) {
    return {eta,        k,          ax,         ay, float(o.x), float(o.y),     float(o.z),
            float(i.x), float(i.y), float(i.z), u,  v,          unsigned(remap)};
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
    require(input != nil && output != nil, "Conductor allocation failed");
    auto *data = static_cast<R *>(output.contents);
    data[q.size()].prepared = 0x12345678;
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    require(queue != nil && command != nil && encoder != nil,
            "Conductor command allocation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    auto width = metal::group_width(device, pipeline);
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
    [encoder endEncoding];
    require(metal::complete(command, "Conductor probe").empty(), "Conductor GPU execution failed");
    require(data[q.size()].prepared == 0x12345678, "Conductor guard overwritten");
    return {data, data + q.size()};
}
void pinned_checks(const std::string &executable, const std::vector<Q> &queries,
                   const std::vector<R> &results) {
    if (executable.empty())
        return;
    char pattern[] = "/tmp/rypath-conductor-pbrt-XXXXXX";
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
            stream << q.eta << ' ' << q.k << ' ' << q.u_roughness << ' ' << q.v_roughness << ' '
                   << q.ox << ' ' << q.oy << ' ' << q.oz << ' ' << q.ix << ' ' << q.iy << ' '
                   << q.iz << ' ' << q.u << ' ' << q.v << ' ' << q.remap << '\n';
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
    std::cout << "Conductor 4096 pinned PBRT binary32 comparisons\n";
}
void point_checks(id<MTLDevice> d, id<MTLComputePipelineState> p, const std::string &oracle) {
    std::vector<Q> cases;
    std::mt19937 random(812724);
    auto uniform = [&]() { return std::generate_canonical<float, 24>(random); };
    for (int n = 0; n < 4096; ++n) {
        double z = .01 + .99 * uniform(), phi = 2 * ref::pi * uniform();
        ref::V o{std::sqrt(1 - z * z) * std::cos(phi), std::sqrt(1 - z * z) * std::sin(phi), z};
        z = .01 + .99 * uniform();
        phi = 2 * ref::pi * uniform();
        ref::V i{std::sqrt(1 - z * z) * std::cos(phi), std::sqrt(1 - z * z) * std::sin(phi), z};
        if (n % 3 == 0) {
            o = -o;
            i = -i;
        }
        cases.push_back(query(.05f + 4 * uniform(), .01f + 6 * uniform(), .02f + 2 * uniform(),
                              .02f + 2 * uniform(), o, i, uniform(), uniform(), n % 2));
    }
    for (float ax : {0.f, .000999f, .001f, .3f, 2.f})
        for (float ay : {0.f, .000999f, .001f, .7f})
            for (float z : {.0001f, .01f, .5f, 1.f})
                for (float u : {0.f, .5f, std::nextafter(1.f, 0.f)})
                    cases.push_back(query(.27f, 3.2f, ax, ay, {std::sqrt(1 - double(z) * z), 0, z},
                                          {0, 0, 1}, u, .375f));
    for (auto nk : std::array<std::array<float, 2>, 8>{{{0.f, 3.f},
                                                        {1.f, 0.f},
                                                        {.5f, 0.f},
                                                        {1.5f, 0.f},
                                                        {1e-10f, 1e-10f},
                                                        {1e10f, 1e10f},
                                                        {1.f, 1e-4f},
                                                        {.001f, 10.f}}})
        for (float z : {.0001f, .01f, .5f, 1.f})
            cases.push_back(
                query(nk[0], nk[1], 0, 0, {std::sqrt(1 - double(z) * z), 0, z}, {0, 0, 1}, 0, 0));
    for (float eta : {std::nextafter(1.f, 0.f), std::nextafter(1.f, 2.f), .5f})
        for (float k : {0.f, 1e-8f, 1e-4f})
            for (float z : {.0001f, .001f, .01f, float(std::sqrt(.75)),
                            std::nextafter(float(std::sqrt(.75)), 0.f)})
                cases.push_back(
                    query(eta, k, 0, 0, {std::sqrt(1 - double(z) * z), 0, z}, {0, 0, 1}, 0, 0));
    // Cross optical scale and angle: small index at normal incidence and
    // near-unit index at grazing require retaining different square residuals.
    for (float eta : {0.f, 1e-8f, 1e-6f, 1e-4f, .01f, .1f, .5f, .9f, std::nextafter(1.f, 0.f), 1.f,
                      std::nextafter(1.f, 2.f), 1.1f, 2.f, 10.f})
        for (float k : {0.f, 1e-8f, 1e-6f, 1e-4f, .01f, .1f, 1.f, 10.f}) {
            if (eta == 0 && k == 0)
                continue;
            for (float z : {1e-6f, 1e-4f, .01f, .1f, .5f, .9f, std::nextafter(1.f, 0.f), 1.f})
                cases.push_back(
                    query(eta, k, 0, 0, {std::sqrt(1 - double(z) * z), 0, z}, {0, 0, 1}, 0, 0));
            if (eta > 0 && eta < 1) {
                float critical = float(std::sqrt(1 - double(eta) * eta));
                for (float z :
                     {std::nextafter(critical, 0.f), critical, std::nextafter(critical, 1.f)})
                    cases.push_back(
                        query(eta, k, 0, 0, {std::sqrt(1 - double(z) * z), 0, z}, {0, 0, 1}, 0, 0));
            }
        }
    auto results = dispatch(d, p, cases);
    pinned_checks(oracle, cases, results);
    for (std::size_t n = 0; n < cases.size(); ++n) {
        auto q = cases[n];
        auto r = results[n];
        auto par = ref::prepare(q.eta, q.k, q.u_roughness, q.v_roughness, q.remap);
        // Compare the double oracle at the exact device-prepared alpha values.
        require(r.prepared && r.smooth == par.smooth, "Conductor preparation flag");
        close(r.alpha_x, par.ax, 1e-7, 1e-6, "alpha x", n);
        close(r.alpha_y, par.ay, 1e-7, 1e-6, "alpha y", n);
        par.ax = r.alpha_x;
        par.ay = r.alpha_y;
        ref::V o{q.ox, q.oy, q.oz}, i{q.ix, q.iy, q.iz};
        auto e = ref::evaluate(par, o, i);
        auto s = ref::sample(par, o, q.u, q.v);
        require(r.evaluation_valid && r.fresnel_valid && r.valid,
                "Conductor numeric validity case " + std::to_string(n));
        close(r.fresnel, ref::fresnel(std::abs(o.z), q.eta, q.k), 2e-6, 2e-5, "Fresnel", n);
        close(r.f, e.f, 2e-6, 3e-4, "f", n);
        close(r.pdf, e.pdf, 2e-6, 3e-4, "pdf", n);
        if (bool(r.accepted) != s.accepted || bool(r.delta) != par.smooth) {
            std::ostringstream msg;
            msg << "Conductor sample flags case " << n << " ax " << q.u_roughness << " ay "
                << q.v_roughness << " oz " << q.oz << " u " << q.u << " GPU z " << r.iz << " CPU z "
                << s.i.z;
            throw std::runtime_error(msg.str());
        }
        close(r.ix, s.i.x, 3e-5, 0, "sample x", n);
        close(r.iy, s.i.y, 3e-5, 0, "sample y", n);
        close(r.iz, s.i.z, 3e-5, 0, "sample z", n);
        if (s.accepted) {
            close(r.weight, s.weight, 2e-6, 3e-4, "weight", n);
            close(r.sample_pdf, s.pdf, 2e-6, 3e-4, "sample pdf", n);
        }
    }
    // Paired device evaluations test reciprocity and 90-degree anisotropic axis rotation.
    std::vector<Q> paired;
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
    }
    auto pairs = dispatch(d, p, paired);
    for (std::size_t n = 0; n < 128; ++n) {
        close(pairs[2 * n].f, results[n].f, 2e-6, 3e-4, "reciprocity", n);
        close(pairs[2 * n + 1].f, results[n].f, 2e-6, 3e-4, "rotation f", n);
        close(pairs[2 * n + 1].pdf, results[n].pdf, 2e-6, 3e-4, "rotation pdf", n);
    }
    std::vector<Q> consistency;
    std::vector<std::size_t> indices;
    for (std::size_t n = 0; n < 128; ++n)
        if (results[n].accepted) {
            auto q = cases[n];
            q.ix = results[n].ix;
            q.iy = results[n].iy;
            q.iz = results[n].iz;
            consistency.push_back(q);
            indices.push_back(n);
        }
    auto evaluated = dispatch(d, p, consistency);
    for (std::size_t n = 0; n < evaluated.size(); ++n) {
        auto r = results[indices[n]];
        close(evaluated[n].pdf, r.sample_pdf, 2e-6, 3e-4, "sample/evaluation pdf", n);
        close(evaluated[n].f * std::abs(r.iz) / evaluated[n].pdf, r.weight, 2e-6, 3e-4,
              "sample/evaluation weight", n);
    }
    std::cout << "Conductor " << cases.size()
              << " independent point/sample cases; 128 reciprocity/axis pairs\n";
}
void energy_checks(id<MTLDevice> d, id<MTLComputePipelineState> p) {
    constexpr unsigned count = 131072;
    std::mt19937 random(170424);
    for (auto axay :
         std::array<std::array<float, 2>, 4>{{{.3f, .3f}, {.2f, .7f}, {1.f, 1.f}, {2.f, .6f}}}) {
        ref::V o = ref::unit({.6, 0, 1});
        auto par = ref::prepare(.27f, 3.2f, axay[0], axay[1], false);
        auto coarse = ref::integrate(par, o, 512), fine = ref::integrate(par, o, 1024);
        require(std::abs(coarse.f - fine.f) < 2e-5 && std::abs(coarse.pdf - fine.pdf) < 2e-5,
                "Quadrature refinement");
        require(fine.f <= 1 + 2e-4, "Quadrature energy conservation");
        std::vector<Q> queries;
        queries.reserve(count);
        for (unsigned n = 0; n < count; ++n)
            queries.push_back(query(.27f, 3.2f, axay[0], axay[1], o, {0, 0, 1},
                                    std::generate_canonical<float, 24>(random),
                                    std::generate_canonical<float, 24>(random)));
        auto results = dispatch(d, p, queries);
        double sum = 0, square = 0, accepted = 0;
        for (auto r : results) {
            require(r.valid, "Energy sample invalid");
            double w = r.accepted ? r.weight : 0;
            sum += w;
            square += w * w;
            accepted += r.accepted;
        }
        double mean = sum / count, variance = (square - count * mean * mean) / (count - 1),
               se = std::sqrt(variance / count), mass = accepted / count;
        require(std::abs(mean - fine.f) <= 5 * se + 2e-4, "Monte Carlo energy vs quadrature");
        require(mean <= 1 + 2e-4, "Monte Carlo energy conservation");
        double mass_se = std::sqrt(fine.pdf * (1 - fine.pdf) / count);
        require(std::abs(mass - fine.pdf) <= 5 * mass_se + 2e-4,
                "VNDF accepted/null probability mass");
        std::cout << "Conductor alpha " << axay[0] << ',' << axay[1] << " energy " << mean
                  << " quadrature " << fine.f << " SE " << se << " null probability " << 1 - mass
                  << " reference " << 1 - fine.pdf << '\n';
    }
}
void invalid_checks(id<MTLDevice> d, id<MTLComputePipelineState> p) {
    for (float roughness : {0.f, .3f}) {
        auto matched = query(1, 0, roughness, roughness, {0, 0, 1}, {0, 0, 1}, .25f, .5f);
        for (auto r : dispatch(d, p, {matched}))
            require(r.valid && !r.accepted && r.weight == 0, "Zero Fresnel null contract");
    }
    for (float roughness : {std::nextafter(1e-6f, 0.f), 1e-6f, std::nextafter(1e-6f, 1.f)}) {
        auto q = query(.2f, 3.f, roughness, roughness, {0, 0, 1}, {0, 0, 1}, .25f, .5f, true);
        for (auto r : dispatch(d, p, {q}))
            require(r.prepared && bool(r.smooth) == (std::sqrt(roughness) < 1e-3f),
                    "Remapped smooth threshold");
    }
    auto cutoff = query(.2f, 3.f, 1, 1, {1, 0, 1e-5}, {0, 0, 1}, .9999f, .625f);
    for (auto r : dispatch(d, p, {cutoff}))
        require(r.valid && !r.accepted && r.sample_pdf == 0 && r.weight == 0,
                "Pinned grazing D cutoff null contract");
    Q base = query(.2f, 3.f, .3f, .4f, {0, 0, 1}, {0, 0, 1}, .5f, .5f);
    std::vector<Q> cases;
    for (float v :
         {-1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto q = base;
        q.eta = v;
        cases.push_back(q);
        q = base;
        q.k = v;
        cases.push_back(q);
        q = base;
        q.u_roughness = v;
        cases.push_back(q);
        q = base;
        q.v_roughness = v;
        cases.push_back(q);
    }
    auto q = base;
    q.eta = q.k = 0;
    cases.push_back(q);
    for (auto r : dispatch(d, p, cases))
        require(!r.prepared, "Invalid conductor parameters accepted");
    cases.clear();
    q = base;
    q.u = 1;
    cases.push_back(q);
    q = base;
    q.v = -.1f;
    cases.push_back(q);
    q = base;
    q.oz = std::numeric_limits<float>::quiet_NaN();
    cases.push_back(q);
    for (auto r : dispatch(d, p, cases))
        require(r.prepared && !r.valid, "Invalid random/direction accepted");
    cases = {base};
    cases[0].eta = std::numeric_limits<float>::max();
    for (auto r : dispatch(d, p, cases))
        require(r.prepared && !r.valid && !r.fresnel_valid,
                "Unrepresentable Fresnel must fail visibly");
    q = base;
    q.oz = 0;
    q.ox = 1;
    cases = {q};
    for (auto r : dispatch(d, p, cases))
        require(r.valid && !r.accepted && r.pdf == 0, "Tangent null contract");
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP Conductor GPU probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Conductor probe requires Metal");
            std::string diagnostic;
            std::filesystem::path library_path;
            std::string oracle;
            int argument = 1;
            if (argument < argc && std::string(argv[argument]) != "--pbrt-probe")
                library_path = argv[argument++];
            if (argument < argc) {
                require(argument + 2 == argc && std::string(argv[argument]) == "--pbrt-probe",
                        "Expected [METALLIB] [--pbrt-probe EXECUTABLE]");
                oracle = argv[argument + 1];
            }
            auto library = metal::load_library(device, library_path, diagnostic);
            require(library != nil, diagnostic);
            auto pipeline =
                metal::load_pipeline(device, library, @"rypath_conductor_probe", diagnostic);
            require(pipeline != nil, diagnostic);
            point_checks(device, pipeline, oracle);
            invalid_checks(device, pipeline);
            energy_checks(device, pipeline);
        } catch (const std::exception &e) {
            std::cerr << "Conductor probe: " << e.what() << '\n';
            return 1;
        }
    }
}
