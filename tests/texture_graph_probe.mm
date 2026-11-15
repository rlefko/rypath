// SPDX-License-Identifier: GPL-3.0-only

#include "checker_reference.hpp"
#include "metal_execution.hpp"
#include "rgb_spectrum.hpp"
#include "texture_graph.hpp"
#include "texture_graph_probe_wire.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>

namespace {

using namespace rypath;
namespace wire = texture_graph_wire;
using texture_graph_probe::Query;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

static std::optional<float> image(const void *, wire::Word id, bool spectral, bool unbounded,
                                  wire::Mapped2D c, float) {
    if (id != 0 || spectral || unbounded)
        return std::nullopt;
    return c.s + c.t + c.dsdx + 2.0f * c.dtdx + 3.0f * c.dsdy + 4.0f * c.dtdy;
}

void run_probe(id<MTLDevice> device) {
    TextureGraph graph{
        .nodes =
            {
                {.values = {1}},
                {.values = {0}},
                {.kind = wire::image, .mapping = {.kind = wire::uv, .values = {2, -3, .2f, -.4f}}},
                {.kind = wire::image,
                 .mapping = {.kind = wire::planar,
                             .values = {.3f, -.6f, .9f, -.4f, -.1f, .7f, .23f, -.4f}}},
                {.kind = wire::image, .mapping = {.kind = wire::spherical}},
                {.kind = wire::image, .mapping = {.kind = wire::cylindrical}},
                {.kind = wire::fbm,
                 .payload = {8},
                 .values = {.55f},
                 .mapping = {.kind = wire::spatial}},
                {.kind = wire::wrinkled,
                 .payload = {8},
                 .values = {.55f},
                 .mapping = {.kind = wire::spatial}},
                {.kind = wire::windy, .mapping = {.kind = wire::spatial}},
                {.kind = wire::marble,
                 .type = wire::spectrum,
                 .payload = {8},
                 .values = {.55f, 1.42f, .28f},
                 .mapping = {.kind = wire::spatial}},
                {.kind = wire::checkerboard, .children = {0, 1}},
                {.kind = wire::checkerboard,
                 .children = {0, 1},
                 .mapping = {.kind = wire::spatial}},
                {.kind = wire::bilerp, .children = {0, 6, 1, 7}},
                {.kind = wire::dots, .children = {0, 1}},
                {.kind = wire::scale, .type = wire::spectrum, .children = {9, 0}},
                {.kind = wire::mix, .children = {0, 2, 6}},
                {.kind = wire::directionmix, .children = {0, 2}, .values = {0, 0, -1}},
                {.kind = wire::knots, .type = wire::spectrum, .payload = {0, 2}},
                {.kind = wire::blackbody, .type = wire::spectrum, .values = {6500}},
                {.kind = wire::rgb_sigmoid, .type = wire::spectrum, .values = {0, 0, .9f}},
                {.kind = wire::image, .payload = {1}},
                {.kind = wire::scale, .children = {20, 1}},
                {.kind = wire::mix, .children = {20, 0, 0}},
            },
        .knots = {{400, 3}, {780, 6}},
    };
    using W = wire::Word;
    const W normal_roots = static_cast<W>(graph.nodes.size());
    for (W i = normal_roots; i < normal_roots + 64; ++i) {
        graph.nodes.push_back(
            {.kind = wire::scale, .children = {i == normal_roots ? 0 : i - 1, 0}});
    }
    const W first_work = normal_roots + 64;
    for (W i = first_work; i < first_work + 11; ++i) {
        const W c = i == first_work ? 0 : i - 1;
        graph.nodes.push_back({.kind = wire::scale, .children = {c, c}});
    }
    graph.nodes.push_back({.kind = wire::dots, .children = {first_work + 10, 0}});
    graph.nodes.push_back({.kind = wire::scale, .children = {first_work + 11, 0}});
    const W first_axis = static_cast<W>(graph.nodes.size());
    for (const auto kind : {wire::spherical, wire::cylindrical}) {
        graph.nodes.push_back(
            {.kind = wire::bilerp, .children = {1, 1, 0, 0}, .mapping = {.kind = kind}});
        graph.nodes.push_back({.kind = wire::dots, .children = {0, 1}, .mapping = {.kind = kind}});
    }

    std::mt19937 rng(35u);
    const auto draw = [&] { return std::generate_canonical<float, 24>(rng); };
    std::vector<Query> queries;
    for (W i = 0; i < 60; ++i) {
        const auto coordinate = [&] { return draw() * 4.0f - 2.0f; };
        const auto gradient = [&] { return draw() * .4f - .2f; };
        wire::Context c{.p = {coordinate(), coordinate(), coordinate()},
                        .n = {0.6f, 0.0f, 0.8f},
                        .dpdx = {gradient(), gradient(), gradient()},
                        .dpdy = {gradient(), gradient(), gradient()},
                        .u = coordinate(),
                        .v = coordinate(),
                        .dudx = gradient(),
                        .dvdx = gradient(),
                        .dudy = gradient(),
                        .dvdy = gradient()};
        if (i % 4 == 0)
            c.dpdx = c.dpdy = {};
        if (i % 5 == 0)
            c.p = {0, 0, 1};
        const float wavelength = 360.0f + 470.0f * draw();
        for (W root = 0; root < normal_roots; ++root)
            queries.push_back({c, root, wavelength});
    }
    for (W root = normal_roots + 62; root <= first_work + 12; ++root)
        queries.push_back({{}, root, 550});
    const auto axis_offset = queries.size();
    for (const float z : {.25f, -.25f}) {
        const wire::Context axis{.p = {0, 0, z}, .dpdx = {1, 0, 0}, .dpdy = {0, 1, 0}};
        for (W root = first_axis; root < graph.nodes.size(); ++root) {
            queries.push_back({axis, root, 550});
        }
    }

    const auto checker_offset = queries.size();
    std::vector<double> checker_expected;
    for (const auto [x, radius] : checker_reference::cases()) {
        wire::Context c{.u = x, .v = .5f, .dudx = radius / 1.5f};
        queries.push_back({c, 10, 550});
        checker_expected.push_back(.5 + .5 * checker_reference::axis(x, 1.5f * c.dudx));
    }

    const auto pinned_checker_offset = queries.size();
    const auto pinned_regressions = checker_reference::pinned_regressions();
    const W checker_a = static_cast<W>(graph.nodes.size());
    graph.nodes.push_back({.values = {.25f}});
    const W checker_b = static_cast<W>(graph.nodes.size());
    graph.nodes.push_back({.values = {.82f}});
    for (const auto &regression : pinned_regressions) {
        const W root = static_cast<W>(graph.nodes.size());
        graph.nodes.push_back({.kind = wire::checkerboard,
                               .children = {checker_a, checker_b},
                               .mapping = regression.mapping});
        queries.push_back({regression.context, root, 550});
    }

    std::string diagnostic;
    const auto library = metal::load_library(device, {}, diagnostic);
    require(library != nil, diagnostic);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_texture_graph_probe", diagnostic);
    require(pipeline != nil, diagnostic);

    const auto buffer = [&](auto array) {
        const auto data = [device newBufferWithBytes:array.data()
                                              length:array.size_bytes()
                                             options:MTLResourceStorageModeShared];
        require(data != nil, "Texture graph probe upload");
        return data;
    };
    const auto table = srgb_reconstruction_table();
    const texture_graph_probe::Parameters parameters{static_cast<W>(graph.nodes.size()),
                                                     static_cast<W>(graph.knots.size()),
                                                     static_cast<W>(queries.size())};
    id<MTLBuffer> out = [device newBufferWithLength:queries.size() * sizeof(float)
                                            options:MTLResourceStorageModeShared];
    const auto queue = [device newCommandQueue];
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(out && encoder, "Texture graph probe output and command allocation");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer(std::span(graph.nodes)) offset:0 atIndex:0];
    [encoder setBuffer:buffer(std::span(graph.knots)) offset:0 atIndex:1];
    [encoder setBuffer:buffer(std::span(queries)) offset:0 atIndex:2];
    [encoder setBuffer:buffer(table) offset:0 atIndex:3];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:4];
    [encoder setBuffer:out offset:0 atIndex:5];
    [encoder dispatchThreads:MTLSizeMake(queries.size(), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    const auto error = metal::complete(command, "Texture graph probe");
    require(error.empty(), error);

    const auto values = static_cast<const float *>([out contents]);
    // Independent pinned-PBRT coordinates and selected dots branches at ±Z.
    constexpr std::array expected_axis{0.f, 1.f, .5f, 1.f, 1.f, 1.f, .5f, 1.f};
    for (std::size_t i = 0; i < expected_axis.size(); ++i) {
        require(std::abs(values[axis_offset + i] - expected_axis[i]) <= 4e-6f,
                "Metal coordinate-only angular axis differs from pinned PBRT");
    }
    for (std::size_t i = 0; i < checker_expected.size(); ++i) {
        const float actual = values[checker_offset + i];
        require(std::isfinite(actual) && actual >= 0 && actual <= 1 &&
                    std::abs(actual - checker_expected[i]) < 8e-7,
                "Metal checker differs from independent piecewise tent integral or leaves [0,1]");
    }
    for (std::size_t i = 0; i < pinned_regressions.size(); ++i) {
        require(std::abs(values[pinned_checker_offset + i] - pinned_regressions[i].expected) <=
                    3e-5,
                "Metal checker disagrees with independent pinned-context integral");
    }
    float max_delta = 0;
    int errors = 0, failures = 0;
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto &q = queries[i];
        const auto cpu =
            evaluate_texture_graph(graph, q.root, q.ctx, q.wavelength, {.sample = image});
        errors += !cpu;
        if (cpu && std::isfinite(values[i]))
            max_delta = std::max(max_delta, std::abs(values[i] - *cpu));
        if (cpu ? !(std::abs(values[i] - *cpu) <= 0.0003f) : std::isfinite(values[i])) {
            std::cerr << "Mismatch root " << q.root << ", tid " << i << ", CPU "
                      << cpu.value_or(NAN) << ", Metal " << values[i] << '\n';
            ++failures;
        }
    }
    std::cout << queries.size() << " queries, " << errors << " expected failures, max delta "
              << max_delta << ", " << failures << " mismatches\n";
    require(failures == 0, "CPU and Metal texture graphs diverged");
}

} // namespace

int main() {
    @autoreleasepool {
        try {
            const auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP Texture graph probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Texture graph probe requires a Metal device");
            run_probe(device);
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
        return 0;
    }
}
