// SPDX-License-Identifier: GPL-3.0-only

#include "masked_probe_wire.h"
#include "metal_execution.hpp"
#include "metal_geometry_access.hpp"
#include "triangle_candidate_fixture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace rypath;
namespace wire = geometry_wire;
namespace probe = masked_probe;
using wire::Word;

constexpr Word absent = 0xffffffffu;
constexpr std::array<unsigned, 10> lanes{0, 1, 2, 3, 3, 4, 4, 5, 5, 6};
constexpr unsigned coincident_patch_lane = 6;
constexpr unsigned coincident_accepted_patch = 10;

void require(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

GeometryData geometry(bool reversed) {
    GeometryMesh patch;
    patch.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 1}};
    patch.patches = {{0, 1, 2, 3}};
    GeometryMesh triangle;
    triangle.positions = {{-1, -1, 0}, {3, -1, 0}, {-1, 3, 0}};
    triangle.triangles = {{0, 1, 2}};
    GeometryData result{
        .meshes = {patch, triangle},
        .instances =
            {
                {.mesh_index = 0},                     // Two-root patch.
                {.mesh_index = 1, .non_opaque = true}, // Manual triangle.
                {.mesh_index = 1},                     // Opaque triangle, still manually validated.
                {.mesh_index = 0},                     // Patch ties an opaque triangle.
                {.mesh_index = 1},
                {.mesh_index = 1, .non_opaque = true}, // Triangle ties an opaque triangle.
                {.mesh_index = 1},
                {.mesh_index = 0}, // Rejected near root, behind a blocker.
                {.mesh_index = 1},
                {.mesh_index = 0}, // Two coincident patches.
                {.mesh_index = 0},
            },
    };
    for (std::size_t i = 0; i < result.instances.size(); ++i) {
        auto &placement = result.instances[i];
        placement.transform.values[7] =
            10.0f * static_cast<float>(i < lanes.size() ? lanes[i] : coincident_patch_lane);
        if (placement.mesh_index == 1) {
            // On z=x-3/16 the triangle's t is 1/4, or 5/8 on the last plane.
            placement.transform.values[11] = i == 8 ? 0.4375f : 0.0625f;
        }
    }
    if (reversed) {
        std::ranges::reverse(result.instances);
    }
    return result;
}

struct Check {
    std::string_view name;
    probe::Case input;
    Word kind;
    Word source = absent;
    float t = 0;
    float uv = 0;
    bool manual = false;
};

probe::Case ray(unsigned lane, Word triangle_rule, Word patch_rule, float tmin = 0, float tmax = 1,
                Word any_hit = 0) {
    return {{{0, 10.0f * static_cast<float>(lane), -0.1875f}, {1, 1, 1}, tmin, tmax},
            triangle_rule,
            patch_rule,
            absent,
            any_hit,
            {0, absent}};
}

void observe(id<MTLDevice> device, id<MTLComputePipelineState> pipeline, bool reversed) {
    const auto prepared = geometry(reversed);
    const auto built = build_metal_geometry(prepared);
    require(built.status == GeometryStatus::success, built.diagnostic);
    const auto &scene = *built.scene;
    const auto source = [&](Word ordinal) {
        const auto source_index = MetalGeometryAccess::source_instance(scene, ordinal);
        return reversed ? static_cast<Word>(prepared.instances.size() - 1 - source_index)
                        : source_index;
    };

    const auto A = probe::accept, R = probe::reject, F = probe::reject_near, E = probe::invalid;
    std::array checks{
        Check{"Patch near root", ray(0, E, A), wire::bilinear_patch, 0, .25f, .25f},
        Check{"Patch far root survives near rejection", ray(0, E, F), wire::bilinear_patch, 0, .75f,
              .75f},
        Check{"Fully rejected patch", ray(0, E, R), wire::miss},
        Check{"Far patch root excluded by ray maximum", ray(0, E, F, 0, .5f), wire::miss},
        Check{"Near patch root excluded by ray minimum", ray(0, E, A, .5f), wire::bilinear_patch, 0,
              .75f, .75f},
        Check{"Shadow finds accepted far root", ray(0, E, F, 0, 1, 1), wire::bilinear_patch, 0,
              .75f, .75f},
        Check{"Shadow rejects near root and terminates at ray maximum", ray(0, E, F, 0, .5f, 1),
              wire::miss},
        Check{"Invalid patch callback", ray(0, E, E), wire::invalid_hit},
        Check{"Manual triangle accepted", ray(1, A, E), wire::triangle, 1, .25f, .3125f, true},
        Check{"Manual triangle rejected", ray(1, R, E), wire::miss, absent, 0, 0, true},
        Check{"Invalid triangle callback", ray(1, E, E), wire::invalid_hit, absent, 0, 0, true},
        // All triangles are manually validated; opaque instances also call alpha.
        Check{"Opaque triangle accepted", ray(2, A, E), wire::triangle, 2, .25f, .3125f, true},
        Check{"Opaque triangle rejected", ray(2, R, E), wire::miss, absent, 0, 0, true},
        Check{"Invalid opaque triangle callback", ray(2, E, E), wire::invalid_hit, absent, 0, 0,
              true},
        Check{"Rejected patch root ties opaque triangle", ray(3, A, F), wire::triangle, 4, .25f,
              .3125f},
        Check{"Both coincident triangles rejected", ray(4, R, E), wire::miss},
        Check{"Opaque triangle between patch roots", ray(5, A, F), wire::triangle, 8, .625f,
              .40625f},
        Check{"Accepted patch ties rejected patch", ray(6, E, A), wire::bilinear_patch,
              coincident_accepted_patch, .25f, .25f},
    };
    for (Word ordinal = 0; ordinal < MetalGeometryAccess::instance_count(scene); ++ordinal) {
        if (source(ordinal) == coincident_accepted_patch - 1) {
            checks.back().input.ignore_instance = ordinal;
        }
    }
    require(checks.back().input.ignore_instance != absent, "find coincident patch ordinal");
    std::array<probe::Case, checks.size()> cases{};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        cases[i] = checks[i].input;
    }
    const auto input = [device newBufferWithBytes:cases.data()
                                           length:sizeof(cases)
                                          options:MTLResourceStorageModeShared];
    const auto output = [device newBufferWithLength:cases.size() * sizeof(probe::Result)
                                            options:MTLResourceStorageModeShared];
    require(input && output, "allocate masked intersection probe buffers");
    const auto command = [MetalGeometryAccess::queue(scene) commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(command && encoder, "create masked intersection probe command and encoder");
    [encoder setComputePipelineState:pipeline];
    MetalGeometryAccess::bind(scene, encoder, 0);
    [encoder setBuffer:input offset:0 atIndex:1];
    [encoder setBuffer:output offset:0 atIndex:2];
    const auto count = static_cast<Word>(cases.size());
    [encoder setBytes:&count length:sizeof(count) atIndex:3];
    [encoder dispatchThreads:MTLSizeMake(cases.size(), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    const auto error = metal::complete(command, "Masked intersection probe");
    require(error.empty(), error);
    const auto *results = static_cast<const probe::Result *>(output.contents);
    require(results != nullptr, "masked intersection output contents");
    for (std::size_t i = 0; i < checks.size(); ++i) {
        const auto &expected = checks[i];
        const auto &actual = results[i];
        const auto &hit = actual.hit;
        const auto callbacks = std::string(expected.name) + " triangle callbacks: mask=" +
                               std::to_string(actual.triangle_primitive_mask) +
                               ", count=" + std::to_string(actual.triangle_calls);
        const auto difference = [&](float x, float reference) {
            return std::abs(x - reference) <= 1e-6f;
        };
        require(hit.kind == expected.kind, callbacks + " type");
        if (expected.source != absent) {
            require(hit.instance < MetalGeometryAccess::instance_count(scene) &&
                        source(hit.instance) == expected.source && hit.geometry == 0 &&
                        hit.primitive == 0,
                    callbacks + " committed identity");
            require(difference(hit.t, expected.t) && difference(hit.u, expected.uv) &&
                        difference(hit.v, expected.uv),
                    callbacks + " committed coordinates");
        }
        require((actual.triangle_primitive_mask & probe::unexpected_triangle_primitive) == 0,
                callbacks + " unexpected primitive");
        if (expected.manual) {
            // Accepted nearest hits reset traversal and may revisit this primitive.
            // Rejection/invalid termination does not reset the query.
            const bool count_ok = expected.kind == wire::triangle ? actual.triangle_calls >= 1
                                                                  : actual.triangle_calls == 1;
            require(count_ok && actual.triangle_primitive_mask == 1,
                    callbacks + " expected primitive 0 only");
        }
    }
}

// Native false positives 556/562 must be rejected before alpha observes them;
// the valid farther 601 must remain available after either candidate order.
void observe_regression(id<MTLDevice> device, id<MTLComputePipelineState> pipeline) {
    for (bool non_opaque : {false, true}) {
        for (unsigned fresh = 0; fresh < 4; ++fresh) {
            auto geometry = test::triangle_fixture::regression();
            geometry.instances[0].non_opaque = non_opaque;
            auto built = build_metal_geometry(geometry);
            require(built.status == GeometryStatus::success, built.diagnostic);
            const auto &scene = *built.scene;
            const auto &r = test::triangle_fixture::rays[1];
            std::array<probe::Case, 4> cases{};
            for (auto &c : cases) {
                c.ray = {{r.origin.x, r.origin.y, r.origin.z},
                         {r.direction.x, r.direction.y, r.direction.z},
                         r.t_min,
                         r.t_max};
                c.patch_rule = probe::invalid;
                c.ignore_instance = absent;
                c.expected_triangle_primitives[0] = 601;
                c.expected_triangle_primitives[1] = 609;
            }
            cases[0].triangle_rule = probe::accept;
            cases[1].triangle_rule = probe::reject;
            cases[2].triangle_rule = probe::invalid;
            cases[3].triangle_rule = probe::accept;
            cases[3].any_hit = 1;
            auto input = [device newBufferWithBytes:cases.data()
                                             length:sizeof(cases)
                                            options:MTLResourceStorageModeShared];
            auto output = [device newBufferWithLength:cases.size() * sizeof(probe::Result)
                                              options:MTLResourceStorageModeShared];
            require(input && output, "regression alpha buffers");
            auto command = [MetalGeometryAccess::queue(scene) commandBuffer];
            auto encoder = [command computeCommandEncoder];
            require(command && encoder, "regression alpha command");
            [encoder setComputePipelineState:pipeline];
            MetalGeometryAccess::bind(scene, encoder, 0);
            [encoder setBuffer:input offset:0 atIndex:1];
            [encoder setBuffer:output offset:0 atIndex:2];
            const Word count = cases.size();
            [encoder setBytes:&count length:sizeof(count) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
            [encoder endEncoding];
            require(metal::complete(command, "Triangle regression alpha probe").empty(),
                    "regression alpha completion");
            auto *results = static_cast<const probe::Result *>(output.contents);
            require(results != nullptr, "regression alpha output contents");
            const auto callbacks = [&](unsigned i) {
                return "regression case " + std::to_string(i) + " triangle callbacks: mask=" +
                       std::to_string(results[i].triangle_primitive_mask) +
                       ", count=" + std::to_string(results[i].triangle_calls);
            };
            const auto accepted_mask = results[0].triangle_primitive_mask;
            require(results[0].hit.kind == wire::triangle && results[0].hit.primitive == 601 &&
                        (accepted_mask & 1u) != 0 && (accepted_mask & ~3u) == 0 &&
                        results[0].triangle_calls >=
                            static_cast<Word>(std::popcount(accepted_mask)),
                    callbacks(0) + " nearest true hit and only expected primitive IDs");
            require(results[1].hit.kind == wire::miss && results[1].triangle_calls == 2 &&
                        results[1].triangle_primitive_mask == 3,
                    callbacks(1) + " both distinct true hits rejected without reset");
            const auto one_allowed = [](Word mask) { return mask == 1 || mask == 2; };
            require(results[2].hit.kind == wire::invalid_hit && results[2].triangle_calls == 1 &&
                        one_allowed(results[2].triangle_primitive_mask),
                    callbacks(2) + " invalid true-hit alpha stays visible");
            require(results[3].hit.kind == wire::triangle &&
                        (results[3].hit.primitive == 601 || results[3].hit.primitive == 609) &&
                        results[3].triangle_calls == 1 &&
                        one_allowed(results[3].triangle_primitive_mask) &&
                        results[3].triangle_primitive_mask ==
                            (results[3].hit.primitive == 601 ? 1u : 2u),
                    callbacks(3) + " shadow terminates at observed validated true hit");
        }
    }
}

// Read the packed little-endian byte stream directly, following MurmurHash64A.
// This keeps the oracle independent of the shader's pairing of Metal vector components.
Word expected_alpha_hash(const probe::HashCase &input) {
    const auto bytes = std::bit_cast<std::array<std::uint8_t, 28>>(input);
    constexpr std::uint64_t multiplier = 0xc6a4a7935bd1e995ull;
    std::uint64_t state = bytes.size() * multiplier;
    const auto load = [&](std::size_t begin, std::size_t count) {
        std::uint64_t result = 0;
        for (std::size_t i = count; i != 0; --i) {
            result = (result << 8) + bytes[begin + i - 1];
        }
        return result;
    };
    for (std::size_t i = 0; i < 24; i += 8) {
        const auto chunk = load(i, 8) * multiplier;
        state = (state ^ ((chunk ^ (chunk >> 47)) * multiplier)) * multiplier;
    }
    state = (state ^ load(24, 4)) * multiplier;
    state = (state ^ (state >> 47)) * multiplier;
    const auto low_bits = static_cast<Word>(state ^ (state >> 47));
    return std::bit_cast<Word>(static_cast<float>(low_bits) * 0x1p-32f);
}

void compare_alpha_hash(id<MTLDevice> device, id<MTLLibrary> library) {
    std::string diagnostic;
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_alpha_hash_probe", diagnostic);
    require(pipeline != nil, diagnostic);

    constexpr std::array bit_patterns{
        0x00000000u, 0x80000000u,                           // Signed zeros.
        0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu, // Signed subnormals.
        0x00800000u, 0x80800000u, 0x7f7fffffu, 0xff7fffffu, // Finite extremes.
        0x3f800000u, 0xbf800000u, 0x3f800001u,
    };
    std::vector<probe::HashCase> cases;
    for (std::size_t i = 0; i < 7; ++i) {
        for (const Word bits : bit_patterns) {
            probe::HashCase c{{0x3fc12345u, 0xc0dde011u, 0x44002010u, 0x3fccaa10u, 0x80000000u,
                               0xbf123456u, 0x3f018000u}};
            c.words[i] = bits;
            cases.push_back(c);
        }
    }
    std::mt19937 rng(0x7eedu);
    for (int i = 0; i < 1024; ++i) {
        probe::HashCase c;
        for (Word &word : c.words) {
            word = static_cast<Word>(rng());
            // Preserve every finite representation. Convert non-finite exponents to finite.
            if ((word & 0x7f800000u) == 0x7f800000u) {
                word ^= 0x00800000u;
            }
        }
        cases.push_back(c);
    }

    const auto input = [device newBufferWithBytes:cases.data()
                                           length:cases.size() * sizeof(probe::HashCase)
                                          options:MTLResourceStorageModeShared];
    const auto output = [device newBufferWithLength:cases.size() * sizeof(Word)
                                            options:MTLResourceStorageModeShared];
    const auto queue = [device newCommandQueue];
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(input && output && encoder, "create alpha hash probe resources");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    const auto count = static_cast<Word>(cases.size());
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(cases.size(), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    const auto error = metal::complete(command, "Alpha hash probe");
    require(error.empty(), error);
    const auto *results = static_cast<const Word *>(output.contents);
    require(results != nullptr, "alpha hash output contents");
    for (std::size_t i = 0; i < cases.size(); ++i) {
        require(results[i] == expected_alpha_hash(cases[i]),
                "alpha hash case " + std::to_string(i));
    }
    std::cout << cases.size() << " exact CPU and Metal alpha hash comparisons\n";
}

} // namespace

int main(int argc, char *argv[]) {
    @autoreleasepool {
        try {
            bool allow_no_device = false;
            bool allow_no_ray_tracing = false;
            for (int i = 1; i < argc; ++i) {
                const std::string_view flag = argv[i];
                if (flag == "--allow-no-device" && !allow_no_device) {
                    allow_no_device = true;
                } else if (flag == "--allow-no-ray-tracing" && !allow_no_ray_tracing) {
                    allow_no_ray_tracing = true;
                } else {
                    throw std::runtime_error("unexpected masked intersection test flag");
                }
            }
            const auto device = MTLCreateSystemDefaultDevice();
            if (!device && allow_no_device) {
                std::cout << "SKIP Masked intersection probe: no Metal device available\n";
                return 77;
            }
            require(device != nil, "Masked intersection probe requires a Metal device");
            if (!device.supportsRaytracing && allow_no_ray_tracing) {
                std::cout
                    << "SKIP Masked intersection probe: no Metal compute ray tracing available\n";
                return 77;
            }
            require(device.supportsRaytracing,
                    "Masked intersection probe requires Metal ray tracing");
            std::string error;
            const auto library = metal::load_library(device, {}, error);
            require(library != nil, error);
            const auto pipeline =
                metal::load_pipeline(device, library, @"rypath_masked_intersection_probe", error);
            require(pipeline != nil, error);
            observe(device, pipeline, false);
            observe(device, pipeline, true);
            observe_regression(device, pipeline);
            compare_alpha_hash(device, library);
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
        return 0;
    }
}
