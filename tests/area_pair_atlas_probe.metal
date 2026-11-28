// SPDX-License-Identifier: GPL-3.0-only
#include <metal_stdlib>
using namespace metal;
#include "../src/area_pair_atlas_shared.h"
namespace atlas = rypath_area_pair_atlas;
struct Query {
    uint pixel, width, height, candidate, canonical;
    atlas::Tile tile;
    atlas::Pose pose;
    uint entry_count;
};
struct Result {
    uint pixel, status, reverse_pixel, reverse_status;
};
kernel void rypath_area_pair_atlas_probe(device const Query *queries [[buffer(0)]],
                                         device const atlas::Entry *entries [[buffer(1)]],
                                         device const atlas::Config *config [[buffer(2)]],
                                         device Result *results [[buffer(3)]],
                                         constant uint &count [[buffer(4)]],
                                         uint index [[thread_position_in_grid]]) {
    if (index >= count)
        return;
    const auto q = queries[index];
    const auto c = *config;
    const auto value = q.canonical
                           ? atlas::neighbor(c, entries, q.pixel, q.width, q.height, q.candidate)
                           : atlas::neighbor_tile(q.tile, q.pose, q.entry_count, entries, q.pixel,
                                                  q.width, q.height);
    auto reverse = value;
    if (value.status == atlas::Status::ok)
        reverse = q.canonical
                      ? atlas::neighbor(c, entries, value.pixel, q.width, q.height, q.candidate)
                      : atlas::neighbor_tile(q.tile, q.pose, q.entry_count, entries, value.pixel,
                                             q.width, q.height);
    results[index] = {value.pixel, uint(value.status), reverse.pixel, uint(reverse.status)};
}
