// SPDX-License-Identifier: GPL-3.0-only
#include "metal_execution.hpp"
#include "render_wire.h"
#include "wavefront_wire.h"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace rypath;
namespace w = render_wire;
namespace wf = wavefront_wire;
static_assert(wf::contract == 14 && sizeof(wf::Config) == 64 && sizeof(wf::Contract) == 88);
constexpr unsigned variants = 16, guard = 256, instances_count = 136;
void need(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
int run(const std::filesystem::path &path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for contribution prefix differential");
    // The genuine hero_direct closure contains visibility even though this
    // pretraced fixture has zero lights and never issues an AS traversal.
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
    if (!device.supportsRaytracing)
        return 77;
#endif
    need(device.supportsRaytracing, "Contribution event oracle requires compute ray tracing");
    std::string error;
    auto library = metal::load_library(device, path, error);
    need(library != nil, error);
    auto constants = [MTLFunctionConstantValues new];
    const bool yes = true, no = false;
    for (unsigned i = 0; i <= 18; ++i)
        [constants setConstantValue:&no type:MTLDataTypeBool atIndex:i];
    const std::uint32_t zero_hint = 0;
    [constants setConstantValue:&zero_hint type:MTLDataTypeUInt atIndex:19];
    for (unsigned i : {20u, 21u, 22u})
        [constants setConstantValue:&no type:MTLDataTypeBool atIndex:i];
    [constants setConstantValue:&yes type:MTLDataTypeBool atIndex:25];
    [constants setConstantValue:&yes type:MTLDataTypeBool atIndex:wf::contribution_mode_constant];
    auto layout =
        metal::load_pipeline(device, library, @"rypath_contribution_prefix_layout", error);
    need(layout != nil, error);
    auto differential = metal::load_pipeline(
        device, library, @"rypath_contribution_prefix_differential", error, nil, constants);
    need(differential != nil, error);
    auto queue = [device newCommandQueue];
    need(queue != nil, "Prefix differential command queue");
    const auto allocate = [&](std::size_t bytes, bool poison = false) {
        auto b = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        need(b != nil, "Prefix differential buffer allocation");
        std::memset(b.contents, poison ? 0xa5 : 0, bytes);
        return b;
    };
    auto sizes = allocate(4 * sizeof(std::uint32_t));
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    need(command && encoder, "Prefix layout encoder");
    [encoder setComputePipelineState:layout];
    [encoder setBuffer:sizes offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    error = metal::complete(command, "contribution prefix layout");
    need(error.empty(), error);
    const auto *abi = static_cast<const std::uint32_t *>(sizes.contents);
    need(abi[0] == 0x43504631u && abi[1] == 80 && abi[2] == 4 && abi[3] == 16,
         "Contribution prefix state80/align4/evaluation16 contract");
    unsigned tested = 0, positive = 0, omitted = 0, divergent_failures = 0;
    for (unsigned depth : {0u, 3u, 65u}) {
        const unsigned slots = 2 * depth + 1, count = variants * slots + 1;
        std::array<id<MTLBuffer>, 12> b{};
        b[0] = allocate(sizeof(w::Parameters));
        b[1] = allocate(sizeof(wf::Config));
        b[2] = allocate(instances_count * sizeof(w::Instance));
        b[3] = allocate(instances_count * 3 * sizeof(w::Point));
        b[4] = allocate(sizeof(w::Triangle));
        b[5] = allocate(3 * sizeof(w::Material));
        b[6] = allocate(3 * sizeof(w::AreaEmission));
        b[7] = allocate(471 * sizeof(float));
        b[8] = allocate(471 * sizeof(w::Point));
        b[9] = allocate(count * abi[1] + 2 * guard, true);
        b[10] = allocate(std::size_t(count) * slots * 4 * sizeof(float) + 2 * guard, true);
        b[11] = allocate(count * 8 * sizeof(std::uint32_t) + 2 * guard, true);
        auto *p = static_cast<w::Parameters *>(b[0].contents);
        *p = {};
        p->width = count;
        p->height = 1;
        p->pixel_count = p->lane_count = count;
        p->model = w::pbrt_model;
        p->packet_transport = 2;
        p->max_depth = depth;
        p->emergency_limit = 128;
        p->boundary_limit = 2;
        p->instance_count = instances_count;
        p->material_count = 3;
        p->emission_count = 3;
        p->camera_normal = {0, 0, 1};
        p->seed_low = 4117;
        p->sampler_kind = w::independent_sampler;
        auto *cfg = static_cast<wf::Config *>(b[1].contents);
        *cfg = {};
        cfg->pixel_count = cfg->capacity = count;
        cfg->packet = 2;
        cfg->model = w::pbrt_model;
        cfg->group_width = cfg->packet_events = 1;
        cfg->replay_enabled = 1;
        cfg->contribution_mode = 1;
        cfg->contribution_depth = depth;
        cfg->contribution_target_scale = 1;
        cfg->contribution_serial_low = 0xabcdef01u;
        cfg->contribution_serial_high = 17;
        cfg->contribution_replay = 1;
        auto *instances = static_cast<w::Instance *>(b[2].contents);
        auto *positions = static_cast<w::Point *>(b[3].contents);
        for (unsigned i = 0; i < instances_count; ++i) {
            instances[i] = {};
            instances[i].matrix[0] = instances[i].matrix[5] = instances[i].matrix[10] = 1;
            instances[i].vertex_begin = 3 * i;
            instances[i].triangle_count = 1;
            instances[i].material = i == 66 ? 1 : i == 68 ? 2 : 0;
            instances[i].normal_begin = w::no_normals;
            instances[i].emission = i == 69 ? 1 : i >= 70 ? 2 : 0;
            instances[i].invisible_emitter = i == 67;
            const float z = -float(i);
            positions[3 * i] = {-2, -2, z};
            positions[3 * i + 1] = {2, -2, z};
            positions[3 * i + 2] = {0, 2, z};
        }
        *static_cast<w::Triangle *>(b[4].contents) = {{0, 1, 2}};
        auto *materials = static_cast<w::Material *>(b[5].contents);
        for (unsigned i = 0; i < 3; ++i) {
            materials[i] = {};
            materials[i].kind = w::constant_material;
            materials[i].scalar_value = i == 2 ? 0.f : .75f;
        }
        materials[1].scattering = 10;
        auto *emissions = static_cast<w::AreaEmission *>(b[6].contents);
        for (unsigned i = 0; i < 3; ++i) {
            emissions[i] = {};
            emissions[i].two_sided = 1;
            for (float &x : emissions[i].radiance)
                x = i == 1   ? std::numeric_limits<float>::quiet_NaN()
                    : i == 2 ? .75f * std::numeric_limits<float>::max()
                             : .125f;
        }
        auto *environment = static_cast<float *>(b[7].contents);
        auto *cie = static_cast<w::Point *>(b[8].contents);
        for (unsigned i = 0; i < 471; ++i) {
            environment[i] = .2f;
            // Positive synthetic sensor basis, not a physical CIE quadrature.
            cie[i] = {.125f + float(i % 17) / 256.f, .0625f + float(i % 13) / 512.f,
                      .03125f + float(i % 11) / 1024.f};
        }
        command = [queue commandBuffer];
        encoder = [command computeCommandEncoder];
        need(command && encoder, "Prefix differential encoder");
        [encoder setComputePipelineState:differential];
        for (unsigned i = 0; i < b.size(); ++i)
            [encoder setBuffer:b[i] offset:i >= 9 ? guard : 0 atIndex:i];
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        error = metal::complete(command, "contribution prefix all-slot differential");
        need(error.empty(), error);
        const auto *out = reinterpret_cast<const std::uint32_t *>(
            static_cast<const unsigned char *>(b[11].contents) + guard);
        for (unsigned n = 0; n < count; ++n) {
            const unsigned variant = n / slots, j = variant == variants ? 0 : n % slots;
            need(out[8 * n] == 0,
                 "Prefix D=" + std::to_string(depth) + " variant=" + std::to_string(variant) +
                     " slot=" + std::to_string(j) + " failure_mask=" + std::to_string(out[8 * n]) +
                     " full_status=" + std::to_string(out[8 * n + 1]) +
                     " prefix_status=" + std::to_string(out[8 * n + 2]) + " full/prefix_steps=" +
                     std::to_string(out[8 * n + 3]) + "/" + std::to_string(out[8 * n + 4]));
            ++tested;
            positive +=
                (variant <= 2 || variant == 5 || variant == 6 || variant == 14) && (j % 2 == 0);
            omitted +=
                (variant == 3 && j > 0) || (variant == 4 && j > 0) || (variant == 11 && j > 2);
            divergent_failures += (out[8 * n + 7] & 3) == 1;
        }
        for (unsigned i : {9u, 10u, 11u}) {
            const auto *bytes = static_cast<const unsigned char *>(b[i].contents);
            for (unsigned k = 0; k < guard; ++k)
                need(bytes[k] == 0xa5 && bytes[b[i].length - guard + k] == 0xa5,
                     "Prefix sidecar/collector/result pool guard");
        }
    }
    need(tested == 2227 && positive == 426 && omitted == 404 && divergent_failures == 274,
         "Prefix fixture coverage witnesses");
    std::cout << tested << " pretraced fixed-slot cases, " << positive
              << " positive endpoint witnesses, " << omitted << " omitted-slot witnesses, "
              << divergent_failures << " explicit full-failure/prefix-success cases passed; "
              << "zero NEE, no AS traversal, synthetic sensor, RR disabled\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Expected contribution-prefix metallib");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
