// SPDX-License-Identifier: GPL-3.0-only
#include "hero_scattering_probe_wire.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
namespace p = hero_scattering_probe;
constexpr double pi = 3.1415926535897932384626433832795;
using V = std::array<double, 3>;
void need(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
void close(double actual, double expected, const std::string &label) {
    // Fixtures stay clear of the critical boundary itself; binary32 arithmetic has this
    // predeclared relative envelope against independent binary64 scalar formulas.
    need(std::isfinite(actual) && std::isfinite(expected) &&
             std::abs(actual - expected) <= 2e-4 * std::abs(expected) + 1e-8,
         label);
}
V vector(const float *a) { return {a[0], a[1], a[2]}; }
double dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V unit(V a) {
    const double length = std::sqrt(dot(a, a));
    for (auto &x : a)
        x /= length;
    return a;
}
// Independent Fresnel equations and Snell law; no shared renderer math is used
// by these expected values. eta is the interior/exterior refractive-index ratio.
double fresnel(double cosine, double eta) {
    const double c = std::min(1., std::abs(cosine)), ep = cosine < 0 ? 1 / eta : eta;
    const double sin2 = (1 - c * c) / (ep * ep);
    if (sin2 >= 1)
        return 1;
    const double ct = std::sqrt(1 - sin2);
    const double rp = (ep * c - ct) / (ep * c + ct), rs = (c - ep * ct) / (c + ep * ct);
    return (rp * rp + rs * rs) / 2;
}
struct E {
    double f = 0, pdf = 0;
};
E dielectric(double eta, double alpha, V wo, V wi, bool importance) {
    if (alpha == 0 || eta == 1 || wo[2] == 0 || wi[2] == 0)
        return {};
    const bool reflection = (wo[2] > 0) == (wi[2] > 0), entering = wo[2] > 0;
    V h;
    for (unsigned j = 0; j < 3; ++j)
        h[j] = reflection ? wo[j] + wi[j] : entering ? wo[j] + eta * wi[j] : eta * wo[j] + wi[j];
    V m = unit(h);
    if (m[2] < 0)
        for (auto &x : m)
            x = -x;
    const double co = dot(wo, m), ci = dot(wi, m);
    if (co * wo[2] < 0 || ci * wi[2] < 0)
        return {};
    const double F = fresnel(co, eta), a2 = alpha * alpha;
    const auto lambda = [a2](V w) {
        return (std::sqrt(1 + a2 * (w[0] * w[0] + w[1] * w[1]) / (w[2] * w[2])) - 1) / 2;
    };
    const double lo = lambda(wo), li = lambda(wi),
                 denD = m[0] * m[0] + m[1] * m[1] + a2 * m[2] * m[2];
    const double D = a2 / (pi * denD * denD), zo = std::abs(wo[2]), zi = std::abs(wi[2]);
    if (reflection)
        return {D * F / (4 * zo * zi * (1 + lo + li)), D * F / (4 * zo * (1 + lo))};
    const double den = dot(h, m) / (entering ? eta : 1), ep = entering ? eta : 1 / eta;
    const double scale = importance ? 1 : 1 / (ep * ep);
    return {D * (1 - F) * std::abs(co * ci) * scale / (zo * zi * den * den * (1 + lo + li)),
            D * std::abs(co) / zo / (1 + lo) * std::abs(ci) / (den * den) * (1 - F)};
}
struct Case {
    p::Query q;
    std::string label;
    unsigned cyclic_hero = 0;
    unsigned critical_branch = 2; // 0=reflection mass, 1=transmission mass, 2=ordinary fixture.
};
p::Query base(unsigned kind) {
    p::Query q{};
    q.kind = kind;
    q.active_mask = 15;
    const float eta[4] = {1.25f, 1.5f, 2, 2.5f}, R[4] = {.2f, .1f, .8f, .3f},
                T[4] = {.8f, .9f, .2f, .7f};
    std::copy(eta, eta + 4, q.eta);
    std::copy(R, R + 4, q.reflectance);
    std::copy(T, T + 4, q.transmittance);
    q.wo[0] = .6f;
    q.wo[2] = .8f;
    q.wi[0] = -.3f;
    q.wi[2] = std::sqrt(.91f);
    q.uc = .01f;
    q.u[0] = .15f;
    q.u[1] = .25f;
    return q;
}
std::vector<Case> cases() {
    std::vector<Case> out;
    for (unsigned mode : {0u, 1u}) {
        auto q = base(4);
        q.importance = mode;
        out.push_back({q, "wavelength-dependent diffuse reflection"});
        q.uc = .95f;
        q.wi[2] = -q.wi[2];
        out.push_back({q, "wavelength-dependent diffuse transmission"});
        q = base(2);
        q.importance = mode;
        q.alpha = .25f;
        out.push_back({q, "rough dispersive reflection and direct PDFs"});
        q.uc = .95f;
        q.wi[2] = -q.wi[2];
        out.push_back({q, "rough dispersive transmission and direct PDFs"});
        q = base(2);
        q.importance = mode;
        out.push_back({q, "aligned dispersive smooth reflection"});
        q.uc = .95f;
        q.eta[2] = q.eta[0];
        out.push_back({q, "dispersive smooth transmission support"});
        q = base(2);
        q.importance = mode;
        q.uc = .95f;
        q.wo[0] = 0;
        q.wo[2] = 1;
        out.push_back({q, "normal-incidence aligned smooth transmission"});
        q = base(2);
        q.importance = mode;
        q.alpha = .25f;
        q.force_rough = 1;
        q.eta[0] = 1;
        q.uc = .95f;
        out.push_back({q, "rough matched-index hero is a Dirac"});
        q = base(3);
        q.importance = mode;
        out.push_back({q, "aligned thin-sheet reflection"});
        q.uc = .95f;
        out.push_back({q, "aligned thin-sheet transmission"});
        // Exiting theta=acos(.75): sin²(theta)*eta² is .8575/.984375/
        // 1.12/1.264375. Two wavelengths transmit; two undergo TIR. This
        // straddles spectral support while staying well clear of arithmetic
        // ambiguity at the critical boundary. Rotate every lane into hero0.
        const std::array<float, 4> indices{1.4f, 1.5f, 1.6f, 1.7f};
        for (unsigned hero = 0; hero < 4; ++hero)
            for (unsigned branch = 0; branch < 2; ++branch) {
                q = base(2);
                q.importance = mode;
                q.wo[0] = std::sqrt(1.f - .75f * .75f);
                q.wo[2] = -.75f;
                q.uc = branch ? .99f : 0.f;
                for (unsigned i = 0; i < 4; ++i)
                    q.eta[i] = indices[(i + hero) % 4];
                out.push_back({q,
                               "exit critical-support cyclic hero" + std::to_string(hero) +
                                   (branch ? " transmission draw" : " reflection draw"),
                               hero, branch});
            }
    }
    auto q = base(1);
    q.wo[0] = 0;
    q.wo[2] = 1;
    for (unsigned i = 0; i < 4; ++i) {
        q.eta[i] = float(i + 1);
        q.transmittance[i] = float(i);
    }
    out.push_back({q, "black hero conductor retains bright companions"});
    q = base(11);
    q.wo[0] = 0;
    q.wo[2] = 1;
    q.reflectance[0] = 0;
    out.push_back({q, "black hero glTF retains bright companions"});
    q = base(5);
    q.alpha = .25f;
    q.uc = 0;
    out.push_back({q, "coated packet requires prior collapse"});
    q.active_mask = 1;
    out.push_back({q, "collapsed coated joint weight and proxy separation"});
    return out;
}
E continuous(const p::Query &q, unsigned i, V wi) {
    const V wo = vector(q.wo);
    if (q.kind == 2)
        return dielectric(q.eta[i], q.alpha, wo, wi, q.importance);
    if (q.kind != 4)
        return {};
    if (wo[2] == 0 || wi[2] == 0)
        return {};
    const double R = q.reflectance[i], T = q.transmittance[i];
    const double value = ((wo[2] > 0) == (wi[2] > 0)) ? R : T;
    return {value / pi, value / (R + T) * std::abs(wi[2]) / pi};
}
void check(const Case &c, const p::Result &v) {
    const auto &q = c.q;
    const auto &label = c.label;
    if (q.kind == 5) {
        need(v.requires_single, "Coated material must declare single-hero requirement");
        if (q.active_mask != 1) {
            need(!v.valid && !v.evaluation_valid, label + ": missing collapse fails visibly");
            return;
        }
        need(v.valid && v.accepted && v.evaluation_valid && v.fallback, label + ": valid fallback");
        need(v.fallback_pdf > 0 && v.fallback_weight[0] > 0, label + ": nonempty legacy control");
        for (unsigned i = 0; i < 4; ++i) {
            need(v.pdf[i] == 0 && v.numerator[i] == 0, label + ": proxy excluded from likelihood");
            need(v.fallback_weight[i] == v.legacy_weight[i] && v.f[i] == v.legacy_f[i],
                 label + ": exact joint-weight/value adapter agreement");
        }
        need(v.fallback_pdf == v.legacy_pdf && v.evaluation_pdf[0] == v.legacy_evaluation_pdf,
             label + ": exact directional-proxy adapter agreement");
        return;
    }
    need(v.valid && v.accepted && v.evaluation_valid && !v.fallback && !v.requires_single,
         label + ": valid supported sample/evaluation");
    const V wo = vector(q.wo), wi = vector(v.wi);
    close(dot(wi, wi), 1, label + ": unit direction");
    const bool transmission = (wo[2] > 0) != (wi[2] > 0);
    need(bool(v.transmission) == transmission, label + ": transmission metadata");
    const bool expected_delta = q.kind == 1 || q.kind == 3 || q.kind == 11 ||
                                (q.kind == 2 && (q.alpha == 0 || q.eta[0] == 1));
    need(bool(v.delta) == expected_delta, label + ": Dirac classification");
    if (q.kind == 2 && q.eta[0] == 1)
        for (unsigned j = 0; j < 3; ++j)
            close(wi[j], -wo[j], label + ": matched-index transmission");
    if (q.kind == 2 && expected_delta) {
        const double F = fresnel(wo[2], q.eta[0]);
        need(transmission == (q.uc >= F), label + ": independent hero Fresnel branch support");
        V expected{-wo[0], -wo[1], wo[2]};
        if (transmission) {
            const double ep = wo[2] > 0 ? q.eta[0] : 1 / q.eta[0];
            const double sin2 = (1 - wo[2] * wo[2]) / (ep * ep);
            need(sin2 < 1, label + ": actual transmission cannot be TIR");
            expected = {-wo[0] / ep, -wo[1] / ep, -std::copysign(std::sqrt(1 - sin2), wo[2])};
        }
        for (unsigned j = 0; j < 3; ++j)
            close(wi[j], expected[j], label + ": independent reflected/refracted direction");
    }
    for (unsigned i = 0; i < 4; ++i) {
        const E direct = continuous(q, i, vector(q.wi));
        close(v.f[i], direct.f, label + ": independent direct f");
        close(v.evaluation_pdf[i], direct.pdf, label + ": independent direct PDF");
        if (!expected_delta) {
            const E e = continuous(q, i, wi);
            close(v.numerator[i], e.f * std::abs(wi[2]), label + ": independent f*cos");
            close(v.pdf[i], e.pdf, label + ": independent sampled-direction PDF");
            const double eta =
                (q.kind == 2 && transmission) ? (wo[2] > 0 ? q.eta[i] : 1 / q.eta[i]) : 1;
            close(v.eta[i], eta, label + ": per-wavelength eta");
            continue;
        }
        double density = 0, mass = 0, eta = 1;
        if (q.kind == 1) {
            const double e = q.eta[i], k = q.transmittance[i];
            density = 1;
            mass = ((e - 1) * (e - 1) + k * k) / ((e + 1) * (e + 1) + k * k);
        } else if (q.kind == 11) {
            density = 1;
            mass = q.reflectance[i];
        } else if (q.kind == 3) {
            const double F = fresnel(wo[2], q.eta[i]), R = 2 * F / (1 + F);
            mass = density = transmission ? 1 - R : R;
        } else if (q.alpha == 0 || q.eta[i] == 1) {
            bool aligned = true;
            if (transmission) {
                const double ep = wo[2] > 0 ? q.eta[i] : 1 / q.eta[i];
                const double sin2 = (1 - wo[2] * wo[2]) / (ep * ep);
                if (sin2 >= 1)
                    aligned = false;
                else {
                    const V expected{-wo[0] / ep, -wo[1] / ep,
                                     -std::copysign(std::sqrt(1 - sin2), wo[2])};
                    for (unsigned j = 0; j < 3; ++j)
                        aligned = aligned && std::abs(wi[j] - expected[j]) < 1e-6;
                }
            }
            if (aligned) {
                const double F = fresnel(wo[2], q.eta[i]);
                density = transmission ? 1 - F : F;
                eta = transmission ? (wo[2] > 0 ? q.eta[i] : 1 / q.eta[i]) : 1;
                mass = density * (transmission && !q.importance ? 1 / (eta * eta) : 1);
            }
        }
        close(v.pdf[i], density, label + ": independent Dirac branch mass");
        close(v.numerator[i], mass, label + ": independent physical Dirac mass");
        close(v.eta[i], eta, label + ": independent Dirac eta");
    }
}
// Independent finite enumeration of ONE interface with a unit-radiance
// endpoint on each supported outgoing direction. Every physical wavelength can
// be the hero with probability1/4; integrate both branch probabilities. This
// checks actual GPU branch masses/support against Fresnel/Snell expectations,
// without invoking the production hero density/throughput helper as an oracle.
void critical_expectation(const std::vector<Case> &tests, const std::vector<p::Result> &values) {
    const std::array<double, 4> indices{double(1.4f), double(1.5f), double(1.6f), double(1.7f)};
    for (unsigned mode : {0u, 1u}) {
        std::array<double, 4> mean{};
        unsigned count = 0;
        for (std::size_t n = 0; n < tests.size(); ++n) {
            const auto &c = tests[n];
            if (c.critical_branch > 1 || c.q.importance != mode)
                continue;
            ++count;
            const auto &v = values[n];
            const double F = fresnel(c.q.wo[2], c.q.eta[0]);
            // A transmission draw on a TIR hero duplicates its reflected
            // result but has ZERO transmission probability in this integral.
            const double probability = c.critical_branch == 0 ? F : 1 - F;
            double density = 0;
            for (float pdf : v.pdf)
                density += pdf;
            need(density > 0, "Critical-support event has a positive mixture density");
            for (unsigned i = 0; i < 4; ++i)
                mean[(i + c.cyclic_hero) % 4] += probability * v.numerator[i] / density;
        }
        need(count == 8, "Enumerate exactly four heroes and two branch outcomes");
        for (unsigned i = 0; i < 4; ++i) {
            const double F = fresnel(-.75, indices[i]);
            const double expected = F + (1 - F) * (mode ? 1 : indices[i] * indices[i]);
            close(mean[i], expected, "Independent cyclic-hero critical-support expectation");
        }
    }
}
std::string error(NSError *e) {
    return e ? e.localizedDescription.UTF8String : "No Metal diagnostic";
}
int run(const char *path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for hero scattering probe");
    NSError *diagnostic = nil;
    auto library =
        [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]
                            error:&diagnostic];
    need(library != nil, "Hero library: " + error(diagnostic));
    auto function = [library newFunctionWithName:@"rypath_hero_scattering_probe"];
    need(function != nil, "Missing hero scattering probe kernel");
    auto pipeline = [device newComputePipelineStateWithFunction:function error:&diagnostic];
    need(pipeline != nil, "Hero pipeline: " + error(diagnostic));
    const auto tests = cases();
    std::vector<p::Query> queries;
    for (const auto &c : tests)
        queries.push_back(c.q);
    auto input = [device newBufferWithBytes:queries.data()
                                     length:queries.size() * sizeof(p::Query)
                                    options:MTLResourceStorageModeShared];
    auto output = [device newBufferWithLength:(queries.size() + 1) * sizeof(p::Result)
                                      options:MTLResourceStorageModeShared];
    need(input != nil && output != nil, "Hero probe buffer allocation");
    std::memset(output.contents, 0xa5, output.length);
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    need(queue != nil && command != nil && encoder != nil, "Hero probe command allocation");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    const auto count = unsigned(queries.size());
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(
                                  std::min<NSUInteger>(32, pipeline.maxTotalThreadsPerThreadgroup),
                                  1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    need(command.status == MTLCommandBufferStatusCompleted,
         "Hero command: " + error(command.error));
    const auto *bytes = static_cast<const unsigned char *>(output.contents);
    need(std::all_of(bytes + queries.size() * sizeof(p::Result), bytes + output.length,
                     [](unsigned char v) { return v == 0xa5; }),
         "Hero result guard overwritten");
    std::vector<p::Result> values(queries.size());
    std::memcpy(values.data(), output.contents, values.size() * sizeof(p::Result));
    for (std::size_t i = 0; i < tests.size(); ++i)
        check(tests[i], values[i]);
    critical_expectation(tests, values);
    std::cout << tests.size()
              << " hero BSDF cases passed: independent scalar formulas; coated adapter controls\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Usage: hero-scattering-probe LIBRARY");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
