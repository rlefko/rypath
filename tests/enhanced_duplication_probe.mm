// SPDX-License-Identifier: GPL-3.0-only
#include "area_wire.h"
#include "enhanced_duplication_oracle.hpp"
#include "enhanced_duplication_wire.h"
#include "metal_execution.hpp"
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
namespace {
using namespace rypath;
namespace wire = enhanced_duplication_wire;
namespace d = rypath_enhanced_duplication;
namespace ref = duplication_oracle;
void need(bool value, const char *why) { ref::require(value, why); }
constexpr unsigned maximum = 17 * 17, guard = 256;
int run(const std::filesystem::path &path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for duplication kernel probe");
    std::string why;
    auto library = metal::load_library(device, path, why);
    need(library != nil, why.c_str());
    auto function = metal::load_function(library, @"rypath_enhanced_duplication_count_v1", why);
    auto count =
        metal::load_pipeline(device, library, @"rypath_enhanced_duplication_count_v1", why);
    auto reset = metal::load_pipeline(device, library, @"rypath_enhanced_duplication_reset", why);
    auto contract_pipeline =
        metal::load_pipeline(device, library, @"rypath_enhanced_duplication_contract_v1", why);
    need(function && count && reset && contract_pipeline, why.c_str());
    auto queue = [device newCommandQueue];
    auto encoder = [function newArgumentEncoderWithBufferIndex:2];
    need(queue && encoder, "Duplication resources");
    auto arguments = [device newBufferWithLength:encoder.encodedLength
                                         options:MTLResourceStorageModeShared];
    const std::array<std::size_t, 7> lengths{sizeof(wire::Config),
                                             maximum * sizeof(d::Label),
                                             maximum * sizeof(d::Label),
                                             maximum * sizeof(d::Label),
                                             maximum * sizeof(std::uint16_t),
                                             sizeof(wire::Counters),
                                             maximum * sizeof(area_wire::History)};
    std::array<id<MTLBuffer>, 7> buffers{};
    [encoder setArgumentBuffer:arguments offset:0];
    for (unsigned id = 0; id < buffers.size(); ++id) {
        buffers[id] = [device newBufferWithLength:lengths[id] + 2 * guard
                                          options:MTLResourceStorageModeShared];
        need(buffers[id] != nil, "Duplication guarded allocation");
        [encoder setBuffer:buffers[id] offset:guard atIndex:id];
    }
    const auto data = [&](unsigned id) {
        return static_cast<unsigned char *>(buffers[id].contents) + guard;
    };
    auto contract_buffer = [device newBufferWithLength:sizeof(wire::Contract) + 2 * guard
                                               options:MTLResourceStorageModeShared];
    need(contract_buffer != nil, "Duplication contract output");
    std::memset(contract_buffer.contents, 0xa5, contract_buffer.length);
    auto cc = [queue commandBuffer];
    auto ce = [cc computeCommandEncoder];
    [ce setComputePipelineState:contract_pipeline];
    [ce setBuffer:contract_buffer offset:guard atIndex:0];
    [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [ce endEncoding];
    need(metal::complete(cc, "Duplication ABI").empty(), "Duplication contract completion");
    wire::Contract contract{};
    std::memcpy(&contract, static_cast<const unsigned char *>(contract_buffer.contents) + guard,
                sizeof(contract));
    const auto *abi_bytes = static_cast<const unsigned char *>(contract_buffer.contents);
    for (unsigned j = 0; j < guard; ++j)
        need(abi_bytes[j] == 0xa5 && abi_bytes[guard + sizeof(contract) + j] == 0xa5,
             "Diagnostic ABI output guards");
    const wire::Contract expected_contract{1, 32, 16, 1180, 2, 291, 8, 8};
    need(std::memcmp(&contract, &expected_contract, sizeof(contract)) == 0,
         "Separate diagnostic ABI leaves WF22/Area16 unchanged");
    const auto dispatch = [&](id<MTLComputePipelineState> pipeline, unsigned width,
                              unsigned height) {
        auto command = [queue commandBuffer];
        auto compute = [command computeCommandEncoder];
        need(command && compute, "Duplication encoder");
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:arguments offset:0 atIndex:2];
        for (auto buffer : buffers)
            [compute useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        if (pipeline == count)
            [compute dispatchThreadgroups:MTLSizeMake((width + 7) / 8, (height + 7) / 8, 1)
                    threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        else
            [compute dispatchThreads:MTLSizeMake(291, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [compute endEncoding];
        need(metal::complete(command, "Duplication tiled count").empty(),
             "Duplication GPU completion");
    };
    const auto check = [&](std::vector<ref::Pixel> pixels, unsigned width, unsigned height,
                           unsigned mode, bool unknown = false) {
        const unsigned N = width * height;
        need(N <= maximum && pixels.size() == N, "Probe extent");
        for (auto buffer : buffers)
            std::memset(buffer.contents, 0xa5, buffer.length);
        const wire::Config config{1, width, height, mode, 0x12345678u, 0x9abcdef0u, 0, 0};
        std::memcpy(data(wire::config_id), &config, sizeof(config));
        auto *labels = reinterpret_cast<d::Label *>(data(wire::history_destination_labels_id));
        auto *history = reinterpret_cast<area_wire::History *>(data(wire::history_destination_id));
        for (unsigned p = 0; p < N; ++p) {
            labels[p] = pixels[p].valid ? d::Label{pixels[p].birth[0], pixels[p].birth[1],
                                                   pixels[p].birth[2], pixels[p].birth[3]}
                                        : d::invalid_label();
            auto &h = history[p];
            h = {};
            h.valid = 1;
            std::memcpy(&h.payload, pixels[p].current.data(), 7 * sizeof(unsigned));
            h.payload.version = rypath_replay::contract;
            h.selected_slot =
                pixels[p].null ? contribution_wire::enhanced_null_slot : pixels[p].current[7];
            // Deliberately different physical tuple data with identical U,j:
            // current equality is not equality of XYZ, h or owned chart records.
            h.xyz = {float(1 + p), float(2 * p), 0};
            h.normalizer = float(1 + p);
            h.target = float(3 + p);
            h.record = {p % 2, p};
            h.confidence = 1 + p % 7;
            h.target_h_fraction = .5f;
            h.target_h_exponent = 1;
        }
        std::vector<unsigned char> frozen_labels(data(wire::history_destination_labels_id),
                                                 data(wire::history_destination_labels_id) +
                                                     lengths[wire::history_destination_labels_id]);
        std::vector<unsigned char> frozen_history(data(wire::history_destination_id),
                                                  data(wire::history_destination_id) +
                                                      lengths[wire::history_destination_id]);
        dispatch(reset, width, height);
        auto *c = reinterpret_cast<wire::Counters *>(data(wire::counters_id));
        c->temporal_completed = N;
        c->final_completed = N;
        dispatch(count, width, height);
        const auto expected = ref::count(pixels, width, height, mode);
        need((c->failure != 0) == unknown,
             "Unknown positive provenance fails; valid diagnostics succeed");
        if (!unknown) {
            need(c->counted_completed == N, "Partial8x8 groups count every center exactly once");
            const auto *map = reinterpret_cast<const std::uint16_t *>(data(wire::count_map_id));
            for (unsigned p = 0; p < N; ++p)
                need(map[p] == expected.counts[p],
                     "Actual GPU map equals independent all-pairs finite coordinate oracle");
            for (unsigned bin = 0; bin < 291; ++bin)
                need(c->histogram[bin] == expected.histogram[bin], "Exact291-bin GPU histogram");
        }
        need(std::memcmp(data(wire::history_destination_labels_id), frozen_labels.data(),
                         frozen_labels.size()) == 0 &&
                 std::memcmp(data(wire::history_destination_id), frozen_history.data(),
                             frozen_history.size()) == 0 &&
                 std::memcmp(data(wire::config_id), &config, sizeof(config)) == 0,
             "Count kernel preserves committed labels, physical history and config");
        for (unsigned id = 0; id < buffers.size(); ++id) {
            const auto *bytes = static_cast<const unsigned char *>(buffers[id].contents);
            for (unsigned j = 0; j < guard; ++j)
                need(bytes[j] == 0xa5 && bytes[guard + lengths[id] + j] == 0xa5,
                     "Duplication allocation guards");
            if (id == wire::history_source_labels_id || id == wire::temporal_labels_id)
                for (unsigned j = 0; j < lengths[id]; ++j)
                    need(data(id)[j] == 0xa5,
                         "Count never reads/writes unrelated provenance scratch/source");
        }
        for (unsigned j = N * sizeof(std::uint16_t); j < lengths[wire::count_map_id]; ++j)
            need(data(wire::count_map_id)[j] == 0xa5,
                 "Odd/partial groups cannot publish map outside extent");
    };
    for (unsigned mode : {d::initial_seed, d::initial_atom, d::current}) {
        for (unsigned pattern = 0; pattern < 4; ++pattern)
            check(ref::fixture(17, 17, pattern), 17, 17, mode);
        check(ref::fixture(19, 11, 3), 19, 11, mode);
        check(ref::fixture(1, 1, 0), 1, 1, mode);
        auto atoms = ref::fixture(17, 17, 0);
        for (unsigned p = 0; p < atoms.size(); ++p) {
            atoms[p].birth[3] = p % 2;
            atoms[p].current[0] = p;
        }
        check(atoms, 17, 17, mode);
        auto high_words = ref::fixture(17, 17, 0);
        for (unsigned p = 0; p < high_words.size(); ++p)
            high_words[p].birth[2] = p % 2;
        check(high_words, 17, 17, mode);
        auto missing = ref::fixture(17, 17, 0);
        for (unsigned p = 0; p < missing.size(); p += 3) {
            missing[p].valid = false;
            missing[p].null = true;
        }
        check(missing, 17, 17, mode);
        for (auto &p : missing) {
            p.valid = false;
            p.null = true;
        }
        check(missing, 17, 17, mode);
        missing[7].null = false;
        check(missing, 17, 17, mode, true);
    }
    check(ref::fixture(1, 1, 0), 1, 1, d::initial_seed);
    // A second count without reset must fail instead of wrapping completion or
    // histogram counters. This is a diagnostic protocol test, not a new sample.
    dispatch(count, 1, 1);
    auto *counters = reinterpret_cast<wire::Counters *>(data(wire::counters_id));
    need((counters->failure & wire::invalid_count) != 0,
         "Duplicate count dispatch fails bounded completion/histogram counters");
    auto *bad_config = reinterpret_cast<wire::Config *>(data(wire::config_id));
    bad_config->reserved0 = 1;
    dispatch(reset, 1, 1);
    dispatch(count, 1, 1);
    need((counters->failure & wire::invalid_config) != 0,
         "Malformed diagnostic config fails without map publication");
    bad_config->reserved0 = 0;
    auto *bad_history = reinterpret_cast<area_wire::History *>(data(wire::history_destination_id));
    bad_history[0].valid = 0;
    dispatch(reset, 1, 1);
    dispatch(count, 1, 1);
    need((counters->failure & wire::invalid_payload) != 0,
         "Malformed positive history is not silently counted as null");
    bad_history[0].valid = 1;
    bad_history[0].payload.version = 0;
    dispatch(reset, 1, 1);
    dispatch(count, 1, 1);
    need((counters->failure & wire::invalid_provenance) != 0,
         "Current key omits only a separately validated replay version");
    bad_history[0].payload.version = rypath_replay::contract;
    bad_history[0].selected_slot = contribution_wire::enhanced_null_slot;
    auto *bad_labels = reinterpret_cast<d::Label *>(data(wire::history_destination_labels_id));
    bad_labels[0] = {0xffffffffu, 1, 0, 0};
    dispatch(reset, 1, 1);
    dispatch(count, 1, 1);
    need((counters->failure & wire::invalid_provenance) != 0,
         "Null ancestry requires canonical missing words");
    std::cout << "Actual tiled duplication count/histogram/identity/guard controls passed\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Expected production metallib");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
