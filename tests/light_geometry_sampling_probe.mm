// SPDX-License-Identifier: GPL-3.0-only
#include "canonical_full_context_golden.h"
#include "checkpoint_contract.h"
#include "geometry_wire.h"
#include "light_geometry_sampling_probe_wire.h"
#include "light_geometry_sampling_shared.h"
#include "metal_execution.hpp"
#include "metal_geometry_access.hpp"
#include "offset_rounding_cases.hpp"
#include "packet_wire.h"
#include "sobol_shared.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace rypath;
namespace wire = render_wire;
namespace probe = light_geometry_probe;
namespace math = rypath_light_geometry_math;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename Range> id<MTLBuffer> buffer(id<MTLDevice> device, const Range &values) {
    const auto bytes = std::as_bytes(std::span(values));
    return [device newBufferWithBytes:bytes.data()
                               length:bytes.size()
                              options:MTLResourceStorageModeShared];
}
struct Case {
    std::string name;
    wire::Point reference;
    wire::Point normal{};
    unsigned emitter;
    bool cone = false;
    float solid_angle = 0;
};
wire::Instance instance() {
    wire::Instance result{};
    result.matrix[0] = result.matrix[5] = result.matrix[10] = 1;
    result.normal_begin = wire::no_normals;
    result.analytic_begin = UINT32_MAX;
    return result;
}
void offset_rounding(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue) {
    std::vector<probe::OffsetQuery> queries;
    for (const auto &test : offset_rounding_test::cases)
        for (wire::Word axis = 0; axis < 3; ++axis)
            for (wire::Word negative : {0u, 1u})
                for (wire::Word move : {0u, 1u})
                    queries.push_back({test.input, axis, negative, move});
    constexpr wire::Word sentinel = 0xcdfacadeu;
    std::vector<probe::OffsetResult> initial(
        queries.size() + 1, {{sentinel, sentinel, sentinel}, sentinel, sentinel, sentinel});
    std::string diagnostic;
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_probe_offset_rounding", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto input = buffer(device, queries), output = buffer(device, initial);
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(input != nil && output != nil && input.contents && output.contents && command != nil &&
                encoder != nil,
            "Offset rounding probe allocation failed");
    const auto count = static_cast<wire::Word>(queries.size());
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(queries.size() + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Offset rounding probe");
    require(diagnostic.empty(), diagnostic);
    require(std::memcmp(input.contents, queries.data(),
                        queries.size() * sizeof(probe::OffsetQuery)) == 0,
            "Offset rounding query changed");
    const auto *result = static_cast<const probe::OffsetResult *>(output.contents);
    require(std::memcmp(result + queries.size(), initial.data() + queries.size(),
                        sizeof(probe::OffsetResult)) == 0,
            "Offset rounding guard overwritten");
    std::size_t at = 0;
    for (const auto &test : offset_rounding_test::cases)
        for (wire::Word axis = 0; axis < 3; ++axis)
            for (bool negative : {false, true})
                for (bool move : {false, true}) {
                    const auto &value = result[at++];
                    const auto expected = negative ? test.negative : test.positive;
                    const auto valid = negative ? test.negative_valid : test.positive_valid;
                    require(value.magic == 0x4f464631u && value.scalar_bits == expected &&
                                value.scalar_valid == (valid ? 1u : 0u),
                            "Offset shared helper boundary word " + std::to_string(test.input));
                    const std::array<wire::Word, 3> unchanged{0x3e800000u, 0xbf000000u,
                                                              0x3f800000u};
                    for (unsigned i = 0; i < 3; ++i)
                        require(value.rounded[i] ==
                                    (i == axis ? (move ? expected : test.input) : unchanged[i]),
                                "Actual round_out axis/sign/no-round word " +
                                    std::to_string(test.input));
                }
    std::cout << "Offset rounding Metal: " << queries.size()
              << " exact axis/sign/no-round cases passed\n";
}

void density_conversion(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue) {
    struct DensityCase {
        std::string name;
        probe::DensityQuery query;
        wire::Word expected;
        bool nan;
        unsigned
            edge; // 0 coincident,1 underflow,2 length2,3 overflow,4 nonfinite,5 infinite target.
    };
    std::vector<DensityCase> cases;
    const auto add = [&](std::string name, wire::Word pdf, wire::Point from, wire::Point to,
                         wire::Word infinite, wire::Word expected, bool nan, unsigned edge) {
        cases.push_back(
            {std::move(name), {from, to, {0, 0, 1}, pdf, infinite}, expected, nan, edge});
    };
    constexpr wire::Word one = 0x3f800000u, negative_zero = 0x80000000u;
    const wire::Point zero{}, tiny{0, 0, 0x1p-80f}, ordinary{0, 0, 2};
    for (const wire::Word pdf : {0x7fc00001u, 0x7f800000u, 0xff800000u, 0xbf800000u}) {
        const auto label = "Invalid input " + std::to_string(pdf);
        add(label + " coincident", pdf, zero, zero, 0, 0, true, 0);
        add(label + " squared underflow", pdf, zero, tiny, 0, 0, true, 1);
        add(label + " infinite target", pdf, zero, zero, 1, 0, true, 5);
    }
    for (const wire::Word pdf : {0u, one, negative_zero}) {
        add("Valid coincident " + std::to_string(pdf), pdf, zero, zero, 0, 0, false, 0);
        add("Valid squared underflow " + std::to_string(pdf), pdf, zero, tiny, 0, 0, false, 1);
        add("Valid infinite target " + std::to_string(pdf), pdf, zero, zero, 1, pdf, false, 5);
        // Distance2=4 and cosine=1: the exact expected result is pdf/4.
        // Explicit words, rather than a host reimplementation of convert_density.
        add("Valid ordinary " + std::to_string(pdf), pdf, zero, ordinary, 0,
            pdf == one ? 0x3e800000u : pdf, false, 2);
    }
    const float infinity = std::bit_cast<float>(wire::Word{0x7f800000u});
    const float nan = std::bit_cast<float>(wire::Word{0x7fc00001u});
    const float maximum = std::bit_cast<float>(wire::Word{0x7f7fffffu});
    for (const wire::Word pdf : {0u, one}) {
        add("Infinite edge " + std::to_string(pdf), pdf, zero, {0, 0, infinity}, 0, 0, true, 4);
        add("NaN edge " + std::to_string(pdf), pdf, zero, {0, 0, nan}, 0, 0, true, 4);
        add("Squared overflow " + std::to_string(pdf), pdf, zero, {0, 0, 0x1p100f}, 0, 0, true, 3);
        add("Subtraction overflow " + std::to_string(pdf), pdf, {0, 0, -maximum}, {0, 0, maximum},
            0, 0, true, 3);
    }
    std::vector<probe::DensityQuery> queries;
    for (const auto &test : cases)
        queries.push_back(test.query);
    constexpr wire::Word sentinel = 0xcdfacadeu;
    std::vector<probe::DensityResult> initial(cases.size() + 1,
                                              {sentinel, sentinel, sentinel, sentinel});
    std::string diagnostic;
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_probe_density_conversion", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto input = buffer(device, queries), output = buffer(device, initial);
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(input != nil && output != nil && input.contents && output.contents && command != nil &&
                encoder != nil,
            "Density conversion probe allocation failed");
    const auto count = static_cast<wire::Word>(cases.size());
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(cases.size() + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Density conversion probe");
    require(diagnostic.empty(), diagnostic);
    require(std::memcmp(input.contents, queries.data(),
                        queries.size() * sizeof(probe::DensityQuery)) == 0,
            "Density conversion input changed");
    const auto *result = static_cast<const probe::DensityResult *>(output.contents);
    require(std::memcmp(result + cases.size(), initial.data() + cases.size(),
                        sizeof(probe::DensityResult)) == 0,
            "Density conversion guard overwritten");
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto &test = cases[i];
        const auto &value = result[i];
        require(value.magic == 0x44454e31u, test.name + " missing result");
        require(test.nan ? std::isnan(std::bit_cast<float>(value.pdf_bits))
                         : value.pdf_bits == test.expected,
                test.name + " density policy mismatch, word " + std::to_string(value.pdf_bits));
        if (test.edge == 0 || test.edge == 1)
            require(value.distance_squared_bits == 0 && value.nonzero_edge == test.edge,
                    test.name + " did not exercise declared coincident/underflow edge");
        if (test.edge == 2)
            require(value.distance_squared_bits == 0x40800000u && value.nonzero_edge == 1,
                    test.name + " ordinary distance2 must be exactly4");
        if (test.edge == 3 || test.edge == 4)
            require(!std::isfinite(std::bit_cast<float>(value.distance_squared_bits)) &&
                        value.nonzero_edge == 1,
                    test.name + " did not exercise nonfinite geometry arithmetic");
    }
    std::cout << "Density conversion Metal: " << cases.size() << " exact policy cases passed\n";
}

// Deterministic represented-path contract, not a rendered-mean/unbiasedness test.
using CanonicalVector = std::array<float, 3>;
CanonicalVector canonical_unit(CanonicalVector v) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (auto &x : v)
        x /= length;
    return v;
}
CanonicalVector canonical_cross(CanonicalVector a, CanonicalVector b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
wire::Point canonical_point(CanonicalVector a) { return {a[0], a[1], a[2]}; }
void canonical_mis(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue) {
    std::array<probe::CanonicalQuery, probe::canonical_contexts> queries{};
    std::array<wire::Parameters, probe::canonical_contexts> parameters{};
    unsigned at = 0;
    for (unsigned frame = 0; frame < 2; ++frame)
        for (unsigned angle = 0; angle < 2; ++angle)
            for (unsigned zero = 0; zero < 2; ++zero) {
                auto &q = queries[at];
                auto &p = parameters[at++];
                const std::array<float, 9> axes =
                    frame ? std::array<float, 9>{.6f, .8f, 0, -.64f, .48f, .6f, .48f, -.36f, .8f}
                          : std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1};
                std::copy(axes.begin(), axes.end(), q.frame);
                const CanonicalVector wo =
                    angle ? CanonicalVector{.9659258262890683f, 0, .25881904510252074f}
                          : CanonicalVector{-.00149562897f, -.04186568409f, .999122083f};
                const auto wi = canonical_unit({-.3f, .2f, 1});
                std::copy(wo.begin(), wo.end(), q.wo);
                std::copy(wi.begin(), wi.end(), q.wi);
                q.frame_id = frame;
                q.angle_id = angle;
                q.zero_base = zero;
                q.material_seed = 24;
                CanonicalVector view{};
                for (unsigned i = 0; i < 3; ++i)
                    view[i] = wo[0] * q.frame[i] + wo[1] * q.frame[3 + i] + wo[2] * q.frame[6 + i];
                view = canonical_unit(view);
                CanonicalVector camera = view, normal = view;
                for (auto &x : camera)
                    x *= 2;
                for (auto &x : normal)
                    x = -x;
                const auto right = canonical_unit(canonical_cross(normal, {0, 1, 0}));
                const auto up = canonical_unit(canonical_cross(right, normal));
                CanonicalVector forward{};
                for (unsigned i = 0; i < 3; ++i)
                    forward[i] = normal[i] - .5f * right[i] - .5f * up[i];
                p.origin = canonical_point(camera);
                p.forward = canonical_point(forward);
                p.right = canonical_point(right);
                p.up = canonical_point(up);
                p.camera_normal = canonical_point(normal);
                p.camera_image_area = p.camera_plane_depth = 1;
                p.width = p.height = p.pixel_count = p.lane_count = 1;
                p.model = wire::pbrt_model;
                p.filter_kind = wire::box_filter;
                p.filter_x_radius = p.filter_y_radius = .5f;
                p.sampler_kind = wire::independent_sampler;
                p.seed_low = 19317;
                p.material_seed = 24;
                p.packet_transport = 1;
                p.bidirectional = 1;
                p.max_depth = 1;
                p.emergency_limit = 64;
                p.light_sampler = 2;
                p.light_count = 1;
                p.environment_light = 0;
                p.infinite_light_count = 1;
                p.scene_radius = 2;
                p.material_data_words = 16;
                p.sample_low = at - 1;
            }
    auto constants = [[MTLFunctionConstantValues alloc] init];
    for (NSUInteger i = 0; i < 19; ++i) {
        bool enabled = i == 6 || i == 7 || i == 17;
        [constants setConstantValue:&enabled type:MTLDataTypeBool atIndex:i];
    }
    unsigned hint = 0;
    [constants setConstantValue:&hint type:MTLDataTypeUInt atIndex:19];
    const auto input = buffer(device, queries), params = buffer(device, parameters);
    require(input && params && input.contents && params.contents, "Canonical input allocation");
    // The existing test owner retains diagnostics on stdout. No acceleration
    // structures are dispatched; Resources{} legality needs a root compile check.
    auto dispatch = [&](NSString *name, std::size_t rows, std::size_t row_bytes) {
        std::string diagnostic;
        const auto pipeline =
            metal::load_pipeline(device, library, name, diagnostic, nil, constants);
        require(pipeline != nil, diagnostic);
        constexpr std::size_t guard = 64;
        const std::size_t scratch_bytes =
            rows * probe::canonical_scratch_slots * packet_wire::vertex_bytes;
        [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
            [device newBufferWithLength:rows * row_bytes + guard
                                options:MTLResourceStorageModeShared];
        [[clang::objc_precise_lifetime]] id<MTLBuffer> scratch =
            [device newBufferWithLength:scratch_bytes + guard options:MTLResourceStorageModeShared];
        require(output && scratch && output.contents && scratch.contents,
                "Canonical output allocation");
        std::memset(output.contents, 0xcd, output.length);
        std::memset(scratch.contents, 0xa5, scratch.length);
        const auto command = [queue commandBuffer];
        const auto encoder = [command computeCommandEncoder];
        require(command && encoder, "Canonical command allocation");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:params offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBuffer:scratch offset:0 atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(rows + 1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
        [encoder endEncoding];
        diagnostic = metal::complete(command, "Canonical MIS");
        std::cout << name.UTF8String << " command=" << unsigned(command.status)
                  << " diagnostic=" << diagnostic << '\n';
        require(diagnostic.empty(), diagnostic);
        require(std::memcmp(input.contents, queries.data(), sizeof(queries)) == 0 &&
                    std::memcmp(params.contents, parameters.data(), sizeof(parameters)) == 0,
                "Canonical immutable inputs");
        const auto *bytes = static_cast<const unsigned char *>(output.contents);
        for (std::size_t i = rows * row_bytes; i < output.length; ++i)
            require(bytes[i] == 0xcd, "Canonical output guard");
        bytes = static_cast<const unsigned char *>(scratch.contents);
        for (std::size_t i = scratch_bytes; i < scratch.length; ++i)
            require(bytes[i] == 0xa5, "Canonical scratch guard");
        return output;
    };
    const auto word = [](float f) { return std::bit_cast<wire::Word>(f); };
    const auto same_point = [&](wire::Point a, wire::Point b) {
        return word(a.x) == word(b.x) && word(a.y) == word(b.y) && word(a.z) == word(b.z);
    };
    constexpr std::array<float, 4> rho{.13526731729507446f, .08804553747177124f,
                                       .19378170371055603f, .16280370950698853f};
    const auto authored = [&](const probe::CanonicalProtected &v, unsigned context, bool camera,
                              wire::Word id, wire::Word prev, wire::Word next, bool delta) {
        bool good = v.path_id == id && v.prev_id == prev && v.next_id == next &&
                    v.pair_valid == !camera && v.delta == delta &&
                    v.active_mask == (camera ? 0u : 15u);
        for (unsigned lane = 0; lane < 4; ++lane)
            good =
                good && word(v.beta[lane]) == word(1.f) && v.kind[lane] == (camera ? 0u : 5u) &&
                word(v.rho[lane]) == word(camera || queries[context].zero_base ? 0.f : rho[lane]) &&
                word(v.top_rho[lane]) == 0;
        if (camera)
            good = good && same_point(v.sampled_wo, {}) && same_point(v.sampled_wi, {});
        return good;
    };
    [[clang::objc_precise_lifetime]] id<MTLBuffer> partition = dispatch(
        @"rypath_probe_canonical_partition", queries.size(), sizeof(probe::CanonicalPartition));
    const auto *result = static_cast<const probe::CanonicalPartition *>(partition.contents);
    bool partitions_ok = true;
    unsigned common_count = 0;
    for (unsigned i = 0; i < queries.size(); ++i) {
        const auto &r = result[i];
        bool common = true;
        bool good = r.magic == 0x434d5031u && r.context == i && r.status == wire::success &&
                    r.vertex_bytes > 0 && r.vertex_bytes <= packet_wire::vertex_bytes &&
                    r.vertex_alignment <= 16 && r.renderer == checkpoint_contract::renderer &&
                    r.active_mask == 15 && r.compatible == 1;
        for (unsigned lane = 0; lane < 4; ++lane)
            good = good && word(r.rho[lane]) == word(queries[i].zero_base ? 0.f : rho[lane]) &&
                   word(r.top_rho[lane]) == 0;
        const bool stored_camera = authored(r.stored_camera, i, false, 1, 0, 2, false);
        const bool stored_light =
            authored(r.stored_light, i, false, 0x80000001u, 0x80000000u, 0x80000002u, false);
        good = good && stored_camera && stored_light;
        std::cout << "Canonical post-MIS context=" << i
                  << " authored camera/light=" << stored_camera << '/' << stored_light;
        for (unsigned lane = 0; lane < 4; ++lane)
            std::cout << " rho" << lane << '=' << word(r.stored_camera.rho[lane]) << '/'
                      << word(r.stored_light.rho[lane]);
        std::cout << '\n';
        double sum = 0;
        for (unsigned s = 0; s < 3; ++s) {
            const auto &v = r.strategy[s];
            const auto &first = r.strategy[0];
            const bool factors = word(v.C) == word(first.C) && word(v.S) == word(first.S) &&
                                 word(v.O) == word(first.O) && word(v.L) == word(first.L);
            const bool keys = same_point(v.canonical_wo, first.canonical_wo) &&
                              same_point(v.canonical_wi, first.canonical_wi);
            common = common && factors && keys;
            // Independent double balance weights use this representation's own
            // measured factors: never copy the first row or renormalize weights.
            const std::array<double, 3> density{double(v.C) * v.S, double(v.C) * v.O,
                                                double(v.L) * v.O};
            const double total = density[0] + density[1] + density[2];
            const double expected = density[s] / total;
            sum += v.weight;
            const bool domain = std::isfinite(v.C) && v.C > 0 && std::isfinite(v.S) && v.S > 0 &&
                                std::isfinite(v.O) && v.O > 0 && std::isfinite(v.L) && v.L > 0 &&
                                std::isfinite(v.weight) && v.weight >= 0 && v.weight <= 1 &&
                                std::isfinite(expected);
            good = good && v.status == wire::success && v.s == s && v.t == 3 - s && domain &&
                   word(v.S) == word(v.expected_S) &&
                   std::abs(double(v.weight) - expected) <= 2e-5 && v.delta[0] == 0 &&
                   v.delta[1] == 0 && v.delta[2] == 0;
            const std::array<wire::Word, 3> ids =
                s == 0   ? std::array<wire::Word, 3>{0, 1, 2}
                : s == 1 ? std::array<wire::Word, 3>{0, 1, 0xffffffffu}
                         : std::array<wire::Word, 3>{0xffffffffu, 0x80000001u, 0x80000000u};
            for (unsigned k = 0; k < 3; ++k)
                good = good && v.ids[k] == ids[k];
            std::cout << "Canonical context=" << i << " strategy=" << s << " status=" << v.status
                      << " C/S/O/L=" << word(v.C) << '/' << word(v.S) << '/' << word(v.O) << '/'
                      << word(v.L) << " wo=" << word(v.canonical_wo.x) << ','
                      << word(v.canonical_wo.y) << ',' << word(v.canonical_wo.z)
                      << " wi=" << word(v.canonical_wi.x) << ',' << word(v.canonical_wi.y) << ','
                      << word(v.canonical_wi.z) << " weight=" << v.weight
                      << " independent=" << expected << " common=" << factors << '/' << keys
                      << '\n';
        }
        common_count += common;
        partitions_ok = partitions_ok && good && common && std::abs(sum - 1) <= 6e-5;
        std::cout << "Canonical context=" << i << " sum=" << sum << " schema=" << good
                  << " common=" << common << '\n';
    }
    std::cout << "Canonical common contexts=" << common_count << "/8\n";
    // Old-source negative control fails here, after all8 contexts are logged and
    // before loading a finalizer entrypoint unavailable in the old source.
    require(partitions_ok, "Canonical represented-path partition contract");
    [[clang::objc_precise_lifetime]] id<MTLBuffer> finalized =
        dispatch(@"rypath_probe_canonical_finalizer", probe::canonical_helper_rows,
                 sizeof(probe::CanonicalFinalizer));
    const auto *rows = static_cast<const probe::CanonicalFinalizer *>(finalized.contents);
    bool helpers_ok = true, joint_witness = false;
    unsigned called = 0, accepted = 0;
    for (unsigned i = 0; i < probe::canonical_helper_rows; ++i) {
        const auto &r = rows[i];
        const unsigned context = i < 8 ? i : 0, control = i < 8 ? 0 : i - 7;
        const bool should_accept = context % 2 == 0;
        called += r.called;
        accepted += i < 8 && r.joint.accepted;
        bool good = r.magic == 0x434d4631u && r.index == i && r.context == context &&
                    r.control == control && r.status == wire::success &&
                    r.renderer == checkpoint_contract::renderer && r.vertex_bytes > 0 &&
                    r.vertex_bytes <= packet_wire::vertex_bytes && r.vertex_alignment <= 16 &&
                    r.joint.valid == 1 && r.joint.accepted == should_accept &&
                    r.called == should_accept;
        if (should_accept) {
            good = good && r.valid == 1 && r.evaluation_valid == 1 && r.joint.delta == 0 &&
                   r.joint.pdf > 0 && std::isfinite(r.joint.pdf) &&
                   std::isfinite(r.joint.reverse_pdf) && r.joint.reverse_pdf >= 0 &&
                   std::isfinite(r.joint.eta) && r.joint.eta > 0 &&
                   std::memcmp(&r.before, &r.after, sizeof(r.before)) == 0 &&
                   word(r.forward) == word(r.expected_forward) &&
                   word(r.reverse) == word(r.expected_reverse) && std::isfinite(r.forward) &&
                   r.forward >= 0 && r.prior_forward == -2 && r.prior_reverse == -3 &&
                   r.count == (control == 2 ? 1u : 2u) && r.mode == (control == 1 ? 1u : 0u);
            const bool camera = control == 2;
            const auto absolute = [&](const probe::CanonicalProtected &v) {
                return authored(v, context, camera, camera ? 0u : 1u, 0, camera ? 0u : 2u,
                                control == 3) &&
                       (camera ||
                        (same_point(v.sampled_wi, r.joint.wi) &&
                         same_point(v.sampled_wo, result[context].stored_camera.sampled_wo)));
            };
            good = good && absolute(r.before) && absolute(r.after);
            if (control == 3)
                good = good && word(r.forward) == 0 && word(r.reverse) == 0;
            else if (control == 2)
                good = good && word(r.reverse) == word(-3.f);
            else
                good = good && std::isfinite(r.reverse) && r.reverse >= 0;
            bool nonzero = false;
            for (unsigned lane = 0; lane < 4; ++lane) {
                good = good && std::isfinite(r.joint.weight[lane]) && r.joint.weight[lane] >= 0 &&
                       word(r.next_beta[lane]) == word(r.joint.weight[lane]) &&
                       std::isfinite(r.f_cos_over_proxy[lane]);
                nonzero = nonzero || r.joint.weight[lane] > 0;
                joint_witness =
                    joint_witness || word(r.joint.weight[lane]) != word(r.f_cos_over_proxy[lane]);
            }
            good = good && nonzero;
        } else {
            // Rejection publishes zero joint weight and no continuation record.
            // Rejected wi/pdf are not constrained to an undocumented zero value.
            const probe::CanonicalProtected empty{};
            good = good && r.valid == 0 && r.count == 0 && r.mode == 0 && r.evaluation_valid == 0 &&
                   std::memcmp(&r.before, &empty, sizeof(empty)) == 0 &&
                   std::memcmp(&r.after, &empty, sizeof(empty)) == 0 && word(r.forward) == 0 &&
                   word(r.reverse) == 0 && word(r.expected_forward) == 0 &&
                   word(r.expected_reverse) == 0 && word(r.prior_forward) == 0 &&
                   word(r.prior_reverse) == 0;
            for (unsigned lane = 0; lane < 4; ++lane)
                good = good && word(r.joint.weight[lane]) == 0 && word(r.next_beta[lane]) == 0 &&
                       word(r.f_cos_over_proxy[lane]) == 0;
        }
        helpers_ok = helpers_ok && good;
        std::cout << "Canonical helper=" << i << " context=" << context << " control=" << control
                  << " status=" << r.status << " accepted=" << r.joint.accepted
                  << " called=" << r.called << " valid=" << r.valid
                  << " forward=" << word(r.forward) << '/' << word(r.expected_forward)
                  << " reverse=" << word(r.reverse) << '/' << word(r.expected_reverse)
                  << " protected=" << (std::memcmp(&r.before, &r.after, sizeof(r.before)) == 0)
                  << " checks=" << good;
        for (unsigned lane = 0; lane < 4; ++lane)
            std::cout << " rho" << lane << '=' << word(r.before.rho[lane]) << '/'
                      << word(r.after.rho[lane]) << " joint/next" << lane << '='
                      << word(r.joint.weight[lane]) << '/' << word(r.next_beta[lane]);
        std::cout << '\n';
    }
    std::cout << "Canonical joint accepted=" << accepted << "/4 helper calls=" << called
              << "/7 joint witness=" << joint_witness << '\n';
    require(helpers_ok && accepted == 4 && called == 7 && joint_witness,
            "Canonical finalizer/joint protection contract");
}

// Full-context reproduction remains independent of the earlier partition gate.
namespace full_context_test {
namespace c = strategy_context21;
namespace w = rypath::render_wire;
using V = std::array<float, 3>;
using Words = std::array<w::Word, c::vertex_fields>;
w::Word bits(float x) { return std::bit_cast<w::Word>(x); }
float value(w::Word x) { return std::bit_cast<float>(x); }
V unit(V v) {
    float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (auto &x : v)
        x /= n;
    return v;
}
V cross(V a, V b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
w::Point point(V v) { return {v[0], v[1], v[2]}; }
V world(const c::Query &q, V v) {
    V r{};
    for (unsigned i = 0; i < 3; ++i)
        r[i] = v[0] * q.frame[i] + v[1] * q.frame[3 + i] + v[2] * q.frame[6 + i];
    return r;
}
Words expected_vertex(const c::Result &r, const w::Parameters &p, unsigned slot) {
    Words e{};
    const auto *out = r.words;
    const auto copy = [&](unsigned at, const w::Word *from, unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            e[at + i] = from[i];
    };
    const auto xyz = [&](unsigned at, w::Point v) {
        e[at] = bits(v.x);
        e[at + 1] = bits(v.y);
        e[at + 2] = bits(v.z);
    };
    for (unsigned i = 0; i < 4; ++i)
        e[c::beta_begin + i] = bits(1.f);
    e[218] = 1; // connectible; all other unset source members are exact +0/default.
    if (slot == 0) {
        xyz(0, p.origin);
        xyz(6, p.camera_normal);
        xyz(12, p.camera_normal);
        return e;
    }
    if (slot == 2 || slot == 3) {
        copy(0, out + 33, 3);
        for (unsigned i = 0; i < 3; ++i)
            e[6 + i] = e[12 + i] = e[c::wo_begin + i] = bits(-value(out[33 + i]));
        e[c::pdf_begin] = out[slot == 2 ? 128 + 6 : 128 + 160 + 7];
        e[201] = 3;
        e[207] = slot == 2 ? 2u : 0x80000000u;
        return e;
    }
    for (unsigned i = 0; i < 3; ++i)
        e[6 + i] = e[9 + i] = e[12 + i] = out[42 + i];
    copy(36, out + 36, 9);
    constexpr float rho[4] = {.13526731729507446f, .08804553747177124f, .19378170371055603f,
                              .16280370950698853f};
    for (unsigned lane = 0; lane < 4; ++lane) {
        const unsigned base = c::prepared_begin + lane * c::lane_fields;
        e[base] = 5;
        e[base + 3] = bits(float(400 + 100 * lane));
        e[base + 13] = bits(1.5f);
        e[base + 14] = e[base + 15] = bits(.10201960802078247f);
        e[base + 23] = bits(out[6] ? 0.f : rho[lane]);
        e[base + 24] = bits(.009999999776482582f);
        e[base + 27] = 10;
        e[base + 28] = 1;
        e[base + 29] = 24;
    }
    e[189] = 15;
    copy(c::wo_begin, out + (slot == 1 ? 30 : 33), 3);
    e[c::pdf_begin] = out[slot == 1 ? 128 + 5 : 128 + 160 + 8];
    e[201] = 1;
    e[206] = 0x1234u;
    e[207] = slot == 1 ? 1u : 0x80000001u;
    e[208] = slot == 1 ? 0u : 0x80000000u;
    e[209] = slot == 1 ? 2u : 0x80000002u;
    e[210] = 1;
    copy(c::sampled_begin, out + (slot == 1 ? 24 : 128 + 160 + 15), 3);
    copy(c::sampled_begin + 3, out + (slot == 1 ? 27 : 128 + 160 + 18), 3);
    return e;
}
bool output_contract(const c::Result &r, unsigned index, const c::Query &query,
                     const w::Parameters &parameters) {
    const auto *q = r.words;
    bool good = q[0] == c::magic && q[1] == index && q[2] == 0 && q[3] == 1 && q[4] == index / 4 &&
                q[5] == (index / 2) % 2 && q[6] == index % 2 && q[7] == 24 && q[8] > 0 &&
                q[8] <= rypath::packet_wire::vertex_bytes && q[9] > 0 && q[9] <= 16 &&
                q[10] == sizeof(w::Parameters) && q[11] == rypath::checkpoint_contract::renderer;
    for (unsigned i = 118; i < 128; ++i)
        good = good && q[i] == 0;
    for (unsigned i = 0; i < 9; ++i)
        good = good && q[36 + i] == bits(query.frame[i]);
    good = good && q[45] == bits(parameters.origin.x) && q[46] == bits(parameters.origin.y) &&
           q[47] == bits(parameters.origin.z);
    const auto finite = [&](unsigned first, unsigned last) {
        for (unsigned i = first; i < last; ++i)
            if (!std::isfinite(value(q[i])))
                return false;
        return true;
    };
    for (const auto range : std::array<std::array<unsigned, 2>, 10>{{{{12, 58}},
                                                                     {{60, 72}},
                                                                     {{73, 82}},
                                                                     {{83, 96}},
                                                                     {{100, 104}},
                                                                     {{109, 117}},
                                                                     {{24, 45}},
                                                                     {{45, 53}},
                                                                     {{53, 58}},
                                                                     {{60, 64}}}})
        good = good && finite(range[0], range[1]);
    for (unsigned start : {16u, 20u, 53u, 64u, 68u, 73u, 83u, 88u, 92u, 109u})
        for (unsigned lane = 0; lane < 4; ++lane)
            good = good && value(q[start + lane]) >= 0;
    good = good && q[59] == 1 && q[58] == (index % 2 == 0 ? 1u : 0u) && q[72] == 0 && q[77] == 0 &&
           q[82] == 1 && q[117] == 0;
    good = good && q[96] == 0 && q[97] == 1 && q[98] == 2 && q[99] == 0xffffffffu &&
           q[104] == 0x1234u && q[105] == 0 && q[106] == index && q[107] == 0 && q[108] == 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        constexpr float rho[4] = {.13526731729507446f, .08804553747177124f, .19378170371055603f,
                                  .16280370950698853f};
        good =
            good && q[100 + lane] == bits(index % 2 ? 0.f : rho[lane]) && q[92 + lane] == bits(1.f);
        good = good && q[109 + lane] == (q[58] ? q[53 + lane] : 0u);
        if (!q[58])
            good = good && q[53 + lane] == 0 && q[64 + lane] == 0 && q[68 + lane] == 0;
    }
    good = good && q[113] == q[114] && q[115] == q[116];
    if (q[117])
        for (unsigned i = 113; i < 117; ++i)
            good = good && q[i] == 0;
    const double ps = value(q[12]), pl = value(q[13]), den = ps * ps + pl * pl;
    good = good && ps > 0 && pl > 0 && std::abs(double(value(q[14])) - ps * ps / den) <= 2e-5 &&
           std::abs(double(value(q[15])) - pl * pl / den) <= 2e-5;
    double sum = 0;
    for (unsigned strategy = 0; strategy < 3; ++strategy) {
        const auto *r = q + 128 + 80 * strategy;
        const auto *first = q + 128;
        good = good && r[0] == strategy && r[1] == 3 - strategy && r[2] == 0 && r[3] == 0 &&
               r[24] == 0x1234u && r[27] == 1;
        for (unsigned i = 36; i < 40; ++i)
            good = good && r[i] == 0;
        for (unsigned i = 59; i < 80; ++i)
            good = good && r[i] == 0;
        for (unsigned i = 4; i < 21; ++i)
            good = good && std::isfinite(value(r[i]));
        for (unsigned i = 28; i < 36; ++i)
            good = good && std::isfinite(value(r[i])) && value(r[i]) >= 0;
        for (unsigned i = 40; i < 54; ++i)
            good = good && std::isfinite(value(r[i]));
        for (unsigned i = 5; i < 15; ++i)
            good = good && r[i] == first[i];
        const double C = value(r[5]), S = value(r[6]), O = value(r[7]), L = value(r[8]);
        const std::array<double, 3> mass{C * S, C * O, L * O};
        const double total = mass[0] + mass[1] + mass[2], weight = value(r[4]);
        good = good && C > 0 && S > 0 && O > 0 && L > 0 && weight >= 0 && weight <= 1 &&
               std::isfinite(mass[strategy] / total) &&
               std::abs(weight - mass[strategy] / total) <= 2e-5 && r[6] == r[50] && r[6] == r[52];
        sum += weight;
        const std::array<w::Word, 3> ids =
            strategy == 0   ? std::array<w::Word, 3>{0, 1, 2}
            : strategy == 1 ? std::array<w::Word, 3>{0, 1, 0xffffffffu}
                            : std::array<w::Word, 3>{0xffffffffu, 0x80000001u, 0x80000000u};
        for (unsigned i = 0; i < 3; ++i)
            good = good && r[21 + i] == ids[i];
        good = good && r[25] == (strategy == 2 ? 0x80000000u : 0u) &&
               r[26] == (strategy == 2 ? 0x80000002u : 2u) &&
               r[54] == (strategy == 0 ? 0u : 0xffffffffu) && r[55] == 0x1234u && r[56] == 0 &&
               r[57] == 0 && r[58] == 0;
        for (unsigned i = 0; i < 6; ++i)
            good = good && r[44 + i] == r[9 + i];
        good = good && r[40] == (strategy == 2 ? r[8] : first[5]) &&
               r[41] == (strategy == 0 ? q[12] : r[7]) && r[42] == 0 && r[43] == 0;
        good = good && r[6] == q[12];
        for (unsigned i = 40; i < 44; ++i)
            good = good && value(r[i]) >= 0;
        for (unsigned i = 50; i < 54; ++i)
            good = good && value(r[i]) >= 0;
    }
    return good && std::abs(sum - 1) <= 6e-5;
}

int run(const std::filesystem::path &probe_library, const std::filesystem::path &geometry_library) {
    std::array<c::Query, c::cases> queries{};
    std::array<w::Parameters, c::cases> parameters{};
    unsigned at = 0;
    for (unsigned frame = 0; frame < 2; ++frame)
        for (unsigned angle = 0; angle < 2; ++angle)
            for (unsigned zero = 0; zero < 2; ++zero) {
                auto &q = queries[at];
                auto &p = parameters[at++];
                const std::array<float, 9> axes =
                    frame ? std::array<float, 9>{.6f, .8f, 0, -.64f, .48f, .6f, .48f, -.36f, .8f}
                          : std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1};
                std::copy(axes.begin(), axes.end(), q.frame);
                const V wo = angle ? V{.9659258262890683f, 0, .25881904510252074f}
                                   : V{-.00149562897f, -.04186568409f, .999122083f};
                const V wi = unit({-.3f, .2f, 1});
                std::copy(wo.begin(), wo.end(), q.wo);
                std::copy(wi.begin(), wi.end(), q.wi);
                q.frame_id = frame;
                q.angle_id = angle;
                q.zero_base = zero;
                q.material_seed = 24;
                auto view = unit(world(q, wo));
                V camera = view;
                for (auto &x : camera)
                    x *= 2;
                V normal = view;
                for (auto &x : normal)
                    x = -x;
                auto right = unit(cross(normal, {0, 1, 0}));
                auto up = unit(cross(right, normal));
                V forward{};
                for (unsigned i = 0; i < 3; ++i)
                    forward[i] = normal[i] - .5f * right[i] - .5f * up[i];
                p.origin = point(camera);
                p.forward = point(forward);
                p.right = point(right);
                p.up = point(up);
                p.camera_normal = point(normal);
                p.camera_image_area = 1;
                p.camera_plane_depth = 1;
                p.width = p.height = p.pixel_count = p.lane_count = 1;
                p.model = w::pbrt_model;
                p.filter_kind = w::box_filter;
                p.filter_x_radius = p.filter_y_radius = .5f;
                p.sampler_kind = w::independent_sampler;
                p.seed_low = 19317;
                p.material_seed = 24;
                p.packet_transport = 1;
                p.bidirectional = 1;
                p.max_depth = 1;
                p.emergency_limit = 64;
                p.light_sampler = 2;
                p.light_count = 1;
                p.environment_light = 0;
                p.infinite_light_count = 1;
                p.scene_radius = 2;
                p.material_data_words = 16;
                p.sample_low = at - 1;
            }
    require(at == c::cases, "Full context fixed case inventory");

    GeometryData geometry;
    geometry.meshes.push_back({});
    geometry.meshes[0].positions = {{-1, -1, -8}, {1, -1, -8}, {0, 1, -8}};
    geometry.meshes[0].triangles = {{0, 1, 2}};
    geometry.instances.emplace_back();
    auto built = build_metal_geometry(geometry, geometry_library);
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
    if (built.status == GeometryStatus::no_ray_tracing) {
        std::cout << "SKIP full context regression: " << built.diagnostic << '\n';
        return 77;
    }
#endif
    require(built.status == GeometryStatus::success && bool(built.scene), built.diagnostic);
    const auto device = MetalGeometryAccess::device(*built.scene);
    const auto queue = MetalGeometryAccess::queue(*built.scene);
    std::string diagnostic;
    const auto library = metal::load_library(device, probe_library, diagnostic);
    require(library != nil, diagnostic);
    auto constants = [[MTLFunctionConstantValues alloc] init];
    for (NSUInteger i = 0; i < 19; ++i) {
        bool enabled = i == 6 || i == 7 || i == 17;
        [constants setConstantValue:&enabled type:MTLDataTypeBool atIndex:i];
    }
    unsigned hint = 0;
    [constants setConstantValue:&hint type:MTLDataTypeUInt atIndex:19];
    const auto pipeline = metal::load_pipeline(device, library, @"strategy_context21_probe",
                                               diagnostic, nil, constants);
    require(pipeline != nil, diagnostic);
    constexpr std::size_t guard = 64;
    constexpr std::size_t scratch_bytes = c::cases * 5 * packet_wire::vertex_bytes;
    std::array<float, 471> sky;
    sky.fill(1);
    std::array<unsigned, 16> metadata{};
    [[clang::objc_precise_lifetime]] id<MTLBuffer> input = buffer(device, queries);
    [[clang::objc_precise_lifetime]] id<MTLBuffer> params = buffer(device, parameters);
    [[clang::objc_precise_lifetime]] id<MTLBuffer> env = buffer(device, sky);
    [[clang::objc_precise_lifetime]] id<MTLBuffer> meta = buffer(device, metadata);
    [[clang::objc_precise_lifetime]] id<MTLBuffer> tables =
        [device newBufferWithLength:sizeof(sobol::Tables) options:MTLResourceStorageModeShared];
    [[clang::objc_precise_lifetime]] id<MTLBuffer> scratch =
        [device newBufferWithLength:scratch_bytes + guard options:MTLResourceStorageModeShared];
    [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
        [device newBufferWithLength:sizeof(c::Result) * c::cases + guard
                            options:MTLResourceStorageModeShared];
    [[clang::objc_precise_lifetime]] id<MTLBuffer> semantic =
        [device newBufferWithLength:sizeof(c::SemanticVertex) * c::cases * 5 + guard
                            options:MTLResourceStorageModeShared];
    require(input && params && env && meta && tables && scratch && output && semantic &&
                input.contents && params.contents && env.contents && meta.contents &&
                tables.contents && scratch.contents && output.contents && semantic.contents,
            "Full context buffers");
    std::memset(tables.contents, 0, tables.length);
    std::memset(scratch.contents, 0xcd, scratch.length);
    std::memset(output.contents, 0xff, output.length);
    std::memset(semantic.contents, 0xa5, semantic.length);
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(command && encoder, "Full context command allocation");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:params offset:0 atIndex:1];
    [encoder setBuffer:env offset:0 atIndex:2];
    [encoder setBuffer:meta offset:0 atIndex:3];
    [encoder setBuffer:tables offset:0 atIndex:4];
    [encoder setBuffer:scratch offset:0 atIndex:5];
    [encoder setBuffer:output offset:0 atIndex:6];
    MetalGeometryAccess::bind(*built.scene, encoder, 7);
    [encoder dispatchThreads:MTLSizeMake(c::cases + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(
                                  std::min(NSUInteger{8}, pipeline.maxTotalThreadsPerThreadgroup),
                                  1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Canonical full context");
    std::cout << "Full context command=" << unsigned(command.status) << " diagnostic=" << diagnostic
              << '\n';
    const auto *result = static_cast<const c::Result *>(output.contents);
    for (unsigned i = 0; i < c::cases; ++i) {
        std::cout << "Full context words case=" << i;
        for (auto word : result[i].words)
            std::cout << ' ' << word;
        std::cout << '\n';
    }
    require(command.status == MTLCommandBufferStatusCompleted && diagnostic.empty(),
            "Full context primary command failed");
    const auto *scratch_start = static_cast<const unsigned char *>(scratch.contents);
    const std::vector<unsigned char> before(scratch_start, scratch_start + scratch.length);
    const auto reader =
        metal::load_pipeline(device, library, @"rypath_probe_canonical_full_readback", diagnostic);
    require(reader != nil, diagnostic);
    const auto read_command = [queue commandBuffer];
    const auto read_encoder = [read_command computeCommandEncoder];
    require(read_command && read_encoder, "Full context readback command allocation");
    [read_encoder setComputePipelineState:reader];
    [read_encoder setBuffer:scratch offset:0 atIndex:0];
    [read_encoder setBuffer:semantic offset:0 atIndex:1];
    [read_encoder dispatchThreads:MTLSizeMake(c::cases * 5 + 1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(
                                      std::min(NSUInteger{8}, reader.maxTotalThreadsPerThreadgroup),
                                      1, 1)];
    [read_encoder endEncoding];
    diagnostic = metal::complete(read_command, "Canonical full context readback");
    std::cout << "Full context readback command=" << unsigned(read_command.status)
              << " diagnostic=" << diagnostic << '\n';
    require(read_command.status == MTLCommandBufferStatusCompleted && diagnostic.empty(),
            "Full context readback failed");
    require(std::memcmp(scratch.contents, before.data(), scratch.length) == 0,
            "Read-only observation changed full scratch");
    require(std::memcmp(input.contents, queries.data(), sizeof(queries)) == 0 &&
                std::memcmp(params.contents, parameters.data(), sizeof(parameters)) == 0 &&
                std::memcmp(env.contents, sky.data(), sizeof(sky)) == 0 &&
                std::memcmp(meta.contents, metadata.data(), sizeof(metadata)) == 0,
            "Full context immutable inputs");
    const auto guarded = [&](id<MTLBuffer> b, std::size_t bytes, unsigned char sentinel) {
        const auto *p = static_cast<const unsigned char *>(b.contents);
        for (std::size_t i = bytes; i < b.length; ++i)
            require(p[i] == sentinel, "Full context guard");
    };
    guarded(output, sizeof(c::Result) * c::cases, 0xff);
    guarded(scratch, scratch_bytes, 0xcd);
    guarded(semantic, sizeof(c::SemanticVertex) * c::cases * 5, 0xa5);
    const auto *table_bytes = static_cast<const unsigned char *>(tables.contents);
    for (NSUInteger i = 0; i < tables.length; ++i)
        require(table_bytes[i] == 0, "Full context immutable sampler tables");
    const auto *vertices = static_cast<const c::SemanticVertex *>(semantic.contents);
    bool good = true, joint_witness = false, schemas_ok = true, early_ok = true, headers_ok = true;
    unsigned bad_physical = 0, wrong_rho = 0, typed_errors = 0, other_typed_errors = 0,
             accepted = 0;
    for (unsigned i = 0; i < c::cases; ++i) {
        const auto &r = result[i];
        const bool schema = output_contract(r, i, queries[i], parameters[i]);
        bool row = schema;
        schemas_ok = schemas_ok && schema;
        accepted += r.words[58];
        for (unsigned lane = 0; lane < 4; ++lane) {
            const bool early =
                r.words[16 + lane] == canonical_full_golden::physical[i][0][lane] &&
                r.words[20 + lane] == canonical_full_golden::physical[i][0][4 + lane];
            early_ok = early_ok && early;
            row = row && early;
            joint_witness =
                joint_witness || (r.words[58] && r.words[53 + lane] != r.words[64 + lane]);
        }
        for (unsigned strategy = 0; strategy < 3; ++strategy) {
            const auto *physical = r.words + 128 + 80 * strategy + 28;
            const auto &expected = canonical_full_golden::physical[i][strategy == 2 ? 1 : 0];
            bool exact = true;
            for (unsigned word = 0; word < 8; ++word)
                exact = exact && physical[word] == expected[word];
            if (i % 2 == 0 && !exact)
                ++bad_physical;
            row = row && exact;
            std::cout << "Full context physical case=" << i << " strategy=" << strategy
                      << " exact=" << exact << '\n';
        }
        for (unsigned slot = 0; slot < 5; ++slot) {
            const auto &v = vertices[i * 5 + slot];
            const auto expected = expected_vertex(r, parameters[i], slot);
            const bool header = v.magic == c::readback_magic && v.context == i && v.slot == slot &&
                                v.field_count == c::vertex_fields && v.vertex_bytes == r.words[8] &&
                                v.vertex_alignment == r.words[9];
            headers_ok = headers_ok && header;
            row = row && header;
            std::cout << "Full context semantic case=" << i << " slot=" << slot;
            for (unsigned field = 0; field < c::vertex_fields; ++field) {
                std::cout << ' ' << v.fields[field];
                if (v.fields[field] != expected[field]) {
                    ++typed_errors;
                    row = false;
                    const bool target = i % 2 == 0 && (slot == 1 || slot == 4) &&
                                        field >= c::prepared_begin && field < 189 &&
                                        (field - c::prepared_begin) % c::lane_fields == 23;
                    if (!target)
                        ++other_typed_errors;
                }
            }
            std::cout << '\n';
            for (unsigned field = 0; field < c::vertex_fields; ++field)
                if (v.fields[field] != expected[field])
                    std::cout << "Full context mismatch case=" << i << " slot=" << slot
                              << " field=" << field << " actual=" << v.fields[field]
                              << " expected=" << expected[field] << '\n';
            if (i % 2 == 0 && (slot == 1 || slot == 4))
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const unsigned field = c::prepared_begin + lane * c::lane_fields + 23;
                    wrong_rho += v.fields[field] != expected[field];
                }
        }
        good = good && row;
        std::cout << "Full context case=" << i << " contract=" << row << '\n';
    }
    std::cout << "Full context summary contexts=8 MIS=24 output_words=2944 typed_slots=40 "
                 "typed_fields=8800 bad_physical="
              << bad_physical << " wrong_nested_rho=" << wrong_rho
              << " typed_errors=" << typed_errors << " other_typed_errors=" << other_typed_errors
              << " schema_ok=" << schemas_ok << " early_physical_ok=" << early_ok
              << " readback_headers_ok=" << headers_ok << " joint_accepted=" << accepted
              << " joint_witness=" << joint_witness << '\n';
    require(good && accepted == 4 && joint_witness,
            "Canonical full-context material/physical contract");
    return 0;
}
} // namespace full_context_test

void run(id<MTLDevice> device, const std::filesystem::path &library_path) {
    std::string diagnostic;
    const auto library = metal::load_library(device, library_path, diagnostic);
    require(library != nil, diagnostic);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_probe_light_geometry", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto queue = [device newCommandQueue];
    require(queue != nil, "Geometry probe queue allocation failed");
    offset_rounding(device, library, queue);
    density_conversion(device, library, queue);
    canonical_mis(device, library, queue);
    std::vector<wire::Point> positions;
    std::vector<wire::Instance> instances;
    std::vector<wire::Emitter> emitters;
    std::vector<wire::Word> data;
    const std::array triangles{wire::Triangle{{0, 1, 2}}};
    const std::array patches{wire::Patch{{0, 1, 2, 3}}};
    const auto add_mesh = [&](std::span<const wire::Point> vertices, bool patch,
                              wire::Instance transform) {
        transform.vertex_begin = static_cast<wire::Word>(positions.size());
        transform.triangle_count = patch ? 0 : 1;
        transform.patch_count = patch ? 1 : 0;
        positions.insert(positions.end(), vertices.begin(), vertices.end());
        const auto index = static_cast<wire::Word>(instances.size());
        instances.push_back(transform);
        emitters.push_back({index, 0, patch ? wire::patch_primitive : wire::triangle_primitive});
        return static_cast<unsigned>(emitters.size() - 1);
    };
    const std::array triangle{wire::Point{-1, -1, 0}, wire::Point{1, -1, 0}, wire::Point{0, 1, 0}};
    const std::array rectangle{wire::Point{-1, -.5f, 0}, wire::Point{1, -.5f, 0},
                               wire::Point{-1, .5f, 0}, wire::Point{1, .5f, 0}};
    const auto tri = add_mesh(triangle, false, instance());
    auto reflected = instance();
    reflected.matrix[0] = -1;
    const auto reflected_tri = add_mesh(triangle, false, reflected);
    const auto rect = add_mesh(rectangle, true, instance());
    auto shear = instance();
    shear.matrix[1] = .3f;
    const auto sheared = add_mesh(rectangle, true, shear);
    auto warped_vertices = rectangle;
    warped_vertices[3].z = 1;
    const auto warped = add_mesh(warped_vertices, true, instance());
    const auto add_analytic = [&](geometry_wire::Analytic shape, wire::Instance transform) {
        transform.analytic_begin = static_cast<wire::Word>(data.size());
        transform.analytic_count = 1;
        const auto offset = data.size();
        data.resize(offset + sizeof(shape) / sizeof(wire::Word));
        std::memcpy(data.data() + offset, &shape, sizeof(shape));
        const auto index = static_cast<wire::Word>(instances.size());
        instances.push_back(transform);
        emitters.push_back({index, 0, wire::analytic_primitive});
        return static_cast<unsigned>(emitters.size() - 1);
    };
    geometry_wire::Analytic sphere{.kind = geometry_wire::analytic,
                                   .subtype = 1,
                                   .radius = .7f,
                                   .z_min = -.7f,
                                   .z_max = .7f,
                                   .phi_max = 2 * math::pi};
    const auto full_sphere = add_analytic(sphere, instance());
    auto scaled = reflected;
    scaled.matrix[0] = -2;
    scaled.matrix[5] = scaled.matrix[10] = 2;
    const auto reflected_sphere = add_analytic(sphere, scaled);
    auto nonuniform = instance();
    nonuniform.matrix[0] = 2;
    const auto stretched_sphere = add_analytic(sphere, nonuniform);
    auto clipped = sphere;
    clipped.z_min = 0;
    clipped.phi_max = math::pi;
    const auto clipped_sphere = add_analytic(clipped, instance());
    auto disk_shape = sphere;
    disk_shape.subtype = 2;
    disk_shape.inner_radius = .1f;
    const auto disk = add_analytic(disk_shape, shear);
    auto cylinder_shape = sphere;
    cylinder_shape.subtype = 3;
    const auto cylinder = add_analytic(cylinder_shape, nonuniform);
    const math::V3 ta{-1, -1, 0}, tb{1, -1, 0}, tc{0, 1, 0};
    const auto triangle_angle = [&](math::V3 reference) {
        return math::triangle_solid_angle(ta, tb, tc, reference);
    };
    const auto rect_angle = [&](math::V3 reference) {
        return math::rectangle_solid_angle({-1, -.5f, 0}, {1, -.5f, 0}, {-1, .5f, 0}, {1, .5f, 0},
                                           reference);
    };
    const std::vector<Case> cases{
        {"Triangle Spherical", {.2f, .3f, 1}, {}, tri, false, triangle_angle({.2f, .3f, 1})},
        {"Triangle Cosine", {.2f, .3f, 1}, {0, 0, 1}, tri, false, triangle_angle({.2f, .3f, 1})},
        {"Triangle Far Spherical", {0, 0, 60}, {}, tri, false, triangle_angle({0, 0, 60})},
        {"Triangle Far Area", {0, 0, 1000}, {}, tri, false, triangle_angle({0, 0, 1000})},
        {"Triangle Near Area", {0, 0, .00001f}, {}, tri},
        {"Triangle Reflected",
         {0, 0, 1},
         {0, 0, 1},
         reflected_tri,
         false,
         triangle_angle({0, 0, 1})},
        {"Rectangle Spherical", {.2f, .1f, 1}, {}, rect, false, rect_angle({.2f, .1f, 1})},
        {"Rectangle Cosine", {.2f, .1f, 1}, {0, 0, 1}, rect, false, rect_angle({.2f, .1f, 1})},
        {"Rectangle Far Spherical", {0, 0, 100}, {}, rect, false, rect_angle({0, 0, 100})},
        {"Rectangle Far Area", {0, 0, 1000}, {}, rect, false, rect_angle({0, 0, 1000})},
        {"Rectangle Sheared", {.2f, .1f, 1}, {}, sheared},
        {"Bilinear Nonplanar", {.2f, .1f, 2}, {}, warped},
        {"Sphere Outside", {0, 0, 2}, {}, full_sphere, true, 1 / math::sphere_cone_pdf(.7f, 4)},
        {"Sphere Inside", {0, 0, 0}, {}, full_sphere},
        {"Sphere On Boundary", {0, 0, .7f}, {}, full_sphere},
        {"Sphere Reflected Similarity",
         {0, 0, 3},
         {},
         reflected_sphere,
         true,
         1 / math::sphere_cone_pdf(1.4f, 9)},
        {"Sphere Nonuniform", {0, 0, 3}, {}, stretched_sphere},
        {"Sphere Clipped", {0, 0, 3}, {}, clipped_sphere},
        {"Disk Clipped Sheared", {.2f, .1f, 2}, {}, disk},
        {"Cylinder Clipped Nonuniform", {.2f, .1f, 3}, {}, cylinder}};
    constexpr unsigned n = 64;
    std::vector<probe::Query> queries;
    for (const auto &test : cases)
        for (unsigned y = 0; y < n; ++y)
            for (unsigned x = 0; x < n; ++x)
                queries.push_back(
                    {test.reference, test.normal, {(x + .5f) / n, (y + .5f) / n}, test.emitter});
    const auto query_buffer = buffer(device, queries), position_buffer = buffer(device, positions);
    const auto triangle_buffer = buffer(device, triangles), patch_buffer = buffer(device, patches);
    const auto instance_buffer = buffer(device, instances), data_buffer = buffer(device, data);
    const auto emitter_buffer = buffer(device, emitters);
    const auto output = [device newBufferWithLength:queries.size() * sizeof(probe::Result)
                                            options:MTLResourceStorageModeShared];
    wire::Parameters parameters{};
    parameters.instance_count = static_cast<wire::Word>(instances.size());
    const auto count = static_cast<wire::Word>(queries.size());
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(query_buffer != nil && position_buffer != nil && triangle_buffer != nil &&
                patch_buffer != nil && instance_buffer != nil && data_buffer != nil &&
                emitter_buffer != nil && output != nil && command != nil && encoder != nil,
            "Geometry probe allocation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:query_buffer offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBuffer:position_buffer offset:0 atIndex:2];
    [encoder setBuffer:triangle_buffer offset:0 atIndex:3];
    [encoder setBuffer:patch_buffer offset:0 atIndex:4];
    [encoder setBuffer:instance_buffer offset:0 atIndex:5];
    [encoder setBuffer:data_buffer offset:0 atIndex:6];
    [encoder setBuffer:emitter_buffer offset:0 atIndex:7];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:8];
    [encoder setBytes:&count length:sizeof(count) atIndex:9];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(metal::group_width(device, pipeline), 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Incident geometry probe");
    require(diagnostic.empty(), diagnostic);
    const auto results = static_cast<const probe::Result *>(output.contents);
    for (std::size_t test_index = 0; test_index < cases.size(); ++test_index) {
        const auto &test = cases[test_index];
        double solid_angle_estimate = 0;
        for (unsigned i = 0; i < n * n; ++i) {
            const auto &result = results[test_index * n * n + i];
            require(result.valid && result.sampled_pdf > 0 && std::isfinite(result.area_pdf),
                    test.name + " invalid proposal");
            require(result.sampled_pdf == result.queried_pdf, test.name + " sample/PDF mismatch");
            require(bool(result.cone) == test.cone, test.name + " incorrect cone/fallback policy");
            require(!test.cone || result.far_root_pdf == 0,
                    test.name + " incorrectly assigned cone density to far root");
            const auto normal = result.normal;
            require(std::abs(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z - 1) <
                        2e-5,
                    test.name + " invalid surface normal");
            solid_angle_estimate += 1. / result.sampled_pdf / (n * n);
        }
        if (test.solid_angle > 0)
            require(std::abs(solid_angle_estimate - test.solid_angle) < .005 * test.solid_angle,
                    test.name + " failed independent reciprocal-PDF solid-angle moment");
        std::cout << test.name << ": " << n * n << " proposals passed\n";
    }
    std::cout << "Incident geometry Metal: " << queries.size()
              << " production-adapter proposals passed\n";
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            const auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP incident geometry probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Incident geometry probe requires a Metal device");
            if (argc == 4 && std::string(argv[2]) == "--canonical-full-context")
                return full_context_test::run(argv[1], argv[3]);
            require(argc == 2, "Expected standalone geometry probe metallib path");
            run(device, argv[1]);
        } catch (const std::exception &error) {
            std::cerr << "Incident geometry probe: " << error.what() << '\n';
            return 1;
        }
    }
}
