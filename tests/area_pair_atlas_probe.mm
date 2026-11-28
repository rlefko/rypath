// SPDX-License-Identifier: GPL-3.0-only
#include "area_pair_atlas.hpp"
#include "area_pair_atlas_oracle.hpp"
#include "metal_execution.hpp"
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
using namespace rypath;
namespace a = rypath_area_pair_atlas;
namespace ref = atlas_oracle;
struct Query {
    unsigned pixel, width, height, candidate, canonical;
    a::Tile tile;
    a::Pose pose;
    unsigned entry_count;
};
struct Result {
    unsigned pixel, status, reverse_pixel, reverse_status;
};
void need(bool value, const char *why) { ref::require(value, why); }
std::vector<a::Entry> half_period_tile(unsigned L, bool vertical) {
    std::vector<a::Entry> tile(L * L);
    for (unsigned y = 0; y < L; ++y)
        for (unsigned x = 0; x < L; ++x) {
            const int delta = (vertical ? y : x) < L / 2 ? int(L / 2) : -int(L / 2);
            tile[y * L + x] = vertical ? ref::pack(0, delta) : ref::pack(delta, 0);
        }
    return tile;
}
int run(const std::filesystem::path &path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for atlas integer probe");
    std::string why;
    auto library = metal::load_library(device, path, why);
    need(library != nil, why.c_str());
    auto pipeline = metal::load_pipeline(device, library, @"rypath_area_pair_atlas_probe", why);
    need(pipeline != nil, why.c_str());
    auto asset = area_pair_atlas::generate();
    need(bool(asset), asset.diagnostic.c_str());
    auto config = a::base_config();
    // Production bytes stay unchanged; append separately addressed immutable
    // half-period fixtures for generic neighbor_tile queries only.
    auto packed_entries = asset.entries;
    std::vector<Query> queries;
    std::vector<unsigned> expected;
    const auto wide_queries = [&](const std::vector<a::Entry> &tile, a::Tile descriptor,
                                  unsigned candidate) {
        const unsigned L = descriptor.width;
        for (const auto dims :
             {std::array<unsigned, 2>{0xffffffffu, 1}, {1, 0xffffffffu}, {65535, 65537}})
            for (unsigned orientation = 0; orientation < 8; ++orientation)
                for (const auto offsets :
                     {std::array<unsigned, 2>{0, 0}, {L - 1, L - 1}, {L / 2, 1}, {1, L / 2}})
                    for (unsigned pixel : {0u, 1u, L - 1, L, L + 1, 0x7ffffffeu, 0x7fffffffu,
                                           0x80000000u, 0x80000001u, 0xfffffffdu, 0xfffffffeu}) {
                        const a::Pose pose{orientation, offsets[0], offsets[1], 0};
                        queries.push_back({pixel, dims[0], dims[1], candidate, 0, descriptor, pose,
                                           unsigned(packed_entries.size())});
                        expected.push_back(ref::neighbor(tile, L, dims[0], dims[1], pixel,
                                                         orientation, offsets[0], offsets[1]));
                    }
    };
    for (unsigned k = 0; k < 4; ++k) {
        const unsigned L = a::tile_width(k), start = config.tiles[k].first_entry;
        const std::vector<a::Entry> tile(asset.entries.begin() + start,
                                         asset.entries.begin() + start + L * L);
        for (unsigned orientation = 0; orientation < 8; ++orientation)
            for (unsigned offset : {0u, L - 1})
                for (unsigned n = 0; n < 128; ++n) {
                    const unsigned width = L + 3, height = L + 1,
                                   pixel = (n * 7919u) % (width * height);
                    const a::Pose pose{orientation, offset, (offset + 17) % L, 0};
                    queries.push_back(
                        {pixel, width, height, k + 1, 0, config.tiles[k], pose, a::total_entries});
                    expected.push_back(ref::neighbor(tile, L, width, height, pixel, orientation,
                                                     pose.offset_x, pose.offset_y));
                }
        for (unsigned pixel : {0u, 1u, 127u, 32767u}) {
            const unsigned width = 257, height = 129, p = pixel % (width * height);
            queries.push_back({p, width, height, k + 1, 1, {}, {}, a::total_entries});
            expected.push_back(ref::neighbor(tile, L, width, height, p, 0, 0, 0));
        }
        wide_queries(tile, config.tiles[k], k + 1);
        for (bool vertical : {false, true}) {
            const auto fixture = half_period_tile(L, vertical);
            const a::Tile descriptor{unsigned(packed_entries.size()), L};
            packed_entries.insert(packed_entries.end(), fixture.begin(), fixture.end());
            wide_queries(fixture, descriptor, k + 1);
            if (!vertical) {
                constexpr unsigned pixel = 0x80000000u;
                const auto correct = ref::neighbor(fixture, L, 0xffffffffu, 1, pixel, 0, 0, 0);
                const long long wrong_signed = static_cast<long long>(pixel) - (1ll << 32);
                const auto delta = ref::decode(fixture[unsigned(ref::modulo(wrong_signed, L))]);
                const auto wrong = unsigned(static_cast<long long>(pixel) + delta[0]);
                need(correct != wrong,
                     "GPU high-coordinate fixture distinguishes wrong signed32 reduction");
                queries.push_back({pixel,
                                   0xffffffffu,
                                   1,
                                   k + 1,
                                   0,
                                   descriptor,
                                   {0, 0, 0, 0},
                                   unsigned(packed_entries.size())});
                expected.push_back(correct);
            }
        }
    }
    constexpr unsigned guard = 256;
    auto input = [device newBufferWithBytes:queries.data()
                                     length:queries.size() * sizeof(Query)
                                    options:MTLResourceStorageModeShared];
    auto entries = [device newBufferWithLength:packed_entries.size() * sizeof(a::Entry) + 2 * guard
                                       options:MTLResourceStorageModeShared];
    auto packed = [device newBufferWithLength:sizeof(config) + 2 * guard
                                      options:MTLResourceStorageModeShared];
    auto output = [device newBufferWithLength:queries.size() * sizeof(Result) + 2 * guard
                                      options:MTLResourceStorageModeShared];
    need(input && entries && packed && output, "Atlas probe buffers");
    std::memset(entries.contents, 0xa5, entries.length);
    std::memset(packed.contents, 0xa5, packed.length);
    auto *entry_bytes = static_cast<unsigned char *>(entries.contents) + guard;
    auto *config_bytes = static_cast<unsigned char *>(packed.contents) + guard;
    std::memcpy(entry_bytes, packed_entries.data(), packed_entries.size() * sizeof(a::Entry));
    std::memcpy(config_bytes, &config, sizeof(config));
    auto queue = [device newCommandQueue];
    const auto dispatch = [&](unsigned count) {
        std::memset(output.contents, 0xa5, output.length);
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        need(command && encoder, "Atlas probe encoder");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:entries offset:guard atIndex:1];
        [encoder setBuffer:packed offset:guard atIndex:2];
        [encoder setBuffer:output offset:guard atIndex:3];
        [encoder setBytes:&count length:sizeof(count) atIndex:4];
        [encoder dispatchThreads:MTLSizeMake(count + 3, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        need(metal::complete(command, "Atlas integer lookup").empty(), "Atlas GPU completion");
        for (auto buffer : {entries, packed, output}) {
            const auto *bytes = static_cast<const unsigned char *>(buffer.contents);
            for (unsigned j = 0; j < guard; ++j)
                need(bytes[j] == 0xa5 && bytes[buffer.length - guard + j] == 0xa5,
                     "Atlas GPU guards");
        }
    };
    dispatch(unsigned(queries.size()));
    const auto *results = reinterpret_cast<const Result *>(
        static_cast<const unsigned char *>(output.contents) + guard);
    for (unsigned i = 0; i < queries.size(); ++i)
        need(results[i].status == unsigned(a::Status::ok) && results[i].pixel == expected[i] &&
                 results[i].reverse_status == unsigned(a::Status::ok) &&
                 results[i].reverse_pixel == queries[i].pixel,
             "GPU lookup equals independent integer endpoint oracle and reverses exactly");
    need(std::memcmp(entry_bytes, packed_entries.data(),
                     packed_entries.size() * sizeof(a::Entry)) == 0 &&
             std::memcmp(config_bytes, &config, sizeof(config)) == 0 &&
             std::memcmp(input.contents, queries.data(), queries.size() * sizeof(Query)) == 0,
         "GPU lookup preserves immutable atlas, config and queries");
    for (unsigned fault = 0; fault < 7; ++fault) {
        Query q{0, 17, 3, 1, 0, config.tiles[0], {0, 0, 0, 0}, a::total_entries};
        auto c = config;
        auto status = a::Status::invalid_config;
        if (fault == 0)
            q.pose.transform = 8;
        else if (fault == 1)
            q.tile.width = 3;
        else if (fault == 2) {
            q.entry_count = 1;
        } else if (fault == 3) {
            q.pixel = 51;
            status = a::Status::invalid_coordinate;
        } else if (fault == 4) {
            q.canonical = 1;
            c.poses[0].reserved = 1;
        } else if (fault == 5) {
            reinterpret_cast<a::Entry *>(entry_bytes)[0] = 0;
            status = a::Status::invalid_entry;
        } else {
            reinterpret_cast<a::Entry *>(entry_bytes)[0] = ref::pack(1, 0);
            reinterpret_cast<a::Entry *>(entry_bytes)[1] = ref::pack(1, 0);
            status = a::Status::invalid_entry;
        }
        std::memcpy(input.contents, &q, sizeof(q));
        std::memcpy(config_bytes, &c, sizeof(c));
        dispatch(1);
        need(results[0].status == unsigned(status),
             "GPU malformed descriptor/pose/coordinate/entry rejected");
    }
    // Candidate0 must not read even a deliberately nonreciprocal poisoned asset.
    const Query center{0, 17, 3, 0, 1, {}, {}, a::total_entries};
    std::memcpy(input.contents, &center, sizeof(center));
    std::memcpy(config_bytes, &config, sizeof(config));
    std::memset(entry_bytes, 0xff, packed_entries.size() * sizeof(a::Entry));
    dispatch(1);
    need(results[0].status == unsigned(a::Status::ok) && results[0].pixel == 0,
         "Center candidate never dereferences atlas entries");
    for (unsigned i = 0; i < packed_entries.size() * sizeof(a::Entry); ++i)
        need(entry_bytes[i] == 0xff, "Center candidate preserves poisoned inactive atlas");
    std::cout << queries.size() << " paired atlas CPU/GPU exact lookup controls passed\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Expected probe metallib");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
