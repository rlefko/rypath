// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table.hpp"
#include "bsdf_table_fixture.hpp"
#include "metal_execution.hpp"
#include "rgb_spectrum.hpp"
#include "wavefront_wire.h"

#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace rypath;
namespace wf = wavefront_wire;
void require(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
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
    std::array<id<MTLBuffer>, wf::hero_cold_id + 1> buffers{};
    [argument_encoder setArgumentBuffer:arguments offset:0];
    for (unsigned i = 0; i < buffers.size(); ++i) {
        buffers[i] = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        require(buffers[i] != nil, "Queue fault test allocation");
        [argument_encoder setBuffer:buffers[i] offset:0 atIndex:i];
    }
    auto queue = [device newCommandQueue];
    require(queue != nil && arguments != nil, "Queue fault test command resources");
    const wf::Config config{4, 2, 0, render_wire::pbrt_model, 32, 0, 1, 0};
    const std::array<wf::Counters, 7> cases{{
        {2, 2, 0, 4, 2, 0, 7, 2}, // Valid two-owner continuation clears its range.
        {2, 3, 0, 4, 1, 0, 7, 0}, // Output exceeds physical capacity.
        {2, 1, 3, 4, 3, 0, 7, 0}, // Free list exceeds physical capacity.
        {2, 0, 0, 4, 5, 0, 7, 0}, // More completions than pixels.
        {2, 0, 0, 5, 4, 0, 7, 0}, // Allocator exceeds pixel domain.
        {2, 0, 0, 4, 2, 0, 7, 0}, // Lost work cannot become an empty successful queue.
        {2, 2, 0, 4, 2, wf::queue_overflow, 7, 0}, // Producer overflow stays sticky.
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
                    result.next_pixel == cases[n].next_pixel,
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
    const std::array<wf::Counters, 12> refill_cases{{
        {2, 0, 2, 2, 2, 0, 7, 0}, // Full refill owns [2,4).
        {2, 0, 2, 3, 3, 0, 7, 0}, // Partial final range owns [3,4).
        {2, 0, 2, 4, 4, 0, 7, 0}, // Exhausted domain claims nothing.
        {2, 1, 1, 3, 2, 0, 7, 0}, // One continuing owner plus one new owner.
        {2, 2, 0, 2, 0, 0, 7, 0}, // All owners continue; no free slots.
        {0, 0, 0, 4, 4, 0, 7, 0}, // Extra encoded round after all work completed.
        {2, 0, 3, 3, 3, 0, 7, 0}, // Invalid free count.
        {2, 0, 2, 5, 5, 0, 7, 0}, // Invalid next owner.
        {2, 0, 2, 2, 3, 0, 7, 0}, // Completions exceed issued owners.
        {2, 0, 1, 3, 3, 0, 7, 0}, // Lost active owner.
        {2, 1, 1, 3, 3, 0, 7, 0}, // Continuing count disagrees with issued work.
        {2, 0, 2, 2, 2, wf::queue_overflow, 7, 0},
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
        [constants setConstantValue:&compiled[0]
                               type:MTLDataTypeUInt
                            atIndex:wf::packet_layout_constant];
        [constants setConstantValue:&compiled[1]
                               type:MTLDataTypeUInt
                            atIndex:wf::packet_events_constant];
        auto reset =
            metal::load_pipeline(device, library, @"rypath_wavefront_reset", why, nil, constants);
        require(reset != nil, why);
        for (const auto &requested : policies) {
            for (auto buffer : buffers)
                std::memset(buffer.contents, 0xa5, buffer.length);
            auto cfg = config;
            cfg.packet = 1;
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
                const auto written = i == wf::counters_id ? sizeof(wf::Counters)
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
    auto contract_pipeline =
        metal::load_pipeline(device, library, @"rypath_wavefront_contract_v10", why);
    require(contract_pipeline != nil, why);
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
                history_contract.hero_cold_bytes && history_contract.hot_bytes,
            "History storage contract mismatch");
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
        std::array<id<MTLBuffer>, wf::hero_cold_id + 1> history_buffers{};
        std::array<std::size_t, wf::hero_cold_id + 1> strides{};
        strides[wf::packet_state_id] = history_contract.packet_bytes;
        strides[wf::packet_hot_id] = history_contract.hot_bytes;
        strides[wf::packet_cold_id] = history_contract.cold_bytes;
        strides[wf::hero_state_id] = history_contract.hero_bytes;
        strides[wf::hero_cold_id] = history_contract.hero_cold_bytes;
        for (unsigned i = 0; i < history_buffers.size(); ++i) {
            const auto bytes = strides[i] ? 2 * guard_bytes + slot_count * strides[i] : guard_bytes;
            history_buffers[i] = [device newBufferWithLength:bytes
                                                     options:MTLResourceStorageModeShared];
            require(history_buffers[i] != nil, "Guarded history pool allocation");
            std::memset(history_buffers[i].contents, 0xa5, bytes);
            [history_arguments_encoder setBuffer:history_buffers[i]
                                          offset:strides[i] ? guard_bytes : 0
                                         atIndex:i];
        }
        const wf::Config history_config{7, 7, 2, render_wire::pbrt_model, 1, 0, 1, layout};
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
