// SPDX-License-Identifier: GPL-3.0-only
#include "layered_probe_wire.h"
#include "layered_reference.hpp"
#include "metal_execution.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace {
namespace ref = layered_reference;
using Q = rypath::layered_probe::Query;
using R = rypath::layered_probe::Result;
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
void close(double a, double b, double tolerance, const std::string &label) {
    if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > tolerance)
        throw std::runtime_error(label + ": " + std::to_string(a) + " expected " +
                                 std::to_string(b));
}
Q query() {
    return {1.5f, 0, 0, .3f, 3,   0,   0,   .6f, .2f, 0, 0,  0, 0,
            1,    0, 0, 1,   .5f, .3f, .7f, 0,   16,  4, 17, 0, 17};
}
ref::P reference(Q q) {
    return {dielectric_reference::prepare(q.eta, q.ax, q.ay, false),
            conductor_reference::prepare(q.base_eta, q.base_k, q.base_ax, q.base_ay, false),
            bool(q.conductor_base),
            q.reflectance,
            q.thickness,
            q.albedo,
            q.g,
            q.max_depth};
}
struct Moments {
    double sum = 0, square = 0;
    unsigned count = 0;
    void add(double x) {
        sum += x;
        square += x * x;
        ++count;
    }
    double mean() const { return sum / count; }
    double variance_mean() const { return std::max(0., square / count - mean() * mean()) / count; }
};
std::vector<R> dispatch(id<MTLDevice> device, id<MTLComputePipelineState> pipeline,
                        const std::vector<Q> &queries) {
    unsigned count = static_cast<unsigned>(queries.size());
    auto input = [device newBufferWithBytes:queries.data()
                                     length:queries.size() * sizeof(Q)
                                    options:MTLResourceStorageModeShared];
    [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
        [device newBufferWithLength:(queries.size() + 1) * sizeof(R)
                            options:MTLResourceStorageModeShared];
    require(input && output, "Layered probe allocation");
    auto *data = static_cast<R *>(output.contents);
    data[queries.size()].valid = 0x12345678;
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    require(queue && command && encoder, "Layered command allocation");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(rypath::metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    require(rypath::metal::complete(command, "Layered probe").empty(), "Layered GPU failure");
    require(data[queries.size()].valid == 0x12345678, "Layered guard overwrite");
    return {data, data + queries.size()};
}
std::vector<std::array<double, 8>> oracle(const std::string &, const std::vector<Q> &);
void points(id<MTLDevice> device, id<MTLComputePipelineState> pipeline,
            const std::string &executable) {
    std::vector<Q> queries;
    auto q = query();
    q.max_depth = 1;
    queries.push_back(q);
    q.max_depth = 0;
    queries.push_back(q);
    q = query();
    q.eta = 1;
    q.thickness = 0;
    q.max_depth = 2;
    queries.push_back(q);
    q = query();
    q.operation = 1;
    q.uc = .01;
    queries.push_back(q);
    q.uc = .9;
    queries.push_back(q);
    q.operation = 2;
    q.reflectance = .2;
    q.albedo = .7;
    queries.push_back(q);
    q = query();
    // PBRT hashes exact float bytes after its two-sided flip. Negate every
    // component, including zero's sign: changing just z would hash -0 x/y on
    // one side and +0 on the other, legitimately choosing unrelated walks.
    q.ox = -q.ox;
    q.oy = -q.oy;
    q.oz = -q.oz;
    q.ix = -q.ix;
    q.iy = -q.iy;
    q.iz = -q.iz;
    require(std::signbit(q.ox) && std::signbit(q.oy) && std::signbit(q.ix) && std::signbit(q.iy),
            "Two-sided fixture retains full signed-zero negation");
    queries.push_back(q);
    q = query();
    queries.push_back(q);
    q.samples = 0;
    queries.push_back(q);
    q = query();
    q.g = 1;
    q.albedo = .8;
    queries.push_back(q);
    q.g = -1;
    queries.push_back(q);
    q = query();
    q.thickness = -10;
    queries.push_back(q);
    q = query();
    q.ax = .1;
    q.ay = .4;
    q.base_ax = .3;
    q.base_ay = .1;
    q.conductor_base = 1;
    queries.push_back(q);
    q = query();
    q.eta = 1;
    q.ax = .1;
    q.ay = .4;
    q.max_depth = 1;
    q.thickness = 0;
    queries.push_back(q);
    auto r = dispatch(device, pipeline, queries);
    auto upstream = oracle(executable, std::vector<Q>(queries.begin(), queries.begin() + 6));
    if (!upstream.empty())
        for (unsigned n = 0; n < 6; ++n) {
            close(r[n].f, upstream[n][0], 2e-5, "Pinned analytic f");
            close(r[n].pdf, upstream[n][1], 2e-5, "Pinned analytic directional PDF");
            close(r[n].accepted ? r[n].weight : 0, upstream[n][2], 2e-5, "Pinned analytic weight");
        }
    for (std::size_t n = 0; n < r.size(); ++n) {
        if (n == 8) {
            require(!r[n].valid && !r[n].evaluation_valid, "Invalid parameters diagnosed");
            continue;
        }
        require(r[n].valid && r[n].evaluation_valid, "Point numeric failure " + std::to_string(n));
    }
    close(r[0].f, ref::smooth_diffuse_first(reference(queries[0]), {0, 0, 1}, {0, 0, 1}), 2e-6,
          "First base connection");
    require(r[0].f > 0 && !r[0].accepted,
            "Depth-one evaluation and sampling support differ explicitly");
    require(r[1].f == 0 && !r[1].accepted, "Depth-zero model");
    require(r[2].accepted && !r[2].delta, "Matched-index diffuse sample");
    close(r[2].weight, .6, 1e-6, "Matched-index throughput");
    require(std::abs(r[2].weight - r[2].f * std::abs(r[2].iz) / r[2].sample_pdf) > 1e-4,
            "Joint throughput cannot be recomputed with directional PDF");
    double thin = ref::thin_reflection(1, 1.5);
    close(r[3].sample_pdf, thin, 1e-6, "Thin analytic internal bounces");
    require(r[3].delta && r[3].iz == 1 && r[4].iz == -1 && r[4].weight == 1, "Thin two lobes");
    close(r[5].weight, .9, 1e-6, "Diffuse throughput");
    close(r[6].f, r[7].f, 0, "Two-sided evaluation");
    close(r[6].weight, r[7].weight, 0, "Two-sided sampling");
    if (!executable.empty()) {
        auto z_only_flip = query();
        z_only_flip.oz = z_only_flip.iz = -1;
        const auto flips = oracle(executable, {queries[6], queries[7], z_only_flip});
        close(flips[0][0], flips[1][0], 0, "Pinned full two-sided negation");
        require(std::abs(flips[2][0] - flips[1][0]) > 1e-4,
                "Pinned signed-zero hashes select different finite walks");
    }
    close(r[0].conditional_pdf, 1, 1e-6, "Conditional transmission delta PDF");
    close(r[0].conditional_weight, .96 / 2.25, 1e-6, "Conditional transmission weight");
    close(r[0].pdf, .1 / (4 * ref::pi) + .9 / ref::pi, 1e-6, "PBRT approximate directional PDF");
    close(r[9].hg_pdf, ref::hg(1, 1), 1e-6, "HG positive endpoint");
    close(r[10].hg_pdf, ref::hg(1, -1), .3, "HG negative endpoint");
    // Pinned DielectricBxDF::Flags remains glossy for authored roughness at
    // eta=1, although its sampled transmission is delta. At one base event
    // the NEE term therefore retains the conditional-PDF power weight.
    const double cosine_pdf = 1 / ref::pi;
    const double mis = 1 / (1 + cosine_pdf * cosine_pdf);
    close(r[13].f, .6 / ref::pi * mis, 2e-6, "Matched-index authored-roughness NEE flags");
    close(r[13].pdf, .1 / (4 * ref::pi) + .9 * mis / ref::pi, 2e-6,
          "Matched-index authored-roughness directional PDF flags");
    if (!executable.empty()) {
        auto matched = oracle(executable, {queries[13]});
        close(r[13].f, matched[0][0], 2e-6, "Pinned matched-index rough NEE");
        close(r[13].pdf, matched[0][1], 2e-6, "Pinned matched-index rough directional PDF");
    }
    std::cout << "PASS layered analytic contracts, finite-depth support, conditional/proportional "
                 "PDFs, thin/diffuse, HG endpoints, two-sided and invalid inputs\n";
}
// Pinned executable consumes the same query scalar fields and emits upstream
// f/PDF/weight/joint-PDF/direction/proportional-flag. Independent RNG implies
// statistical comparisons, never per-seed equality for layered walks.
std::vector<std::array<double, 8>> oracle(const std::string &executable,
                                          const std::vector<Q> &queries) {
    if (executable.empty())
        return {};
    char pattern[] = "/tmp/rypath-layered-pbrt-XXXXXX";
    auto path = mkdtemp(pattern);
    require(path, "Oracle temporary directory");
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove_all(p, e);
        }
    } cleanup{path};
    auto input = cleanup.p / "in", output = cleanup.p / "out";
    {
        std::ofstream s(input);
        s << std::setprecision(9);
        for (auto q : queries)
            s << q.eta << ' ' << q.ax << ' ' << q.ay << ' ' << q.base_eta << ' ' << q.base_k << ' '
              << q.base_ax << ' ' << q.base_ay << ' ' << q.reflectance << ' ' << q.thickness << ' '
              << q.albedo << ' ' << q.g << ' ' << q.ox << ' ' << q.oy << ' ' << q.oz << ' ' << q.ix
              << ' ' << q.iy << ' ' << q.iz << ' ' << q.uc << ' ' << q.u << ' ' << q.v << ' '
              << q.conductor_base << ' ' << q.max_depth << ' ' << q.samples << ' '
              << q.material_seed << ' ' << q.operation << '\n';
        require(bool(s), "Oracle input write");
    }
    std::ofstream(output).close();
    auto task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:executable.c_str()]];
    task.standardInput =
        [NSFileHandle fileHandleForReadingAtPath:[NSString stringWithUTF8String:input.c_str()]];
    task.standardOutput =
        [NSFileHandle fileHandleForWritingAtPath:[NSString stringWithUTF8String:output.c_str()]];
    NSError *error = nil;
    require([task launchAndReturnError:&error], "Oracle launch");
    [task waitUntilExit];
    require(task.terminationStatus == 0, "Oracle exit");
    std::ifstream s(output);
    std::vector<std::array<double, 8>> result;
    for (std::size_t n = 0; n < queries.size(); ++n) {
        std::array<double, 8> a{};
        for (auto &v : a)
            require(bool(s >> v), "Oracle output");
        result.push_back(a);
    }
    return result;
}
void mis_contract(id<MTLDevice> device, id<MTLComputePipelineState> pipeline) {
    ref::Random random{std::mt19937_64(271828)};
    std::vector<Q> queries;
    for (unsigned n = 0; n < 256; ++n) {
        auto q = query();
        q.ax = .1;
        q.ay = .4;
        q.albedo = .8;
        q.g = .6;
        q.thickness = .9;
        q.seed = n;
        q.uc = static_cast<float>(random.next());
        q.u = static_cast<float>(random.next());
        q.v = static_cast<float>(random.next());
        queries.push_back(q);
    }
    auto samples = dispatch(device, pipeline, queries);
    std::vector<Q> evaluations;
    std::vector<float> sample_pdfs;
    for (unsigned n = 0; n < samples.size(); ++n) {
        const auto &s = samples[n];
        require(s.valid, "MIS sample arithmetic");
        if (!s.accepted)
            continue;
        auto q = queries[n];
        q.ix = s.ix;
        q.iy = s.iy;
        q.iz = s.iz;
        q.seed ^= 0x12345678u;
        evaluations.push_back(q);
        q.seed ^= 0xabcdef01u;
        evaluations.push_back(q);
        q.ox = -q.ox;
        q.oy = -q.oy;
        q.oz = -q.oz;
        q.ix = -q.ix;
        q.iy = -q.iy;
        q.iz = -q.iz;
        evaluations.push_back(q);
        // A reconstructed local direction may change a few ULPs. PBRT hashes
        // those exact bits, so this is a new PDF query. The integrator must reuse
        // the captured local pair for complementary MIS on the original edge.
        auto rounded = queries[n];
        float x = .6f * s.ix - .8f * s.iy;
        float y = .8f * s.ix + .6f * s.iy;
        float z = s.iz;
        const float scale = std::max({std::abs(x), std::abs(y), std::abs(z)});
        x /= scale;
        y /= scale;
        z /= scale;
        const float norm = std::sqrt(x * x + y * y + z * z);
        x /= norm;
        y /= norm;
        z /= norm;
        rounded.ix = .6f * x + .8f * y;
        rounded.iy = -.8f * x + .6f * y;
        rounded.iz = z;
        rounded.seed ^= 0xf0e1d2c3u;
        evaluations.push_back(rounded);
        auto different_material_seed = queries[n];
        different_material_seed.ix = s.ix;
        different_material_seed.iy = s.iy;
        different_material_seed.iz = s.iz;
        different_material_seed.material_seed ^= 0x12345678;
        evaluations.push_back(different_material_seed);
        sample_pdfs.push_back(s.sample_pdf);
    }
    require(!evaluations.empty(), "MIS accepted samples");
    auto results = dispatch(device, pipeline, evaluations);
    bool varied_f = false, varied_pdf = false, rounded_pdf_changed = false;
    for (unsigned n = 0; n < sample_pdfs.size(); ++n) {
        auto a = results[5 * n], b = results[5 * n + 1], flipped = results[5 * n + 2],
             rounded = results[5 * n + 3], material = results[5 * n + 4];
        require(a.evaluation_valid && b.evaluation_valid && flipped.evaluation_valid &&
                    rounded.evaluation_valid && material.evaluation_valid,
                "MIS evaluation arithmetic");
        close(a.pdf, sample_pdfs[n], 0, "Sample/evaluate directional PDF agreement");
        close(a.pdf, b.pdf, 0, "MIS PDF independent of event seed");
        close(a.pdf, flipped.pdf, 0, "MIS PDF canonical two-sided directions");
        close(a.f, b.f, 0, "MIS value independent of event seed");
        rounded_pdf_changed |= rounded.pdf != a.pdf;
        const double light_pdf = .37, bsdf_pdf = sample_pdfs[n];
        const double denominator = light_pdf * light_pdf + bsdf_pdf * bsdf_pdf;
        const double light_weight = light_pdf * light_pdf / denominator;
        const double bsdf_weight = double(b.pdf) * b.pdf / denominator;
        close(light_weight + bsdf_weight, 1, 2e-15, "Complementary directional MIS weights");
        varied_f |= a.f != material.f;
        varied_pdf |= a.pdf != material.pdf;
    }
    require(varied_f && varied_pdf, "Global material seed selects PBRT value/PDF streams");
    require(rounded_pdf_changed, "Reconstructed ULP changes exercise a distinct PDF hash query");
    std::cout << "PASS exact local-pair MIS, event-seed and two-sided invariance, "
                 "new rounded-direction queries and global material-seed sensitivity\n";
}
void statistical(id<MTLDevice> device, id<MTLComputePipelineState> pipeline, unsigned count,
                 const std::string &executable) {
    for (unsigned scenario = 0; scenario < 11; ++scenario) {
        auto q = query();
        q.samples = 4;
        q.max_depth = 16;
        if (scenario == 1) {
            q.ax = .12;
            q.ay = .4;
        }
        if (scenario == 2) {
            q.conductor_base = 1;
            q.base_ax = .2;
            q.base_ay = .4;
        }
        if (scenario == 3) {
            q.albedo = .7;
            q.g = 0;
            q.thickness = .8;
        }
        if (scenario == 4) {
            q.albedo = .8;
            q.g = .6;
            q.thickness = 1.2;
            q.ax = .1;
            q.ay = .3;
        }
        if (scenario == 5) {
            q.albedo = .6;
            q.g = -.7;
            q.thickness = .6;
            q.conductor_base = 1;
            q.base_ax = .15;
            q.base_ay = .3;
        }
        if (scenario == 6) {
            q.conductor_base = 1;
            q.thickness = 2;
        }
        if (scenario == 7) {
            q.ox = .8;
            q.oz = .6;
            q.ix = -.6;
            q.iz = .8;
            q.ax = .3;
            q.ay = .1;
        }
        if (scenario == 8) {
            q.max_depth = 1;
            q.albedo = .8;
            q.thickness = .7;
            q.ax = .1;
            q.ay = .3;
        }
        if (scenario == 9 || scenario == 10) {
            q.eta = 1;
            q.ax = scenario == 9 ? .1 : 0;
            q.ay = scenario == 9 ? .4 : 0;
            q.conductor_base = 1;
            q.albedo = .95;
            q.g = .9;
            q.thickness = .8;
            q.ox = .99995;
            q.oz = .01;
            q.ix = -.6;
            q.iz = .8;
        }
        ref::Random random{std::mt19937_64(1000 + scenario)},
            cpu_random{std::mt19937_64(9000 + scenario)};
        std::vector<Q> queries;
        queries.reserve(count);
        for (unsigned n = 0; n < count; ++n) {
            q.uc = static_cast<float>(random.next());
            q.u = static_cast<float>(random.next());
            q.v = static_cast<float>(random.next());
            q.seed = n * 0x9e3779b9u + scenario;
            q.material_seed = static_cast<int>(q.seed);
            queries.push_back(q);
        }
        auto results = dispatch(device, pipeline, queries);
        auto pinned = oracle(executable, queries);
        Moments gpu, cpu, upstream, eval, up_eval;
        auto p = reference(q);
        for (unsigned n = 0; n < count; ++n) {
            auto r = results[n];
            require(r.valid && r.evaluation_valid, "Statistical numeric failure " +
                                                       std::to_string(scenario) + "/" +
                                                       std::to_string(n));
            require(!r.accepted || (r.sample_pdf > 0 && r.weight >= 0 && r.iz * q.oz > 0),
                    "Sample support");
            gpu.add(r.accepted ? r.weight : 0);
            eval.add(r.f);
            auto c = ref::sample(p, {q.ox, q.oy, q.oz}, cpu_random);
            cpu.add(c.accepted ? c.weight : 0);
            if (!pinned.empty()) {
                upstream.add(pinned[n][2]);
                up_eval.add(pinned[n][0]);
                require(pinned[n][3] == 0 || pinned[n][7] == 1, "Pinned proportional-path flag");
            }
        }
        // Fixed before observation: seven estimated standard errors plus .003
        // absolute for float/cutoff differences. Separate streams avoid covariance.
        auto compare = [&](const Moments &a, const Moments &b, const std::string &label) {
            close(a.mean(), b.mean(), 7 * std::sqrt(a.variance_mean() + b.variance_mean()) + .003,
                  label + " scenario " + std::to_string(scenario));
        };
        compare(gpu, cpu, "Double path energy");
        require(gpu.mean() <= 1 + 7 * std::sqrt(gpu.variance_mean()) + .003,
                "Passive energy bound");
        if (!pinned.empty()) {
            compare(gpu, upstream, "Pinned path energy");
            compare(eval, up_eval, "Pinned f");
            // A deterministic PDF has one fixed estimate at any one pair of
            // directions. Compare its distribution over independent directions,
            // not a constant estimate against PBRT's average over seed changes.
            std::vector<Q> density_queries;
            ref::Random directions{std::mt19937_64(3000 + scenario)};
            for (unsigned n = 0; n < 2048; ++n) {
                auto d = q;
                const double z = directions.next(), phi = 2 * ref::pi * directions.next();
                const double sine = std::sqrt(1 - z * z);
                d.ix = static_cast<float>(sine * std::cos(phi));
                d.iy = static_cast<float>(sine * std::sin(phi));
                d.iz = static_cast<float>(z);
                d.seed = n;
                d.material_seed = static_cast<int>(n);
                density_queries.push_back(d);
            }
            const auto density_results = dispatch(device, pipeline, density_queries);
            const auto density_pinned = oracle(executable, density_queries);
            Moments pdf, up_pdf;
            for (unsigned n = 0; n < density_results.size(); ++n) {
                require(density_results[n].evaluation_valid, "Directional density arithmetic");
                pdf.add(density_results[n].pdf);
                up_pdf.add(density_pinned[n][1]);
            }
            compare(pdf, up_pdf, "Pinned PDF over directions");
        }
        std::cout << "PASS layered scenario " << scenario << " samples=" << count
                  << " GPU weight=" << gpu.mean() << " CPU=" << cpu.mean();
        if (!pinned.empty())
            std::cout << " PBRT=" << upstream.mean() << " f=" << eval.mean() << "/"
                      << up_eval.mean();
        std::cout << '\n';
    }
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            std::filesystem::path library_path;
            std::string upstream;
            bool extended = false;
            for (int n = 1; n < argc; ++n) {
                std::string a = argv[n];
                if (a == "--extended")
                    extended = true;
                else if (a == "--pbrt-probe") {
                    require(n + 1 < argc, "Missing PBRT executable");
                    upstream = argv[++n];
                } else {
                    require(library_path.empty(),
                            "Expected [METALLIB] [--extended] [--pbrt-probe EXECUTABLE]");
                    library_path = a;
                }
            }
            auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP layered GPU probe: no Metal device\n";
                return 77;
            }
#endif
            require(device != nil, "Layered probe requires Metal");
            std::string diagnostic;
            auto library = rypath::metal::load_library(device, library_path, diagnostic);
            require(library != nil, diagnostic);
            auto pipeline =
                rypath::metal::load_pipeline(device, library, @"rypath_layered_probe", diagnostic);
            require(pipeline != nil, diagnostic);
            points(device, pipeline, upstream);
            mis_contract(device, pipeline);
            statistical(device, pipeline, extended ? 32768 : 8192, upstream);
        } catch (const std::exception &e) {
            std::cerr << "Layered probe: " << e.what() << '\n';
            return 1;
        }
    }
}
