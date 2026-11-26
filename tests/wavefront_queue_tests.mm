// SPDX-License-Identifier: GPL-3.0-only
#include "area_wire.h"
#include "bsdf_table.hpp"
#include "bsdf_table_fixture.hpp"
#include "metal_execution.hpp"
#include "rgb_spectrum.hpp"
#include "sobol_shared.h"
#include "wavefront_wire.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
using namespace rypath;
namespace wf = wavefront_wire;
namespace aw = area_wire;
static_assert(wf::contract == 16 && sizeof(wf::Contract) == 96 && sizeof(wf::Config) == 64);
static_assert(wf::contribution_evaluation_id == 18 && wf::contribution_state_id == 19);
static_assert(aw::contract == 9 && sizeof(aw::Contract) == 32 && sizeof(aw::Config) == 88);
static_assert(sizeof(contribution_wire::Evaluation) == 16);
void require(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
void area_disabled_wire(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue) {
    std::string why;
    auto contract_pipeline = metal::load_pipeline(device, library, @"rypath_area_contract", why);
    require(contract_pipeline != nil, why);
    auto contract_buffer = [device newBufferWithLength:sizeof(aw::Contract)
                                               options:MTLResourceStorageModeShared];
    require(contract_buffer != nil, "Area v9 contract allocation");
    std::memset(contract_buffer.contents, 0, contract_buffer.length);
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    require(command && encoder, "Area v9 contract encoder");
    [encoder setComputePipelineState:contract_pipeline];
    [encoder setBuffer:contract_buffer offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    require(metal::complete(command, "Area v9 contract").empty(), "Area contract completion");
    aw::Contract contract{};
    std::memcpy(&contract, contract_buffer.contents, sizeof(contract));
    require(contract.version == 9 && contract.config_bytes == 88 && contract.history_bytes == 60 &&
                contract.temporal_bytes == 64 && contract.accumulator_bytes == 76 &&
                contract.replay_bytes == 32 && contract.counters_bytes == 112 &&
                contract.contribution_evaluation_bytes == 16,
            "Area v9 contract mismatch; stale Area libraries are incompatible");
    auto function = metal::load_function(library, @"rypath_area_reset", why);
    auto reset = metal::load_pipeline(device, library, @"rypath_area_reset", why);
    auto prepare = metal::load_pipeline(device, library, @"rypath_area_prepare", why);
    require(function && reset && prepare, why);
    auto argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
    require(argument_encoder != nil, "Area disabled argument encoder");
    auto arguments = [device newBufferWithLength:argument_encoder.encodedLength
                                         options:MTLResourceStorageModeShared];
    require(arguments != nil, "Area disabled arguments allocation");
    [argument_encoder setArgumentBuffer:arguments offset:0];
    constexpr unsigned pixels = 4, guard = 256;
    std::array<id<MTLBuffer>, aw::contribution_evaluation_id + 1> buffers{};
    const std::array<std::size_t, aw::contribution_evaluation_id + 1> lengths{
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
        16};
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
    }
}
// Real null ownership uses only the Enhanced sidecar. Invalid transport pools
// make accidental initialization/load/store observable independently of pixels.
void enhanced_null_owners(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue,
                          const wf::Contract &contract) {
    namespace cw = contribution_wire;
    constexpr unsigned pixels = 7;
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
    auto reset = load(@"rypath_wavefront_reset");
    auto initialize = load(@"rypath_wavefront_initialize");
    auto probe = load(@"rypath_wavefront_null_probe");
    auto prepare = load(@"rypath_wavefront_prepare_refill");
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
    for (unsigned capacity : {2u, 7u}) {
        const wf::Config config{
            pixels,       capacity, 2,   render_wire::pbrt_model, 1,  1,  1,     0,
            cw::enhanced, 0,        1.f, cw::prefix_replay,       11, 13, {0, 0}};
        std::array<id<MTLBuffer>, wf::contribution_state_id + 1> buffers{};
        std::array<std::size_t, wf::contribution_state_id + 1> lengths{};
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
                   id == wf::packet_hot_id || id == wf::packet_cold_id || id == wf::packet_hit_id ||
                   id == wf::hero_state_id || id == wf::hero_cold_id || id == wf::cie_id;
        };
        const auto bind = [&] {
            [argument_encoder setArgumentBuffer:arguments offset:0];
            for (unsigned i = 0; i < buffers.size(); ++i)
                [argument_encoder setBuffer:buffers[i] offset:guard atIndex:i];
        };
        const auto fresh = [&] {
            for (unsigned i = 0; i < buffers.size(); ++i) {
                std::memset(buffers[i].contents, 0xa5, buffers[i].length);
                if (unrelated(i))
                    std::memset(data(i), 0xff, lengths[i]);
            }
            std::memcpy(data(wf::config_id), &config, sizeof(config));
            for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                const auto replay = rypath_replay::fresh({pixel, 17, 19, 23, 29});
                require(rypath_replay::valid(replay), "Host fresh replay receipt");
                std::memcpy(data(wf::replay_input_id) + pixel * sizeof(replay), &replay,
                            sizeof(replay));
                const cw::Evaluation request{cw::enhanced_null_slot, 0xffffffffu, 0.f, 0};
                std::memcpy(data(wf::contribution_evaluation_id) + pixel * sizeof(request),
                            &request, sizeof(request));
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
                [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
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
        const auto guards = [&] {
            for (unsigned i = 0; i < buffers.size(); ++i) {
                const auto *bytes = static_cast<const unsigned char *>(buffers[i].contents);
                for (std::size_t j = 0; j < buffers[i].length; ++j) {
                    if (j >= guard && j < guard + lengths[i]) {
                        if (unrelated(i))
                            require(bytes[j] == 0xff, "Null owner touched ray/BSDF/unused pool");
                    } else
                        require(bytes[j] == 0xa5, "Null owner touched a buffer guard");
                }
            }
            require(std::memcmp(data(wf::config_id), &config, sizeof(config)) == 0,
                    "Null owner changed immutable config");
            for (auto buffer : {tables, environment}) {
                const auto *bytes = static_cast<const unsigned char *>(buffer.contents);
                for (std::size_t j = 0; j < buffer.length; ++j)
                    require(bytes[j] == 0xff, "Null owner modified unused external resource");
            }
            for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                const auto replay = rypath_replay::fresh({pixel, 17, 19, 23, 29});
                require(std::memcmp(data(wf::replay_input_id) + pixel * sizeof(replay), &replay,
                                    sizeof(replay)) == 0,
                        "Null owner modified canonical replay");
            }
        };
        fresh();
        execute({{reset, pixels}, {initialize, capacity}});
        auto before = counters();
        require(before.failure == 0 && before.current == capacity &&
                    before.next_pixel == capacity && before.completed == 0 &&
                    before.null_completed == 0,
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
                        result.free == 0 && result.refill_begin == 0 && result.rounds == rounds + 1,
                    "Null owners must conserve completions through real refill/advance");
            for (unsigned pixel = 0; pixel < pixels; ++pixel) {
                cw::Evaluation evaluation{};
                std::memcpy(&evaluation,
                            data(wf::contribution_evaluation_id) + pixel * sizeof(evaluation),
                            sizeof(evaluation));
                require(evaluation.requested_slot == cw::enhanced_null_slot &&
                            evaluation.selected_slot ==
                                (pixel < completed ? cw::enhanced_null_slot : 0xffffffffu) &&
                            std::bit_cast<std::uint32_t>(evaluation.normalizer) == 0 &&
                            evaluation.ready == unsigned(pixel < completed),
                        "Null result must publish ready exactly once with canonical zero");
                std::uint32_t done;
                std::memcpy(&done, data(wf::done_id) + pixel * sizeof(done), sizeof(done));
                require(done == unsigned(pixel < completed), "Null done ownership mismatch");
                for (std::size_t byte = 0; byte < sizeof(render_wire::Point); ++byte)
                    require(data(wf::raw_xyz_id)[pixel * sizeof(render_wire::Point) + byte] ==
                                (pixel < completed ? 0 : 0xa5),
                            "Only completed null owners may publish raw zero");
            }
            guards();
            std::swap(buffers[wf::queue_in_id], buffers[wf::queue_out_id]);
            bind();
            before = result;
            ++rounds;
        }
        if (capacity != 2)
            continue;
        // Serial single-lane failures avoid a valid sibling racing publication.
        // Every error must preserve zero physical/null completions and raw output.
        for (unsigned fault = 0; fault < 6; ++fault) {
            fresh();
            if (fault == 0 || fault == 5) {
                auto *request =
                    reinterpret_cast<cw::Evaluation *>(data(wf::contribution_evaluation_id));
                if (fault == 0)
                    request[0].ready = 1; // Stale ready rejected before issuing transport.
                else
                    request[0].requested_slot = 1; // D0 has only slot0, plus the null tag.
            }
            execute({{reset, pixels}, {initialize, 1}});
            if (fault != 0 && fault != 5) {
                auto *request =
                    reinterpret_cast<cw::Evaluation *>(data(wf::contribution_evaluation_id));
                if (fault == 1)
                    request[0].requested_slot = 0; // Changed after null owner capture.
                else if (fault == 2)
                    request[0].ready = 1; // A stale result cannot be consumed as pending.
                else if (fault == 3)
                    reinterpret_cast<std::uint32_t *>(data(wf::done_id))[0] = 1;
                else {
                    auto value = counters();
                    value.null_completed = 1; // Isolate null>completed under Enhanced constants.
                    value.out = capacity; // All other advance conservation predicates are valid.
                    std::memcpy(data(wf::counters_id), &value, sizeof(value));
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
                    "Invalid null owner must fail without fabricating completions");
            for (std::size_t byte = 0; byte < lengths[wf::raw_xyz_id]; ++byte)
                require(data(wf::raw_xyz_id)[byte] == 0xa5, "Failed null owner published raw");
            guards();
        }
        for (const auto mode : {std::array{true, false}, std::array{false, true}}) {
            fresh();
            auto mismatch = metal::load_pipeline(device, library, @"rypath_wavefront_reset", why,
                                                 nil, constants_for(mode[0], mode[1]));
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
    std::array<id<MTLBuffer>, wf::contribution_state_id + 1> buffers{};
    [argument_encoder setArgumentBuffer:arguments offset:0];
    for (unsigned i = 0; i < buffers.size(); ++i) {
        buffers[i] = [device newBufferWithLength:i >= wf::contribution_evaluation_id ? 16 : 256
                                         options:MTLResourceStorageModeShared];
        require(buffers[i] != nil, "Queue fault test allocation");
        [argument_encoder setBuffer:buffers[i] offset:0 atIndex:i];
    }
    auto queue = [device newCommandQueue];
    require(queue != nil && arguments != nil, "Queue fault test command resources");
    area_disabled_wire(device, library, queue);
    const wf::Config config{4, 2, 0,     render_wire::pbrt_model, 32, 0, 1, 0, 0, 0, 0.f, 0,
                            0, 0, {0, 0}};
    const std::array<wf::Counters, 9> cases{{
        {2, 2, 0, 4, 2, 0, 7, 2, 0}, // Valid two-owner continuation clears its range.
        {2, 3, 0, 4, 1, 0, 7, 0, 0}, // Output exceeds physical capacity.
        {2, 1, 3, 4, 3, 0, 7, 0, 0}, // Free list exceeds physical capacity.
        {2, 0, 0, 4, 5, 0, 7, 0, 0}, // More completions than pixels.
        {2, 0, 0, 5, 4, 0, 7, 0, 0}, // Allocator exceeds pixel domain.
        {2, 0, 0, 4, 2, 0, 7, 0, 0}, // Lost work cannot become an empty successful queue.
        {2, 2, 0, 4, 2, wf::queue_overflow, 7, 0, 0}, // Producer overflow stays sticky.
        {2, 2, 0, 4, 2, 0, 7, 0, 3},                  // Null completions exceed all completions.
        {2, 2, 0, 4, 2, 0, 7, 0, 1}, // Legacy mode cannot publish null completions.
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
                    result.null_completed == cases[n].null_completed,
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
        {2, 0, 2, 2, 2, 0, 7, 0, 0}, // Full refill owns [2,4).
        {2, 0, 2, 3, 3, 0, 7, 0, 0}, // Partial final range owns [3,4).
        {2, 0, 2, 4, 4, 0, 7, 0, 0}, // Exhausted domain claims nothing.
        {2, 1, 1, 3, 2, 0, 7, 0, 0}, // One continuing owner plus one new owner.
        {2, 2, 0, 2, 0, 0, 7, 0, 0}, // All owners continue; no free slots.
        {0, 0, 0, 4, 4, 0, 7, 0, 0}, // Extra encoded round after all work completed.
        {2, 0, 3, 3, 3, 0, 7, 0, 0}, // Invalid free count.
        {2, 0, 2, 5, 5, 0, 7, 0, 0}, // Invalid next owner.
        {2, 0, 2, 2, 3, 0, 7, 0, 0}, // Completions exceed issued owners.
        {2, 0, 1, 3, 3, 0, 7, 0, 0}, // Lost active owner.
        {2, 1, 1, 3, 3, 0, 7, 0, 0}, // Continuing count disagrees with issued work.
        {2, 0, 2, 2, 2, wf::queue_overflow, 7, 0, 0},
        {2, 0, 2, 2, 2, 0, 7, 0, 3}, // Null count cannot exceed completions.
        {2, 0, 2, 2, 2, 0, 7, 0, 1}, // Legacy null count is invalid.
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
                    result.null_completed == before.null_completed &&
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
        metal::load_pipeline(device, library, @"rypath_wavefront_contract_v16", why);
    require(contract_pipeline != nil,
            "Wavefront v15 contract unavailable; rebuild the metallib: " + why);
    for (NSString *stale in @[
             @"rypath_wavefront_contract_v11", @"rypath_wavefront_contract_v12",
             @"rypath_wavefront_contract_v13", @"rypath_wavefront_contract_v14",
             @"rypath_wavefront_contract_v15"
         ])
        require([library newFunctionWithName:stale] == nil,
                "Stale Wavefront stage entry must not advertise the v15 ABI");
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
                history_contract.config_bytes == 64 && history_contract.counters_bytes == 36 &&
                history_contract.dispatch_bytes == 12 &&
                history_contract.contribution_state_bytes == 80 &&
                history_contract.contribution_state_alignment == 4 &&
                history_contract.enhanced_state_bytes == 104 &&
                history_contract.enhanced_state_alignment == 4,
            "History storage contract mismatch");
    enhanced_null_owners(device, library, queue, history_contract);
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
        std::array<id<MTLBuffer>, wf::contribution_state_id + 1> history_buffers{};
        std::array<std::size_t, wf::contribution_state_id + 1> strides{};
        strides[wf::packet_state_id] = history_contract.packet_bytes;
        strides[wf::packet_hot_id] = history_contract.hot_bytes;
        strides[wf::packet_cold_id] = history_contract.cold_bytes;
        strides[wf::hero_state_id] = history_contract.hero_bytes;
        strides[wf::hero_cold_id] = history_contract.hero_cold_bytes;
        for (unsigned i = 0; i < history_buffers.size(); ++i) {
            const auto bytes = strides[i] ? 2 * guard_bytes + slot_count * strides[i]
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
            7, 7, 2, render_wire::pbrt_model, 1, 0, 1, layout, 0, 0, 0.f, 0, 0, 0, {0, 0}};
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
