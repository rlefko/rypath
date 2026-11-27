// SPDX-License-Identifier: GPL-3.0-only
#include "contribution_wire.h"
#include "enhanced_wire.h"
#include "metal_execution.hpp"
#include "render_wire.h"
#include "wavefront_wire.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace rypath;
namespace w = render_wire;
namespace wf = wavefront_wire;
static_assert(wf::contract == 19 && sizeof(wf::Config) == 64 && sizeof(wf::Contract) == 104);
static_assert(contribution_wire::enhanced_canonical_law == 4 && enhanced_wire::record_version == 2);
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

    // Keep the original 2,227 cases unchanged. The additional kernel enables
    // only conductor/coating families needed to witness selected-event cuts.
    for (unsigned i : {5u, 7u, 17u})
        [constants setConstantValue:&yes type:MTLDataTypeBool atIndex:i];
    auto selected_event = metal::load_pipeline(
        device, library, @"rypath_contribution_selected_event", error, nil, constants);
    need(selected_event != nil, error);
    constexpr unsigned event_count = 16, event_depth = 3, event_slots = 7;
    std::array<id<MTLBuffer>, 19> b{};
    b[0] = allocate(sizeof(w::Parameters));
    b[1] = allocate(sizeof(wf::Config));
    b[2] = allocate(8 * sizeof(w::Instance));
    b[3] = allocate(3 * sizeof(w::Point));
    b[4] = allocate(sizeof(w::Triangle));
    b[5] = allocate(5 * sizeof(w::Material));
    b[6] = allocate(2 * sizeof(w::AreaEmission));
    b[7] = allocate(471 * sizeof(float));
    b[8] = allocate(471 * sizeof(w::Point));
    b[9] = allocate(event_count * abi[1] + 2 * guard, true);
    b[10] = allocate(event_count * (event_slots + 1) * 4 * sizeof(float) + 2 * guard, true);
    b[11] = allocate(event_count * 8 * sizeof(std::uint32_t) + 2 * guard, true);
    b[12] = allocate(sizeof(w::TextureParameters));
    b[13] = allocate(sizeof(w::TextureFrame));
    b[14] = allocate(4 * sizeof(w::SpectralKnot));
    b[15] = allocate(sizeof(w::MaterialAuxiliary));
    b[16] = allocate(sizeof(w::TextureGraphParameters));
    b[17] = allocate(3 * sizeof(w::SurfaceVertex));
    b[18] = allocate(8 * sizeof(w::SurfaceTransform));
    auto *p = static_cast<w::Parameters *>(b[0].contents);
    *p = {};
    p->width = p->pixel_count = p->lane_count = event_count;
    p->height = 1;
    p->model = w::pbrt_model;
    p->packet_transport = 2;
    p->max_depth = event_depth;
    p->emergency_limit = 128;
    p->boundary_limit = 2;
    p->instance_count = 8;
    p->material_count = 5;
    p->emission_count = 2;
    p->knot_count = 4;
    p->material_data_words = sizeof(w::MaterialAuxiliary) / sizeof(std::uint32_t);
    p->origin = {0, 0, 2};
    p->camera_normal = {0, 0, 1};
    p->seed_low = 4117;
    p->sampler_kind = w::independent_sampler;
    auto *cfg = static_cast<wf::Config *>(b[1].contents);
    *cfg = {};
    cfg->pixel_count = cfg->capacity = event_count;
    cfg->packet = 2;
    cfg->model = w::pbrt_model;
    cfg->group_width = cfg->packet_events = 1;
    cfg->replay_enabled = cfg->contribution_mode = cfg->contribution_replay = 1;
    cfg->contribution_depth = event_depth;
    cfg->contribution_target_scale = 1;
    cfg->contribution_serial_low = 0xabcdef01u;
    cfg->contribution_serial_high = 17;
    auto *instances = static_cast<w::Instance *>(b[2].contents);
    for (unsigned i = 0; i < 8; ++i) {
        instances[i] = {};
        instances[i].matrix[0] = instances[i].matrix[5] = instances[i].matrix[10] = 1;
        instances[i].triangle_count = 1;
        instances[i].material = i <= 1 ? 0 : i <= 4 ? i - 1 : 4;
        instances[i].normal_begin = w::no_normals;
        instances[i].uv_begin = UINT32_MAX;
        instances[i].emission = i == 7 ? 1 : 0;
    }
    auto *positions = static_cast<w::Point *>(b[3].contents);
    positions[0] = {-2, -2, 0};
    positions[1] = {2, -2, 0};
    positions[2] = {0, 2, 0};
    *static_cast<w::Triangle *>(b[4].contents) = {{0, 1, 2}};
    auto *materials = static_cast<w::Material *>(b[5].contents);
    for (unsigned i = 0; i < 5; ++i) {
        materials[i] = {};
        materials[i].kind = w::constant_material;
        materials[i].scalar_value = .75f;
    }
    materials[0].scattering = 5;
    materials[0].auxiliary = 0;
    materials[0].eta_count = materials[1].eta_count = 2;
    materials[0].u_roughness = .2f;
    materials[0].v_roughness = .3f;
    materials[0].remap_roughness = 0;
    materials[1].scattering = 1;
    materials[1].eta_begin = 2;
    materials[1].k_count = 2; // A finite k is required before the invalid Fresnel sample.
    materials[1].u_roughness = materials[1].v_roughness = .2f;
    materials[1].remap_roughness = 0;
    materials[2].scalar_value = 0;
    materials[3].scalar_value = std::numeric_limits<float>::quiet_NaN();
    auto *emissions = static_cast<w::AreaEmission *>(b[6].contents);
    for (unsigned i = 0; i < 2; ++i) {
        emissions[i] = {};
        emissions[i].two_sided = 1;
        for (float &value : emissions[i].radiance)
            value = i == 0 ? .125f : std::numeric_limits<float>::quiet_NaN();
    }
    auto *cie = static_cast<w::Point *>(b[8].contents);
    for (unsigned i = 0; i < 471; ++i)
        cie[i] = {.125f + float(i % 17) / 256.f, .0625f + float(i % 13) / 512.f,
                  .03125f + float(i % 11) / 1024.f};
    auto *tp = static_cast<w::TextureParameters *>(b[12].contents);
    *tp = {};
    tp->frame_count = 1;
    tp->spp_scale = 1;
    for (unsigned diagonal : {0u, 4u, 8u})
        tp->world_from_camera[diagonal] = tp->camera_from_world[diagonal] = 1;
    tp->minimum_dx = {.01f, 0, 0};
    tp->minimum_dy = {0, .01f, 0};
    *static_cast<w::TextureFrame *>(b[13].contents) = {{4, 0, 0}, {2, 4, 0}};
    auto *knots = static_cast<w::SpectralKnot *>(b[14].contents);
    knots[0] = {360, 1.5f};
    knots[1] = {830, 1.5f};
    // Valid preparation, but eta^2 overflows the conductor Fresnel arithmetic.
    knots[2] = {360, 1e30f};
    knots[3] = {830, 1e30f};
    auto *aux = static_cast<w::MaterialAuxiliary *>(b[15].contents);
    *aux = {};
    aux->eta_root = aux->k_root = aux->conductor_reflectance_root = UINT32_MAX;
    aux->transmittance_root = aux->transmission_scale_root = UINT32_MAX;
    aux->interface_eta_root = aux->interface_u_roughness_root = aux->interface_v_roughness_root =
        UINT32_MAX;
    aux->thickness_root = aux->albedo_root = aux->g_root = aux->mix_amount_root = UINT32_MAX;
    aux->thickness = 1;
    aux->samples = 1;
    aux->max_depth = 4;
    auto *gp = static_cast<w::TextureGraphParameters *>(b[16].contents);
    *gp = {};
    gp->surface_vertex_count = 3;
    auto *vertices = static_cast<w::SurfaceVertex *>(b[17].contents);
    for (unsigned i = 0; i < 3; ++i) {
        vertices[i] = {};
        vertices[i].position = positions[i];
    }
    auto *transforms = static_cast<w::SurfaceTransform *>(b[18].contents);
    for (unsigned i = 0; i < 8; ++i) {
        transforms[i] = {};
        transforms[i].vertex_count = 3;
        for (unsigned diagonal : {0u, 4u, 8u})
            transforms[i].shape[diagonal] = transforms[i].shape_normal[diagonal] =
                transforms[i].placement[diagonal] = transforms[i].placement_normal[diagonal] = 1;
    }
    command = [queue commandBuffer];
    encoder = [command computeCommandEncoder];
    need(command && encoder, "Selected-event encoder");
    [encoder setComputePipelineState:selected_event];
    for (unsigned i = 0; i < b.size(); ++i)
        [encoder setBuffer:b[i] offset:i >= 9 && i <= 11 ? guard : 0 atIndex:i];
    [encoder dispatchThreads:MTLSizeMake(event_count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    error = metal::complete(command, "contribution selected-event elision");
    need(error.empty(), error);
    const auto *out = reinterpret_cast<const std::uint32_t *>(
        static_cast<const unsigned char *>(b[11].contents) + guard);
    unsigned event_finished = 0, event_positive = 0, event_divergences = 0;
    for (unsigned n = 0; n < event_count; ++n) {
        need(out[8 * n] == 0, "Selected-event fixture=" + std::to_string(n / 2) +
                                  " slot=" + std::to_string(2 + n % 2) +
                                  " failure_mask=" + std::to_string(out[8 * n]) +
                                  " full/prefix_status=" + std::to_string(out[8 * n + 1]) + "/" +
                                  std::to_string(out[8 * n + 2]) +
                                  " full/prefix_mask=" + std::to_string(out[8 * n + 3]) + "/" +
                                  std::to_string(out[8 * n + 4]));
        event_finished += (out[8 * n + 7] & 1u) != 0;
        event_positive += (out[8 * n + 7] & 2u) != 0;
        event_divergences += (out[8 * n + 7] & 4u) != 0;
    }
    need(event_finished == 12 && event_positive == 6 && event_divergences == 4,
         "Selected-event success/emission/failure-partition witnesses");
    for (unsigned i : {9u, 10u, 11u}) {
        const auto *bytes = static_cast<const unsigned char *>(b[i].contents);
        for (unsigned k = 0; k < guard; ++k)
            need(bytes[k] == 0xa5 && bytes[b[i].length - guard + k] == 0xa5,
                 "Selected-event sidecar/collector/result pool guard");
    }
    std::cout << "16 selected-event controls, 6 positive arrival endpoints, 4 explicit "
                 "full-failure/prefix-success cases passed; collapse, rejected/invalid "
                 "continuation, retained preparation/emission failure, RR rejection/survival; "
                 "zero NEE, no AS traversal\n";

    auto discovery =
        metal::load_pipeline(device, library, @"rypath_enhanced_discovery_witness", error);
    need(discovery != nil, error);
    auto discovery_finish =
        metal::load_pipeline(device, library, @"rypath_enhanced_discovery_finish", error);
    need(discovery_finish != nil, error);
    constexpr unsigned discovery_count = 6, discovery_words = 36, enhanced_bytes = 284;
    std::array<id<MTLBuffer>, 7> d{};
    d[0] = allocate(sizeof(w::Parameters));
    d[1] = allocate(discovery_count * sizeof(wf::Config));
    d[2] = b[8]; // Same synthetic sensor, checked independently below.
    d[3] = allocate(discovery_count * enhanced_bytes + 2 * guard, true);
    d[4] = allocate(discovery_count * discovery_words * sizeof(std::uint32_t) + 2 * guard, true);
    namespace ew = enhanced_wire;
    d[5] = allocate(ew::bank_count * discovery_count * sizeof(ew::Record) + 2 * guard, true);
    d[6] = allocate(discovery_count * sizeof(ew::Evaluation) + 2 * guard, true);
    auto *owned_records =
        reinterpret_cast<ew::Record *>(static_cast<unsigned char *>(d[5].contents) + guard);
    auto *owned_evaluations =
        reinterpret_cast<ew::Evaluation *>(static_cast<unsigned char *>(d[6].contents) + guard);
    for (unsigned i = 0; i < discovery_count; ++i) {
        const auto source = i == 4 ? ew::Reference{ew::history_bank_0, i} : ew::absent();
        const auto result = i == 4 ? source : ew::Reference{ew::fresh_bank, i};
        owned_evaluations[i] = {source, result, 0.f, 0, 0, 0};
    }
    owned_records[ew::history_bank_0 * discovery_count + 4] = ew::empty_record();
    std::array<ew::Record, ew::bank_count * discovery_count> expected_records{};
    std::memset(expected_records.data(), 0xa5, sizeof(expected_records));
    expected_records[ew::history_bank_0 * discovery_count + 4] = ew::empty_record();
    for (unsigned i = 0; i < discovery_count; ++i)
        if (i != 4)
            expected_records[ew::fresh_bank * discovery_count + i] = ew::empty_record();
    auto *dp = static_cast<w::Parameters *>(d[0].contents);
    *dp = *p;
    dp->width = dp->pixel_count = dp->lane_count = discovery_count;
    auto *dc = static_cast<wf::Config *>(d[1].contents);
    // Fixed identities, chosen before execution: first-cut, second-cut, survive.
    // Actual24-bit RR words at gates0/1 respectively:
    // (11906587,1140079), (6753705,9652826), (7827353,301643).
    constexpr unsigned serials[] = {1003, 1001, 1002, 1003, 1004, 1005};
    for (unsigned i = 0; i < discovery_count; ++i) {
        dc[i] = *cfg;
        dc[i].pixel_count = dc[i].capacity = discovery_count;
        dc[i].contribution_mode = 2;
        dc[i].contribution_serial_low = serials[i];
    }
    command = [queue commandBuffer];
    encoder = [command computeCommandEncoder];
    need(command && encoder, "Enhanced discovery encoder");
    [encoder setComputePipelineState:discovery];
    for (unsigned i = 0; i < d.size(); ++i)
        [encoder setBuffer:d[i] offset:i >= 3 ? guard : 0 atIndex:i];
    [encoder dispatchThreads:MTLSizeMake(discovery_count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
    [encoder setComputePipelineState:discovery_finish];
    [encoder dispatchThreads:MTLSizeMake(discovery_count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    error = metal::complete(command, "Enhanced actual sink discovery witness");
    need(error.empty(), error);
    const auto *discovered = reinterpret_cast<const std::uint32_t *>(
        static_cast<const unsigned char *>(d[4].contents) + guard);
    constexpr unsigned fresh_slot = 0xffffffffu, null_slot = 0xfffffffeu;
    constexpr unsigned wavelengths[] = {400, 500, 600, 700};
    constexpr float sensor_pdfs[] = {1, 2, 4, 8};
    constexpr float coefficients[] = {.0625f, 0, .125f, .25f, .375f, .5f, 1};
    // Independent projection of known terms, not ep::target/finalize/readout.
    std::array<std::array<float, 3>, 7> projected{};
    std::array<long double, 7> targets{};
    for (unsigned slot = 0; slot < 7; ++slot) {
        std::array<long double, 3> xyz{};
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto sensor = cie[wavelengths[lane] - 360];
            const std::array<float, 3> response{sensor.x, sensor.y, sensor.z};
            const long double term =
                static_cast<long double>(coefficients[slot]) * static_cast<long double>(1u << lane);
            for (unsigned channel = 0; channel < 3; ++channel)
                xyz[channel] += static_cast<long double>(response[channel]) * term /
                                static_cast<long double>(sensor_pdfs[lane]) / 4;
        }
        for (unsigned channel = 0; channel < 3; ++channel)
            projected[slot][channel] = static_cast<float>(xyz[channel]);
        if (coefficients[slot] != 0)
            targets[slot] = static_cast<long double>(1e-6f) +
                            ((std::abs(xyz[0]) / 4 + std::abs(xyz[1]) / 4) + std::abs(xyz[2]) / 4);
    }
    constexpr unsigned observed[] = {3, 5, 7, 7, 0, 7};
    constexpr unsigned gates[] = {1, 2, 2, 2, 0, 2};
    constexpr float probability[] = {.5f, .125f, .125f, 1, 1, 1};
    constexpr float arrival[] = {1, .5f, .125f, 1, 1, 1};
    constexpr float direct[] = {.5f, .125f, .125f, 1, 1, 1};
    constexpr unsigned gate1[] = {0, 1, 1, 1, 1, 1};
    constexpr unsigned gate2[] = {2, 0, 1, 1, 1, 1};
    const auto f32 = [](unsigned word) { return std::bit_cast<float>(word); };
    const auto bits = [](float value) { return std::bit_cast<unsigned>(value); };
    const auto exponent = [](unsigned word) { return std::bit_cast<int>(word); };
    const auto decode = [&](unsigned fraction_word, unsigned exponent_word) {
        const auto fraction = f32(fraction_word);
        const auto power = exponent(exponent_word);
        need((fraction_word == 0 && power == 0) ||
                 (std::isfinite(fraction) && fraction >= .5f && fraction < 1 && power >= -4096 &&
                  power <= 4096),
             "Enhanced discovery Number must retain its normalized mantissa/exponent");
        return std::ldexp(static_cast<long double>(fraction), power);
    };
    for (unsigned i = 0; i < discovery_count; ++i) {
        const auto *v = discovered + i * discovery_words;
        const std::string context = "Enhanced discovery fixture=" + std::to_string(i);
        const bool imported = i == 4, black = i == 5;
        need(v[0] == 0 && v[3] == 1 && v[5] == observed[i] &&
                 v[6] == (imported ? 6u : observed[i]) && v[7] == gates[i] &&
                 v[8] == bits(probability[i]) && v[9] == bits(arrival[i]) &&
                 v[10] == bits(direct[i]) && v[11] == unsigned(i < 2) && v[20] == 0 && v[21] == 7 &&
                 v[22] == 0 && v[23] == 0 && v[24] == unsigned(!imported) && v[25] == gate1[i] &&
                 v[26] == gate2[i] && v[31] == i && v[35] == enhanced_bytes,
             context + " actual gate/observation/status/snapshot contract");
        need(v[1] == (imported ? 5u : fresh_slot), context + " request identity");
        const auto &receipt = owned_evaluations[i];
        const auto source = imported ? ew::Reference{ew::history_bank_0, i} : ew::absent();
        const auto result = imported ? source : ew::Reference{ew::fresh_bank, i};
        need(receipt.source.bank == source.bank && receipt.source.pixel == source.pixel &&
                 receipt.result.bank == result.bank && receipt.result.pixel == result.pixel &&
                 bits(receipt.target_h_fraction) == bits(.5f) && receipt.target_h_exponent == 1 &&
                 receipt.ready == 1 && receipt.normalizer_exponent == exponent(v[34]),
             context + " complete owned receipt publishes unit h once after pending validation");
        const unsigned selected = v[2];
        long double expected_sum = 0;
        if (!imported && !black)
            for (unsigned slot = 0; slot < observed[i]; ++slot) {
                const long double a = i == 3 || slot <= 2 ? 1.L : slot <= 4 ? .5L : .125L;
                expected_sum += targets[slot] / a;
            }
        // At most six positive targets and five ordered additions. These fixed
        // relative epsilon allowances cover scaled FLOAT grouping, not MC bias.
        const auto close = [&](long double actual, long double expected, long double epsilons,
                               const char *label) {
            const long double tolerance =
                epsilons * std::numeric_limits<float>::epsilon() * std::abs(expected);
            need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
                 context + " " + label);
        };
        close(decode(v[4], v[34]), expected_sum, 32, "independent discovery-weighted normalizer");
        need(v[4] == v[27] && v[34] == v[33],
             context + " finish preserves selected scaled normalizer");
        if (imported) {
            need(selected == 5 && v[13] == null_slot && v[12] == 0 && v[32] == 0 && v[34] == 0,
                 context + " fixed import has no fresh WRS selection");
        } else if (black) {
            need(selected == null_slot && v[13] == null_slot && v[12] == 0 && v[32] == 0 &&
                     v[34] == 0,
                 context + " all-black publishes explicit null");
        } else {
            need(selected < observed[i] && coefficients[selected] > 0 && v[13] == selected,
                 context + " selected atom was actually observed and nonzero");
            close(decode(v[12], v[32]), targets[selected], 8,
                  "selected canonical target is alpha, not alpha/a");
        }
        for (unsigned channel = 0; channel < 3; ++channel) {
            const unsigned expected = black ? 0u : bits(projected[selected][channel]);
            need(v[14 + channel] == expected, context + " original four sensor PDFs");
            need(v[(imported ? 28u : 17u) + channel] == expected,
                 context + " selected/requested XYZ device persistence");
            if (black || imported)
                need(v[(imported ? 17u : 28u) + channel] == 0,
                     context + " inactive XYZ fields remain initialized");
        }
    }
    need(std::memcmp(owned_records, expected_records.data(), sizeof(expected_records)) == 0,
         "Enhanced generic/null records and untouched arena banks");
    for (unsigned i : {3u, 4u, 5u, 6u}) {
        const auto *bytes = static_cast<const unsigned char *>(d[i].contents);
        for (unsigned k = 0; k < guard; ++k)
            need(bytes[k] == 0xa5 && bytes[d[i].length - guard + k] == 0xa5,
                 "Enhanced discovery sidecar/result pool guard");
    }
    std::cout << "6 actual Enhanced sink controls passed: first/second RR cutoff, both survive, "
                 "unit gates, fixed import, all-black null; independent alpha/a normalizer, "
                 "selected XYZ/target, sensor PDFs and device state284 and owned-record/receipt "
                 "checks; no AS/transport\n";
    // Actual v4 Sink: no geometry is involved, and the host oracle does not use
    // production target/projection or selector helpers.
    auto hybrid_witness =
        metal::load_pipeline(device, library, @"rypath_enhanced_hybrid_sink_witness", error);
    auto hybrid_finish =
        metal::load_pipeline(device, library, @"rypath_enhanced_hybrid_sink_finish", error);
    need(hybrid_witness && hybrid_finish, error);
    constexpr unsigned hybrid_count = 17, hybrid_words = 32;
    std::array<id<MTLBuffer>, 7> hb{};
    hb[0] = allocate(sizeof(w::Parameters));
    hb[1] = allocate(hybrid_count * sizeof(wf::Config));
    hb[2] = allocate(471 * sizeof(w::Point));
    hb[3] = allocate(hybrid_count * enhanced_bytes + 2 * guard, true);
    hb[4] = allocate(hybrid_count * hybrid_words * sizeof(unsigned) + 2 * guard, true);
    hb[5] = allocate(ew::bank_count * hybrid_count * sizeof(ew::Record) + 2 * guard, true);
    hb[6] = allocate(hybrid_count * sizeof(ew::Evaluation) + 2 * guard, true);
    auto *hp = static_cast<w::Parameters *>(hb[0].contents);
    *hp = *dp;
    hp->width = hp->pixel_count = hp->lane_count = hybrid_count;
    auto *hc = static_cast<wf::Config *>(hb[1].contents);
    auto *sensor = static_cast<w::Point *>(hb[2].contents);
    for (unsigned wavelength : {400u, 500u, 600u, 700u})
        sensor[wavelength - 360] = {1, -2, 4};
    auto *hr = reinterpret_cast<ew::Record *>(static_cast<unsigned char *>(hb[5].contents) + guard);
    auto *he =
        reinterpret_cast<ew::Evaluation *>(static_cast<unsigned char *>(hb[6].contents) + guard);
    const auto imported_hybrid = [](unsigned n) { return (n >= 5 && n <= 12) || n == 16; };
    const auto hybrid_slot = [](unsigned n) { return n == 3 || n == 6 || n == 10 ? 5u : 4u; };
    const auto chart_record = [&](unsigned primitive) {
        return ew::Record{ew::record_version,
                          5,
                          bits(0),
                          1,
                          0,
                          primitive,
                          w::triangle_primitive,
                          bits(0),
                          {bits(.25f), bits(.25f), bits(1), 0}};
    };
    const auto source_record = [&](unsigned n) {
        if (n == 9)
            return ew::empty_record();
        if (n == 10)
            return ew::Record{ew::record_version, ew::positional_mass, 0, 2,           ew::invalid,
                              ew::invalid,        ew::invalid,         0, {0, 0, 0, 0}};
        if (n == 12)
            return ew::Record{ew::record_version,
                              6,
                              0,
                              2,
                              0,
                              1,
                              w::triangle_primitive,
                              0,
                              {bits(.25f), bits(.25f), 0, 0}};
        return chart_record(1);
    };
    std::array<ew::Record, ew::bank_count * hybrid_count> expected_hybrid_records{};
    std::memset(expected_hybrid_records.data(), 0xa5, sizeof(expected_hybrid_records));
    for (unsigned n = 0; n < hybrid_count; ++n) {
        hc[n] = dc[0];
        hc[n].pixel_count = hc[n].capacity = hybrid_count;
        hc[n].contribution_depth = 3;
        const auto source =
            imported_hybrid(n) ? ew::Reference{ew::history_bank_0, n} : ew::absent();
        const ew::Reference result{ew::fresh_bank, n};
        he[n] = {source, result, 0, 0, 0, 0};
        if (imported_hybrid(n)) {
            hr[ew::history_bank_0 * hybrid_count + n] = source_record(n);
            expected_hybrid_records[ew::history_bank_0 * hybrid_count + n] = source_record(n);
            if (n != 12 && n != 16)
                expected_hybrid_records[ew::fresh_bank * hybrid_count + n] = source_record(n);
        } else if (n <= 4 || n == 15) {
            expected_hybrid_records[ew::fresh_bank * hybrid_count + n] =
                n == 4 ? ew::empty_record() : chart_record(1);
        }
    }
    command = [queue commandBuffer];
    encoder = [command computeCommandEncoder];
    need(command && encoder, "Hybrid Sink encoder");
    [encoder setComputePipelineState:hybrid_witness];
    for (unsigned i = 0; i < hb.size(); ++i)
        [encoder setBuffer:hb[i] offset:i >= 3 ? guard : 0 atIndex:i];
    [encoder dispatchThreads:MTLSizeMake(hybrid_count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
    [encoder setComputePipelineState:hybrid_finish];
    [encoder dispatchThreads:MTLSizeMake(hybrid_count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    error = metal::complete(command, "Actual hybrid Sink record/projection controls");
    need(error.empty(), error);
    const auto *hybrid_output = reinterpret_cast<const unsigned *>(
        static_cast<const unsigned char *>(hb[4].contents) + guard);
    const auto close_hybrid = [&](long double actual, long double expected,
                                  const std::string &label) {
        // Chosen before execution: <=16 float eps for target/product/ordered
        // sum rounding. Every spectral F oracle here is an exact dyadic.
        const long double bound = 16.L * std::numeric_limits<float>::epsilon() * std::abs(expected);
        need(std::isfinite(actual) && std::abs(actual - expected) <= bound, label);
    };
    for (unsigned n = 0; n < hybrid_count; ++n) {
        const auto *v = hybrid_output + hybrid_words * n;
        const std::string label = "Hybrid Sink fixture=" + std::to_string(n);
        const bool imported = imported_hybrid(n);
        const bool failed = n >= 12;
        need(v[0] == unsigned(!failed) && v[1] == unsigned(failed && n != 15) &&
                 v[3] == unsigned(!failed) && v[18] == enhanced_bytes,
             label + " finish/failure/device ABI");
        need(v[24] == ((4u << 8) | ((imported ? 0u : 4u) << 16)) &&
                 v[25] == (n == 4 ? 0u : null_slot),
             label + " activation follows earlier atoms and never rewrites their winner");
        need(v[19] == unsigned(n != 7) && v[20] == 0 && v[11] == unsigned(n != 7) &&
                 v[12] == (n == 7 ? 0u : 5u) && v[23] == 2 && v[26] == 3,
             label + " single-activation/firsteligible/active record persistence");
        const auto source = imported ? ew::Reference{ew::history_bank_0, n} : ew::absent();
        need(he[n].source.bank == source.bank && he[n].source.pixel == source.pixel &&
                 he[n].result.bank == ew::fresh_bank && he[n].result.pixel == n,
             label + " immutable record owner receipts");
        const int active_h_exponent = n == 7                       ? 1
                                      : n == 0 || n == 1 || n == 5 ? -159
                                      : n == 2 || n == 6           ? 161
                                                                   : 4;
        need(v[27] == bits(.5f) && exponent(v[28]) == active_h_exponent && v[21] == bits(.5f) &&
                 exponent(v[22]) == (n == 1 ? 2 : 1),
             label + " active density/device tail persistence independent of WRS winner");
        if (failed) {
            need(bits(he[n].target_h_fraction) == 0 && he[n].target_h_exponent == 0 &&
                     he[n].normalizer_exponent == 0,
                 label + " failed frame never publishes owned density");
            if (n == 15)
                need(v[31] == 4 && decode(v[29], v[30]) > 0,
                     label + " finish outside guard tested after valid positive selection");
            continue;
        }
        const bool mapped_zero = n >= 7 && n <= 11;
        const int h_power = n == 0 || n == 1 || n == 5 ? -160 : n == 2 || n == 6 ? 160 : 3;
        const int coefficient_power = n == 0 || n == 1 || n == 5 ? 120
                                      : n == 2 || n == 6         ? -120
                                                                 : 0;
        const long double h_value = std::ldexp(1.L, h_power);
        const long double generation = n == 1 ? 2.L : 1.L;
        const long double base = n == 4 ? 1.L : std::ldexp(1.L, coefficient_power) * h_value;
        const std::array<long double, 3> F{{base, -2 * base, 4 * base}};
        for (unsigned channel = 0; channel < 3; ++channel)
            close_hybrid(f32(v[8 + channel]), mapped_zero ? 0.L : F[channel],
                         label + " independent signed Fchart");
        const long double query_h = mapped_zero || n == 4 ? 1.L : h_value;
        close_hybrid(decode(v[16], v[17]), query_h, label + " query h (not hgen)");
        need(v[2] == (imported ? hybrid_slot(n)
                      : n == 4 ? 0u
                               : hybrid_slot(n)),
             label + " parity-independent selected slot");
        if (imported) {
            need(v[4] == 0 && v[5] == 0 && v[14] == 0 && v[31] == null_slot,
                 label + " fixed import never creates fresh WRS mass");
        } else {
            const long double target_h = n == 4 ? 1.L : h_value;
            const long double T = ((std::abs(F[0]) / 4 + std::abs(F[1]) / 4) + std::abs(F[2]) / 4) +
                                  target_h * static_cast<long double>(1e-6f);
            close_hybrid(decode(v[6], v[7]), T, label + " independent canonical target");
            close_hybrid(decode(v[4], v[5]), T / generation, label + " hgen only in fresh score");
            need(v[14] == 7 && v[15] == 7, label + " complete ordered zero prefix and suffix");
        }
        need(v[13] == 0xa5a5a5a5u, label + " unrelated output word untouched");
    }
    need(std::memcmp(hr, expected_hybrid_records.data(), sizeof(expected_hybrid_records)) == 0,
         "Hybrid actual record copies, parity-independent winner and all untouched banks");
    for (unsigned i : {3u, 4u, 5u, 6u}) {
        const auto *bytes = static_cast<const unsigned char *>(hb[i].contents);
        for (unsigned k = 0; k < guard; ++k)
            need(bytes[k] == 0xa5 && bytes[hb[i].length - guard + k] == 0xa5,
                 "Hybrid Sink sidecar/result/record/receipt guard");
    }
    std::cout << "17 actual hybrid Sink controls passed: scaled signed F/hgen, E/N record routing, "
                 "earlier winner, partial import maps, tag6/doubleactivation/outside failures; no "
                 "traversal\n";
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
