// SPDX-License-Identifier: GPL-3.0-only
// Native triangle hint coverage, alpha, resource and complete-seed regression tests.
#include "composition_wire.h"
#include "geometry_reference.hpp"
#include "geometry_wire.h"
#include "hint_probe_wire.h"
#include "masked_probe_wire.h"
#include "metal_execution.hpp"
#include "metal_geometry_access.hpp"
#include "seed_wire.h"
#include "triangle_candidate_fixture.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace rypath;
namespace wire = geometry_wire;
namespace masked = masked_probe;
namespace fs = std::filesystem;
namespace {
void need(bool b, const std::string &s) {
    if (!b)
        throw std::runtime_error(s);
}
struct Expected {
    unsigned kind, source = UINT32_MAX, primitive = UINT32_MAX;
    bool invalid_ray = false;
};
struct Fixture {
    std::string name;
    GeometryData data;
    std::vector<GeometryRay> rays;
    std::vector<Expected> expected;
};
GeometryPoint moved(GeometryPoint p, const GeometryTransform &m, bool origin) {
    const auto &a = m.values;
    return {a[0] * p.x + a[1] * p.y + a[2] * p.z + (origin ? a[3] : 0),
            a[4] * p.x + a[5] * p.y + a[6] * p.z + (origin ? a[7] : 0),
            a[8] * p.x + a[9] * p.y + a[10] * p.z + (origin ? a[11] : 0)};
}
std::vector<Fixture> fixtures() {
    std::vector<Fixture> out;
    const std::array<GeometryTransform, 5> transforms{
        {{},
         {{-1, 0, 0, 0, 0, 2, 0, 0, 0, 0, .5f, 0}},
         {{0, 0, 2, 0, 1, 0, 0, 0, 0, .5f, 0, 0}},
         {{1, .25f, 0, 0, 0, 1, .125f, 0, .25f, 0, 1, 0}},
         {{1, 0, 0, 2, 0, 1, 0, -1, 0, 0, 1, 4}}}};
    for (std::size_t t = 0; t < transforms.size(); ++t) {
        Fixture f;
        f.name = "day-transform-" + std::to_string(t);
        f.data = test::triangle_fixture::regression();
        f.data.instances[0].transform = transforms[t];
        for (const auto &original : test::triangle_fixture::rays) {
            auto r = original;
            r.origin = moved(r.origin, transforms[t], true);
            r.direction = moved(r.direction, transforms[t], false);
            f.rays.push_back(r);
            const auto oracle = test::reference_closest(f.data, r);
            if (oracle.empty())
                f.expected.push_back({wire::miss});
            else {
                const auto chosen = *std::min_element(
                    oracle.begin(), oracle.end(), [](const auto &a, const auto &b) {
                        return a.instance_index < b.instance_index ||
                               (a.instance_index == b.instance_index &&
                                a.primitive_index < b.primitive_index);
                    });
                f.expected.push_back(
                    {wire::triangle, chosen.instance_index, chosen.primitive_index});
            }
        }
        if (t == 0)
            need(f.expected[0].primitive == 773 && f.expected[1].primitive == 601,
                 "Captured day independent oracle identities");
        out.push_back(std::move(f));
    }
    GeometryData tie;
    tie.meshes = {{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 1, 2}, {0, 1, 2}}}};
    tie.instances.emplace_back();
    out.push_back({"computed-pair-tie",
                   tie,
                   {{{.25f, .25f, 1}, {0, 0, -1}, .5f, 1.5f}},
                   {{wire::triangle, 0, 0}}});
    // Exact original two-plane geometry and third-query bits from admitted Sobol capture.
    auto simple = tie;
    simple.meshes[0].triangles.resize(1);
    out.push_back({"opaque-simple",
                   simple,
                   {{{.25f, .25f, 1}, {0, 0, -1}, .5f, 1.5f}},
                   {{wire::triangle, 0, 0}}});
    out.push_back(
        {"original-strict-bounds",
         simple,
         {{{.25f, .25f, 1}, {0, 0, -1}, 0, std::nextafter(1.f, 0.f)},
          {{.25f, .25f, 1}, {0, 0, -1}, std::nextafter(1.f, 0.f), std::nextafter(1.f, 2.f)},
          {{.25f, .25f, 1}, {0, 0, -1}, std::nextafter(1.f, 2.f), 2}},
         {{wire::miss}, {wire::triangle, 0, 0}, {wire::miss}}});
    auto coincident = tie;
    coincident.instances.emplace_back();
    out.push_back({"coincident-instance-tie",
                   coincident,
                   {{{.25f, .25f, 1}, {0, 0, -1}, .5f, 1.5f}},
                   {{wire::triangle, 0, 0}}});
    GeometryData sobol;
    for (float z : {-1.f, 1.f}) {
        GeometryMesh mesh;
        mesh.positions = {{-8192, -8192, z}, {-8192, 8192, z}, {8192, 8192, z}, {8192, -8192, z}};
        mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
        sobol.meshes.push_back(mesh);
        sobol.instances.push_back({static_cast<unsigned>(sobol.meshes.size() - 1), {}});
    }
    const auto point = [](std::array<unsigned, 3> x) {
        return GeometryPoint{std::bit_cast<float>(x[0]), std::bit_cast<float>(x[1]),
                             std::bit_cast<float>(x[2])};
    };
    GeometryRay witness{point({1073684480u, 0u, 3212722158u}),
                        point({3207922931u, 3011821667u, 1060439283u}), 0,
                        std::numeric_limits<float>::infinity()};
    out.push_back({"sobol-missed-neighbor", sobol, {witness}, {{wire::triangle, 1, 1}}});
    // Source mesh expands to three prepared ordinals. Nontriangle masks must
    // retain fallback blockers and original prepared/source identity.
    GeometryMesh mixed;
    mixed.positions = {{.1f, -1, -1}, {.1f, 1, -1}, {.1f, -1, 1}, {.1f, 1, 1},
                       {.2f, -1, -1}, {.2f, 1, -1}, {.2f, -1, 1}};
    mixed.patches = {{0, 1, 2, 3}};
    mixed.triangles = {{4, 5, 6}};
    mixed.analytics.push_back(
        {.kind = AnalyticKind::sphere, .radius = .25f, .z_min = -.25f, .z_max = .25f});
    GeometryData data;
    data.meshes = {mixed};
    data.instances.emplace_back();
    out.push_back({"mixed-nearer-surfaces",
                   data,
                   {{{-.5f, 0, 0}, {1, 0, 0}, 0, 1}, {{-.5f, 0, 0}, {1, 0, 0}, .5f, 1}},
                   {{wire::analytic, 0, 0}, {wire::bilinear_patch, 0, 0}}});
    Fixture bad{"invalid-rays", tie, {}, {}};
    const GeometryRay good{{.25f, .25f, 1}, {0, 0, -1}, .5f, 1.5f};
    const auto add = [&](GeometryRay r) {
        bad.rays.push_back(r);
        bad.expected.push_back({wire::invalid_hit, UINT32_MAX, UINT32_MAX, true});
    };
    auto r = good;
    r.origin.x = std::numeric_limits<float>::quiet_NaN();
    add(r);
    r = good;
    r.direction.z = std::numeric_limits<float>::infinity();
    add(r);
    r = good;
    r.direction = {};
    add(r);
    r = good;
    r.t_min = -1;
    add(r);
    r = good;
    r.t_min = std::numeric_limits<float>::denorm_min();
    add(r);
    r = good;
    r.t_max = std::numeric_limits<float>::quiet_NaN();
    add(r);
    r = good;
    r.t_max = .25f;
    add(r);
    out.push_back(std::move(bad));
    tie.meshes[0].positions[2].x = std::bit_cast<float>(512u);
    out.push_back({"unsupported-triangle", tie, {good}, {{wire::invalid_hit}}});
    return out;
}
bool write_bytes(const fs::path &p, const void *data, std::size_t bytes) {
    if (fs::exists(p))
        return false;
    std::ofstream o(p, std::ios::binary);
    o.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
    o.flush();
    return bool(o);
}
id<MTLComputePipelineState> pipeline(id<MTLDevice> device, id<MTLLibrary> library, const char *name,
                                     int mode = -1) {
    NSError *error = nil;
    NSString *entry = [NSString stringWithUTF8String:name];
    id<MTLFunction> function;
    if (mode < 0)
        function = [library newFunctionWithName:entry];
    else {
        MTLFunctionConstantValues *constants = [[MTLFunctionConstantValues alloc] init];
        const unsigned value = static_cast<unsigned>(mode);
        [constants setConstantValue:&value type:MTLDataTypeUInt atIndex:19];
        function = [library newFunctionWithName:entry constantValues:constants error:&error];
    }
    need(function != nil, std::string("Function ") + name + ": " +
                              (error ? error.localizedDescription.UTF8String : "missing"));
    auto p = [device newComputePipelineStateWithFunction:function error:&error];
    need(p != nil, std::string("Pipeline ") + name + ": " +
                       (error ? error.localizedDescription.UTF8String : "missing"));
    return p;
}
struct Counters {
    std::size_t dispatches = 0, input_bytes_checked = 0, output_guard_bytes_checked = 0,
                closest_fields = 0, shadow_fields = 0, masked_fields = 0;
};
std::vector<unsigned char> dispatch(MetalGeometryScene &scene, id<MTLComputePipelineState> p,
                                    const void *values, std::size_t count, std::size_t input_stride,
                                    std::size_t output_stride, unsigned mask, bool hints,
                                    bool alpha, const fs::path &stem, Counters &c) {
    auto device = MetalGeometryAccess::device(scene);
    const std::size_t width = std::min<NSUInteger>(32, metal::group_width(device, p));
    need(width > 0, "Positive group width");
    const std::size_t rounded = ((count + width - 1) / width) * width, prefix = 256;
    std::vector<unsigned char> input(prefix + rounded * input_stride + 256, 0xa5),
        initial_output(prefix + rounded * output_stride + 256, 0x5a);
    std::memcpy(input.data() + prefix, values, count * input_stride);
    const auto in = [device newBufferWithBytes:input.data()
                                        length:input.size()
                                       options:MTLResourceStorageModeShared];
    const auto output = [device newBufferWithBytes:initial_output.data()
                                            length:initial_output.size()
                                           options:MTLResourceStorageModeShared];
    need(in && output, "Guarded buffers");
    const auto command = [MetalGeometryAccess::queue(scene) commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    need(command && encoder, "Probe command");
    [encoder setComputePipelineState:p];
    const unsigned n = static_cast<unsigned>(count);
    if (alpha) {
        MetalGeometryAccess::bind(scene, encoder, 0);
        [encoder setBuffer:in offset:prefix atIndex:1];
        [encoder setBuffer:output offset:prefix atIndex:2];
        [encoder setBytes:&n length:sizeof(n) atIndex:3];
    } else {
        [encoder setBuffer:in offset:prefix atIndex:0];
        [encoder setBuffer:output offset:prefix atIndex:1];
        [encoder setBytes:&n length:sizeof(n) atIndex:2];
        MetalGeometryAccess::bind(scene, encoder, 3);
        if (hints)
            MetalGeometryAccess::bind_hint(scene, encoder, 4);
        [encoder setBytes:&mask length:sizeof(mask) atIndex:5];
    }
    [encoder dispatchThreadgroups:MTLSizeMake(rounded / width, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
    [encoder endEncoding];
    const auto error = metal::complete(command, "Hint comparison probe");
    const auto *actual_input = static_cast<const unsigned char *>(in.contents);
    const auto *bytes = static_cast<const unsigned char *>(output.contents);
    // Save every available readback before checking completion, mutation or guards.
    // Both saves are attempted even if one buffer is unavailable or a write fails.
    const bool input_saved =
        actual_input && write_bytes(stem.string() + ".input.bin", actual_input, input.size());
    const bool output_saved =
        bytes && write_bytes(stem.string() + ".output.bin", bytes, initial_output.size());
    std::ofstream completion(stem.string() + ".command.txt");
    completion << "command_status=" << static_cast<NSUInteger>(command.status)
               << "\ncompletion_error=" << error << "\ninput_available=" << bool(actual_input)
               << "\noutput_available=" << bool(bytes) << "\ninput_saved=" << input_saved
               << "\noutput_saved=" << output_saved << "\ninput_bytes=" << input.size()
               << "\noutput_bytes=" << initial_output.size() << "\nlogical_count=" << count
               << "\ngroup_width=" << width << "\ninput_stride=" << input_stride
               << "\noutput_stride=" << output_stride << "\nprefix_bytes=" << prefix << "\n";
    completion.flush();
    need(input_saved && output_saved && bool(completion),
         "Available readback/diagnostic archival failure " + stem.string() + ": " + error);
    need(error.empty(), error);
    ++c.dispatches;
    need(std::memcmp(actual_input, input.data(), input.size()) == 0,
         "Input and guards unchanged " + stem.string());
    c.input_bytes_checked += input.size();
    need(bytes != nullptr, "Output contents");
    need(std::equal(bytes, bytes + prefix, initial_output.data()) &&
             std::equal(bytes + prefix + count * output_stride, bytes + initial_output.size(),
                        initial_output.data() + prefix + count * output_stride),
         "Output prefix/padded/tail guards " + stem.string());
    c.output_guard_bytes_checked += initial_output.size() - count * output_stride;
    for (std::size_t i = 0; i < count; ++i)
        need(!std::all_of(bytes + prefix + i * output_stride,
                          bytes + prefix + (i + 1) * output_stride,
                          [](unsigned char v) { return v == 0x5a; }),
             "Each output record written");
    return {bytes + prefix, bytes + prefix + count * output_stride};
}
wire::Ray packed(const GeometryRay &r) {
    return {{r.origin.x, r.origin.y, r.origin.z},
            {r.direction.x, r.direction.y, r.direction.z},
            r.t_min,
            r.t_max};
}
void oracle(MetalGeometryScene &scene, const Fixture &f, unsigned mask,
            const std::vector<unsigned char> &closest, const std::vector<unsigned char> &shadow) {
    for (std::size_t i = 0; i < f.rays.size(); ++i) {
        wire::Closest h;
        unsigned s;
        std::memcpy(&h, closest.data() + i * sizeof(h), sizeof(h));
        std::memcpy(&s, shadow.data() + i * sizeof(s), sizeof(s));
        auto e = f.expected[i];
        if (mask == 0 && !e.invalid_ray)
            e.kind = wire::miss;
        need(h.kind == e.kind, f.name + " independent expected kind row" + std::to_string(i));
        need(s == (e.kind == wire::invalid_hit ? wire::invalid_hit
                   : e.kind == wire::miss      ? 0u
                                               : 1u),
             f.name + " independent shadow status");
        if (e.kind != wire::miss && e.kind != wire::invalid_hit) {
            need(h.instance < MetalGeometryAccess::instance_count(scene) &&
                     MetalGeometryAccess::source_instance(scene, h.instance) == e.source &&
                     h.geometry == 0 && h.primitive == e.primitive,
                 f.name + " original prepared/source/primitive identity");
            need(std::isfinite(h.t) && h.t >= f.rays[i].t_min && h.t <= f.rays[i].t_max &&
                     std::isfinite(h.u) && std::isfinite(h.v),
                 f.name + " original ray bounds and finite public coordinates");
        }
    }
}

// Low-level real TLAS mask + actual shared opaque validator + masked fallback.
// Compact metadata below is not a renderer material/global-eligibility fixture.
void composition_cases(const fs::path &render_library, const fs::path &probe_library,
                       const fs::path &out, Counters &counters, std::ostream &rows) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        GeometryMesh gaps;
        gaps.positions = {{0, -1, -1}, {0, 1, -1}, {0, -1, 1}, {0, 1, 1}};
        gaps.patches = {{0, 1, 2, 3}};
        gaps.analytics.push_back(
            {.kind = AnalyticKind::sphere, .radius = .1f, .z_min = -.1f, .z_max = .1f});
        GeometryMesh a;
        a.positions = {{-1, -1, .25f}, {1, -1, .25f}, {0, 1, .25f}};
        a.triangles = {{0, 1, 2}};
        auto b = a;
        for (auto &p : b.positions) {
            p.z = .75f;
            if (variant != 0)
                p.x += 4;
        }
        GeometryData data;
        data.meshes = {gaps, a, b};
        GeometryTransform off;
        off.values[3] = 4;
        data.instances = {{.mesh_index = 0, .transform = off},
                          {.mesh_index = 1, .non_opaque = true},
                          {.mesh_index = 2, .non_opaque = variant == 2}};
        auto built = build_metal_geometry_with_hints(data, render_library, true);
        need(built.status == GeometryStatus::success && built.scene, built.diagnostic);
        auto &scene = *built.scene;
        need(MetalGeometryAccess::has_hint(scene), "All-mask-zero direct builder keeps valid TLAS");
        auto stats = MetalGeometryAccess::hint_stats(scene);
        need(stats.eligible_instance_count == (variant == 2 ? 0u : 1u),
             "Actual eligible native triangle placements");
        const auto device = MetalGeometryAccess::device(scene);
        std::string error;
        auto library = metal::load_library(device, probe_library, error);
        need(library != nil, error);
        const auto count = MetalGeometryAccess::instance_count(scene);
        need(count <= 8, "Bounded metadata slots");
        unsigned alpha_ordinal = UINT32_MAX;
        for (unsigned i = 0; i < count; ++i)
            if (MetalGeometryAccess::source_instance(scene, i) == 1)
                alpha_ordinal = i;
        need(alpha_ordinal != UINT32_MAX && alpha_ordinal != 1,
             "Nontriangle split preserves source/ordinal gap");
        for (unsigned mode : {0u, 1u, 2u, 3u}) {
            std::array<shadow_composition::Case, 3> inputs{};
            for (unsigned rule = 0; rule < 3; ++rule) {
                auto &c = inputs[rule];
                c.ray = {{0, 0, 0}, {0, 0, 1}, 0, 1};
                c.mask = wire::all_visible;
                c.alpha_ordinal = alpha_ordinal;
                c.rule = rule;
                c.instance_count = count;
                c.material_count = count;
                for (unsigned i = 0; i < count; ++i) {
                    auto source = MetalGeometryAccess::source_instance(scene, i);
                    const auto &mesh = data.meshes[data.instances[source].mesh_index];
                    c.instances[i] = {static_cast<unsigned>(mesh.triangles.size()),
                                      i,
                                      data.instances[source].non_opaque ? 0u : UINT32_MAX,
                                      0,
                                      0,
                                      0};
                    c.materials[i] = {0};
                }
            }
            // Mixed case excludes invalid-alpha: any-hit intentionally need not
            // visit an invalid alpha surface behind a valid opaque blocker.
            const auto logical = variant == 0 ? 2u : 3u;
            const auto label =
                "composition-" + std::to_string(variant) + "-mode" + std::to_string(mode);
            const auto output = dispatch(
                scene, pipeline(device, library, "composition_shadow", mode), inputs.data(),
                logical, sizeof(inputs[0]), sizeof(shadow_composition::Result), wire::all_visible,
                true, false, out / label, counters);
            for (unsigned rule = 0; rule < logical; ++rule) {
                shadow_composition::Result r;
                std::memcpy(&r, output.data() + rule * sizeof(r), sizeof(r));
                unsigned expected = variant == 0 ? 1u
                                    : rule == 0  ? 1u
                                    : rule == 1  ? 0u
                                                 : wire::invalid_hit;
                need(r.status == expected, "Independent mixed/alpha shadow oracle");
                need(r.hint_valid == 1, "Valid optional hint infrastructure");
                if ((mode & 1u) == 0 || variant != 0)
                    need(r.hint_available == 0,
                         "Disabled/off-ray/allalpha native hint unavailable");
                if (variant == 0 && (mode & 1u) != 0)
                    need(r.hint_available == 1 && r.alpha_calls == 0 &&
                             MetalGeometryAccess::source_instance(scene, r.hint_instance) == 2 &&
                             r.hint_primitive == 0,
                         "Eligible opaque source bypasses masked fallback through shared actual "
                         "helper");
                if (variant != 0)
                    need(r.alpha_calls > 0,
                         "No opaque blocker retains actual generic alpha callback");
                rows << "{\"fixture\":" << std::quoted(label) << ",\"rule\":" << rule
                     << ",\"status\":" << r.status << ",\"alpha_calls\":" << r.alpha_calls
                     << ",\"hint_available\":" << r.hint_available
                     << ",\"eligible_instances\":" << stats.eligible_instance_count
                     << ",\"low_level_composition_only\":true}\n";
            }
        }
    }
}
#include "seed_host.inc"
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 4,
                 "Usage: native_hint_probe CONTROL_METALLIB HINT_METALLIB RENDER_METALLIB");
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            const fs::path out =
                fs::temp_directory_path() / ("rypath-native-hint-" + std::to_string(stamp));
            std::cout << "Native hint observations: " << out << std::endl;
            need(!fs::exists(out), "Fresh probe output only");
            need(fs::create_directories(out), "Create fresh output");
            std::ofstream rows(out / "rows.jsonl");
            std::ofstream observations(out / "hints.jsonl");
            need(bool(observations), "Open separate hint observations");
            need(bool(rows), "Open comparison rows");
            Counters counters;
            const auto cases = fixtures();
            for (const auto &f : cases) {
                auto built = build_metal_geometry_with_hints(f.data, argv[3], true);
                if (built.status == GeometryStatus::no_device) {
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
                    std::cerr << built.diagnostic << '\n';
                    return 77;
#endif
                }
                if (built.status == GeometryStatus::no_ray_tracing) {
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
                    std::cerr << built.diagnostic << '\n';
                    return 77;
#endif
                }
                need(built.status == GeometryStatus::success && built.scene, built.diagnostic);
                auto &scene = *built.scene;
                need(MetalGeometryAccess::has_hint(scene), "Hint AS exists for nonempty fixture");
                const auto stats = MetalGeometryAccess::hint_stats(scene);
                const auto eligible =
                    std::ranges::count_if(f.data.instances, [&](const auto &instance) {
                        return !instance.non_opaque &&
                               !f.data.meshes[instance.mesh_index].triangles.empty();
                    });
                need(stats.acceleration_bytes > 0 && stats.native_blas_count > 0 &&
                         stats.shared_record_bytes > 0 &&
                         stats.instance_count == MetalGeometryAccess::instance_count(scene) &&
                         stats.eligible_instance_count == static_cast<std::uint64_t>(eligible),
                     "Native resources preserve expanded slots and only opaque triangle placement "
                     "masks");
                const auto device = MetalGeometryAccess::device(scene);
                std::string error;
                const auto old = metal::load_library(device, argv[1], error);
                need(old != nil, error);
                const auto current = metal::load_library(device, argv[2], error);
                need(current != nil, error);
                auto oldclosest = pipeline(device, old, "control_closest"),
                     oldshadow = pipeline(device, old, "control_shadow");
                std::vector<wire::Ray> rays;
                for (auto r : f.rays)
                    rays.push_back(packed(r));
                for (unsigned mask : {static_cast<unsigned>(wire::all_visible), 0u}) {
                    const auto label = f.name + "-mask" + std::to_string(mask);
                    auto closest = dispatch(scene, oldclosest, rays.data(), rays.size(),
                                            sizeof(wire::Ray), sizeof(wire::Closest), mask, false,
                                            false, out / (label + "-control-closest"), counters);
                    auto shadow = dispatch(scene, oldshadow, rays.data(), rays.size(),
                                           sizeof(wire::Ray), sizeof(unsigned), mask, false, false,
                                           out / (label + "-control-shadow"), counters);
                    oracle(scene, f, mask, closest, shadow);
                    for (unsigned mode : {0u, 1u, 2u, 3u}) {
                        const auto stem = label + "-mode" + std::to_string(mode);
                        auto a =
                            dispatch(scene, pipeline(device, current, "hint_closest"), rays.data(),
                                     rays.size(), sizeof(wire::Ray), sizeof(wire::Closest), mask,
                                     false, false, out / (stem + "-closest"), counters);
                        auto b =
                            dispatch(scene, pipeline(device, current, "hint_shadow", mode),
                                     rays.data(), rays.size(), sizeof(wire::Ray), sizeof(unsigned),
                                     mask, true, false, out / (stem + "-shadow"), counters);
                        need(a == closest, stem + " exact kind/ordinal/IDs/public-t/UV vs "
                                                  "independently compiled old traversal");
                        need(b == shadow, stem + " shadow Boolean/invalid vs old traversal");
                        oracle(scene, f, mask, a, b);
                        for (bool shadow_observation : {true}) {
                            const char *entry = "hint_observe_shadow";
                            const auto values =
                                dispatch(scene, pipeline(device, current, entry, mode), rays.data(),
                                         rays.size(), sizeof(wire::Ray),
                                         sizeof(hint_probe::Observation), mask, true, false,
                                         out / (stem + (shadow_observation ? "-observe-shadow"
                                                                           : "-observe-closest")),
                                         counters);
                            for (std::size_t i = 0; i < rays.size(); ++i) {
                                hint_probe::Observation h;
                                std::memcpy(&h, values.data() + i * sizeof(h), sizeof(h));
                                const bool enabled = (mode & 1u) != 0;
                                const bool supported = !f.expected[i].invalid_ray;
                                need(h.enabled == enabled && h.domain_supported == supported &&
                                         h.queried == (enabled && supported),
                                     stem + " separate observation obeys mode/ray guards");
                                if (h.available) {
                                    need(h.queried && h.valid && h.kind == wire::triangle &&
                                             h.instance <
                                                 MetalGeometryAccess::instance_count(scene) &&
                                             MetalGeometryAccess::primitive_kind(
                                                 scene, h.instance) == PrimitiveKind::triangle &&
                                             h.geometry == 0,
                                         stem + " separate validated hint identity");
                                }
                                if (f.name == "opaque-simple" && mask && enabled) {
                                    need(h.available && h.valid && h.instance == 0 &&
                                             h.primitive == 0 && shadow_observation,
                                         "Simple opaque triangle exercises an available validated "
                                         "shadow hint");
                                }
                                observations
                                    << "{\"fixture\":" << std::quoted(f.name)
                                    << ",\"mask\":" << mask << ",\"mode\":" << mode
                                    << ",\"row\":" << i
                                    << ",\"operation\":" << std::quoted("shadow")
                                    << ",\"domain_supported\":" << h.domain_supported
                                    << ",\"enabled\":" << h.enabled << ",\"queried\":" << h.queried
                                    << ",\"valid\":" << h.valid << ",\"available\":" << h.available
                                    << ",\"kind\":" << h.kind << ",\"instance\":" << h.instance
                                    << ",\"geometry\":" << h.geometry
                                    << ",\"primitive\":" << h.primitive
                                    << ",\"validated_t_bits\":" << h.validated_t_bits
                                    << ",\"validated_u_bits\":" << h.validated_u_bits
                                    << ",\"validated_v_bits\":" << h.validated_v_bits
                                    << ",\"separate_helper_observation\":true}\n";
                                observations.flush();
                                need(bool(observations), "Write separate observations");
                            }
                        }
                        counters.closest_fields += 7 * rays.size();
                        counters.shadow_fields += rays.size();
                        rows << "{\"fixture\":" << std::quoted(f.name) << ",\"mask\":" << mask
                             << ",\"mode\":" << mode << ",\"rays\":" << rays.size()
                             << ",\"closest_fields_exact\":" << 7 * rays.size()
                             << ",\"shadow_fields_exact\":" << rays.size() << ",\"oracle\":true}\n";
                        rows.flush();
                        need(bool(rows), "Write comparison rows");
                    }
                }
            }
            // Existing generic alpha entry has no hint AS parameter or FC19 specialization.
            GeometryMesh tiny;
            tiny.positions = {
                {-0x1p-10f, -0x1p-10f, .25f}, {0x1p-10f, -0x1p-10f, .25f}, {0, 0x1p-10f, .25f}};
            tiny.triangles = {{0, 1, 2}};
            GeometryData alpha_data{.meshes = {tiny},
                                    .instances = {{.mesh_index = 0, .non_opaque = true}}};
            auto alpha_built = build_metal_geometry_with_hints(alpha_data, argv[3], true);
            need(alpha_built.status == GeometryStatus::success && alpha_built.scene,
                 alpha_built.diagnostic);
            auto &alpha_scene = *alpha_built.scene;
            const auto device = MetalGeometryAccess::device(alpha_scene);
            std::string error;
            const auto old = metal::load_library(device, argv[1], error),
                       current = metal::load_library(device, argv[2], error);
            need(old && current, error);
            std::array<masked::Case, 5> alpha{};
            const std::array<unsigned, 5> rules{masked::accept, masked::reject, masked::invalid,
                                                masked::accept_then_reject,
                                                masked::accept_then_invalid};
            for (unsigned i = 0; i < alpha.size(); ++i) {
                alpha[i].ray = {{0, 0, 0}, {0, 0, 1}, 0, 1};
                alpha[i].triangle_rule = rules[i];
                alpha[i].patch_rule = masked::invalid;
                alpha[i].ignore_instance = UINT32_MAX;
                alpha[i].expected_triangle_primitives[0] = 0;
                alpha[i].expected_triangle_primitives[1] = UINT32_MAX;
            }
            auto a =
                dispatch(alpha_scene, pipeline(device, old, "rypath_masked_intersection_probe"),
                         alpha.data(), alpha.size(), sizeof(masked::Case), sizeof(masked::Result),
                         wire::all_visible, false, true, out / "alpha-control", counters);
            auto b =
                dispatch(alpha_scene, pipeline(device, current, "rypath_masked_intersection_probe"),
                         alpha.data(), alpha.size(), sizeof(masked::Case), sizeof(masked::Result),
                         wire::all_visible, false, true, out / "alpha-new-unhinted", counters);
            need(a == b, "Unchanged generic alpha path exact hit and actual callback records");
            for (unsigned i = 0; i < alpha.size(); ++i) {
                masked::Result r;
                std::memcpy(&r, b.data() + i * sizeof(r), sizeof(r));
                const unsigned expected = i == 1             ? wire::miss
                                          : i == 2 || i == 4 ? wire::invalid_hit
                                                             : wire::triangle;
                need(r.hit.kind == expected && r.patch_calls == 0 &&
                         r.triangle_primitive_mask == 1 && r.repeated_triangle_values_changed == 0,
                     "Generic alpha expected observable behavior");
                need(i >= 3 ? r.triangle_calls >= 2 : r.triangle_calls >= 1,
                     "Generic alpha actual callbacks incl forced reoffer");
            }
            counters.masked_fields += alpha.size() * 11;
            rows << "{\"fixture\":\"generic-alpha-no-hint-binding\",\"rays\":5,\"fields_exact\":55,"
                    "\"actual_callbacks_checked\":true}\n";
            composition_cases(argv[3], argv[2], out, counters, rows);
            seed_cases(argv[3], argv[1], argv[2], out, counters, rows);
            // Public empty-scene route owns no AS/queue; preserve its no-dispatch contract.
            auto empty = build_metal_geometry_with_hints({}, argv[3], true);
            need(empty.status == GeometryStatus::success && empty.scene, empty.diagnostic);
            const std::array er{GeometryRay{{0, 0, 1}, {0, 0, -1}}};
            const auto ec = empty.scene->closest_hits(er);
            const auto es = empty.scene->occlusion(er);
            need(ec.status == GeometryStatus::success && es.status == GeometryStatus::success &&
                     ec.hits.size() == 1 && !ec.hits[0].hit && es.occluded.size() == 1 &&
                     es.occluded[0] == 0 && ec.guard_count == 0 && es.guard_count == 0 &&
                     !MetalGeometryAccess::has_hint(*empty.scene),
                 "Empty public scene returns miss without dispatch/hint AS");
            rows << "{\"fixture\":\"empty-public-no-dispatch\",\"rays\":1,\"closest_guards\":0,"
                    "\"shadow_guards\":0,\"hint_AS\":false}\n";
            rows.flush();
            need(bool(rows), "Write final rows");
            std::ofstream summary(out / "summary.json");
            summary << "{\"status\":\"GPU_comparison_pass\",\"dispatches\":" << counters.dispatches
                    << ",\"input_bytes_checked\":" << counters.input_bytes_checked
                    << ",\"output_guard_bytes_checked\":" << counters.output_guard_bytes_checked
                    << ",\"closest_field_comparisons\":" << counters.closest_fields
                    << ",\"shadow_field_comparisons\":" << counters.shadow_fields
                    << ",\"masked_field_comparisons\":" << counters.masked_fields
                    << ",\"empty_public_queries\":2}\n";
            need(bool(summary), "Write summary");
            std::cout << "PASS hint comparison; actual dispatches=" << counters.dispatches
                      << " closest_fields=" << counters.closest_fields
                      << " shadow_fields=" << counters.shadow_fields
                      << " input_bytes=" << counters.input_bytes_checked
                      << " output_guard_bytes=" << counters.output_guard_bytes_checked << '\n';
            return 0;
        } catch (const std::exception &e) {
            std::cerr << "Hint comparison failure: " << e.what() << '\n';
            return 1;
        }
    }
}
