// SPDX-License-Identifier: GPL-3.0-only
#include "gltf_probe_wire.h"
#include "gltf_reference.hpp"
#include "metal_execution.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace {
namespace ref = gltf_reference;
using namespace rypath;
using Q = gltf_probe::Query;
using R = gltf_probe::Result;
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
// Predeclared before device observations: ordinary f/PDF/weight comparisons use
// 1e-7 absolute + 2e-4 relative. Direction comparisons allow 3e-5 absolute.
void close(double actual, double expected, const char *label, std::size_t index,
           double absolute = 1e-7, double relative = 2e-4) {
    if (!std::isfinite(actual) || !std::isfinite(expected) ||
        std::abs(actual - expected) > absolute + relative * std::abs(expected)) {
        std::ostringstream message;
        message.precision(12);
        message << label << " case " << index << ": " << actual << " expected " << expected;
        throw std::runtime_error(message.str());
    }
}
Q query(float color, float metallic, float roughness, ref::V o, ref::V i, float choice = .25f,
        float u = .375f, float v = .625f) {
    return {color,      metallic,   roughness,  float(o.x), float(o.y), float(o.z),
            float(i.x), float(i.y), float(i.z), choice,     u,          v};
}
ref::P parameters(const Q &q) { return {q.color, q.metallic, double(q.roughness) * q.roughness}; }
ref::V outgoing(const Q &q) { return {q.ox, q.oy, q.oz}; }
std::vector<R> dispatch(id<MTLDevice> device, id<MTLComputePipelineState> pipeline,
                        const std::vector<Q> &queries) {
    const auto count = static_cast<std::uint32_t>(queries.size());
    auto input = [device newBufferWithBytes:queries.data()
                                     length:queries.size() * sizeof(Q)
                                    options:MTLResourceStorageModeShared];
    [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
        [device newBufferWithLength:(queries.size() + 1) * sizeof(R)
                            options:MTLResourceStorageModeShared];
    require(input != nil && output != nil, "glTF probe allocation failed");
    auto *data = static_cast<R *>(output.contents);
    data[queries.size()].prepared = 0x12345678;
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    require(queue != nil && command != nil && encoder != nil, "glTF command allocation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    const auto diagnostic = metal::complete(command, "glTF BSDF probe");
    require(diagnostic.empty(), diagnostic);
    require(data[queries.size()].prepared == 0x12345678, "glTF probe guard overwritten");
    return {data, data + queries.size()};
}
void point_checks(id<MTLDevice> device, id<MTLComputePipelineState> pipeline) {
    std::vector<Q> cases;
    std::mt19937 random(0x984a37);
    auto uniform = [&] { return std::generate_canonical<float, 24>(random); };
    for (unsigned n = 0; n < 4096; ++n) {
        const auto o = ref::direction(.02 + .98 * uniform(), 2 * ref::pi * uniform());
        const auto i = ref::direction(.02 + .98 * uniform(), 2 * ref::pi * uniform());
        cases.push_back(query(uniform(), uniform(), .1f + .9f * uniform(), o, i, uniform(),
                              uniform(), uniform()));
    }
    for (float color : {0.f, .3f, 1.f})
        for (float metal : {0.f, .5f, 1.f})
            for (float rough : {0.f, .1f, .5f, 1.f})
                for (float cosine : {1e-5f, .01f, .5f, 1.f})
                    for (float choice : {0.f, .5f, std::nextafter(1.f, 0.f)})
                        cases.push_back(query(color, metal, rough, ref::direction(cosine),
                                              ref::direction(.4, 1.2), choice, .375f, .625f));
    const auto results = dispatch(device, pipeline, cases);
    for (std::size_t n = 0; n < cases.size(); ++n) {
        const auto q = cases[n];
        const auto r = results[n];
        auto p = parameters(q);
        require(r.prepared && r.evaluation_valid && r.reverse_valid && r.valid,
                "glTF point validity case " + std::to_string(n));
        close(r.alpha, p.alpha, "roughness square", n, 0, 1e-7);
        require(r.probability == p.q(), "glTF mixture selection probability");
        // Use the exact device-prepared alpha to isolate transport arithmetic
        // from the already separately checked binary32 roughness multiplication.
        p.alpha = r.alpha;
        const auto o = outgoing(q);
        const auto e = ref::evaluate(p, o, {q.ix, q.iy, q.iz});
        close(r.f, e.f, "f", n);
        close(r.pdf, e.pdf, "pdf", n);
        close(r.reverse_f, r.f, "reciprocity", n);
        const bool delta = p.alpha == 0 && q.choice < p.q();
        require(bool(r.delta) == delta, "glTF exact-zero delta selection");
        if (delta) {
            close(r.sample_pdf, p.q(), "delta mass", n, 0, 0);
            close(r.weight, ref::amplitude(p, o.z) / p.q(), "delta weight", n);
            close(r.ix, -o.x, "mirror x", n, 0, 0);
            close(r.iy, -o.y, "mirror y", n, 0, 0);
            close(r.iz, o.z, "mirror z", n, 0, 0);
            require(bool(r.accepted) == (r.weight > 0), "glTF zero-amplitude delta accounting");
        } else if (r.accepted) {
            const ref::V i{r.ix, r.iy, r.iz};
            const auto sampled = ref::evaluate(p, o, i);
            close(r.sample_pdf, sampled.pdf, "sample PDF", n);
            close(r.weight, sampled.f * i.z / sampled.pdf, "sample weight", n);
            close(r.sample_pdf, r.sampled_evaluation_pdf, "device PDF consistency", n);
            close(r.weight, r.sampled_f * r.iz / r.sampled_evaluation_pdf,
                  "device weight consistency", n);
        }
        const auto reference = ref::sample(p, o, q.choice, q.u, q.v);
        const auto stretch = ref::unit({p.alpha * o.x, p.alpha * o.y, o.z});
        // Isotropic azimuthal mappings may choose another basis near the pole;
        // distribution correctness is checked independently by integrated mass.
        if (p.alpha == 0 || q.choice >= p.q() || stretch.z < .99999 ||
            (stretch.x == 0 && stretch.y == 0)) {
            require(bool(r.accepted) == reference.accepted,
                    "glTF sample support case " + std::to_string(n));
            close(r.ix, reference.i.x, "sample x", n, 3e-5, 0);
            close(r.iy, reference.i.y, "sample y", n, 3e-5, 0);
            close(r.iz, reference.i.z, "sample z", n, 3e-5, 0);
        }
    }
    std::cout << cases.size() << " glTF double-equation evaluation/PDF/weight/reciprocity cases\n";
}
void limits(id<MTLDevice> device, id<MTLComputePipelineState> pipeline) {
    std::vector<Q> cases;
    for (float rough : {1e-1f, 1e-2f, 1e-3f, 1e-4f, 1e-6f})
        for (float cosine : {1e-5f, .01f, .5f, 1.f})
            for (float u : {0.f, .5f, std::nextafter(1.f, 0.f)})
                cases.push_back(
                    query(.3f, 1, rough, ref::direction(cosine), {0, 0, 1}, 0, u, .375f));
    auto results = dispatch(device, pipeline, cases);
    for (std::size_t n = 0; n < cases.size(); ++n) {
        const auto &q = cases[n];
        const auto &r = results[n];
        require(r.prepared && r.valid && !r.delta && r.alpha > 0,
                "Positive representable roughness remains rough case " + std::to_string(n));
        close(r.alpha, double(q.roughness) * q.roughness, "no roughness floor", n, 0, 1e-7);
        if (r.accepted) {
            const auto o = outgoing(q);
            const ref::V i{r.ix, r.iy, r.iz};
            const auto h = ref::unit(o + i);
            const auto p = ref::P{q.color, q.metallic, r.alpha};
            // Pure-metal f*cos/pdf cancels D: this remains well conditioned
            // when a very narrow lobe amplifies half-vector rounding in f/PDF.
            const double expected = ref::amplitude(p, std::abs(ref::dot(o, h))) *
                                    (1 + ref::lambda(p.alpha, o)) /
                                    (1 + ref::lambda(p.alpha, o) + ref::lambda(p.alpha, i));
            close(r.weight, expected, "near-smooth canceled weight", n);
            require(r.sample_pdf > 0 && std::isfinite(r.sample_pdf), "near-smooth positive PDF");
        }
    }
    auto base = query(.3f, .4f, .5f, {0, 0, 1}, {0, 0, 1});
    cases.clear();
    for (float invalid : {-1.f, 1.1f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()})
        for (unsigned field = 0; field < 3; ++field) {
            auto q = base;
            if (field == 0)
                q.color = invalid;
            if (field == 1)
                q.metallic = invalid;
            if (field == 2)
                q.roughness = invalid;
            cases.push_back(q);
        }
    auto q = base;
    q.roughness = std::numeric_limits<float>::denorm_min();
    cases.push_back(q); // A positive roughness whose square cannot be represented.
    for (const auto &r : dispatch(device, pipeline, cases))
        require(!r.prepared, "Invalid/unrepresentable glTF parameters accepted");
    cases.clear();
    for (unsigned field = 0; field < 4; ++field) {
        q = base;
        if (field == 0)
            q.choice = 1;
        if (field == 1)
            q.u = -.1f;
        if (field == 2)
            q.v = 1;
        if (field == 3)
            q.oz = std::numeric_limits<float>::quiet_NaN();
        cases.push_back(q);
    }
    for (const auto &r : dispatch(device, pipeline, cases))
        require(r.prepared && !r.valid, "Invalid glTF sample inputs accepted");
    cases = {query(.3f, .4f, .5f, {1, 0, 0}, {0, 0, 1}),
             query(.3f, .4f, .5f, {0, 0, -1}, {0, 0, 1})};
    for (const auto &r : dispatch(device, pipeline, cases))
        require(r.valid && !r.accepted && r.pdf == 0 && r.weight == 0,
                "Tangent/backside glTF null contract");
    std::cout << "glTF grazing, near-zero roughness, malformed-input checks\n";
}
void integral_checks(id<MTLDevice> device, id<MTLComputePipelineState> pipeline) {
    const std::array<Q, 8> models{query(1, 1, 1, {0, 0, 1}, {0, 0, 1}),
                                  query(.6f, 1, .5f, ref::direction(.3), {0, 0, 1}),
                                  query(.5f, 0, .7f, ref::direction(.6), {0, 0, 1}),
                                  query(.3f, .45f, 1, ref::direction(.3), {0, 0, 1}),
                                  query(0, 0, .6f, ref::direction(.8), {0, 0, 1}),
                                  query(0, 1, 1, {0, 0, 1}, {0, 0, 1}),
                                  query(.5f, .5f, 0, ref::direction(.7), {0, 0, 1}),
                                  query(1, 0, 0, ref::direction(.05), {0, 0, 1})};
    constexpr unsigned sample_count = 131072;
    for (std::size_t model = 0; model < models.size(); ++model) {
        const auto base = models[model];
        const auto p = parameters(base);
        const auto o = outgoing(base);
        const auto coarse = ref::integrate(p, o, 256), fine = ref::integrate(p, o, 512);
        // Acceptance thresholds fixed independently of measured GPU samples:
        // 4x quadrature refinement difference + five standard errors + 2e-4.
        const double quadrature_energy = 4 * std::abs(fine.f - coarse.f);
        const double quadrature_mass = 4 * std::abs(fine.pdf - coarse.pdf);
        require(quadrature_energy < .003 && quadrature_mass < .003,
                "glTF oracle quadrature not converged case " + std::to_string(model));
        if (base.metallic == 1)
            require(fine.f <= 1 + quadrature_energy + 1e-6, "Pure-metal energy exceeds one");
        if (model == 0)
            require(fine.f < .9, "Single-scatter rough metal must retain furnace loss");
        if (model == 7)
            require(fine.f > 1.2, "Reference dielectric coupling energy defect disappeared");
        for (unsigned seed : {1721u, 89123u}) {
            std::mt19937 random(seed);
            auto uniform = [&] { return std::generate_canonical<float, 24>(random); };
            std::vector<Q> queries(sample_count, base);
            for (auto &q : queries) {
                q.choice = uniform();
                q.u = uniform();
                q.v = uniform();
            }
            const auto results = dispatch(device, pipeline, queries);
            double sum = 0, squares = 0, mass = 0;
            for (const auto &r : results) {
                require(r.prepared && r.valid, "glTF integral sample invalid");
                const double weight = r.accepted ? r.weight : 0;
                sum += weight;
                squares += weight * weight;
                // Count zero-weight but admissible deltas in proposal mass.
                mass += r.accepted || r.delta;
            }
            const double mean = sum / sample_count;
            const double variance = std::max(0., squares / sample_count - mean * mean);
            const double mass_mean = mass / sample_count;
            const double mass_variance = mass_mean * (1 - mass_mean);
            close(mean, fine.f, "integrated BSDF", model,
                  quadrature_energy + 5 * std::sqrt(variance / sample_count) + 2e-4, 0);
            close(mass_mean, fine.pdf, "proposal mass including nulls", model,
                  quadrature_mass + 5 * std::sqrt(mass_variance / sample_count) + 2e-4, 0);
            std::cout << "glTF integral " << model << " seed " << seed << " energy " << mean
                      << " oracle " << fine.f << " mass " << mass_mean << " oracle " << fine.pdf
                      << '\n';
        }
    }
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            require(argc <= 2, "Expected [METALLIB]");
            auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP glTF BSDF probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "glTF BSDF probe requires Metal");
            std::string diagnostic;
            auto library = metal::load_library(
                device, argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{},
                diagnostic);
            require(library != nil, diagnostic);
            auto pipeline = metal::load_pipeline(device, library, @"rypath_gltf_probe", diagnostic);
            require(pipeline != nil, diagnostic);
            point_checks(device, pipeline);
            limits(device, pipeline);
            integral_checks(device, pipeline);
        } catch (const std::exception &error) {
            std::cerr << "glTF BSDF probe: " << error.what() << '\n';
            return 1;
        }
    }
}
