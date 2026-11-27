// SPDX-License-Identifier: GPL-3.0-only
#include "area_pair_atlas_oracle.hpp"
#include "area_wire.h"
#include "bsdf_table.hpp"
#include "bsdf_table_fixture.hpp"
#include "enhanced_duplication_wire.h"
#include "enhanced_hybrid_state_shared.h"
#include "enhanced_motion_wire.h"
#include "enhanced_suffix_tape_shared.h"
#include "enhanced_wire.h"
#include "metal_execution.hpp"
#include "rgb_spectrum.hpp"
#include "sobol_shared.h"
#include "wavefront_wire.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using namespace rypath;
namespace wf = wavefront_wire;
namespace aw = area_wire;
static_assert(wf::contract == 23 && sizeof(wf::Contract) == 112 && sizeof(wf::Config) == 64);
static_assert(wf::contribution_evaluation_id == 18 && wf::contribution_state_id == 19);
static_assert(aw::contract == 19 && sizeof(aw::Contract) == 48 && sizeof(aw::Config) == 120);
static_assert(contribution_wire::enhanced_canonical_law == 4 && enhanced_wire::record_version == 2);
static_assert(wf::enhanced_records_id == 20 && wf::enhanced_evaluation_id == 21 &&
              aw::enhanced_records_id == 11 && aw::enhanced_evaluation_id == 12 &&
              wf::enhanced_suffix_id == 22 && wf::work_list_id == 23 &&
              aw::enhanced_suffix_id == 13 && aw::paired_atlas_id == 15 &&
              aw::paired_config_id == 16);
static_assert(sizeof(rypath_area_pair_atlas::Config) == 112 &&
              sizeof(rypath_area_pair_atlas::Entry) == 2);
static_assert(sizeof(wf::Counters) == 64 && offsetof(wf::Counters, work_count) == 52 &&
              offsetof(wf::Config, compact_enhanced_pairs) == 56 &&
              offsetof(wf::Config, mapped_evaluation) == 60);
static_assert(sizeof(enhanced_wire::Record) == 48 && sizeof(enhanced_wire::Reference) == 8 &&
              sizeof(enhanced_wire::Evaluation) == 40);
static_assert(sizeof(contribution_wire::Evaluation) == 16);
void require(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
void area_disabled_wire(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue) {
    std::string why;
    auto contract_pipeline =
        metal::load_pipeline(device, library, @"rypath_area_contract_v19", why);
    require(contract_pipeline != nil, why);
    require([library newFunctionWithName:@"rypath_area_contract"] == nil &&
                [library newFunctionWithName:@"rypath_area_contract_v15"] == nil,
            "Unversioned and stale Area contracts must be absent before reading v16");
    auto contract_buffer = [device newBufferWithLength:sizeof(aw::Contract)
                                               options:MTLResourceStorageModeShared];
    require(contract_buffer != nil, "Area v18 contract allocation");
    std::memset(contract_buffer.contents, 0, contract_buffer.length);
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    require(command && encoder, "Area v18 contract encoder");
    [encoder setComputePipelineState:contract_pipeline];
    [encoder setBuffer:contract_buffer offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    require(metal::complete(command, "Area v18 contract").empty(), "Area contract completion");
    aw::Contract contract{};
    std::memcpy(&contract, contract_buffer.contents, sizeof(contract));
    require(contract.version == 19 && contract.config_bytes == 120 &&
                contract.history_bytes == 88 && contract.temporal_bytes == 92 &&
                contract.accumulator_bytes == 104 && contract.replay_bytes == 32 &&
                contract.counters_bytes == 112 && contract.contribution_evaluation_bytes == 16 &&
                contract.suffix_entry_bytes == 160 && contract.donor_bytes == 16 &&
                contract.paired_config_bytes == 112 && contract.paired_entry_bytes == 2,
            "Area v18 contract mismatch; stale Area libraries are incompatible");
    auto function = metal::load_function(library, @"rypath_area_reset", why);
    auto reset = metal::load_pipeline(device, library, @"rypath_area_reset", why);
    auto prepare = metal::load_pipeline(device, library, @"rypath_area_prepare", why);
    auto merge = metal::load_pipeline(device, library, @"rypath_area_merge", why);
    auto temporal_finish =
        metal::load_pipeline(device, library, @"rypath_area_temporal_finish", why);
    require(function && reset && prepare && merge && temporal_finish, why);
    auto argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
    require(argument_encoder != nil, "Area disabled argument encoder");
    auto arguments = [device newBufferWithLength:argument_encoder.encodedLength
                                         options:MTLResourceStorageModeShared];
    require(arguments != nil, "Area disabled arguments allocation");
    [argument_encoder setArgumentBuffer:arguments offset:0];
    constexpr unsigned pixels = 4, guard = 256;
    std::array<id<MTLBuffer>, aw::paired_config_id + 1> buffers{};
    const std::array<std::size_t, aw::paired_config_id + 1> lengths{
        sizeof(aw::Config) + guard,
        pixels * sizeof(rypath_replay::ReplayState) + guard,
        pixels * sizeof(render_wire::Point) + guard,
        pixels * sizeof(aw::Accumulator) + guard,
        pixels * sizeof(aw::Temporal) + guard,
        pixels * sizeof(aw::History) + guard,
        pixels * sizeof(aw::History) + guard,
        pixels * sizeof(render_wire::Point) + guard,
        sizeof(aw::Counters) + guard,
        pixels * sizeof(float) + guard,
        sizeof(contribution_wire::Evaluation),
        sizeof(enhanced_wire::Record),
        sizeof(enhanced_wire::Evaluation),
        sizeof(rypath_enhanced_suffix_tape::Entry),
        sizeof(enhanced_motion_wire::Donor),
        16,
        sizeof(rypath_area_pair_atlas::Config)};
    for (unsigned i = 0; i < buffers.size(); ++i) {
        buffers[i] = [device newBufferWithLength:lengths[i] options:MTLResourceStorageModeShared];
        require(buffers[i] != nil, "Area disabled pool allocation");
        [argument_encoder setBuffer:buffers[i] offset:0 atIndex:i];
    }
    render_wire::Parameters parameters{};
    parameters.width = parameters.height = 2;
    parameters.pixel_count = pixels;
    parameters.sampler_kind = render_wire::independent_sampler;
    for (bool invalid_depth : {false, true}) {
        for (auto buffer : buffers)
            std::memset(buffer.contents, 0xa5, buffer.length);
        aw::Config config{};
        config.pixel_count = pixels;
        config.width = config.height = 2;
        config.phase = aw::fresh;
        config.target_scale = 1;
        config.contribution_mode = contribution_wire::summed;
        config.contribution_depth = invalid_depth ? 1 : 0;
        std::memcpy(buffers[aw::config_id].contents, &config, sizeof(config));
        command = [queue commandBuffer];
        encoder = [command computeCommandEncoder];
        require(command && encoder, "Area disabled encoding");
        [encoder setBuffer:arguments offset:0 atIndex:1];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:0];
        for (auto buffer : buffers)
            [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder setComputePipelineState:reset];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
        [encoder setComputePipelineState:prepare];
        [encoder dispatchThreads:MTLSizeMake(pixels, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        require(metal::complete(command, "Area disabled contribution binding").empty(),
                "Area disabled completion");
        aw::Counters counters{};
        std::memcpy(&counters, buffers[aw::failure_id].contents, sizeof(counters));
        require(counters.failure == (invalid_depth ? aw::invalid_config : 0u) &&
                    counters.mutations == 0 && counters.accepted == 0 && counters.reserved == 0,
                "Area summed mode/depth validation");
        for (unsigned i = 0; i < buffers.size(); ++i) {
            const auto written = i == aw::config_id    ? sizeof(config)
                                 : i == aw::failure_id ? sizeof(counters)
                                 : i == aw::replay_id && !invalid_depth
                                     ? pixels * sizeof(rypath_replay::ReplayState)
                                     : 0;
            const auto *bytes = static_cast<const unsigned char *>(buffers[i].contents);
            for (std::size_t j = written; j < buffers[i].length; ++j)
                require(bytes[j] == 0xa5,
                        "Area summed execution touched inactive contribution storage/guard");
        }
        if (!invalid_depth) {
            // Exercise production writes, not host value-initialization: the
            // new confidence word remains the legacy zero sentinel after WRS.
            const render_wire::Point leaf{1.f, 2.f, 3.f};
            for (unsigned pixel = 0; pixel < pixels; ++pixel)
                std::memcpy(static_cast<unsigned char *>(buffers[aw::leaf_xyz_id].contents) +
                                pixel * sizeof(leaf),
                            &leaf, sizeof(leaf));
            command = [queue commandBuffer];
            encoder = [command computeCommandEncoder];
            require(command && encoder, "Legacy confidence encoder");
            [encoder setBuffer:arguments offset:0 atIndex:1];
            for (auto buffer : buffers)
                [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            for (auto stage : {merge, temporal_finish}) {
                [encoder setComputePipelineState:stage];
                [encoder dispatchThreads:MTLSizeMake(pixels, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            }
            [encoder endEncoding];
            require(metal::complete(command, "Legacy Area confidence").empty(),
                    "Legacy confidence completion");
            std::memcpy(&counters, buffers[aw::failure_id].contents, sizeof(counters));
            require(counters.failure == 0, "Legacy confidence production failed");
            for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                aw::Temporal temporal{};
                aw::Accumulator accumulator{};
                std::memcpy(&temporal,
                            static_cast<const unsigned char *>(buffers[aw::temporal_id].contents) +
                                pixel * sizeof(temporal),
                            sizeof(temporal));
                std::memcpy(
                    &accumulator,
                    static_cast<const unsigned char *>(buffers[aw::accumulators_id].contents) +
                        pixel * sizeof(accumulator),
                    sizeof(accumulator));
                require(temporal.confidence == 0 && accumulator.confidence == 0,
                        "Legacy production WRS must not manufacture Enhanced confidence");
                require(temporal.normalizer_exponent == 0 && temporal.target_exponent == 0 &&
                            accumulator.score_exponent == 0 && accumulator.target_exponent == 0,
                        "Legacy production must retain actual FLOAT values with zero exponents");
            }
            for (unsigned id : {aw::contribution_evaluation_id, aw::enhanced_records_id,
                                aw::enhanced_evaluation_id, aw::enhanced_suffix_id, aw::donors_id,
                                aw::paired_atlas_id, aw::paired_config_id}) {
                const auto *bytes = static_cast<const unsigned char *>(buffers[id].contents);
                for (std::size_t j = 0; j < buffers[id].length; ++j)
                    require(bytes[j] == 0xa5, "Legacy Area touched an inactive Enhanced sentinel");
            }
        }
    }
}
// Real null ownership uses only the Enhanced sidecar. Invalid transport pools
// make accidental initialization/load/store observable independently of pixels.
void enhanced_null_owners(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue,
                          const wf::Contract &contract) {
    namespace cw = contribution_wire;
    namespace ew = enhanced_wire;
    constexpr unsigned pixels = 79;
    constexpr std::size_t guard = 256;
    std::string why;
    const auto constants_for = [](bool contribution, bool enhanced) {
        auto values = [MTLFunctionConstantValues new];
        const unsigned layout = 0, events = 1;
        [values setConstantValue:&layout type:MTLDataTypeUInt atIndex:wf::packet_layout_constant];
        [values setConstantValue:&events type:MTLDataTypeUInt atIndex:wf::packet_events_constant];
        [values setConstantValue:&contribution
                            type:MTLDataTypeBool
                         atIndex:wf::contribution_mode_constant];
        [values setConstantValue:&enhanced type:MTLDataTypeBool atIndex:wf::enhanced_mode_constant];
        return values;
    };
    auto constants = constants_for(true, true);
    const auto load = [&](NSString *entry) {
        auto result = metal::load_pipeline(device, library, entry, why, nil, constants);
        require(result != nil, "Enhanced null pipeline: " + why);
        return result;
    };
    MTLFunctionConstantValues *count_constants = [constants copy];
    const bool count_slots = true;
    [count_constants setConstantValue:&count_slots
                                 type:MTLDataTypeBool
                              atIndex:wf::queued_slots_constant];
    const auto load_count = [&](NSString *entry) {
        auto result = metal::load_pipeline(device, library, entry, why, nil, count_constants);
        require(result != nil, "Enhanced count pipeline: " + why);
        return result;
    };
    auto reset = load_count(@"rypath_wavefront_reset");
    auto initialize = load(@"rypath_wavefront_initialize");
    auto admit = load(@"rypath_wavefront_admit_all_pixels");
    auto finalize = load(@"rypath_wavefront_finalize_admission");
    auto probe = load(@"rypath_wavefront_null_probe");
    auto pair_probe = load(@"rypath_wavefront_pair_probe");
    auto prepare = load_count(@"rypath_wavefront_prepare_refill");
    auto refill = load(@"rypath_wavefront_refill");
    auto advance = load(@"rypath_wavefront_advance");
    auto function = metal::load_function(library, @"rypath_wavefront_initialize", why, constants);
    require(function != nil, why);
    auto argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
    require(argument_encoder != nil, "Enhanced null argument encoder");
    auto arguments = [device newBufferWithLength:argument_encoder.encodedLength
                                         options:MTLResourceStorageModeShared];
    require(arguments != nil, "Enhanced null argument allocation");
    auto tables = [device newBufferWithLength:sizeof(sobol::Tables) + 2 * guard
                                      options:MTLResourceStorageModeShared];
    auto environment = [device newBufferWithLength:3 * guard options:MTLResourceStorageModeShared];
    require(tables && environment, "Enhanced null unused resources");
    std::memset(tables.contents, 0xff, tables.length);
    std::memset(environment.contents, 0xff, environment.length);
    render_wire::Parameters parameters{};
    parameters.width = parameters.pixel_count = parameters.lane_count = pixels;
    parameters.height = 1;
    parameters.model = render_wire::pbrt_model;
    parameters.packet_transport = 2;
    parameters.sampler_kind = render_wire::independent_sampler;
    parameters.filter_x_radius = parameters.filter_y_radius = .5f;
    parameters.forward = {-.05f, -.05f, 1};
    parameters.right = {.1f, 0, 0};
    parameters.up = {0, .1f, 0};
    parameters.camera_normal = {0, 0, 1};
    parameters.camera_image_area = .01f;
    parameters.camera_plane_depth = 1;
    for (const bool mapped : {false, true})
        for (const bool compact : {false, true})
            for (unsigned capacity :
                 compact ? std::array{1u, 2u, 37u, pixels} : std::array{1u, 2u, 7u, pixels}) {
                if (mapped && !compact)
                    continue;
                const wf::Config config{pixels,
                                        capacity,
                                        2,
                                        render_wire::pbrt_model,
                                        1,
                                        1,
                                        1,
                                        0,
                                        cw::enhanced,
                                        0,
                                        1.f,
                                        cw::prefix_replay,
                                        11,
                                        13,
                                        unsigned(compact),
                                        unsigned(mapped)};
                std::array<id<MTLBuffer>, wf::donors_id + 1> buffers{};
                std::array<std::size_t, wf::donors_id + 1> lengths{};
                lengths.fill(16);
                lengths[wf::config_id] = sizeof(config);
                lengths[wf::counters_id] = sizeof(wf::Counters);
                lengths[wf::queue_in_id] = lengths[wf::queue_out_id] = lengths[wf::free_slots_id] =
                    capacity * sizeof(std::uint32_t);
                lengths[wf::raw_xyz_id] = pixels * sizeof(render_wire::Point);
                lengths[wf::done_id] = pixels * sizeof(std::uint32_t);
                lengths[wf::current_dispatch_id] = lengths[wf::refill_dispatch_id] =
                    sizeof(wf::DispatchArguments);
                lengths[wf::replay_input_id] = pixels * sizeof(rypath_replay::ReplayState);
                lengths[wf::contribution_evaluation_id] = pixels * sizeof(cw::Evaluation);
                lengths[wf::contribution_state_id] = capacity * contract.enhanced_state_bytes;
                lengths[wf::enhanced_records_id] = ew::bank_count * pixels * sizeof(ew::Record);
                lengths[wf::enhanced_evaluation_id] = pixels * sizeof(ew::Evaluation);
                lengths[wf::work_list_id] = compact ? pixels * sizeof(std::uint32_t) : 16;
                lengths[wf::donors_id] = mapped ? pixels * sizeof(enhanced_motion_wire::Donor) : 16;
                lengths[wf::enhanced_suffix_id] =
                    compact ? ew::bank_count * pixels * sizeof(rypath_enhanced_suffix_tape::Entry)
                            : sizeof(rypath_enhanced_suffix_tape::Entry);
                lengths[wf::scalar_state_id] = capacity * contract.scalar_bytes;
                lengths[wf::packet_state_id] = capacity * contract.packet_bytes;
                lengths[wf::packet_hot_id] = capacity * contract.hot_bytes;
                lengths[wf::packet_cold_id] = capacity * contract.cold_bytes;
                lengths[wf::packet_hit_id] = capacity * contract.hit_bytes;
                lengths[wf::hero_state_id] = capacity * contract.hero_bytes;
                lengths[wf::hero_cold_id] = capacity * contract.hero_cold_bytes;
                for (unsigned i = 0; i < buffers.size(); ++i) {
                    buffers[i] = [device newBufferWithLength:lengths[i] + 2 * guard
                                                     options:MTLResourceStorageModeShared];
                    require(buffers[i] != nil, "Enhanced null guarded pool");
                }
                const auto data = [&](unsigned id) {
                    return static_cast<unsigned char *>(buffers[id].contents) + guard;
                };
                const auto unrelated = [](unsigned id) {
                    return id == wf::scalar_state_id || id == wf::packet_state_id ||
                           id == wf::packet_hot_id || id == wf::packet_cold_id ||
                           id == wf::packet_hit_id || id == wf::hero_state_id ||
                           id == wf::hero_cold_id || id == wf::cie_id ||
                           id == wf::enhanced_records_id || id == wf::enhanced_suffix_id;
                };
                const auto bind = [&] {
                    [argument_encoder setArgumentBuffer:arguments offset:0];
                    for (unsigned i = 0; i < buffers.size(); ++i)
                        [argument_encoder setBuffer:buffers[i] offset:guard atIndex:i];
                };
                bool transport_touched = false;
                const auto fresh = [&] {
                    transport_touched = false;
                    for (unsigned i = 0; i < buffers.size(); ++i) {
                        std::memset(buffers[i].contents, 0xa5, buffers[i].length);
                        if (unrelated(i))
                            std::memset(data(i), 0xff, lengths[i]);
                    }
                    std::memcpy(data(wf::config_id), &config, sizeof(config));
                    if (mapped) {
                        auto *donors =
                            reinterpret_cast<enhanced_motion_wire::Donor *>(data(wf::donors_id));
                        for (unsigned pixel = 0; pixel < pixels; ++pixel)
                            donors[pixel] = {
                                pixel / 2,
                                unsigned(rypath_enhanced_motion::Classification::ordinary), 11, 13};
                    }
                    for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                        const auto replay = rypath_replay::fresh({pixel, 17, 19, 23, 29});
                        require(rypath_replay::valid(replay), "Host fresh replay receipt");
                        std::memcpy(data(wf::replay_input_id) + pixel * sizeof(replay), &replay,
                                    sizeof(replay));
                        const cw::Evaluation request{cw::enhanced_null_slot, 0xffffffffu, 0.f, 0};
                        std::memcpy(data(wf::contribution_evaluation_id) + pixel * sizeof(request),
                                    &request, sizeof(request));
                        const ew::Reference reference{ew::history_bank_0, pixel};
                        const ew::Evaluation owned{reference, reference, 0.f, 0, 0, 0, 0, 0};
                        std::memcpy(data(wf::enhanced_evaluation_id) + pixel * sizeof(owned),
                                    &owned, sizeof(owned));
                    }
                    bind();
                };
                using Stage = std::pair<id<MTLComputePipelineState>, unsigned>;
                const auto execute = [&](std::initializer_list<Stage> stages) {
                    auto command = [queue commandBuffer];
                    auto encoder = [command computeCommandEncoder];
                    require(command && encoder, "Enhanced null encoder");
                    [encoder setBuffer:arguments offset:0 atIndex:1];
                    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:0];
                    [encoder setBuffer:environment offset:guard atIndex:7];
                    [encoder setBuffer:tables offset:guard atIndex:12];
                    for (auto buffer : buffers)
                        [encoder useResource:buffer
                                       usage:MTLResourceUsageRead | MTLResourceUsageWrite];
                    for (const auto &[pipeline, count] : stages) {
                        [encoder setComputePipelineState:pipeline];
                        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                        [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
                    }
                    [encoder endEncoding];
                    require(metal::complete(command, "Enhanced null ownership").empty(),
                            "Enhanced null command failed");
                };
                const auto counters = [&] {
                    wf::Counters result{};
                    std::memcpy(&result, data(wf::counters_id), sizeof(result));
                    return result;
                };
                const auto guards = [&](bool scratch_modified = false,
                                        bool donor_modified = false) {
                    for (unsigned i = 0; i < buffers.size(); ++i) {
                        const auto *bytes = static_cast<const unsigned char *>(buffers[i].contents);
                        for (std::size_t j = 0; j < buffers[i].length; ++j) {
                            if (j >= guard && j < guard + lengths[i]) {
                                if (unrelated(i) &&
                                    !(transport_touched && i == wf::hero_state_id) &&
                                    !(scratch_modified && (i == wf::enhanced_records_id ||
                                                           i == wf::enhanced_suffix_id)))
                                    require(bytes[j] == 0xff,
                                            "Null owner touched ray/BSDF/unused pool");
                            } else
                                require(bytes[j] == 0xa5, "Null owner touched a buffer guard");
                        }
                    }
                    if (!mapped)
                        for (std::size_t byte = 0; byte < lengths[wf::donors_id]; ++byte)
                            require(data(wf::donors_id)[byte] == 0xa5,
                                    "Disabled donor sentinel changed");
                    else if (!donor_modified) {
                        const auto *donors = reinterpret_cast<const enhanced_motion_wire::Donor *>(
                            data(wf::donors_id));
                        for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                            const enhanced_motion_wire::Donor expected{
                                pixel / 2,
                                unsigned(rypath_enhanced_motion::Classification::ordinary), 11, 13};
                            require(std::memcmp(&donors[pixel], &expected, sizeof(expected)) == 0,
                                    "Mapped evaluation preserves frozen donor map");
                        }
                    }
                    if (!compact)
                        for (std::size_t byte = 0; byte < lengths[wf::work_list_id]; ++byte)
                            require(data(wf::work_list_id)[byte] == 0xa5,
                                    "Disabled work-list sentinel changed");
                    require(std::memcmp(data(wf::config_id), &config, sizeof(config)) == 0,
                            "Null owner changed immutable config");
                    for (auto buffer : {tables, environment}) {
                        const auto *bytes = static_cast<const unsigned char *>(buffer.contents);
                        for (std::size_t j = 0; j < buffer.length; ++j)
                            require(bytes[j] == 0xff,
                                    "Null owner modified unused external resource");
                    }
                    for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                        const auto replay = rypath_replay::fresh({pixel, 17, 19, 23, 29});
                        require(std::memcmp(data(wf::replay_input_id) + pixel * sizeof(replay),
                                            &replay, sizeof(replay)) == 0,
                                "Null owner modified canonical replay");
                    }
                };
                if (compact) {
                    // Admission must complete imported null requests without creating a
                    // transport owner, even when every ray/BSDF/state byte is poisoned.
                    fresh();
                    execute(
                        {{reset, pixels}, {admit, pixels}, {finalize, 1}, {initialize, capacity}});
                    auto result = counters();
                    require(result.failure == 0 && result.current == 0 && result.work_count == 0 &&
                                result.next_pixel == pixels && result.completed == pixels &&
                                result.null_completed == pixels,
                            "All-null admission must issue zero transport owners");
                    require(result.queued_slots_lo == 0 && result.queued_slots_hi == 0,
                            "Compacted all-null admission queues zero packet calls");
                    for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                        const auto *done =
                            reinterpret_cast<const std::uint32_t *>(data(wf::done_id));
                        const auto *request = reinterpret_cast<const cw::Evaluation *>(
                            data(wf::contribution_evaluation_id));
                        const auto *owned = reinterpret_cast<const ew::Evaluation *>(
                            data(wf::enhanced_evaluation_id));
                        require(done[pixel] == 1 && request[pixel].ready == 1 &&
                                    request[pixel].selected_slot == cw::enhanced_null_slot &&
                                    std::bit_cast<std::uint32_t>(request[pixel].normalizer) == 0 &&
                                    owned[pixel].ready == 1 &&
                                    owned[pixel].target_h_fraction == .5f &&
                                    owned[pixel].target_h_exponent == 1,
                                "Admission null must publish canonical CW/EW and done once");
                    }
                    for (unsigned id : {wf::work_list_id, wf::contribution_state_id})
                        for (std::size_t byte = 0; byte < lengths[id]; ++byte)
                            require(data(id)[byte] == 0xa5,
                                    "All-null admission touched an owner/list pool");
                    for (std::size_t byte = 0; byte < lengths[wf::raw_xyz_id]; ++byte)
                        require(data(wf::raw_xyz_id)[byte] == 0,
                                "Admission null XYZ must be positive zero");
                    guards();

                    // Distinct mutation scratch is invalidated; all source banks and
                    // same-reference records/tapes remain immutable byte witnesses.
                    fresh();
                    auto *owned =
                        reinterpret_cast<ew::Evaluation *>(data(wf::enhanced_evaluation_id));
                    owned[0].result = {ew::fresh_bank, 0};
                    owned[0].suffix_context_low = 123;
                    owned[1].source = owned[1].result = {ew::fresh_bank, 1};
                    owned[1].suffix_context_low = 123;
                    execute({{reset, pixels}, {admit, pixels}, {finalize, 1}});
                    result = counters();
                    require(result.failure == 0 && result.completed == pixels,
                            "Null mutation admission succeeds");
                    const auto empty_record = ew::empty_record();
                    const rypath_enhanced_suffix_tape::Entry empty_tape{};
                    for (unsigned bank = 0; bank < ew::bank_count; ++bank)
                        for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                            const auto index = bank * pixels + pixel;
                            const auto *record =
                                data(wf::enhanced_records_id) + index * sizeof(ew::Record);
                            const auto *tape =
                                data(wf::enhanced_suffix_id) + index * sizeof(empty_tape);
                            if (bank == ew::fresh_bank && pixel == 0) {
                                require(std::memcmp(record, &empty_record, sizeof(empty_record)) ==
                                                0 &&
                                            std::memcmp(tape, &empty_tape, sizeof(empty_tape)) == 0,
                                        "Selected-null mutation clears only its exclusive scratch");
                            } else {
                                for (std::size_t byte = 0; byte < sizeof(ew::Record); ++byte)
                                    require(record[byte] == 0xff,
                                            "Admission changed an immutable record");
                                for (std::size_t byte = 0; byte < sizeof(empty_tape); ++byte)
                                    require(tape[byte] == 0xff,
                                            "Admission changed an immutable tape");
                            }
                        }

                    guards(true);

                    // Classification is structural: imported and fresh real requests retain
                    // their actual pixel IDs regardless of zero/poisoned source data.
                    for (const bool all_real : {false, true}) {
                        fresh();
                        auto *request = reinterpret_cast<cw::Evaluation *>(
                            data(wf::contribution_evaluation_id));
                        unsigned expected = 0;
                        for (unsigned pixel = 0; pixel < pixels; ++pixel)
                            if (all_real || pixel % 2 == 0) {
                                request[pixel].requested_slot = all_real ? 0xffffffffu : 0u;
                                if (all_real) {
                                    auto *receipts = reinterpret_cast<ew::Evaluation *>(
                                        data(wf::enhanced_evaluation_id));
                                    receipts[pixel].source = ew::absent();
                                    receipts[pixel].result = {ew::fresh_bank, pixel};
                                }
                                ++expected;
                            }
                        execute({{reset, pixels}, {admit, pixels}, {finalize, 1}});
                        result = counters();
                        require(result.failure == 0 && result.work_count == expected &&
                                    result.null_completed == pixels - expected &&
                                    result.completed == pixels - expected &&
                                    result.current == std::min(capacity, expected) &&
                                    result.next_pixel == pixels - expected + result.current,
                                "Mixed admission conserves logical and physical ownership");
                        std::array<unsigned, pixels> seen{};
                        const auto *list =
                            reinterpret_cast<const std::uint32_t *>(data(wf::work_list_id));
                        for (unsigned index = 0; index < expected; ++index) {
                            require(list[index] < pixels && (all_real || list[index] % 2 == 0),
                                    "Work list must contain actual real-owner pixel IDs");
                            require(++seen[list[index]] == 1,
                                    "Work list duplicated an admitted owner");
                        }
                        for (unsigned pixel = 0; pixel < pixels; ++pixel)
                            require(seen[pixel] == unsigned(all_real || pixel % 2 == 0),
                                    "Work list dropped an admitted owner");
                        for (std::size_t byte = expected * sizeof(std::uint32_t);
                             byte < lengths[wf::work_list_id]; ++byte)
                            require(data(wf::work_list_id)[byte] == 0xa5,
                                    "Admission exceeded work-list range");
                        guards();
                        // Initialize and refill are the production kernels. Only final
                        // transport responses are synthetic: pair_probe publishes the
                        // sidecar's actual owner pixel through the production done/free
                        // helpers, without tracing or manufacturing CW/EW replies.
                        // This synthetic producer is not a frame_sensor witness.
                        transport_touched = true;
                        execute({{initialize, capacity}});
                        require(counters().failure == 0, "Compact real-owner initialization");
                        unsigned rounds = 0;
                        while (counters().completed != pixels) {
                            require(rounds++ < pixels, "Compact refill must converge");
                            execute({{pair_probe, capacity},
                                     {prepare, 1},
                                     {refill, capacity},
                                     {advance, 1}});
                            result = counters();
                            require(result.failure == 0 &&
                                        result.null_completed == pixels - expected &&
                                        result.work_count == expected &&
                                        result.completed <= result.next_pixel &&
                                        result.current == result.next_pixel - result.completed,
                                    "Compact refill must preserve the logical queue equation");
                            const auto physical_completed =
                                result.completed - result.null_completed;
                            const auto physical_issued = result.next_pixel - result.null_completed;
                            const auto *done =
                                reinterpret_cast<const std::uint32_t *>(data(wf::done_id));
                            for (unsigned index = 0; index < expected; ++index)
                                require(
                                    done[list[index]] == (index < physical_completed ? 1u
                                                          : index < physical_issued  ? 2u
                                                                                     : 0u),
                                    "Refill must claim and complete actual pixels exactly once");
                            guards();
                            std::swap(buffers[wf::queue_in_id], buffers[wf::queue_out_id]);
                            bind();
                        }
                        require(result.queued_slots_hi == 0 && result.queued_slots_lo == expected,
                                "Compacted mixed/real queues count each synthetic step once");
                        execute({{prepare, 1}, {advance, 1}});
                        require(counters().queued_slots_lo == expected,
                                "Compacted empty tail contributes zero calls");
                        for (unsigned pixel = 0; pixel < pixels; ++pixel)
                            require(reinterpret_cast<const std::uint32_t *>(
                                        data(wf::done_id))[pixel] == 1,
                                    "Every compact logical owner must finish exactly once");
                        const auto *xyz =
                            reinterpret_cast<const render_wire::Point *>(data(wf::raw_xyz_id));
                        for (unsigned pixel = 0; pixel < pixels; ++pixel)
                            require(xyz[pixel].x == (mapped && (all_real || pixel % 2 == 0)
                                                         ? float(pixel / 2)
                                                         : 0.f) &&
                                        xyz[pixel].y == 0 && xyz[pixel].z == 0,
                                    "Mapped sample pixel s is validated while raw publication "
                                    "remains at owner d");
                    }
                    // A duplicated real ID must fail during initialization or refill,
                    // including capacity1 where its first owner has already completed.
                    fresh();
                    auto *duplicates =
                        reinterpret_cast<cw::Evaluation *>(data(wf::contribution_evaluation_id));
                    for (unsigned pixel = 0; pixel < pixels; ++pixel)
                        duplicates[pixel].requested_slot = 0;
                    execute({{reset, pixels}, {admit, pixels}, {finalize, 1}});
                    auto *duplicate_list =
                        reinterpret_cast<std::uint32_t *>(data(wf::work_list_id));
                    duplicate_list[1] = duplicate_list[0];
                    transport_touched = true;
                    execute({{initialize, capacity}});
                    if (capacity == 1)
                        execute({{pair_probe, 1}, {prepare, 1}, {refill, 1}});
                    require(
                        counters().failure == wf::duplicate_sample,
                        "Duplicate compact list entry must fail before a second owner publishes");
                    guards();
                    if (mapped)
                        for (unsigned fault = 0; fault < 4; ++fault) {
                            fresh();
                            auto *donors = reinterpret_cast<enhanced_motion_wire::Donor *>(
                                data(wf::donors_id));
                            if (fault == 0)
                                donors[0].source_pixel = pixels;
                            else if (fault == 1)
                                donors[0].classification =
                                    unsigned(rypath_enhanced_motion::Classification::cold);
                            else if (fault == 2)
                                donors[0].serial_high ^= 1;
                            else
                                donors[0].classification = 99;
                            execute({{reset, pixels}, {admit, 1}});
                            require(counters().failure == wf::invalid_sample &&
                                        counters().completed == 0 && counters().null_completed == 0,
                                    "Mapped poisoned null admission validates donor "
                                    "bounds/classification/full serial");
                            guards(false, true);
                        }
                    // A single-lane malformed request must fail before publishing.
                    for (unsigned fault = 0; fault < 8; ++fault) {
                        fresh();
                        auto *request = reinterpret_cast<cw::Evaluation *>(
                            data(wf::contribution_evaluation_id));
                        owned =
                            reinterpret_cast<ew::Evaluation *>(data(wf::enhanced_evaluation_id));
                        switch (fault) {
                        case 0:
                            request[0].ready = 1;
                            break;
                        case 1:
                            request[0].normalizer = 1;
                            break;
                        case 2:
                            request[0].requested_slot = 1;
                            break;
                        case 3:
                            owned[0].ready = 1;
                            break;
                        case 4:
                            owned[0].source.bank = ew::bank_count;
                            break;
                        case 5:
                            owned[0].result.pixel = pixels;
                            break;
                        case 6:
                            owned[0].target_h_exponent = 1;
                            break;
                        case 7:
                            owned[0].normalizer_exponent = 1;
                            break;
                        }
                        execute({{reset, pixels}, {admit, 1}});
                        result = counters();
                        require(result.failure == wf::invalid_sample && result.completed == 0 &&
                                    result.null_completed == 0 && result.work_count == 0,
                                "Malformed admission receipt must fail visibly before completion");
                        guards();
                    }
                    for (const bool out_of_range : {false, true}) {
                        fresh();
                        auto *request = reinterpret_cast<cw::Evaluation *>(
                            data(wf::contribution_evaluation_id));
                        request[0].requested_slot = 0;
                        execute({{reset, pixels}, {admit, pixels}, {finalize, 1}});
                        auto *list = reinterpret_cast<std::uint32_t *>(data(wf::work_list_id));
                        // pixel1 was completed analytically; reusing it must reject
                        // duplicate ownership before transport initialization writes.
                        list[0] = out_of_range ? pixels : 1u;
                        execute({{initialize, 1}});
                        require(counters().failure ==
                                    (out_of_range ? wf::invalid_sample : wf::duplicate_sample),
                                "Damaged list must reject out-of-range and completed owner IDs");
                        guards();
                    }
                    // Capacity overflow is checked before writes, including a damaged
                    // atomic append counter. Finalization independently rejects it.
                    fresh();
                    auto *request =
                        reinterpret_cast<cw::Evaluation *>(data(wf::contribution_evaluation_id));
                    request[0].requested_slot = 0;
                    execute({{reset, pixels}});
                    result = counters();
                    result.work_count = pixels;
                    std::memcpy(data(wf::counters_id), &result, sizeof(result));
                    execute({{admit, 1}});
                    require(counters().failure == wf::queue_overflow,
                            "Admission overflow must be sticky");
                    guards();
                    fresh();
                    execute({{reset, pixels}});
                    result = counters();
                    result.work_count = pixels + 1;
                    std::memcpy(data(wf::counters_id), &result, sizeof(result));
                    execute({{finalize, 1}});
                    require(counters().failure == wf::invalid_counters,
                            "Finalization rejects damaged admission counts");
                    guards();
                    continue;
                }
                // The two-word diagnostic is checked before refill overwrites free.
                for (const bool overflow : {false, true}) {
                    fresh();
                    execute({{reset, pixels}, {initialize, capacity}, {probe, capacity}});
                    auto injected = counters();
                    require(injected.queued_slots_lo == 0 && injected.queued_slots_hi == 0,
                            "Each leaf reset clears the queued-slot diagnostic");
                    injected.queued_slots_lo = 0xffffffffu;
                    injected.queued_slots_hi = overflow ? 0xffffffffu : 41u;
                    std::memcpy(data(wf::counters_id), &injected, sizeof(injected));
                    execute({{prepare, 1}});
                    const auto actual = counters();
                    if (overflow)
                        require(actual.failure == wf::invalid_counters &&
                                    actual.queued_slots_lo == injected.queued_slots_lo &&
                                    actual.queued_slots_hi == injected.queued_slots_hi &&
                                    actual.free == injected.free &&
                                    actual.next_pixel == injected.next_pixel,
                                "High overflow fails before count/refill mutation");
                    else
                        require(actual.failure == 0 && actual.queued_slots_hi == 42 &&
                                    actual.queued_slots_lo == capacity - 1,
                                "Queued-slot low carry increments the high word exactly once");
                }
                fresh();
                execute({{reset, pixels}, {initialize, capacity}});
                auto before = counters();
                require(before.failure == 0 && before.current == capacity &&
                            before.next_pixel == capacity && before.completed == 0 &&
                            before.null_completed == 0 && before.work_count == 0,
                        "Null initialization must issue owners without completing or tracing");
                guards();
                unsigned rounds = 0;
                while (before.completed != pixels) {
                    require(rounds < pixels, "Null owner refill failed to converge");
                    execute({{probe, capacity}, {prepare, 1}, {refill, capacity}, {advance, 1}});
                    const auto result = counters();
                    const auto completed = std::min(pixels, (rounds + 1) * capacity);
                    const auto issued = std::min(pixels, (rounds + 2) * capacity);
                    require(result.failure == 0 && result.completed == completed &&
                                result.null_completed == completed && result.next_pixel == issued &&
                                result.current == issued - completed && result.out == 0 &&
                                result.free == 0 && result.refill_begin == 0 &&
                                result.rounds == rounds + 1,
                            "Null owners must conserve completions through real refill/advance");
                    require(result.queued_slots_hi == 0 &&
                                result.queued_slots_lo == result.null_completed,
                            "Noncompact all-null queues subtract to zero packet calls");
                    for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                        cw::Evaluation evaluation{};
                        std::memcpy(&evaluation,
                                    data(wf::contribution_evaluation_id) +
                                        pixel * sizeof(evaluation),
                                    sizeof(evaluation));
                        require(
                            evaluation.requested_slot == cw::enhanced_null_slot &&
                                evaluation.selected_slot ==
                                    (pixel < completed ? cw::enhanced_null_slot : 0xffffffffu) &&
                                std::bit_cast<std::uint32_t>(evaluation.normalizer) == 0 &&
                                evaluation.ready == unsigned(pixel < completed),
                            "Null result must publish ready exactly once with canonical zero");
                        ew::Evaluation owned{};
                        std::memcpy(&owned,
                                    data(wf::enhanced_evaluation_id) + pixel * sizeof(owned),
                                    sizeof(owned));
                        require(
                            owned.source.bank == ew::history_bank_0 &&
                                owned.source.pixel == pixel &&
                                owned.result.bank == ew::history_bank_0 &&
                                owned.result.pixel == pixel &&
                                std::bit_cast<std::uint32_t>(owned.target_h_fraction) ==
                                    std::bit_cast<std::uint32_t>(pixel < completed ? .5f : 0.f) &&
                                owned.target_h_exponent == (pixel < completed ? 1 : 0) &&
                                owned.ready == unsigned(pixel < completed) &&
                                owned.normalizer_exponent == 0,
                            "Null owner must publish unit h once without changing owned "
                            "references");
                        std::uint32_t done;
                        std::memcpy(&done, data(wf::done_id) + pixel * sizeof(done), sizeof(done));
                        require(done == unsigned(pixel < completed),
                                "Null done ownership mismatch");
                        for (std::size_t byte = 0; byte < sizeof(render_wire::Point); ++byte)
                            require(
                                data(wf::raw_xyz_id)[pixel * sizeof(render_wire::Point) + byte] ==
                                    (pixel < completed ? 0 : 0xa5),
                                "Only completed null owners may publish raw zero");
                    }
                    guards();
                    std::swap(buffers[wf::queue_in_id], buffers[wf::queue_out_id]);
                    bind();
                    before = result;
                    ++rounds;
                }
                execute({{prepare, 1}, {advance, 1}});
                require(counters().queued_slots_hi == 0 && counters().queued_slots_lo == pixels,
                        "Noncompact empty tail leaves exact queued total unchanged");
                if (capacity != 2)
                    continue;
                // Serial single-lane failures avoid a valid sibling racing publication.
                // Every error must preserve zero physical/null completions and raw output.
                for (unsigned fault = 0; fault < 17; ++fault) {
                    fresh();
                    auto *request =
                        reinterpret_cast<cw::Evaluation *>(data(wf::contribution_evaluation_id));
                    auto *owned =
                        reinterpret_cast<ew::Evaluation *>(data(wf::enhanced_evaluation_id));
                    const bool fails_initialize = fault == 0 || (fault >= 5 && fault <= 12);
                    if (fault == 0)
                        request[0].ready = 1;
                    else if (fault == 5)
                        request[0].requested_slot = 1; // D0 has only slot0, plus the null tag.
                    else if (fault == 6)
                        owned[0].ready = 1;
                    else if (fault == 7)
                        owned[0].normalizer_exponent = 1;
                    else if (fault == 8)
                        owned[0].source.bank = ew::bank_count;
                    else if (fault == 9)
                        owned[0].source.pixel = pixels;
                    else if (fault == 10)
                        owned[0].result.bank = ew::bank_count;
                    else if (fault == 11)
                        owned[0].result.pixel = pixels;
                    else if (fault == 12)
                        owned[0].target_h_exponent = 1; // Pending h is exactly the zero sentinel.
                    execute({{reset, pixels}, {initialize, 1}});
                    if (!fails_initialize) {
                        if (fault == 1)
                            request[0].requested_slot = 0; // Changed after null owner capture.
                        else if (fault == 2)
                            request[0].ready = 1;
                        else if (fault == 3)
                            reinterpret_cast<std::uint32_t *>(data(wf::done_id))[0] = 1;
                        else if (fault == 4) {
                            auto value = counters();
                            value.null_completed =
                                1; // Isolate null>completed under Enhanced constants.
                            value.out =
                                capacity; // All other advance conservation predicates are valid.
                            std::memcpy(data(wf::counters_id), &value, sizeof(value));
                        } else if (fault == 13)
                            owned[0].ready = 1;
                        else if (fault == 14)
                            owned[0].normalizer_exponent = 1;
                        else if (fault == 15)
                            owned[0].source.bank = ew::bank_count;
                        else if (fault == 16) {
                            // The null path must not accept any started hybrid
                            // preparation, even though it never loads ray/BSDF pools.
                            const std::size_t hybrid_offset =
                                contract.enhanced_state_bytes -
                                sizeof(rypath_enhanced_hybrid::HybridState) -
                                2 * sizeof(std::uint32_t);
                            const std::size_t offset =
                                hybrid_offset +
                                offsetof(rypath_enhanced_hybrid::HybridState, primary_ready);
                            const std::uint32_t poisoned = 1;
                            std::memcpy(data(wf::contribution_state_id) + offset, &poisoned,
                                        sizeof(poisoned));
                        }
                        execute(fault == 4 ? std::initializer_list<Stage>{{advance, 1}}
                                           : std::initializer_list<Stage>{{probe, 1}});
                    }
                    const auto result = counters();
                    const auto failure = fault == 3   ? wf::duplicate_sample
                                         : fault == 4 ? wf::invalid_counters
                                                      : wf::invalid_sample;
                    require(result.failure == failure && result.completed == 0 &&
                                result.null_completed == (fault == 4 ? 1u : 0u) && result.free == 0,
                            "Invalid null receipt must fail without completions at case " +
                                std::to_string(fault));
                    for (std::size_t byte = 0; byte < lengths[wf::raw_xyz_id]; ++byte)
                        require(data(wf::raw_xyz_id)[byte] == 0xa5,
                                "Failed null owner published raw");
                    guards();
                }
                for (const auto mode : {std::array{true, false}, std::array{false, true}}) {
                    fresh();
                    auto mismatch = metal::load_pipeline(device, library, @"rypath_wavefront_reset",
                                                         why, nil, constants_for(mode[0], mode[1]));
                    require(mismatch != nil, why);
                    execute({{mismatch, pixels}});
                    const auto result = counters();
                    require(result.failure == wf::invalid_config && result.current == 0 &&
                                result.next_pixel == 0 && result.completed == 0 &&
                                result.null_completed == 0,
                            "Enhanced function/buffer mode mismatch must claim no owner");
                    guards();
                }
            }
}
// Protocol fixture only: transport responses are injected after production
// prepare. The scalar reference uses direct double MIS formulas, not shared
// reservoir/target/confidence helpers or the GPU's categorical decisions.
void enhanced_area_protocol(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue,
                            bool duplication_probe = false) {
    namespace cw = contribution_wire;
    namespace ew = enhanced_wire;
    // Radius16 is the smallest authored reciprocal stencil; this extent
    // gives it a real in-bounds edge without a large scene or more dispatches.
    constexpr unsigned width = 17, height = 3, pixels = width * height;
    constexpr std::size_t guard = 256;
    std::string why;
    const auto load = [&](NSString *name) {
        auto value = metal::load_pipeline(device, library, name, why);
        require(value != nil, "Area protocol pipeline: " + why);
        return value;
    };
    auto reset = load(@"rypath_area_reset"), prepare = load(@"rypath_area_prepare");
    auto merge = load(@"rypath_area_merge"), finish = load(@"rypath_area_temporal_finish");
    auto spatial_begin = load(@"rypath_area_spatial_begin"),
         finalize = load(@"rypath_area_finalize");
    auto mutation_prepare = load(@"rypath_area_mutation_prepare");
    auto mutation_finish = load(@"rypath_area_mutation_finish");
    auto cache_evaluation = load(@"rypath_area_cache_evaluation");
    auto function = metal::load_function(library, @"rypath_area_reset", why);
    require(function != nil, why);
    auto argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
    require(argument_encoder != nil, "Area protocol argument encoder");
    auto arguments = [device newBufferWithLength:argument_encoder.encodedLength
                                         options:MTLResourceStorageModeShared];
    require(arguments != nil, "Area protocol arguments");
    std::array<id<MTLBuffer>, aw::paired_config_id + 1> buffers{};
    std::array<std::size_t, aw::paired_config_id + 1> lengths{
        sizeof(aw::Config),
        pixels * sizeof(rypath_replay::ReplayState),
        pixels * sizeof(render_wire::Point),
        pixels * sizeof(aw::Accumulator),
        pixels * sizeof(aw::Temporal),
        pixels * sizeof(aw::History),
        pixels * sizeof(aw::History),
        pixels * sizeof(render_wire::Point),
        sizeof(aw::Counters),
        4 * pixels * (sizeof(float) + sizeof(int)),
        pixels * sizeof(cw::Evaluation),
        ew::bank_count * pixels * sizeof(ew::Record),
        pixels * sizeof(ew::Evaluation),
        sizeof(rypath_enhanced_suffix_tape::Entry),
        pixels * sizeof(enhanced_motion_wire::Donor),
        16,
        sizeof(rypath_area_pair_atlas::Config)};
    for (unsigned i = 0; i < buffers.size(); ++i) {
        buffers[i] = [device newBufferWithLength:lengths[i] + 2 * guard
                                         options:MTLResourceStorageModeShared];
        require(buffers[i] != nil, "Area protocol guarded allocation");
        std::memset(buffers[i].contents, 0xa5, buffers[i].length);
    }
    const auto data = [&](unsigned id) {
        return static_cast<unsigned char *>(buffers[id].contents) + guard;
    };
    auto *temporal = reinterpret_cast<aw::Temporal *>(data(aw::temporal_id));
    auto *accumulator = reinterpret_cast<aw::Accumulator *>(data(aw::accumulators_id));
    auto *leaf = reinterpret_cast<render_wire::Point *>(data(aw::leaf_xyz_id));
    auto *raw = reinterpret_cast<render_wire::Point *>(data(aw::raw_xyz_id));
    auto *requests = reinterpret_cast<cw::Evaluation *>(data(aw::contribution_evaluation_id));
    auto *evaluations = reinterpret_cast<ew::Evaluation *>(data(aw::enhanced_evaluation_id));
    auto *records = reinterpret_cast<ew::Record *>(data(aw::enhanced_records_id));
    namespace tape = rypath_enhanced_suffix_tape;
    auto *suffixes = reinterpret_cast<tape::Entry *>(data(aw::enhanced_suffix_id));
    const auto suffix_at = [&](ew::Reference ref) -> tape::Entry & {
        require(ref.bank < ew::bank_count && ref.pixel < pixels, "Suffix reference bounds");
        return suffixes[ref.bank * pixels + ref.pixel];
    };
    const auto record_at = [&](ew::Reference ref) -> ew::Record & {
        require(ref.bank < 4 && ref.pixel < pixels, "Area protocol record reference bounds");
        return records[ref.bank * pixels + ref.pixel];
    };
    const auto same = [](const auto &a, const auto &b) {
        static_assert(sizeof(a) == sizeof(b));
        return std::memcmp(&a, &b, sizeof(a)) == 0;
    };
    const auto near = [](double actual, long double expected, const std::string &label) {
        // At most five positive scores and vector terms, each with a bounded
        // handful of rounded products/divisions. 64 epsilon covers their linear
        // rounding bound; no threshold is derived from observed test output.
        const auto tolerance =
            64.L * std::numeric_limits<float>::epsilon() * std::max(1.L, std::abs(expected));
        require(std::abs(static_cast<long double>(actual) - expected) <= tolerance,
                label + ": actual=" + std::to_string(actual) +
                    " expected=" + std::to_string(static_cast<double>(expected)));
    };
    const auto proxy = [](render_wire::Point xyz, double h) {
        if (xyz.x == 0 && xyz.y == 0 && xyz.z == 0)
            return 0.f;
        const long double energy =
            (std::abs(static_cast<long double>(xyz.x)) + std::abs(static_cast<long double>(xyz.y)) +
             std::abs(static_cast<long double>(xyz.z))) /
            4;
        return static_cast<float>(energy + static_cast<long double>(1e-6f) * h);
    };
    const auto scale = [](float value) {
        int exponent;
        const auto fraction = std::frexp(value, &exponent);
        return std::pair{fraction, exponent};
    };
    // Independent wire decoding: no production scaled arithmetic or target
    // helper participates in the expected MIS/UCW calculations below.
    const auto number = [](float fraction, int exponent) -> long double {
        require((std::bit_cast<unsigned>(fraction) == 0 && exponent == 0) ||
                    (std::isfinite(fraction) && fraction >= .5f && fraction < 1 &&
                     exponent >= -4096 && exponent <= 4096),
                "Enhanced serialized Number must be zero or normalized within its domain");
        return std::ldexp(static_cast<long double>(fraction), exponent);
    };
    const auto scalar_near = [&](float fraction, int exponent, long double expected,
                                 const std::string &label) {
        const auto actual = number(fraction, exponent);
        // Unlike signed vector sums, these nonnegative scalar sums have no
        // cancellation. An absolute floor would incorrectly admit a lost tiny Z.
        const auto tolerance = 64.L * std::numeric_limits<float>::epsilon() * std::abs(expected);
        require(std::abs(actual - expected) <= tolerance,
                label + ": fraction=" + std::to_string(fraction) +
                    " exponent=" + std::to_string(exponent));
    };
    const auto descriptor = [](unsigned pixel, unsigned revision) {
        if (pixel == 0)
            return ew::empty_record();
        return ew::Record{
            ew::record_version,
            ew::triangle_area,
            100 + revision * 10 + pixel,
            0,
            pixel,
            pixel + 3,
            render_wire::triangle_primitive,
            0,
            {std::bit_cast<std::uint32_t>(.25f), std::bit_cast<std::uint32_t>(.5f), 0, 0}};
    };
    aw::Config config{};
    config.pixel_count = pixels;
    config.width = width;
    config.height = height;
    config.target_scale = 1;
    config.output_mode = aw::conditional_mean_output;
    config.contribution_mode = cw::enhanced;
    config.contribution_depth = 1;
    config.seed_low = 29;
    render_wire::Parameters parameters{};
    parameters.width = width;
    parameters.height = height;
    parameters.pixel_count = parameters.lane_count = pixels;
    parameters.sampler_kind = render_wire::independent_sampler;
    parameters.packet_transport = 2;
    parameters.max_depth = 1;
    parameters.seed_low = 31;
    const auto bind = [&] {
        [argument_encoder setArgumentBuffer:arguments offset:0];
        for (unsigned i = 0; i < buffers.size(); ++i)
            [argument_encoder setBuffer:buffers[i] offset:guard atIndex:i];
    };
    bind();
    id<MTLBuffer> duplication_arguments = nil;
    std::vector<id<MTLBuffer>> duplication_resources;
    const auto dispatch = [&](id<MTLComputePipelineState> pipeline, unsigned count = pixels,
                              bool allow_failure = false) {
        const bool suffix_enabled = config.suffix_context_low || config.suffix_context_high;
        std::array<tape::Entry, ew::bank_count * pixels> frozen_suffixes{};
        std::memcpy(frozen_suffixes.data(), suffixes, lengths[aw::enhanced_suffix_id]);
        std::memcpy(data(aw::config_id), &config, sizeof(config));
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        require(command && encoder, "Area protocol encoder");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:arguments offset:0 atIndex:1];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:0];
        if (duplication_arguments) {
            [encoder setBuffer:duplication_arguments offset:0 atIndex:2];
            for (auto resource : duplication_resources)
                [encoder useResource:resource usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        }
        for (auto buffer : buffers)
            [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        require(metal::complete(command, "Enhanced Area injected protocol").empty(),
                "Area protocol command completion");
        aw::Counters counters{};
        std::memcpy(&counters, data(aw::failure_id), sizeof(counters));
        require(allow_failure || counters.failure == 0,
                "Area protocol failure phase=" + std::to_string(config.phase) +
                    " candidate=" + std::to_string(config.candidate) +
                    " source_line=" + std::to_string(counters.first_failure.source_line));
        if (config.stencil != aw::paired_gaussian16_stencil)
            for (unsigned id : {aw::paired_atlas_id, aw::paired_config_id})
                for (std::size_t j = 0; j < lengths[id]; ++j)
                    require(data(id)[j] == 0xa5,
                            "Nonpaired Area passes preserve typed atlas/config sentinels");
        if (!suffix_enabled) {
            const auto *bytes = reinterpret_cast<const unsigned char *>(suffixes);
            for (std::size_t j = 0; j < lengths[aw::enhanced_suffix_id]; ++j)
                require(bytes[j] == 0xa5, "Zero-context Area must not touch suffix arena");
        } else if (pipeline == prepare || pipeline == mutation_prepare ||
                   pipeline == cache_evaluation) {
            require(
                std::memcmp(frozen_suffixes.data(), suffixes, lengths[aw::enhanced_suffix_id]) == 0,
                "Prepare/cache lookup must preserve all source suffix bytes");
            for (unsigned pixel = 0; pixel < pixels; ++pixel)
                require(evaluations[pixel].suffix_context_low == config.suffix_context_low &&
                            evaluations[pixel].suffix_context_high == config.suffix_context_high,
                        "Every prepared/cached receipt must carry its evaluator context");
        }
        if (pipeline == merge || pipeline == mutation_finish)
            for (unsigned p = 0; p < pixels; ++p)
                require(requests[p].ready == 0 && evaluations[p].ready == 0,
                        "Every successful merge/mutation consumes both receipts");
        for (unsigned i = 0; i < buffers.size(); ++i) {
            const auto *bytes = static_cast<const unsigned char *>(buffers[i].contents);
            for (std::size_t j = 0; j < guard; ++j)
                require(bytes[j] == 0xa5 && bytes[guard + lengths[i] + j] == 0xa5,
                        "Area protocol buffer guard changed");
        }
    };
    const auto response = [&](unsigned pixel, render_wire::Point xyz, float h, unsigned slot,
                              float fresh_Z = 0) {
        require(requests[pixel].ready == 0 && evaluations[pixel].ready == 0,
                "Area prepare must reset both response receipts");
        require(evaluations[pixel].suffix_context_low == config.suffix_context_low &&
                    evaluations[pixel].suffix_context_high == config.suffix_context_high,
                "Transport replies retain the phase evaluator token");
        leaf[pixel] = xyz;
        requests[pixel].selected_slot = slot;
        const auto [Z_fraction, Z_exponent] = scale(fresh_Z);
        requests[pixel].normalizer = Z_fraction;
        evaluations[pixel].normalizer_exponent = Z_exponent;
        requests[pixel].ready = 1;
        const auto [fraction, exponent] = scale(h);
        evaluations[pixel].target_h_fraction = fraction;
        evaluations[pixel].target_h_exponent = exponent;
        evaluations[pixel].ready = 1;
    };
    const auto check_cached = [&](const auto *source) {
        using Source = std::remove_cv_t<std::remove_reference_t<decltype(*source)>>;
        std::array<Source, pixels> frozen{};
        std::memcpy(frozen.data(), source, sizeof(frozen));
        std::array<ew::Record, ew::bank_count * pixels> frozen_records{};
        std::memcpy(frozen_records.data(), records, sizeof(frozen_records));
        std::memset(leaf, 0xff, pixels * sizeof(*leaf));
        config.temporal_cache = 1;
        dispatch(cache_evaluation);
        for (unsigned p = 0; p < pixels; ++p) {
            require(same(leaf[p], source[p].xyz) && requests[p].ready == 1 &&
                        requests[p].selected_slot == source[p].selected_slot &&
                        requests[p].normalizer == 0 && evaluations[p].ready == 1 &&
                        evaluations[p].normalizer_exponent == 0 &&
                        same(evaluations[p].source, source[p].record) &&
                        same(evaluations[p].result, source[p].record) &&
                        evaluations[p].target_h_fraction == source[p].target_h_fraction &&
                        evaluations[p].target_h_exponent == source[p].target_h_exponent,
                    "Exact temporal cache must publish the complete owned response");
        }
        require(
            std::memcmp(frozen.data(), source, sizeof(frozen)) == 0 &&
                std::memcmp(frozen_records.data(), records, sizeof(frozen_records)) == 0,
            "Cache reads must preserve every source and owned record, including poisoned nulls");
        config.temporal_cache = 0;
        dispatch(prepare); // Restore the independent nonidentity replay fixture below.
    };
    if (duplication_probe) {
        namespace dw = enhanced_duplication_wire;
        namespace dup = rypath_enhanced_duplication;
        auto diagnostic_finish = load(@"rypath_area_temporal_finish_duplication_v1");
        auto diagnostic_finalize = load(@"rypath_area_finalize_duplication_v1");
        auto diagnostic_reset = load(@"rypath_enhanced_duplication_reset");
        auto diagnostic_function =
            metal::load_function(library, @"rypath_area_temporal_finish_duplication_v1", why);
        require(diagnostic_function != nil, why);
        auto de = [diagnostic_function newArgumentEncoderWithBufferIndex:2];
        require(de != nil, "Provenance argument encoder");
        duplication_arguments = [device newBufferWithLength:de.encodedLength
                                                    options:MTLResourceStorageModeShared];
        const std::array<std::size_t, 6> sizes{sizeof(dw::Config),
                                               pixels * sizeof(dup::Label),
                                               pixels * sizeof(dup::Label),
                                               pixels * sizeof(dup::Label),
                                               pixels * sizeof(std::uint16_t),
                                               sizeof(dw::Counters)};
        std::array<id<MTLBuffer>, 6> diagnostic{};
        for (unsigned id = 0; id < diagnostic.size(); ++id) {
            diagnostic[id] = [device newBufferWithLength:sizes[id] + 2 * guard
                                                 options:MTLResourceStorageModeShared];
            require(diagnostic[id] != nil, "Provenance guarded allocation");
            std::memset(diagnostic[id].contents, 0xa5, diagnostic[id].length);
            duplication_resources.push_back(diagnostic[id]);
        }
        const auto dd = [&](unsigned id) {
            return static_cast<unsigned char *>(diagnostic[id].contents) + guard;
        };
        auto *source_ids = reinterpret_cast<dup::Label *>(dd(dw::history_source_labels_id));
        auto *destination_ids =
            reinterpret_cast<dup::Label *>(dd(dw::history_destination_labels_id));
        auto *temporal_ids = reinterpret_cast<dup::Label *>(dd(dw::temporal_labels_id));
        auto *dc = reinterpret_cast<dw::Counters *>(dd(dw::counters_id));
        const auto bind_diagnostic = [&] {
            [de setArgumentBuffer:duplication_arguments offset:0];
            for (unsigned id = 0; id < diagnostic.size(); ++id)
                [de setBuffer:diagnostic[id] offset:guard atIndex:id];
            [de setBuffer:buffers[aw::history_destination_id]
                   offset:guard
                  atIndex:dw::history_destination_id];
        };
        bind_diagnostic();
        const auto configure = [&] {
            const dw::Config c{
                1, width, height, dup::initial_atom, config.serial_low, config.serial_high, 0, 0};
            std::memcpy(dd(dw::config_id), &c, sizeof(c));
        };
        const auto check_guards = [&] {
            for (unsigned id = 0; id < diagnostic.size(); ++id) {
                const auto *bytes = static_cast<const unsigned char *>(diagnostic[id].contents);
                for (unsigned j = 0; j < guard; ++j)
                    require(bytes[j] == 0xa5 && bytes[guard + sizes[id] + j] == 0xa5,
                            "Provenance buffer guards");
            }
        };
        const auto equal_label = [](dup::Label a, dup::Label b) {
            return std::memcmp(&a, &b, sizeof(a)) == 0;
        };
        parameters.seed_low = parameters.seed_high = 0;
        parameters.sample_low = parameters.sample_high = 0;
        const auto cold = [&](bool second_positive) {
            config.phase = aw::fresh;
            config.candidate = 0;
            config.history_used = 0;
            config.temporal_reprojection = 0;
            config.mutation_steps = 1;
            configure();
            dispatch(reset, 1);
            dispatch(diagnostic_reset, 291);
            dispatch(prepare);
            for (unsigned p = 0; p < pixels; ++p) {
                const bool positive = p == 0 || (second_positive && p == 1);
                const render_wire::Point xyz =
                    positive ? render_wire::Point{float(4 + 4 * p), 0, 0} : render_wire::Point{};
                const unsigned slot = positive ? (p == 0 ? 0u : 1u) : cw::enhanced_null_slot;
                response(p, xyz, 1, slot, 2 * proxy(xyz, 1));
                if (positive)
                    record_at(evaluations[p].result) = descriptor(p, 0);
            }
            dispatch(merge);
            dispatch(diagnostic_finish);
            require(dc->failure == 0 && dc->temporal_completed == pixels,
                    "Fresh temporal provenance completes every owner");
            for (unsigned p = 0; p < pixels; ++p) {
                const auto expected = (p == 0 || (second_positive && p == 1))
                                          ? dup::Label{p, parameters.sample_low,
                                                       parameters.sample_high, p == 0 ? 0u : 1u}
                                          : dup::invalid_label();
                require(equal_label(temporal_ids[p], expected),
                        "Actual fresh winner captures original pixel/sample64/initial-j including "
                        "zero words");
            }
            check_guards();
        };
        cold(true);
        std::array<dup::Label, pixels> initial_births{};
        std::memcpy(initial_births.data(), temporal_ids, sizeof(initial_births));
        const auto original_zero = temporal[0].payload;
        const auto original_reject = temporal[1];
        config.phase = aw::mutation;
        config.candidate = 0;
        dispatch(mutation_prepare);
        for (unsigned p = 0; p < pixels; ++p) {
            const auto xyz = p == 0 ? render_wire::Point{400, 0, 0} : render_wire::Point{};
            response(p, xyz, 1, temporal[p].selected_slot);
            if (temporal[p].selected_slot != cw::enhanced_null_slot)
                record_at(evaluations[p].result) = descriptor(p, 1);
        }
        dispatch(mutation_finish);
        require(!same(temporal[0].payload, original_zero) && same(temporal[1], original_reject),
                "Forced uphill accepted mutation changes U; zero-target proposal is rejected");
        require(
            std::memcmp(initial_births.data(), temporal_ids, sizeof(initial_births)) == 0,
            "Accepted, rejected and null MCMC moves retain original birth labels byte-for-byte");
        // Fresh sampleHigh1 is a distinct genealogy despite equal low/sample0.
        ++config.serial_low;
        parameters.sample_high = 1;
        cold(false);
        const dup::Label birth{0, 0, 1, 0};
        require(!equal_label(birth, initial_births[0]),
                "High sample word is part of original genealogy");
        config.phase = aw::spatial;
        config.candidate = 0;
        config.stencil = aw::reciprocal16_stencil;
        config.offset_x[0] = config.offset_x[1] = 16;
        config.offset_y[0] = config.offset_y[1] = 0;
        dispatch(spatial_begin);
        for (unsigned k = 1; k <= 4; ++k) {
            config.candidate = k;
            dispatch(prepare);
            for (unsigned p = 0; p < pixels; ++p) {
                const int x = int(p % width) + ((k & 1u) ? 16 : -16);
                const unsigned source =
                    x < 0 || x >= int(width) ? p : unsigned(int(p / width) * int(width) + x);
                const auto &t = temporal[source];
                response(p, t.xyz, 1, t.selected_slot);
            }
            dispatch(merge);
        }
        config.phase = aw::spatial_final;
        config.candidate = 0;
        dispatch(diagnostic_finalize);
        require(dc->failure == 0 && dc->final_completed == pixels,
                "Final provenance completes actual spatial owners");
        for (unsigned p = 0; p < pixels; ++p)
            require(
                equal_label(destination_ids[p], p == 0 || p == 16 ? birth : dup::invalid_label()),
                "Final actual center and duplicated-spatial winners copy temporal source birth; "
                "nulls remain missing");
        const std::array<dup::Label, pixels> published = [&] {
            std::array<dup::Label, pixels> x{};
            std::memcpy(x.data(), destination_ids, sizeof(x));
            return x;
        }();
        // Freeze the actual publication as history, aligning diagnostic banks
        // with reservoir banks rather than the independent geometry front.
        std::swap(buffers[aw::history_source_id], buffers[aw::history_destination_id]);
        bind();
        std::swap(diagnostic[dw::history_source_labels_id],
                  diagnostic[dw::history_destination_labels_id]);
        source_ids = reinterpret_cast<dup::Label *>(dd(dw::history_source_labels_id));
        destination_ids = reinterpret_cast<dup::Label *>(dd(dw::history_destination_labels_id));
        bind_diagnostic();
        auto *old = reinterpret_cast<aw::History *>(data(aw::history_source_id));
        auto *donors = reinterpret_cast<enhanced_motion_wire::Donor *>(data(aw::donors_id));
        config.history_source_bank = 1;
        config.history_used = 1;
        config.temporal_reprojection = 1;
        config.phase = aw::fresh;
        config.candidate = 0;
        ++config.serial_low;
        parameters.sample_high = 2;
        configure();
        for (unsigned p = 0; p < pixels; ++p)
            donors[p] = {p == 5 || p == 6 ? 0u : (p + 1) % pixels,
                         unsigned(rypath_enhanced_motion::Classification::ordinary),
                         config.serial_low, config.serial_high};
        dispatch(reset, 1);
        dispatch(diagnostic_reset, 291);
        dispatch(prepare);
        for (unsigned p = 0; p < pixels; ++p) {
            const auto xyz = p == 2 ? render_wire::Point{12, 0, 0} : render_wire::Point{};
            response(p, xyz, 1, p == 2 ? 1u : cw::enhanced_null_slot, 2 * proxy(xyz, 1));
            if (p == 2)
                record_at(evaluations[p].result) = descriptor(p, 0);
        }
        dispatch(merge);
        config.phase = aw::temporal_reverse;
        dispatch(prepare);
        for (unsigned p = 0; p < pixels; ++p)
            response(p, {}, 1, temporal[p].selected_slot);
        dispatch(merge);
        config.phase = aw::temporal;
        config.candidate = 1;
        dispatch(prepare);
        for (unsigned p = 0; p < pixels; ++p) {
            const unsigned source = donors[p].source_pixel;
            response(p, p == 5 || p == 6 ? old[source].xyz : render_wire::Point{}, 1,
                     old[source].selected_slot);
        }
        dispatch(merge);
        dispatch(diagnostic_finish);
        require(dc->failure == 0 && dc->temporal_completed == pixels,
                "Mapped temporal provenance validates frozen history domains");
        for (unsigned p = 0; p < pixels; ++p) {
            const auto expected = p == 2             ? dup::Label{2, 0, 2, 1}
                                  : p == 5 || p == 6 ? birth
                                                     : dup::invalid_label();
            require(equal_label(temporal_ids[p], expected),
                    "Actual mapped history donor and fresh winner retain independent known birth "
                    "labels");
        }
        require(std::memcmp(source_ids, published.data(), sizeof(published)) == 0,
                "Mapped many-to-one reuse never mutates source ancestry");
        // An unknown positive source is a protocol failure, not a null family.
        source_ids[0] = dup::invalid_label();
        dispatch(diagnostic_reset, 291);
        dispatch(diagnostic_finish, pixels, true);
        require((dc->failure & dw::invalid_provenance) != 0,
                "Unknown positive historical parent fails visibly");
        source_ids[0] = birth;
        // A positive winner from temporalbank is not a legal temporal parent,
        // even though that record itself is structurally valid physical data.
        configure();
        dispatch(diagnostic_reset, 291);
        accumulator[5].record = {ew::temporal_bank, 0};
        record_at(accumulator[5].record) = descriptor(0, 0);
        dispatch(diagnostic_finish, pixels, true);
        require((dc->failure & dw::invalid_provenance) != 0,
                "Wrong positive source bank cannot be silently guessed as ancestry");
        source_ids[1] = {1, 0, 99, 0};
        accumulator[5].record = {config.history_source_bank, 1};
        dispatch(diagnostic_reset, 291);
        dispatch(diagnostic_finish, pixels, true);
        require((dc->failure & dw::invalid_provenance) != 0,
                "A valid but wrong-source-pixel label cannot replace the frozen mapped parent");
        source_ids[1] = published[1];
        accumulator[5].record = {config.history_source_bank, 0};
        [de setBuffer:diagnostic[dw::temporal_labels_id]
               offset:guard
              atIndex:dw::history_source_labels_id];
        dispatch(diagnostic_reset, 291);
        dispatch(diagnostic_finish, pixels, true);
        require((dc->failure & dw::invalid_config) != 0,
                "Aliased source/temporal provenance buffers fail configuration");
        bind_diagnostic();
        check_guards();
        return;
    }
    // Cold all-null generation: every source domain still contributes M1.
    config.phase = aw::fresh;
    dispatch(reset, 1);
    dispatch(prepare);
    for (unsigned pixel = 0; pixel < pixels; ++pixel)
        response(pixel, {}, 1, cw::enhanced_null_slot);
    dispatch(merge);
    dispatch(finish);
    for (unsigned pixel = 0; pixel < pixels; ++pixel) {
        require(temporal[pixel].confidence == 1 && temporal[pixel].normalizer == 0 &&
                    temporal[pixel].normalizer_exponent == 0 && temporal[pixel].target == 0 &&
                    temporal[pixel].target_exponent == 0 &&
                    temporal[pixel].record.bank == ew::temporal_bank &&
                    temporal[pixel].record.pixel == pixel &&
                    same(record_at(temporal[pixel].record), ew::empty_record()),
                "Cold null temporal must retain M1 and normalize its copied record");
    }
    const auto neighbor = [&](unsigned pixel, unsigned direction, bool duplicate) {
        const int x = static_cast<int>(pixel % width), y = static_cast<int>(pixel / width);
        const int dx = duplicate        ? ((direction & 1u) ? 16 : -16)
                       : direction == 1 ? -1
                       : direction == 2 ? 1
                                        : 0;
        const int dy = duplicate ? 0 : direction == 3 ? -1 : direction == 4 ? 1 : 0;
        const int nx = x + dx, ny = y + dy;
        return nx < 0 || nx >= int(width) || ny < 0 || ny >= int(height)
                   ? pixel
                   : static_cast<unsigned>(ny) * width + static_cast<unsigned>(nx);
    };
    const auto spatial = [&](bool duplicate) {
        config.stencil = duplicate ? aw::reciprocal16_stencil : aw::adjacent_stencil;
        config.offset_x[0] = config.offset_x[1] = duplicate ? 16 : 0;
        config.offset_y[0] = config.offset_y[1] = 0;
        config.phase = aw::spatial;
        config.candidate = 0;
        std::array<aw::Temporal, pixels> frozen{};
        std::array<ew::Record, pixels> frozen_records{};
        std::array<tape::Entry, pixels> frozen_suffixes{};
        std::memcpy(frozen.data(), temporal, sizeof(frozen));
        for (unsigned p = 0; p < pixels; ++p) {
            frozen_records[p] = record_at(frozen[p].record);
            if (config.suffix_context_low || config.suffix_context_high)
                frozen_suffixes[p] = suffix_at(frozen[p].record);
        }
        dispatch(spatial_begin);
        for (unsigned direction = 1; direction <= 4; ++direction) {
            config.candidate = direction;
            dispatch(prepare);
            for (unsigned p = 0; p < pixels; ++p) {
                const auto s = neighbor(p, direction, duplicate);
                const auto &source = frozen[s];
                require(evaluations[p].source.bank == ew::temporal_bank &&
                            evaluations[p].source.pixel == s,
                        "Spatial prepare must use the declared source slot, including duplicates");
                const float h = std::ldexp(source.target_h_fraction, source.target_h_exponent);
                response(p, source.xyz, h, source.selected_slot);
            }
            dispatch(merge);
        }
        config.phase = aw::spatial_final;
        config.candidate = 0;
        dispatch(finalize);
        const auto *history =
            reinterpret_cast<const aw::History *>(data(aw::history_destination_id));
        for (unsigned p = 0; p < pixels; ++p) {
            // Each valid pair splits by confidence; boundary absence gives the
            // center its full quarter-share. No deduplication of valid slots.
            const auto &center = frozen[p];
            long double expected_Z = 0;
            std::array<long double, 3> expected_xyz{};
            unsigned expected_M = center.confidence;
            const auto add = [&](const aw::Temporal &source, long double score) {
                expected_Z += score;
                const auto target = number(source.target, source.target_exponent);
                if (target > 0) {
                    expected_xyz[0] += score * source.xyz.x / target;
                    expected_xyz[1] += score * source.xyz.y / target;
                    expected_xyz[2] += score * source.xyz.z / target;
                }
            };
            for (unsigned direction = 1; direction <= 4; ++direction) {
                const auto s = neighbor(p, direction, duplicate);
                if (s == p)
                    add(center, number(center.normalizer, center.normalizer_exponent) / 4);
                else {
                    const auto &other = frozen[s];
                    expected_M += other.confidence;
                    const long double denominator = center.confidence + other.confidence;
                    add(center, number(center.normalizer, center.normalizer_exponent) *
                                    center.confidence / denominator / 4);
                    add(other, number(other.normalizer, other.normalizer_exponent) *
                                   other.confidence / denominator / 4);
                }
            }
            require(history[p].confidence == expected_M && history[p].valid == 1 &&
                        history[p].record.bank == 1u - config.history_source_bank &&
                        history[p].record.pixel == p,
                    "Spatial M must count structural slots and normalize destination bank");
            scalar_near(history[p].normalizer, history[p].normalizer_exponent, expected_Z,
                        "Spatial independent scalar MIS sum");
            near(raw[p].x, expected_xyz[0], "Spatial independent signed X readout");
            near(raw[p].y, expected_xyz[1], "Spatial independent signed Y readout");
            near(raw[p].z, expected_xyz[2], "Spatial independent signed Z readout");
            if (history[p].selected_slot == cw::enhanced_null_slot) {
                require(expected_Z == 0 && same(record_at(history[p].record), ew::empty_record()),
                        "All-null spatial publication must synthesize an empty record");
                if (config.suffix_context_low || config.suffix_context_high)
                    require(same(suffix_at(history[p].record), tape::Entry{}),
                            "Null publication must zero all 160 cache bytes");
            } else {
                bool matched = false;
                for (unsigned s = 0; s < pixels; ++s) {
                    bool admitted = s == p;
                    for (unsigned direction = 1; direction <= 4; ++direction)
                        admitted |= neighbor(p, direction, duplicate) == s;
                    if (admitted && same(history[p].payload, frozen[s].payload)) {
                        matched |= history[p].selected_slot == frozen[s].selected_slot &&
                                   same(history[p].xyz, frozen[s].xyz) &&
                                   history[p].target == frozen[s].target &&
                                   history[p].target_exponent == frozen[s].target_exponent &&
                                   history[p].target_h_fraction == frozen[s].target_h_fraction &&
                                   history[p].target_h_exponent == frozen[s].target_h_exponent &&
                                   same(record_at(history[p].record), frozen_records[s]) &&
                                   (!(config.suffix_context_low || config.suffix_context_high) ||
                                    same(suffix_at(history[p].record), frozen_suffixes[s]));
                    }
                }
                require(matched,
                        "Published payload/XYZ/target/h/record must be one admitted tuple");
            }
        }
        require(std::memcmp(temporal, frozen.data(), sizeof(frozen)) == 0,
                "Spatial reuse must never mutate the frozen temporal bank");
        for (unsigned p = 0; p < pixels; ++p) {
            require(same(record_at(frozen[p].record), frozen_records[p]),
                    "Spatial reuse must never mutate an unselected temporal record");
            if (config.suffix_context_low || config.suffix_context_high)
                require(same(suffix_at(frozen[p].record), frozen_suffixes[p]),
                        "Spatial reuse must preserve immutable temporal suffixes");
        }
    };
    spatial(false);
    // Feed a deliberately high structural input to exercise cap-before-MIS
    // without a long history warmup. Its null draw does not remove that domain.
    std::swap(buffers[aw::history_source_id], buffers[aw::history_destination_id]);
    bind();
    auto *old = reinterpret_cast<aw::History *>(data(aw::history_source_id));
    for (unsigned p = 0; p < pixels; ++p) {
        require(old[p].confidence == 1u + unsigned(p % width > 0) +
                                         unsigned(p % width + 1 < width) + unsigned(p / width > 0) +
                                         unsigned(p / width + 1 < height),
                "Cold adjacent confidence counts in-bounds slots");
        old[p].confidence = 40 + p;
        std::memset(&record_at(old[p].record), 0xff, sizeof(ew::Record));
    }
    for (unsigned cap : {1u, 4u, 20u}) {
        // A fixed cap applies to the inverse and forward weights, the published
        // temporal confidence, and subsequent spatial inputs even for a null donor.
        config.enhanced_history_cap = cap;
        config.history_source_bank = 1;
        config.history_used = 1;
        config.serial_low = 1;
        config.mutation_steps = 1;
        config.stencil = aw::adjacent_stencil;
        config.phase = aw::fresh;
        config.candidate = 0;
        parameters.sample_low = 1;
        dispatch(reset, 1);
        dispatch(prepare);
        std::array<render_wire::Point, pixels> fresh_xyz{};
        std::array<float, pixels> fresh_h{}, fresh_alpha{}, fresh_Z{};
        for (unsigned p = 0; p < pixels; ++p) {
            fresh_h[p] = p == 0 || (p & 1u) ? 1.f : (p == 4 ? 4.f : 2.f);
            fresh_xyz[p] = p & 1u ? render_wire::Point{}
                                  : render_wire::Point{float(4 + 2 * p), float(-2 - int(p)), 0};
            fresh_alpha[p] = proxy(fresh_xyz[p], fresh_h[p]);
            fresh_Z[p] = 2 * fresh_alpha[p];
            response(p, fresh_xyz[p], fresh_h[p], p & 1u ? cw::enhanced_null_slot : 1u, fresh_Z[p]);
            if (!(p & 1u))
                record_at(evaluations[p].result) = descriptor(p, 0);
        }
        dispatch(merge);
        config.phase = aw::temporal_reverse;
        dispatch(prepare);
        check_cached(temporal);
        std::array<long double, pixels> temporal_Z{};
        for (unsigned p = 0; p < pixels; ++p) {
            const render_wire::Point inverse{fresh_xyz[p].x * .5f, fresh_xyz[p].y * .5f, 0};
            const float h = p == 0 || (p & 1u) ? 1.f : 2 * fresh_h[p];
            const float inverse_alpha = proxy(inverse, h);
            response(p, inverse, h, p & 1u ? cw::enhanced_null_slot : 1u);
            temporal_Z[p] =
                fresh_alpha[p] == 0
                    ? 0
                    : static_cast<long double>(fresh_Z[p]) * fresh_alpha[p] /
                          (fresh_alpha[p] + static_cast<long double>(cap) * inverse_alpha);
        }
        dispatch(merge);
        for (unsigned p = 0; p < pixels; ++p)
            scalar_near(accumulator[p].state.score_sum, accumulator[p].score_exponent,
                        temporal_Z[p], "Temporal cap-before-MIS coefficient");
        config.phase = aw::temporal;
        config.candidate = 1;
        dispatch(prepare);
        check_cached(old);
        for (unsigned p = 0; p < pixels; ++p)
            response(p, {}, 1, cw::enhanced_null_slot);
        dispatch(merge);
        dispatch(finish);
        for (unsigned p = 0; p < pixels; ++p) {
            require(temporal[p].confidence == cap + 1u &&
                        temporal[p].record.bank == ew::temporal_bank &&
                        temporal[p].record.pixel == p,
                    "Warm temporal retains capped-null history confidence and current target");
            scalar_near(temporal[p].target, temporal[p].target_exponent, fresh_alpha[p],
                        "Warm temporal selected canonical target");
            scalar_near(temporal[p].normalizer, temporal[p].normalizer_exponent, temporal_Z[p],
                        "Warm temporal independent normalizer");
            if (!(p & 1u))
                require(temporal[p].target_h_fraction == scale(fresh_h[p]).first &&
                            temporal[p].target_h_exponent == scale(fresh_h[p]).second &&
                            same(record_at(temporal[p].record), descriptor(p, 0)),
                        "Fresh record/h must materialize in temporal without unit-density "
                        "substitution");
            else
                require(same(record_at(temporal[p].record), ew::empty_record()),
                        "Null temporal winner must not read poisoned source records");
        }
        std::array<aw::Temporal, pixels> before_mutation{};
        std::array<ew::Record, pixels> before_records{};
        std::memcpy(before_mutation.data(), temporal, sizeof(before_mutation));
        for (unsigned p = 0; p < pixels; ++p)
            before_records[p] = record_at(temporal[p].record);
        // Mutation reuses fresh storage. Its old bytes cannot remain a dependency
        // of the already materialized temporal source, including rejected proposals.
        std::memset(records + ew::fresh_bank * pixels, 0xff, pixels * sizeof(ew::Record));
        config.phase = aw::mutation;
        config.candidate = 0;
        dispatch(mutation_prepare);
        std::array<rypath_replay::ReplayState, pixels> proposed_payloads{};
        std::memcpy(proposed_payloads.data(), data(aw::replay_id), sizeof(proposed_payloads));
        for (unsigned p = 0; p < pixels; ++p) {
            const bool accept = p == 0 || p == 4;
            const auto &source = before_mutation[p];
            const render_wire::Point xyz =
                accept ? render_wire::Point{source.xyz.x * 2, source.xyz.y * 2, 0}
                       : render_wire::Point{};
            const float h = accept ? fresh_h[p] * (p == 0 ? 1.f : 2.f) : 1.f;
            response(p, xyz, h, source.selected_slot);
            if (accept)
                record_at(evaluations[p].result) = descriptor(p, 1);
        }
        dispatch(mutation_finish);
        aw::Counters counters{};
        std::memcpy(&counters, data(aw::failure_id), sizeof(counters));
        require(counters.mutations == pixels && counters.accepted == 2,
                "One MH attempt per domain, two guaranteed accepts, zero/null rejections");
        for (unsigned p = 0; p < pixels; ++p) {
            const bool accept = p == 0 || p == 4;
            require(temporal[p].confidence == cap + 1u &&
                        temporal[p].record.bank == ew::temporal_bank &&
                        temporal[p].record.pixel == p,
                    "MH must preserve M and normalize accepted refs");
            if (accept) {
                const auto alpha = proxy(leaf[p], p == 0 ? fresh_h[p] : fresh_h[p] * 2);
                const auto &source = before_mutation[p];
                const auto source_alpha = number(source.target, source.target_exponent);
                const auto source_Z = number(source.normalizer, source.normalizer_exponent);
                require(alpha > source_alpha, "Accepted MH fixture must be strictly uphill");
                // Z is alpha*UCW. Uphill forward acceptance is 1, while the
                // represented reverse probability is the rounded 24-bit mantissa
                // ratio times the EXACT power-of-two exponent difference. Derive
                // that probability independently, without production MIS helpers.
                int old_exponent, new_exponent;
                const long double old_fraction = std::frexp(source_alpha, &old_exponent);
                const long double new_fraction = std::frexp(alpha, &new_exponent);
                const float rounded_ratio = static_cast<float>(old_fraction / new_fraction);
                const long double reverse_probability = std::ldexp(
                    static_cast<long double>(rounded_ratio), old_exponent - new_exponent);
                require(reverse_probability > 0 && reverse_probability < 1,
                        "Represented reverse MH probability must be nontrivial");
                const auto expected_Z = source_Z * reverse_probability * alpha / source_alpha;
                // Production materializes its correction with intermediate FLOAT
                // rounding; the existing 64-epsilon bound includes those 3 operations.
                scalar_near(temporal[p].normalizer, temporal[p].normalizer_exponent, expected_Z,
                            "Accepted MH represented-dyadic normalizer");
                const auto wrong_Z = source_Z * alpha / source_alpha;
                require(std::abs(wrong_Z - expected_Z) > 1024.L *
                                                             std::numeric_limits<float>::epsilon() *
                                                             std::max(1.L, std::abs(expected_Z)),
                        "Negative control must reject treating stored Z as UCW");
                const auto h = scale(p == 0 ? fresh_h[p] : fresh_h[p] * 2);
                require(same(temporal[p].payload, proposed_payloads[p]) &&
                            same(temporal[p].xyz, leaf[p]) &&
                            temporal[p].target_h_fraction == h.first &&
                            temporal[p].target_h_exponent == h.second &&
                            same(record_at(temporal[p].record), descriptor(p, 1)),
                        "Accepted mutation must publish its actual candidate tuple");
                scalar_near(temporal[p].target, temporal[p].target_exponent, alpha,
                            "Accepted mutation scaled canonical target");
            } else
                require(same(temporal[p], before_mutation[p]) &&
                            same(record_at(temporal[p].record), before_records[p]),
                        "Rejected/zero/null mutation must retain source bytes and record");
        }
        spatial(false);
        spatial(true); // Two identical valid reciprocal offsets are two structural slots.
    }

    // Reproduce the native Pavilion operands in the ACTUAL production spatial
    // kernels. Only the center draw is positive; every surrounding null draw
    // remains a structurally present M21 proposal. Its inverse evaluation is
    // positive, so center attenuation cannot be removed from the denominator.
    // The second run shifts Z by another 128 exponent bits: a final FLOAT
    // readout rounds to zero, but the reusable selected history must stay positive.
    config.enhanced_history_cap = 20;
    constexpr unsigned owner = width + 1;
    const float evidence_Z = std::bit_cast<float>(0x03d76154u);
    const float evidence_alpha = std::bit_cast<float>(0x33f892fcu);
    const float evidence_inverse = std::bit_cast<float>(0x3b258c2cu);
    constexpr float owner_h = .0625f, inverse_h = 0x1p-64f;
    const render_wire::Point owner_xyz{4 * (evidence_alpha - 1e-6f * owner_h), 0, 0};
    const render_wire::Point inverse_xyz{4 * evidence_inverse, 0, 0};
    require(std::bit_cast<unsigned>(proxy(owner_xyz, owner_h)) == 0x33f892fcu &&
                std::bit_cast<unsigned>(proxy(inverse_xyz, inverse_h)) == 0x3b258c2cu,
            "Pavilion fixture XYZ/h must reproduce the retained canonical target operands");
    for (int Z_shift : {0, -128}) {
        config.stencil = aw::adjacent_stencil;
        config.offset_x[0] = config.offset_x[1] = 0;
        config.offset_y[0] = config.offset_y[1] = 0;
        config.phase = aw::spatial;
        config.candidate = 0;
        for (unsigned p = 0; p < pixels; ++p) {
            temporal[p] = {};
            temporal[p].payload = rypath_replay::fresh({p, 17, 19, 23, 29});
            temporal[p].selected_slot = cw::enhanced_null_slot;
            temporal[p].confidence = 21;
            temporal[p].record = {ew::temporal_bank, p};
            temporal[p].target_h_fraction = .5f;
            temporal[p].target_h_exponent = 1;
            // Null atoms must not read the poisoned record bytes.
            std::memset(&record_at(temporal[p].record), 0xff, sizeof(ew::Record));
        }
        const auto [Z_fraction, Z_exponent] = scale(evidence_Z);
        const auto [alpha_fraction, alpha_exponent] = scale(evidence_alpha);
        const auto [h_fraction, h_exponent] = scale(owner_h);
        auto &center = temporal[owner];
        center.normalizer = Z_fraction;
        center.normalizer_exponent = Z_exponent + Z_shift;
        center.target = alpha_fraction;
        center.target_exponent = alpha_exponent;
        center.xyz = owner_xyz;
        center.selected_slot = 1;
        center.target_h_fraction = h_fraction;
        center.target_h_exponent = h_exponent;
        record_at(center.record) = descriptor(owner, 2);
        const auto frozen_center = center;
        dispatch(spatial_begin);
        for (unsigned direction = 1; direction <= 4; ++direction) {
            config.candidate = direction;
            dispatch(prepare);
            for (unsigned p = 0; p < pixels; ++p) {
                const bool center_import = neighbor(p, direction, false) == owner;
                response(p, center_import ? inverse_xyz : render_wire::Point{},
                         center_import ? inverse_h : 1.f,
                         center_import ? 1u : cw::enhanced_null_slot);
            }
            dispatch(merge);
        }
        require(accumulator[owner].logical_count == 4 && accumulator[owner].confidence == 84 &&
                    std::bit_cast<unsigned>(accumulator[owner].state.score_sum) == 0 &&
                    accumulator[owner].score_exponent == 0,
                "Tiny-Z four null neighbors must retain four structural M21 domains");
        config.phase = aw::spatial_final;
        config.candidate = 0;
        dispatch(finalize);
        const auto *history =
            reinterpret_cast<const aw::History *>(data(aw::history_destination_id));
        const auto &selected = history[owner];
        const long double expected_Z =
            std::ldexp(static_cast<long double>(evidence_Z), Z_shift) * evidence_alpha /
            (static_cast<long double>(evidence_alpha) + evidence_inverse);
        require(expected_Z > 0 && expected_Z < std::numeric_limits<float>::min() &&
                    selected.confidence == 105 && selected.selected_slot == 1 &&
                    selected.normalizer >= .5f && selected.normalizer < 1 &&
                    selected.normalizer_exponent < -125 && selected.target == alpha_fraction &&
                    selected.target_exponent == alpha_exponent && same(selected.xyz, owner_xyz) &&
                    same(selected.payload, frozen_center.payload) &&
                    same(record_at(selected.record), descriptor(owner, 2)),
                "Tiny positive history must survive four reciprocal denominators with M105");
        scalar_near(selected.normalizer, selected.normalizer_exponent, expected_Z,
                    "Pavilion independent subnormal-range spatial normalizer");
        require(raw[owner].y == 0 && raw[owner].z == 0 && std::isfinite(raw[owner].x),
                "Tiny-Z frame must complete with finite signed vector readout");
        if (Z_shift == 0) {
            // Gradual-underflow final readout permits an absolute half-unit
            // rounding term; Z itself above is still checked purely relatively.
            const long double expected_x = expected_Z * owner_xyz.x / evidence_alpha;
            const auto tolerance = 64.L * std::numeric_limits<float>::epsilon() * expected_x +
                                   std::numeric_limits<float>::denorm_min();
            require(raw[owner].x > 0 &&
                        std::abs(static_cast<long double>(raw[owner].x) - expected_x) <= tolerance,
                    "Final subnormal readout must agree with independent scalar UCW");
        } else
            require(std::bit_cast<unsigned>(raw[owner].x) == 0,
                    "Below final FLOAT range rounds to zero without nulling selected history");
        require(same(temporal[owner], frozen_center),
                "Tiny-Z spatial reuse must preserve immutable temporal input");
    }
    // Actual Area publication/copy protocol, independent of cache readout math.
    // Positive tuples own a structurally valid N2 tape with deliberately distinct
    // unused operation bytes. Later phases use a different evaluator token: raw
    // copies must preserve the captured token rather than restamp it.
    lengths[aw::enhanced_suffix_id] = ew::bank_count * pixels * sizeof(tape::Entry);
    buffers[aw::enhanced_suffix_id] =
        [device newBufferWithLength:lengths[aw::enhanced_suffix_id] + 2 * guard
                            options:MTLResourceStorageModeShared];
    require(buffers[aw::enhanced_suffix_id] != nil, "Enabled suffix arena allocation");
    std::memset(buffers[aw::enhanced_suffix_id].contents, 0xa5,
                buffers[aw::enhanced_suffix_id].length);
    suffixes = reinterpret_cast<tape::Entry *>(data(aw::enhanced_suffix_id));
    bind();
    config = {};
    config.pixel_count = pixels;
    config.width = width;
    config.height = height;
    config.target_scale = 1;
    config.contribution_mode = cw::enhanced;
    config.contribution_depth = 3;
    config.output_mode = aw::conditional_mean_output;
    config.history_used = 1;
    config.mutation_steps = 1;
    config.seed_low = 29;
    config.suffix_context_low = 0x12345678u;
    config.suffix_context_high = 0x9abcdef0u;
    parameters.max_depth = 3;
    auto *cache_history = reinterpret_cast<aw::History *>(data(aw::history_source_id));
    for (unsigned p = 0; p < pixels; ++p) {
        cache_history[p] = {};
        cache_history[p].payload = rypath_replay::fresh({p, 17, 19, 23, 29});
        cache_history[p].valid = 1;
        cache_history[p].selected_slot = cw::enhanced_null_slot;
        cache_history[p].confidence = 1;
        cache_history[p].record = {ew::history_bank_0, p};
        cache_history[p].target_h_fraction = .5f;
        cache_history[p].target_h_exponent = 1;
        std::memset(&record_at(cache_history[p].record), 0xff, sizeof(ew::Record));
    }
    config.phase = aw::fresh;
    dispatch(reset, 1);
    dispatch(prepare);
    std::array<tape::Entry, pixels> captured{};
    for (unsigned p = 0; p < pixels; ++p) {
        require(ew::absent(evaluations[p].source) &&
                    same(evaluations[p].result, ew::Reference{ew::fresh_bank, p}),
                "Fresh capture must have absent source and exclusive bank2 result");
        const render_wire::Point xyz = p & 1u ? render_wire::Point{} : render_wire::Point{4, -2, 1};
        response(p, xyz, 1, p & 1u ? cw::enhanced_null_slot : 5u, p & 1u ? 0.f : 2 * proxy(xyz, 1));
        if (p & 1u) {
            std::memset(&suffix_at(evaluations[p].result), 0xff, sizeof(tape::Entry));
            continue;
        }
        record_at(evaluations[p].result) = {ew::record_version,
                                            5,
                                            0,
                                            1,
                                            p,
                                            p + 3,
                                            render_wire::triangle_primitive,
                                            0,
                                            {std::bit_cast<unsigned>(.25f),
                                             std::bit_cast<unsigned>(.5f),
                                             std::bit_cast<unsigned>(1.f), 0}};
        auto &entry = captured[p];
        for (unsigned op = 0; op < 2; ++op)
            for (unsigned k = 0; k < 4; ++k) {
                entry.operations[op].numerator[k] = float(1 + p + op * 4 + k);
                entry.operations[op].sampled_likelihood[k] = .5f;
            }
        for (unsigned k = 0; k < 4; ++k) {
            entry.factor_a[k] = 1;
            entry.factor_b[k] = 2;
            entry.bsdf_pdf[k] = .5f;
        }
        entry.cosine = entry.light_pdf = .5f;
        entry.version = tape::version;
        entry.flags = tape::required_capture_flags | tape::nee;
        entry.coverage = tape::known_coverage;
        entry.selected_slot = 5;
        entry.pivot_event = 1;
        entry.context_low = config.suffix_context_low;
        entry.context_high = config.suffix_context_high;
        tape::Input input{};
        input.selected_slot = 5;
        input.pivot_event = 1;
        input.context_low = entry.context_low;
        input.context_high = entry.context_high;
        require(tape::header(entry, input) == tape::Status::ok,
                "Cache-copy fixture must have a valid version1 header");
        suffix_at(evaluations[p].result) = entry;
    }
    dispatch(merge);
    // Reverse reads the fresh bank itself. It must never acquire capture ownership.
    config.phase = aw::temporal_reverse;
    config.suffix_context_low ^= 1u;
    dispatch(prepare);
    for (unsigned p = 0; p < pixels; ++p)
        require(same(evaluations[p].source, ew::Reference{ew::fresh_bank, p}) &&
                    same(evaluations[p].result, evaluations[p].source),
                "Temporal reverse must retain immutable source==result bank2 identity");
    check_cached(temporal);
    for (unsigned p = 0; p < pixels; ++p)
        response(p, temporal[p].xyz, 1, temporal[p].selected_slot);
    dispatch(merge);
    config.phase = aw::temporal;
    config.candidate = 1;
    dispatch(prepare);
    check_cached(cache_history);
    for (unsigned p = 0; p < pixels; ++p)
        response(p, {}, 1, cw::enhanced_null_slot);
    dispatch(merge);
    dispatch(finish);
    for (unsigned p = 0; p < pixels; ++p)
        require(same(suffix_at(temporal[p].record), p & 1u ? tape::Entry{} : captured[p]),
                "Temporal tuple copy must preserve all 160 bytes or zero a null cache");
    config.phase = aw::mutation;
    config.candidate = 0;
    dispatch(mutation_prepare);
    for (unsigned p = 0; p < pixels; ++p) {
        require(same(evaluations[p].source, ew::Reference{ew::temporal_bank, p}) &&
                    same(evaluations[p].result, ew::Reference{ew::fresh_bank, p}),
                "Mutation capture result must be distinct from its immutable source");
        response(p, {}, 1, temporal[p].selected_slot);
    }
    dispatch(mutation_finish);
    spatial(false);
    spatial(true);
    // The published history is a new source; its captured context must still be A.
    std::swap(buffers[aw::history_source_id], buffers[aw::history_destination_id]);
    bind();
    config.history_source_bank = 1;
    config.phase = aw::temporal;
    config.candidate = 1;
    dispatch(prepare);
    check_cached(reinterpret_cast<const aw::History *>(data(aw::history_source_id)));

    // Actual Area temporal passes consume one frozen donor per OWNER. Distinct
    // old targets/normalizers/confidences make owner-indexed source fetches a
    // decisive failure, including the reverse MIS coefficient.
    auto *mapped_history = reinterpret_cast<aw::History *>(data(aw::history_source_id));
    auto *donors = reinterpret_cast<enhanced_motion_wire::Donor *>(data(aw::donors_id));
    auto *replay = reinterpret_cast<rypath_replay::ReplayState *>(data(aw::replay_id));
    config.suffix_context_low = config.suffix_context_high = 0;
    std::memset(suffixes, 0xa5, lengths[aw::enhanced_suffix_id]);
    config.temporal_reprojection = 1;
    config.temporal_cache = 0;
    config.history_used = 1;
    config.mutation_steps = 0;
    ++config.serial_low;
    for (unsigned cap : {1u, 4u, 20u}) {
        config.enhanced_history_cap = cap;
        for (const bool many_to_one : {false, true})
            for (const bool positive_forward : {false, true}) {
                for (unsigned source = 0; source < pixels; ++source) {
                    auto &h = mapped_history[source];
                    h = {};
                    h.payload = rypath_replay::fresh({source, 111, 113, 127, 131});
                    h.valid = 1;
                    h.xyz = {float(4 + source), 0, 0};
                    h.selected_slot = 1;
                    h.confidence = 2 + source;
                    h.record = {config.history_source_bank, source};
                    h.target_h_fraction = .5f;
                    h.target_h_exponent = 1;
                    const auto [alpha, exponent] = scale(proxy(h.xyz, 1));
                    h.target = alpha;
                    h.target_exponent = exponent;
                    h.normalizer = alpha;
                    h.normalizer_exponent = exponent + 1 + int(source % 3);
                    record_at(h.record) = descriptor(source, 0);
                }
                std::array<aw::History, pixels> frozen_history{};
                std::memcpy(frozen_history.data(), mapped_history, sizeof(frozen_history));
                for (unsigned owner = 0; owner < pixels; ++owner)
                    donors[owner] = {many_to_one ? owner / 2 : (owner + 1) % pixels,
                                     unsigned(rypath_enhanced_motion::Classification::ordinary),
                                     config.serial_low, config.serial_high};
                config.phase = aw::fresh;
                config.candidate = 0;
                dispatch(reset, 1);
                dispatch(prepare);
                std::array<float, pixels> alpha{}, normalizer{};
                for (unsigned owner = 0; owner < pixels; ++owner) {
                    const render_wire::Point xyz{float(12 + owner), 0, 0};
                    alpha[owner] = proxy(xyz, 1);
                    normalizer[owner] = 2 * alpha[owner];
                    response(owner, xyz, 1, 1, normalizer[owner]);
                    record_at(evaluations[owner].result) = descriptor(owner, 1);
                }
                dispatch(merge);
                config.phase = aw::temporal_reverse;
                dispatch(prepare);
                std::array<long double, pixels> expected_Z{};
                for (unsigned owner = 0; owner < pixels; ++owner) {
                    const auto source = donors[owner].source_pixel;
                    require(
                        same(replay[owner], temporal[owner].payload) &&
                            same(evaluations[owner].source, ew::Reference{ew::fresh_bank, owner}),
                        "Mapped reverse retains fresh owner payload and receipt identity");
                    const render_wire::Point inverse{float(8 + 3 * source), 0, 0};
                    const auto inverse_alpha = proxy(inverse, 1);
                    const auto M = std::min(cap, mapped_history[source].confidence);
                    expected_Z[owner] =
                        static_cast<long double>(normalizer[owner]) * alpha[owner] /
                        (alpha[owner] + static_cast<long double>(M) * inverse_alpha);
                    response(owner, inverse, 1, 1);
                }
                dispatch(merge);
                bool negative_control = false;
                for (unsigned owner = 0; owner < pixels; ++owner) {
                    scalar_near(accumulator[owner].state.score_sum,
                                accumulator[owner].score_exponent, expected_Z[owner],
                                "Mapped reverse uses donor source confidence");
                    if (donors[owner].source_pixel != owner)
                        negative_control |=
                            std::min(cap, mapped_history[owner].confidence) !=
                            std::min(cap, mapped_history[donors[owner].source_pixel].confidence);
                }
                require(cap == 1 ? !negative_control : negative_control,
                        "Cap1 coefficients are identical; cap4/20 distinguish wrong owner M");
                config.phase = aw::temporal;
                config.candidate = 1;
                dispatch(prepare);
                bool wrong_owner_separated = false, wrong_denominator_separated = false;
                bool wrong_Z_separated = false, wrong_target_separated = false;
                for (unsigned owner = 0; owner < pixels; ++owner) {
                    const auto source = donors[owner].source_pixel;
                    require(
                        same(replay[owner], mapped_history[source].payload) &&
                            same(evaluations[owner].source, mapped_history[source].record) &&
                            requests[owner].requested_slot == mapped_history[source].selected_slot,
                        "Mapped forward uses donor payload, selected atom and owned record at s");
                    // Retain the zero reevaluation control and add a positive one.
                    // Expected arithmetic decodes the wire Number directly in long
                    // double; no production score or scaled-arithmetic helper is used.
                    const render_wire::Point destination_xyz =
                        positive_forward ? render_wire::Point{float(7 + 2 * owner + source), 0, 0}
                                         : render_wire::Point{};
                    const long double destination_target = proxy(destination_xyz, 1);
                    const auto &h = mapped_history[source];
                    const long double source_Z = number(h.normalizer, h.normalizer_exponent);
                    const long double source_target = number(h.target, h.target_exponent);
                    const long double M = std::min(cap, h.confidence);
                    const long double shifted = source_Z * M * destination_target /
                                                (M * source_target + destination_target);
                    const long double canonical = expected_Z[owner];
                    expected_Z[owner] = canonical + shifted;
                    if (positive_forward) {
                        const auto &wrong = mapped_history[owner];
                        const long double wrong_M = std::min(cap, wrong.confidence);
                        const long double wrong_Z =
                            number(wrong.normalizer, wrong.normalizer_exponent);
                        const long double wrong_target =
                            number(wrong.target, wrong.target_exponent);
                        const long double owner_indexed =
                            canonical + wrong_Z * wrong_M * destination_target /
                                            (wrong_M * wrong_target + destination_target);
                        const long double swapped_denominator =
                            canonical + source_Z * M * destination_target /
                                            (M * destination_target + source_target);
                        const long double owner_Z_only =
                            canonical + wrong_Z * M * destination_target /
                                            (M * source_target + destination_target);
                        const long double owner_target_only =
                            canonical + source_Z * M * destination_target /
                                            (M * wrong_target + destination_target);
                        // Strongly separated BEFORE the GPU result is observed. These
                        // bounds exceed the unchanged64epsilon score comparison gate.
                        const long double separation = 1024.L *
                                                       std::numeric_limits<float>::epsilon() *
                                                       std::max(1.L, std::abs(expected_Z[owner]));
                        wrong_owner_separated |=
                            std::abs(owner_indexed - expected_Z[owner]) > separation;
                        wrong_denominator_separated |=
                            std::abs(swapped_denominator - expected_Z[owner]) > separation;
                        wrong_Z_separated |=
                            std::abs(owner_Z_only - expected_Z[owner]) > separation;
                        wrong_target_separated |=
                            std::abs(owner_target_only - expected_Z[owner]) > separation;
                    }
                    response(owner, destination_xyz, 1, 1);
                }
                if (positive_forward)
                    require(wrong_owner_separated &&
                                (cap == 1 ? !wrong_denominator_separated
                                          : wrong_denominator_separated) &&
                                wrong_Z_separated && wrong_target_separated,
                            "Positive mapped score must reject owner-indexed Z/T/M and reversed "
                            "denominator orientation");
                dispatch(merge);
                for (unsigned owner = 0; owner < pixels; ++owner)
                    scalar_near(accumulator[owner].state.score_sum,
                                accumulator[owner].score_exponent, expected_Z[owner],
                                "Mapped forward adds independent positive source score");
                dispatch(finish);
                for (unsigned owner = 0; owner < pixels; ++owner) {
                    const auto source = donors[owner].source_pixel;
                    require(temporal[owner].confidence ==
                                    1 + std::min(cap, mapped_history[source].confidence) &&
                                temporal[owner].record.bank == ew::temporal_bank &&
                                temporal[owner].record.pixel == owner,
                            "Mapped temporal confidence and publication use source s and owner d "
                            "respectively");
                    scalar_near(temporal[owner].normalizer, temporal[owner].normalizer_exponent,
                                expected_Z[owner],
                                positive_forward ? "Mapped positive temporal normalizer is the sum "
                                                   "of canonical and shifted "
                                                   "scores"
                                                 : "Mapped zero-forward temporal normalizer "
                                                   "retains the canonical score");
                }
                require(
                    std::memcmp(frozen_history.data(), mapped_history, sizeof(frozen_history)) == 0,
                    "Many-to-one temporal reuse never mutates old source payloads");
            }
    }
    config.enhanced_history_cap = 20;
    // Four handwritten involutions isolate SAME-k reverse targets from RNG
    // and production atlas construction. All scalar/vector expectations below
    // are finite-domain long-double pairwise MIS, not production score helpers.
    namespace atlas = rypath_area_pair_atlas;
    namespace oracle = atlas_oracle;
    auto paired_config = atlas::base_config();
    std::array<std::vector<atlas::Entry>, 4> tiles;
    std::vector<atlas::Entry> packed_entries;
    for (unsigned k = 0; k < 4; ++k) {
        tiles[k] = oracle::handwritten(atlas::tile_width(k), k);
        packed_entries.insert(packed_entries.end(), tiles[k].begin(), tiles[k].end());
    }
    lengths[aw::paired_atlas_id] = packed_entries.size() * sizeof(atlas::Entry);
    buffers[aw::paired_atlas_id] =
        [device newBufferWithLength:lengths[aw::paired_atlas_id] + 2 * guard
                            options:MTLResourceStorageModeShared];
    require(buffers[aw::paired_atlas_id] != nil, "Handwritten atlas guarded buffer");
    std::memset(buffers[aw::paired_atlas_id].contents, 0xa5, buffers[aw::paired_atlas_id].length);
    std::memcpy(data(aw::paired_atlas_id), packed_entries.data(), lengths[aw::paired_atlas_id]);
    std::memcpy(data(aw::paired_config_id), &paired_config, sizeof(paired_config));
    bind();
    config.temporal_reprojection = 0;
    config.stencil = aw::paired_gaussian16_stencil;
    config.offset_x[0] = config.offset_x[1] = config.offset_y[0] = config.offset_y[1] = 0;
    config.phase = aw::spatial;
    config.candidate = 0;
    std::array<aw::Temporal, pixels> frozen{};
    std::array<ew::Record, pixels> frozen_records{};
    for (unsigned p = 0; p < pixels; ++p) {
        auto &t = temporal[p];
        t = {};
        t.payload = rypath_replay::fresh({p, 211, 223, 227, 229});
        t.xyz = p % 5 == 1 ? render_wire::Point{} : render_wire::Point{float(4 + p), 0, 0};
        t.selected_slot = p % 5 == 1 ? cw::enhanced_null_slot : 1u;
        t.confidence = 1 + p % 7;
        t.record = {ew::temporal_bank, p};
        t.target_h_fraction = .5f;
        t.target_h_exponent = 1;
        const auto [T, e] = scale(proxy(t.xyz, 1));
        t.target = T;
        t.target_exponent = e;
        t.normalizer = T;
        t.normalizer_exponent = T == 0 ? 0 : e + 1 + int(p % 3);
        record_at(t.record) =
            t.selected_slot == cw::enhanced_null_slot ? ew::empty_record() : descriptor(p, 0);
        frozen[p] = t;
        frozen_records[p] = record_at(t.record);
    }
    std::array<std::array<unsigned, pixels>, 4> neighbors{};
    std::array<std::array<long double, pixels>, 4> cross{};
    std::array<std::array<render_wire::Point, pixels>, 4> reevaluated{};
    bool duplicates = false, singleton = false;
    for (unsigned p = 0; p < pixels; ++p) {
        bool all_self = true;
        for (unsigned k = 0; k < 4; ++k) {
            neighbors[k][p] =
                oracle::neighbor(tiles[k], atlas::tile_width(k), width, height, p, 0, 0, 0);
            all_self &= neighbors[k][p] == p;
        }
        duplicates |= neighbors[0][p] != p && neighbors[0][p] == neighbors[1][p];
        singleton |= all_self && frozen[p].normalizer > 0;
    }
    require(
        duplicates && singleton,
        "Handwritten fixture has duplicate candidate neighbors and positive all-edge singleton");
    dispatch(spatial_begin);
    for (unsigned k = 0; k < 4; ++k) {
        config.candidate = k + 1;
        dispatch(prepare);
        for (unsigned p = 0; p < pixels; ++p) {
            const unsigned source = neighbors[k][p];
            require(same(replay[p], frozen[source].payload) &&
                        same(evaluations[p].source, frozen[source].record),
                    "Atlas Area prepare selects the independent candidate-k source payload/record");
            const auto xyz =
                frozen[source].selected_slot == cw::enhanced_null_slot
                    ? render_wire::Point{}
                    : render_wire::Point{float(9 + 11 * k + 2 * p + source), float(1 + k), 0};
            reevaluated[k][p] = xyz;
            cross[k][p] = proxy(xyz, 1);
            response(p, xyz, 1, frozen[source].selected_slot);
        }
        dispatch(merge);
    }
    config.phase = aw::spatial_final;
    config.candidate = 0;
    dispatch(finalize);
    const auto *history = reinterpret_cast<const aw::History *>(data(aw::history_destination_id));
    bool wrong_reverse_separated = false;
    for (unsigned p = 0; p < pixels; ++p) {
        const auto &center = frozen[p];
        const long double Zp = number(center.normalizer, center.normalizer_exponent),
                          Tp = number(center.target, center.target_exponent),
                          Mp = center.confidence;
        long double expected_Z = 0, wrong_Z = 0;
        std::array<long double, 3> expected_raw{};
        unsigned expected_M = center.confidence;
        const auto add = [&](long double score, render_wire::Point xyz, long double target) {
            expected_Z += score;
            if (target > 0) {
                expected_raw[0] += score * xyz.x / target;
                expected_raw[1] += score * xyz.y / target;
                expected_raw[2] += score * xyz.z / target;
            }
        };
        for (unsigned k = 0; k < 4; ++k) {
            const unsigned s = neighbors[k][p];
            if (s == p) {
                add(Zp / 4, center.xyz, Tp);
                wrong_Z += Zp / 4;
                continue;
            }
            const auto &source = frozen[s];
            expected_M += source.confidence;
            const long double Zs = number(source.normalizer, source.normalizer_exponent),
                              Ts = number(source.target, source.target_exponent),
                              Ms = source.confidence, Td = cross[k][p];
            const long double shifted =
                (Zs == 0 || Td == 0) ? 0 : Zs * Ms * Td / (Ms * Ts + Mp * Td) / 4;
            add(shifted, reevaluated[k][p], Td);
            const long double reverse = cross[k][s];
            const long double canonical = Zp == 0 ? 0 : Zp * Mp * Tp / (Mp * Tp + Ms * reverse) / 4;
            add(canonical, center.xyz, Tp);
            const long double old_reverse = cross[k ^ 1u][s];
            wrong_Z += shifted + (Zp == 0 ? 0 : Zp * Mp * Tp / (Mp * Tp + Ms * old_reverse) / 4);
        }
        scalar_near(history[p].normalizer, history[p].normalizer_exponent, expected_Z,
                    "Actual atlas SAME-k finite-domain scalar normalizer");
        near(raw[p].x, expected_raw[0], "Actual atlas independent conditional X");
        near(raw[p].y, expected_raw[1], "Actual atlas independent conditional Y");
        near(raw[p].z, expected_raw[2], "Actual atlas independent conditional Z");
        require(history[p].confidence == expected_M && history[p].record.pixel == p &&
                    history[p].record.bank == 1u - config.history_source_bank,
                "Duplicate/edge/null atlas slots preserve structural confidence and "
                "destination-owned publication");
        wrong_reverse_separated |=
            std::abs(wrong_Z - expected_Z) >
            1024.L * std::numeric_limits<float>::epsilon() * std::max(1.L, std::abs(expected_Z));
        bool tuple = false;
        if (same(history[p].payload, center.payload) && same(history[p].xyz, center.xyz) &&
            same(record_at(history[p].record), frozen_records[p]))
            tuple = true;
        for (unsigned k = 0; k < 4; ++k) {
            const auto source = neighbors[k][p];
            if (source != p && same(history[p].payload, frozen[source].payload) &&
                same(history[p].xyz, reevaluated[k][p]) &&
                same(record_at(history[p].record), frozen_records[source]))
                tuple = true;
        }
        require(tuple,
                "Published atlas selection is one actual admitted owner/source evaluation tuple");
        require(same(record_at(frozen[p].record), frozen_records[p]),
                "Atlas reuse preserves every immutable source record");
    }
    require(wrong_reverse_separated,
            "Old opposite-slot reverse targets fail the fixed atlas MIS oracle");
    // Exhaust both independent atlas choices through ACTUAL production kernels.
    // Uncomputed maps contain invalid Numbers so accidental cross-slot reads fail.
    // The canonical-only leaf must not add another donor or alter the mean.
    for (unsigned policy : {1u, 2u}) {
        config.spatial_mode = policy;
        for (unsigned forward = 0; forward < 4; ++forward)
            for (unsigned canonical = 0; canonical < 4; ++canonical) {
                if (policy == 2 && forward != canonical)
                    continue;
                config.forward_atlas = forward + 1;
                config.canonical_atlas = canonical + 1;
                config.phase = aw::spatial;
                config.candidate = 0;
                std::memset(data(aw::cross_targets_id), 0xa5, lengths[aw::cross_targets_id]);
                dispatch(spatial_begin);
                const auto evaluate = [&](unsigned k) {
                    config.candidate = k + 1;
                    dispatch(prepare);
                    for (unsigned p = 0; p < pixels; ++p) {
                        const auto source = neighbors[k][p];
                        response(p, reevaluated[k][p], 1, frozen[source].selected_slot);
                    }
                    dispatch(merge);
                };
                evaluate(forward);
                if (canonical != forward)
                    evaluate(canonical);
                config.phase = aw::spatial_final;
                config.candidate = 0;
                dispatch(finalize);
                for (unsigned p = 0; p < pixels; ++p) {
                    const auto &center = frozen[p];
                    const long double Zp = number(center.normalizer, center.normalizer_exponent),
                                      Tp = number(center.target, center.target_exponent),
                                      Mp = center.confidence;
                    const unsigned donor = neighbors[forward][p];
                    const auto &source = frozen[donor];
                    const long double Zs = number(source.normalizer, source.normalizer_exponent),
                                      Ts = number(source.target, source.target_exponent),
                                      Ms = source.confidence, Td = cross[forward][p];
                    const long double shifted =
                        donor == p || Zs == 0 || Td == 0 ? 0 : Zs * Ms * Td / (Ms * Ts + Mp * Td);
                    const unsigned reverse_source = neighbors[canonical][p];
                    const long double reverse = cross[canonical][reverse_source];
                    const long double center_score =
                        Zp == 0 ? 0
                        : reverse_source == p
                            ? Zp
                            : Zp * Mp * Tp /
                                  (Mp * Tp + frozen[reverse_source].confidence * reverse);
                    unsigned expected_M = center.confidence;
                    for (unsigned k = 0; k < 4; ++k)
                        if (neighbors[k][p] != p)
                            expected_M += frozen[neighbors[k][p]].confidence;
                    std::array<long double, 3> expected_raw{};
                    const auto add = [&](long double weight, render_wire::Point xyz,
                                         long double target) {
                        if (target > 0) {
                            expected_raw[0] += weight * xyz.x / target;
                            expected_raw[1] += weight * xyz.y / target;
                            expected_raw[2] += weight * xyz.z / target;
                        }
                    };
                    add(shifted, reevaluated[forward][p], Td);
                    add(center_score, center.xyz, Tp);
                    scalar_near(history[p].normalizer, history[p].normalizer_exponent,
                                shifted + center_score,
                                "Stochastic pair actual two-candidate normalizer");
                    near(raw[p].x, expected_raw[0], "Stochastic pair independent conditional X");
                    near(raw[p].y, expected_raw[1], "Stochastic pair independent conditional Y");
                    near(raw[p].z, expected_raw[2], "Stochastic pair independent conditional Z");
                    require(history[p].confidence == expected_M,
                            "Stochastic sampling retains ALL four structural confidences");
                    const bool center_tuple = same(history[p].payload, center.payload) &&
                                              same(history[p].xyz, center.xyz);
                    const bool donor_tuple = donor != p &&
                                             same(history[p].payload, source.payload) &&
                                             same(history[p].xyz, reevaluated[forward][p]);
                    require(center_tuple || donor_tuple ||
                                (shifted + center_score == 0 &&
                                 history[p].selected_slot == cw::enhanced_null_slot),
                            "Canonical-only leaf never enters selected history");
                }
            }
    }
    config.spatial_mode = config.forward_atlas = config.canonical_atlas = 0;
    require(std::memcmp(frozen.data(), temporal, sizeof(frozen)) == 0 &&
                std::memcmp(data(aw::paired_atlas_id), packed_entries.data(),
                            lengths[aw::paired_atlas_id]) == 0 &&
                std::memcmp(data(aw::paired_config_id), &paired_config, sizeof(paired_config)) == 0,
            "Actual atlas passes preserve frozen source tuples, asset and all frame poses");
}
int run(const std::filesystem::path &path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    require(device != nil, "No Metal device for queue failure validation");
    std::string why;
    auto library = metal::load_library(device, path, why);
    require(library != nil, why);
    auto function = metal::load_function(library, @"rypath_wavefront_advance", why);
    require(function != nil, why);
    auto pipeline = metal::load_pipeline(device, library, @"rypath_wavefront_advance", why);
    require(pipeline != nil, why);
    auto argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
    require(argument_encoder != nil, "Queue argument encoder");
    auto arguments = [device newBufferWithLength:argument_encoder.encodedLength
                                         options:MTLResourceStorageModeShared];
    std::array<id<MTLBuffer>, wf::donors_id + 1> buffers{};
    [argument_encoder setArgumentBuffer:arguments offset:0];
    for (unsigned i = 0; i < buffers.size(); ++i) {
        const auto bytes = i == wf::enhanced_records_id      ? sizeof(enhanced_wire::Record)
                           : i == wf::enhanced_evaluation_id ? sizeof(enhanced_wire::Evaluation)
                           : i == wf::enhanced_suffix_id
                               ? sizeof(rypath_enhanced_suffix_tape::Entry)
                           : i >= wf::contribution_evaluation_id ? 16
                                                                 : 256;
        buffers[i] = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        require(buffers[i] != nil, "Queue fault test allocation");
        [argument_encoder setBuffer:buffers[i] offset:0 atIndex:i];
    }
    auto queue = [device newCommandQueue];
    require(queue != nil && arguments != nil, "Queue fault test command resources");
    area_disabled_wire(device, library, queue);
    const wf::Config config{4, 2, 0, render_wire::pbrt_model, 32, 0, 1, 0, 0, 0, 0.f, 0, 0,
                            0, 0, 0};
    const std::array<wf::Counters, 9> cases{{
        {2, 2, 0, 4, 2, 0, 7, 2, 0, {0, 0, 0}, 0, 0, 0, 0}, // Valid two-owner continuation clears
                                                            // its range.
        {2, 3, 0, 4, 1, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Output exceeds physical capacity.
        {2, 1, 3, 4, 3, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Free list exceeds physical capacity.
        {2, 0, 0, 4, 5, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // More completions than pixels.
        {2, 0, 0, 5, 4, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Allocator exceeds pixel domain.
        {2, 0, 0, 4, 2, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Lost work cannot become an empty
                                                            // successful queue.
        {2, 2, 0, 4, 2, wf::queue_overflow, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Producer overflow
                                                                             // stays sticky.
        {2, 2, 0, 4, 2, 0, 7, 0, 3, {0, 0, 0}, 0, 0, 0, 0}, // Null completions exceed all
                                                            // completions.
        {2, 2, 0, 4, 2, 0, 7, 0, 1, {0, 0, 0}, 0, 0, 0, 0}, // Legacy mode cannot publish null
                                                            // completions.
    }};
    for (std::size_t n = 0; n < cases.size(); ++n) {
        for (auto buffer : buffers)
            std::memset(buffer.contents, 0xa5, buffer.length);
        std::memcpy(buffers[wf::config_id].contents, &config, sizeof(config));
        std::memcpy(buffers[wf::counters_id].contents, &cases[n], sizeof(wf::Counters));
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        require(command != nil && encoder != nil, "Queue fault test encoding");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:arguments offset:0 atIndex:1];
        for (auto buffer : buffers)
            [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        require(metal::complete(command, "wavefront queue fault").empty(),
                "Queue dispatch failure");
        wf::Counters result{};
        std::memcpy(&result, buffers[wf::counters_id].contents, sizeof(result));
        const auto expected_failure = n == 0   ? 0u
                                      : n == 6 ? wf::queue_overflow
                                               : wf::invalid_counters;
        require(result.failure == expected_failure && result.current == (n == 0 ? 2u : 0u) &&
                    result.out == 0 && result.free == 0 && result.refill_begin == 0 &&
                    result.rounds == 8 && result.completed == cases[n].completed &&
                    result.next_pixel == cases[n].next_pixel &&
                    result.null_completed == cases[n].null_completed && result.work_count == 0,
                "Queue corruption must fail explicitly without inventing completions");
        for (unsigned i = 0; i < buffers.size(); ++i) {
            const auto written = i == wf::counters_id ? sizeof(wf::Counters)
                                 : i == wf::config_id ? sizeof(wf::Config)
                                 : i == wf::current_dispatch_id || i == wf::refill_dispatch_id
                                     ? sizeof(wf::DispatchArguments)
                                     : 0;
            const auto *bytes = static_cast<const unsigned char *>(buffers[i].contents);
            for (std::size_t j = written; j < buffers[i].length; ++j)
                require(bytes[j] == 0xa5, "Queue failure touched unrelated storage or a guard");
        }
    }
    auto prepare = metal::load_pipeline(device, library, @"rypath_wavefront_prepare_refill", why);
    require(prepare != nil, why);
    const std::array<wf::Counters, 14> refill_cases{{
        {2, 0, 2, 2, 2, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Full refill owns [2,4).
        {2, 0, 2, 3, 3, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Partial final range owns [3,4).
        {2, 0, 2, 4, 4, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Exhausted domain claims nothing.
        {2, 1, 1, 3, 2, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // One continuing owner plus one new
                                                            // owner.
        {2, 2, 0, 2, 0, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // All owners continue; no free slots.
        {0, 0, 0, 4, 4, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Extra encoded round after all work
                                                            // completed.
        {2, 0, 3, 3, 3, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Invalid free count.
        {2, 0, 2, 5, 5, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Invalid next owner.
        {2, 0, 2, 2, 3, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Completions exceed issued owners.
        {2, 0, 1, 3, 3, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Lost active owner.
        {2, 1, 1, 3, 3, 0, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0}, // Continuing count disagrees with
                                                            // issued work.
        {2, 0, 2, 2, 2, wf::queue_overflow, 7, 0, 0, {0, 0, 0}, 0, 0, 0, 0},
        {2, 0, 2, 2, 2, 0, 7, 0, 3, {0, 0, 0}, 0, 0, 0, 0}, // Null count cannot exceed completions.
        {2, 0, 2, 2, 2, 0, 7, 0, 1, {0, 0, 0}, 0, 0, 0, 0}, // Legacy null count is invalid.
    }};
    for (std::size_t n = 0; n < refill_cases.size(); ++n) {
        for (auto buffer : buffers)
            std::memset(buffer.contents, 0xa5, buffer.length);
        std::memcpy(buffers[wf::config_id].contents, &config, sizeof(config));
        const auto before = refill_cases[n];
        std::memcpy(buffers[wf::counters_id].contents, &before, sizeof(before));
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        require(command != nil && encoder != nil, "Refill range encoding");
        [encoder setComputePipelineState:prepare];
        [encoder setBuffer:arguments offset:0 atIndex:1];
        for (auto buffer : buffers)
            [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        require(metal::complete(command, "wavefront range reservation").empty(),
                "Refill range dispatch");
        wf::Counters result{};
        wf::DispatchArguments dispatch{};
        std::memcpy(&result, buffers[wf::counters_id].contents, sizeof(result));
        std::memcpy(&dispatch, buffers[wf::refill_dispatch_id].contents, sizeof(dispatch));
        const bool valid = n < 6;
        const unsigned expected_count = n == 0 ? 2 : (n == 1 || n == 3) ? 1 : 0;
        require(result.failure == (valid     ? 0u
                                   : n == 11 ? wf::queue_overflow
                                             : wf::invalid_counters),
                "Invalid range reservation must fail before claiming owners");
        require(result.completed == before.completed && result.current == before.current &&
                    result.out == before.out && result.rounds == before.rounds &&
                    result.null_completed == before.null_completed && result.work_count == 0 &&
                    result.next_pixel ==
                        (valid ? before.next_pixel + expected_count : before.next_pixel) &&
                    result.free == (valid ? expected_count : before.free) &&
                    result.refill_begin == (valid ? before.next_pixel : before.refill_begin),
                "A single range reservation preserves ownership and completion accounting");
        require(dispatch.x == 1 && dispatch.y == 1 && dispatch.z == 1,
                "Small or empty refill uses one safely masked group");
        for (unsigned i = 0; i < buffers.size(); ++i) {
            const auto written = i == wf::counters_id          ? sizeof(wf::Counters)
                                 : i == wf::config_id          ? sizeof(wf::Config)
                                 : i == wf::refill_dispatch_id ? sizeof(wf::DispatchArguments)
                                                               : 0;
            const auto *bytes = static_cast<const unsigned char *>(buffers[i].contents);
            for (std::size_t j = written; j < buffers[i].length; ++j)
                require(bytes[j] == 0xa5, "Range reservation touched queue storage or a guard");
        }
    }
    // Exercise the real policy constants against every valid wire policy.
    // A mismatched function/buffer pair must fail before issuing any owners.
    const std::array<std::array<std::uint32_t, 2>, 7> policies{
        {{0, 1}, {0, 2}, {0, 4}, {1, 1}, {1, 2}, {1, 4}, {2, 1}}};
    for (const auto &compiled : policies) {
        auto constants = [MTLFunctionConstantValues new];
        const bool summed = false;
        [constants setConstantValue:&summed
                               type:MTLDataTypeBool
                            atIndex:wf::contribution_mode_constant];
        [constants setConstantValue:&compiled[0]
                               type:MTLDataTypeUInt
                            atIndex:wf::packet_layout_constant];
        [constants setConstantValue:&compiled[1]
                               type:MTLDataTypeUInt
                            atIndex:wf::packet_events_constant];
        auto reset =
            metal::load_pipeline(device, library, @"rypath_wavefront_reset", why, nil, constants);
        require(reset != nil, why);
        for (unsigned packet : {1u, 2u}) {
            for (const auto &requested : policies) {
                for (auto buffer : buffers)
                    std::memset(buffer.contents, 0xa5, buffer.length);
                auto cfg = config;
                cfg.packet = packet;
                cfg.packet_layout = requested[0];
                cfg.packet_events = requested[1];
                std::memcpy(buffers[wf::config_id].contents, &cfg, sizeof(cfg));
                auto command = [queue commandBuffer];
                auto encoder = [command computeCommandEncoder];
                require(command != nil && encoder != nil, "Policy consistency encoding");
                [encoder setComputePipelineState:reset];
                [encoder setBuffer:arguments offset:0 atIndex:1];
                for (auto buffer : buffers)
                    [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
                [encoder dispatchThreads:MTLSizeMake(cfg.pixel_count, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [encoder endEncoding];
                require(metal::complete(command, "wavefront policy consistency").empty(),
                        "Policy consistency dispatch");
                wf::Counters result{};
                std::memcpy(&result, buffers[wf::counters_id].contents, sizeof(result));
                const bool matched = compiled == requested;
                require(result.failure == (matched ? 0u : wf::invalid_config) &&
                            result.current == (matched ? cfg.capacity : 0u) &&
                            result.next_pixel == (matched ? cfg.capacity : 0u) &&
                            result.completed == 0 && result.out == 0 && result.free == 0,
                        "Policy mismatch must not claim or complete an owner");
                for (unsigned i = 0; i < buffers.size(); ++i) {
                    const auto written =
                        i == wf::counters_id ? sizeof(wf::Counters)
                        : i == wf::config_id ? sizeof(wf::Config)
                        : i == wf::done_id   ? cfg.pixel_count * sizeof(std::uint32_t)
                        : i == wf::current_dispatch_id || i == wf::refill_dispatch_id
                            ? sizeof(wf::DispatchArguments)
                            : 0;
                    const auto *bytes = static_cast<const unsigned char *>(buffers[i].contents);
                    for (std::size_t j = written; j < buffers[i].length; ++j)
                        require(bytes[j] == 0xa5, "Policy consistency touched unrelated storage");
                }
            }
        }
    }
    auto contract_pipeline =
        metal::load_pipeline(device, library, @"rypath_wavefront_contract_v23", why);
    require(contract_pipeline != nil,
            "Wavefront v23 contract unavailable; rebuild the metallib: " + why);
    for (NSString *stale in @[
             @"rypath_wavefront_contract_v11", @"rypath_wavefront_contract_v12",
             @"rypath_wavefront_contract_v13", @"rypath_wavefront_contract_v14",
             @"rypath_wavefront_contract_v15", @"rypath_wavefront_contract_v16",
             @"rypath_wavefront_contract_v17", @"rypath_wavefront_contract_v18",
             @"rypath_wavefront_contract_v19", @"rypath_wavefront_contract_v20",
             @"rypath_wavefront_contract_v21", @"rypath_wavefront_contract_v22"
         ])
        require([library newFunctionWithName:stale] == nil,
                "Stale Wavefront stage entry must not advertise the v23 ABI");
    auto contract_buffer = [device newBufferWithLength:sizeof(wf::Contract)
                                               options:MTLResourceStorageModeShared];
    require(contract_buffer != nil, "History contract allocation");
    std::memset(contract_buffer.contents, 0, contract_buffer.length);
    auto contract_command = [queue commandBuffer];
    auto contract_encoder = [contract_command computeCommandEncoder];
    require(contract_command != nil && contract_encoder != nil, "History contract encoder");
    [contract_encoder setComputePipelineState:contract_pipeline];
    [contract_encoder setBuffer:contract_buffer offset:0 atIndex:0];
    [contract_encoder dispatchThreads:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [contract_encoder endEncoding];
    require(metal::complete(contract_command, "history storage contract").empty(),
            "History contract dispatch");
    wf::Contract history_contract{};
    std::memcpy(&history_contract, contract_buffer.contents, sizeof(history_contract));
    require(history_contract.version == wf::contract && history_contract.hero_bytes &&
                history_contract.hero_cold_bytes && history_contract.hot_bytes &&
                history_contract.config_bytes == 64 && history_contract.counters_bytes == 64 &&
                history_contract.dispatch_bytes == 12 &&
                history_contract.contribution_state_bytes == 80 &&
                history_contract.contribution_state_alignment == 4 &&
                history_contract.enhanced_state_bytes == 292 &&
                history_contract.enhanced_state_alignment == 4 &&
                history_contract.enhanced_record_bytes == 48 &&
                history_contract.enhanced_evaluation_bytes == 40 &&
                history_contract.suffix_entry_bytes == 160 && history_contract.donor_bytes == 16,
            "History storage contract mismatch");
    enhanced_null_owners(device, library, queue, history_contract);
    enhanced_area_protocol(device, library, queue);
    enhanced_area_protocol(device, library, queue, true);
    constexpr std::uint32_t history_schema = 0x48535435u;
    auto layout_pipeline =
        metal::load_pipeline(device, library, @"rypath_hero_history_layout", why);
    require(layout_pipeline != nil, why);
    auto layout_buffer = [device newBufferWithLength:12 * sizeof(std::uint32_t)
                                             options:MTLResourceStorageModeShared];
    require(layout_buffer != nil, "History layout allocation");
    std::memset(layout_buffer.contents, 0xa5, layout_buffer.length);
    auto layout_command = [queue commandBuffer];
    auto layout_encoder = [layout_command computeCommandEncoder];
    require(layout_command != nil && layout_encoder != nil, "History layout encoder");
    [layout_encoder setComputePipelineState:layout_pipeline];
    [layout_encoder setBuffer:layout_buffer offset:0 atIndex:0];
    [layout_encoder dispatchThreads:MTLSizeMake(1, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [layout_encoder endEncoding];
    require(metal::complete(layout_command, "history layout").empty(), "History layout dispatch");
    std::array<std::uint32_t, 12> history_layout{};
    std::memcpy(history_layout.data(), layout_buffer.contents, layout_buffer.length);
    require(history_layout[0] == history_schema && history_layout[11] == sizeof(std::uint32_t),
            "History layout diagnostic schema/bool ABI mismatch");
    for (std::uint32_t history_case = 0; history_case < 6; ++history_case) {
        const std::uint32_t layout = history_case % 3;
        const bool fixed = history_case >= 3;
        MTLFunctionConstantValues *constants = [MTLFunctionConstantValues new];
        const std::uint32_t events = 1;
        [constants setConstantValue:&layout
                               type:MTLDataTypeUInt
                            atIndex:wf::packet_layout_constant];
        [constants setConstantValue:&events
                               type:MTLDataTypeUInt
                            atIndex:wf::packet_events_constant];
        // Fixed shade entries receive scene constants only. Exercise the same
        // explicit layout helpers with constants 23/24 absent, including split.
        NSString *write_entry = !fixed        ? @"rypath_hero_history_write"
                                : layout == 0 ? @"rypath_hero_history_write_packed"
                                              : @"rypath_hero_history_write_separated";
        NSString *read_entry = !fixed        ? @"rypath_hero_history_read"
                               : layout == 0 ? @"rypath_hero_history_read_packed"
                                             : @"rypath_hero_history_read_separated";
        auto policy = fixed ? nil : constants;
        auto write_function = metal::load_function(library, write_entry, why, policy);
        require(write_function != nil, why);
        auto write_pipeline = metal::load_pipeline(device, library, write_entry, why, nil, policy);
        auto read_pipeline = metal::load_pipeline(device, library, read_entry, why, nil, policy);
        require(write_pipeline != nil && read_pipeline != nil, why);
        auto history_arguments_encoder = [write_function newArgumentEncoderWithBufferIndex:1];
        require(history_arguments_encoder != nil, "History storage argument encoder");
        auto history_arguments = [device newBufferWithLength:history_arguments_encoder.encodedLength
                                                     options:MTLResourceStorageModeShared];
        require(history_arguments != nil, "History storage arguments allocation");
        [history_arguments_encoder setArgumentBuffer:history_arguments offset:0];
        constexpr std::size_t guard_bytes = 256, slot_count = 7;
        std::array<id<MTLBuffer>, wf::donors_id + 1> history_buffers{};
        std::array<std::size_t, wf::donors_id + 1> strides{};
        strides[wf::packet_state_id] = history_contract.packet_bytes;
        strides[wf::packet_hot_id] = history_contract.hot_bytes;
        strides[wf::packet_cold_id] = history_contract.cold_bytes;
        strides[wf::hero_state_id] = history_contract.hero_bytes;
        strides[wf::hero_cold_id] = history_contract.hero_cold_bytes;
        for (unsigned i = 0; i < history_buffers.size(); ++i) {
            const auto bytes = strides[i] ? 2 * guard_bytes + slot_count * strides[i]
                               : i == wf::enhanced_records_id    ? sizeof(enhanced_wire::Record)
                               : i == wf::enhanced_evaluation_id ? sizeof(enhanced_wire::Evaluation)
                               : i == wf::enhanced_suffix_id
                                   ? sizeof(rypath_enhanced_suffix_tape::Entry)
                               : i >= wf::contribution_evaluation_id ? 16
                                                                     : guard_bytes;
            history_buffers[i] = [device newBufferWithLength:bytes
                                                     options:MTLResourceStorageModeShared];
            require(history_buffers[i] != nil, "Guarded history pool allocation");
            std::memset(history_buffers[i].contents, 0xa5, bytes);
            [history_arguments_encoder setBuffer:history_buffers[i]
                                          offset:strides[i] ? guard_bytes : 0
                                         atIndex:i];
        }
        const wf::Config history_config{
            7, 7, 2, render_wire::pbrt_model, 1, 0, 1, layout, 0, 0, 0.f, 0, 0, 0, 0, 0};
        std::memcpy(history_buffers[wf::config_id].contents, &history_config,
                    sizeof(history_config));
        auto history_result = [device newBufferWithLength:guard_bytes
                                                  options:MTLResourceStorageModeShared];
        require(history_result != nil, "History result allocation");
        std::memset(history_result.contents, 0xa5, history_result.length);
        const auto check_guards = [&] {
            for (unsigned i = 0; i < history_buffers.size(); ++i) {
                const bool written_pool = layout == 0
                                              ? i == wf::hero_state_id
                                              : i == wf::packet_hot_id || i == wf::hero_cold_id;
                const auto *bytes = static_cast<const unsigned char *>(history_buffers[i].contents);
                for (std::size_t j = 0; j < history_buffers[i].length; ++j) {
                    if (i == wf::config_id && j < sizeof(history_config))
                        continue;
                    if (written_pool && j >= guard_bytes &&
                        j < guard_bytes + slot_count * strides[i]) {
                        const auto slot = (j - guard_bytes) / strides[i];
                        if (slot == 1 || slot == 3 || slot == 5)
                            continue;
                    }
                    require(bytes[j] == 0xa5,
                            "History storage touched a guard, unused owner or inactive pool");
                }
            }
            require(std::memcmp(history_buffers[wf::config_id].contents, &history_config,
                                sizeof(history_config)) == 0,
                    "History storage modified immutable configuration");
        };
        const auto check_dielectric_bytes = [&] {
            // Slot three contains two active dielectric lanes. Derive their
            // dyadic fixture values independently, without a host Prepared type.
            constexpr unsigned slot = 3;
            const auto pool = layout == 0 ? wf::hero_state_id : wf::hero_cold_id;
            const auto previous_offset = layout == 0 ? history_layout[1] : 0;
            const auto bsdf_offset = previous_offset + history_layout[2];
            require(bsdf_offset + history_layout[3] + 2 * history_layout[6] <= strides[pool],
                    "History raw dielectric witness exceeds owner stride");
            const auto *owner = static_cast<const unsigned char *>(history_buffers[pool].contents) +
                                guard_bytes + slot * strides[pool];
            const auto read_word = [&](std::size_t offset) {
                std::uint32_t word;
                std::memcpy(&word, owner + offset, sizeof(word));
                return word;
            };
            require(read_word(bsdf_offset + history_layout[4]) == 2 &&
                        read_word(bsdf_offset + history_layout[5]) == 3,
                    "Host raw dielectric tag/mask mismatch");
            for (unsigned lane = 0; lane < 2; ++lane) {
                const float f = float(lane + slot * 16) + .125f;
                const std::array expected{f + 6, f + 7, f + 8};
                const auto parameters = bsdf_offset + history_layout[3] + lane * history_layout[6];
                for (unsigned field = 0; field < expected.size(); ++field) {
                    const auto actual = read_word(parameters + history_layout[7 + field]);
                    const auto wanted = std::bit_cast<std::uint32_t>(expected[field]);
                    if (actual != wanted) {
                        std::ostringstream message;
                        message << "Host raw dielectric mismatch: layout=" << layout
                                << ", fixed=" << fixed << ", lane=" << lane << ", field=" << field
                                << std::hex << ", expected/actual=0x" << wanted << "/0x" << actual;
                        require(false, message.str());
                    }
                }
                const auto actual = read_word(parameters + history_layout[10]);
                require(actual == unsigned(bool(lane & 2)),
                        "Host raw dielectric smooth word mismatch: layout=" +
                            std::to_string(layout) + ", fixed=" + std::to_string(fixed) +
                            ", lane=" + std::to_string(lane) + ", word=" + std::to_string(actual));
            }
        };
        // A completed command publishes the write before a distinct read dispatch;
        // there is no thread-local freeze/expand shortcut between the two kernels.
        for (unsigned phase = 0; phase < 2; ++phase) {
            auto history_command = [queue commandBuffer];
            auto history_encoder = [history_command computeCommandEncoder];
            require(history_command != nil && history_encoder != nil, "History storage encoder");
            [history_encoder setComputePipelineState:phase == 0 ? write_pipeline : read_pipeline];
            [history_encoder setBuffer:history_arguments offset:0 atIndex:1];
            if (phase != 0)
                [history_encoder setBuffer:history_result offset:0 atIndex:0];
            for (auto buffer : history_buffers)
                [history_encoder useResource:buffer
                                       usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            [history_encoder dispatchThreads:MTLSizeMake(3, 1, 1)
                       threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [history_encoder endEncoding];
            require(metal::complete(history_command,
                                    phase == 0 ? "hero history write" : "hero history read")
                        .empty(),
                    "History storage dispatch");
            check_guards();
            check_dielectric_bytes();
        }
        const auto *results = static_cast<const std::uint32_t *>(history_result.contents);
        constexpr std::array fields{"position",
                                    "geometric normal",
                                    "shading normal",
                                    "frame",
                                    "vertex metadata",
                                    "frozen BSDF",
                                    "forward area density",
                                    "reverse area density",
                                    "preceding geometry",
                                    "sample identity/status",
                                    "sample scalars",
                                    "sample random words",
                                    "wavelengths",
                                    "hero density",
                                    "previous hero density",
                                    "lens/filter",
                                    "radiance",
                                    "beta",
                                    "ray origin",
                                    "ray direction",
                                    "ray PDF",
                                    "ray interval",
                                    "roulette",
                                    "hero state",
                                    "absolute sample index",
                                    "path counts",
                                    "completion"};
        constexpr unsigned result_words = 12;
        for (unsigned i = 0; i < 3; ++i) {
            const auto *result = results + result_words * i;
            require(result[11] == history_schema, "History result diagnostic schema mismatch");
            if (result[0] == 0)
                continue;
            std::ostringstream message;
            message << "Persistent history mismatch: layout=" << layout << ", fixed=" << fixed
                    << ", slot=" << 2 * i + 1 << ", fields=";
            for (unsigned field = 0; field < fields.size(); ++field)
                if (result[0] & (1u << field))
                    message << fields[field] << "; ";
            message << std::hex << "mask=0x" << result[0] << ", forward expected/actual=0x"
                    << result[1] << "/0x" << result[2] << ", reverse expected/actual=0x"
                    << result[3] << "/0x" << result[4] << ", BSDF kind expected/actual=0x"
                    << result[5] << "/0x" << result[6] << ", active mask=0x" << result[7];
            message << "; host raw dielectric words match both active lanes";
            require(false, message.str());
        }
        const auto *result_bytes = static_cast<const unsigned char *>(history_result.contents);
        for (std::size_t j = 3 * result_words * sizeof(std::uint32_t); j < history_result.length;
             ++j)
            require(result_bytes[j] == 0xa5, "History read touched the result guard");
    }
    MTLFunctionConstantValues *families = [MTLFunctionConstantValues new];
    const bool enabled = true;
    for (unsigned constant : {5u, 6u, 7u, 9u, 13u, 14u, 15u, 16u, 17u})
        [families setConstantValue:&enabled type:MTLDataTypeBool atIndex:constant];
    auto differential = metal::load_pipeline(device, library, @"rypath_hero_frozen_differential",
                                             why, nil, families);
    require(differential != nil, why);
    const auto fixture_directory =
        std::filesystem::temp_directory_path() /
        ("rypath-frozen-hero-" +
         std::to_string(reinterpret_cast<std::uintptr_t>((__bridge void *)device)));
    std::filesystem::create_directories(fixture_directory);
    const auto measured_path = fixture_directory / "measured.bsdf";
    const auto fourier_path = fixture_directory / "fourier.bsdf";
    table_fixture::write(measured_path, table_fixture::measured());
    table_fixture::write(fourier_path, table_fixture::fourier(3, true, 1, true));
    const auto measured_table = read_bsdf_table(measured_path, BsdfTableKind::measured);
    const auto fourier_table = read_bsdf_table(fourier_path, BsdfTableKind::fourier);
    std::filesystem::remove_all(fixture_directory);
    require(measured_table.table && fourier_table.table, "Frozen differential fixtures");
    auto measured = [device newBufferWithBytes:measured_table.table->words.data()
                                        length:measured_table.table->words.size() * 4
                                       options:MTLResourceStorageModeShared];
    auto fourier = [device newBufferWithBytes:fourier_table.table->words.data()
                                       length:fourier_table.table->words.size() * 4
                                      options:MTLResourceStorageModeShared];
    const auto rgb_values = srgb_reconstruction_table();
    auto rgb = [device newBufferWithBytes:rgb_values.data()
                                   length:rgb_values.size() * sizeof(float)
                                  options:MTLResourceStorageModeShared];
    auto differential_result = [device newBufferWithLength:480 * sizeof(std::uint32_t)
                                                   options:MTLResourceStorageModeShared];
    require(measured && fourier && rgb && differential_result, "Frozen differential buffers");
    std::memset(differential_result.contents, 0, differential_result.length);
    auto differential_command = [queue commandBuffer];
    auto differential_encoder = [differential_command computeCommandEncoder];
    require(differential_command && differential_encoder, "Frozen differential encoder");
    [differential_encoder setComputePipelineState:differential];
    [differential_encoder setBuffer:differential_result offset:0 atIndex:0];
    [differential_encoder setBuffer:measured offset:0 atIndex:1];
    [differential_encoder setBuffer:fourier offset:0 atIndex:2];
    [differential_encoder setBuffer:rgb offset:0 atIndex:3];
    [differential_encoder dispatchThreads:MTLSizeMake(480, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [differential_encoder endEncoding];
    require(metal::complete(differential_command, "frozen hero differential").empty(),
            "Frozen differential dispatch");
    const auto *differential_values =
        static_cast<const std::uint32_t *>(differential_result.contents);
    for (unsigned i = 0; i < 480; ++i)
        require(differential_values[i] == 1,
                "Frozen hero evaluation mismatch at case " + std::to_string(i));
    std::cout
        << "Wavefront queue corruption, range reservation and sticky overflow checks passed\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            require(argc == 2, "Expected metallib path");
            return run(argv[1]);
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
