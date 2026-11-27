// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Optional diagnostic ABI only; no production Area/WF layouts are extended.
#include "../src/area_wire.h"
#include "../src/enhanced_duplication_wire.h"
namespace dupw = rypath::enhanced_duplication_wire;
namespace dups = rypath_enhanced_duplication;
struct DuplicationArguments {
    device const dupw::Config *config [[id(dupw::config_id)]];
    device const dups::Label *history_source_labels [[id(dupw::history_source_labels_id)]];
    device dups::Label *history_destination_labels [[id(dupw::history_destination_labels_id)]];
    device dups::Label *temporal_labels [[id(dupw::temporal_labels_id)]];
    device ushort *count_map [[id(dupw::count_map_id)]];
    device dupw::Counters *counters [[id(dupw::counters_id)]];
    device const rypath::area_wire::History *history_destination
        [[id(dupw::history_destination_id)]];
};
inline bool duplication_fail(constant DuplicationArguments &d, uint flag) {
    atomic_fetch_or_explicit(&d.counters->failure, flag, memory_order_relaxed);
    return false;
}
inline bool duplication_config(constant DuplicationArguments &d,
                               const thread rypath::area_wire::Config &a,
                               constant wire::Parameters &p) {
    const auto c = *d.config;
    if (!dupw::valid_config(c) || c.width != a.width || c.height != a.height ||
        c.width != p.width || c.height != p.height || c.width * c.height != a.pixel_count ||
        c.serial_low != a.serial_low || c.serial_high != a.serial_high ||
        a.contribution_mode != rypath::contribution_wire::enhanced ||
        d.history_source_labels == d.history_destination_labels ||
        d.history_source_labels == d.temporal_labels ||
        d.history_destination_labels == d.temporal_labels)
        return duplication_fail(d, dupw::invalid_config);
    return true;
}
inline bool duplication_completed(constant DuplicationArguments &d, device atomic_uint &counter,
                                  uint count = 1u) {
    const uint n = d.config->width * d.config->height;
    const uint old = atomic_fetch_add_explicit(&counter, count, memory_order_relaxed);
    return (count <= n && old <= n - count) || duplication_fail(d, dupw::invalid_count);
}
inline bool duplication_temporal(constant DuplicationArguments &d,
                                 const thread rypath::area_wire::Config &a,
                                 constant wire::Parameters &p, uint pixel, uint source_pixel,
                                 const thread rypath::area_wire::Accumulator &winner) {
    if (!duplication_config(d, a, p))
        return false;
    dups::Label label = dups::invalid_label();
    if (winner.selected_slot != rypath::contribution_wire::enhanced_null_slot) {
        if (winner.selected_slot >= 2u * a.contribution_depth + 1u)
            return duplication_fail(d, dupw::invalid_payload);
        if (winner.record.bank == rypath::enhanced_wire::fresh_bank &&
            winner.record.pixel == pixel) {
            label = {pixel, p.sample_low, p.sample_high, winner.selected_slot};
        } else if (a.history_used && winner.record.bank == a.history_source_bank &&
                   winner.record.pixel == source_pixel && source_pixel < a.pixel_count) {
            label = d.history_source_labels[source_pixel];
            if (!dups::valid_label(label, a.pixel_count) ||
                label.initial_j >= 2u * a.contribution_depth + 1u)
                return duplication_fail(d, dupw::invalid_provenance);
        } else {
            return duplication_fail(d, dupw::invalid_provenance);
        }
    }
    d.temporal_labels[pixel] = label;
    return duplication_completed(d, d.counters->temporal_completed);
}
inline bool duplication_final(constant DuplicationArguments &d,
                              const thread rypath::area_wire::Config &a,
                              constant wire::Parameters &p, uint pixel,
                              const thread rypath::area_wire::Accumulator &winner) {
    if (!duplication_config(d, a, p))
        return false;
    dups::Label label = dups::invalid_label();
    if (winner.selected_slot != rypath::contribution_wire::enhanced_null_slot) {
        if (winner.selected_slot >= 2u * a.contribution_depth + 1u)
            return duplication_fail(d, dupw::invalid_payload);
        if (winner.record.bank != rypath::enhanced_wire::temporal_bank ||
            winner.record.pixel >= a.pixel_count)
            return duplication_fail(d, dupw::invalid_provenance);
        label = d.temporal_labels[winner.record.pixel];
        if (!dups::valid_label(label, a.pixel_count) ||
            label.initial_j >= 2u * a.contribution_depth + 1u)
            return duplication_fail(d, dupw::invalid_provenance);
    }
    d.history_destination_labels[pixel] = label;
    return duplication_completed(d, d.counters->final_completed);
}
kernel void rypath_enhanced_duplication_contract_v1(device dupw::Contract *out [[buffer(0)]]) {
    *out = {dupw::contract,
            sizeof(dupw::Config),
            sizeof(dups::Label),
            sizeof(dupw::Counters),
            sizeof(ushort),
            dups::histogram_bins,
            8,
            8};
}
kernel void rypath_enhanced_duplication_reset(constant DuplicationArguments &d [[buffer(2)]],
                                              uint lane [[thread_position_in_grid]]) {
    if (lane == 0) {
        atomic_store_explicit(&d.counters->failure, 0u, memory_order_relaxed);
        atomic_store_explicit(&d.counters->temporal_completed, 0u, memory_order_relaxed);
        atomic_store_explicit(&d.counters->final_completed, 0u, memory_order_relaxed);
        atomic_store_explicit(&d.counters->counted_completed, 0u, memory_order_relaxed);
    }
    if (lane < dups::histogram_bins)
        atomic_store_explicit(&d.counters->histogram[lane], 0u, memory_order_relaxed);
}
// 0=null, 1=valid comparison key, 2=unknown-positive protocol failure.
inline uint duplication_load_key(constant DuplicationArguments &d, uint pixel,
                                 thread dups::Key &key) {
    const auto h = d.history_destination[pixel];
    const auto label = d.history_destination_labels[pixel];
    const uint n = d.config->width * d.config->height;
    if (h.valid != 1 || h.selected_slot == rypath_reservoir::no_selection) {
        duplication_fail(d, dupw::invalid_payload);
        return 2;
    }
    if (h.selected_slot == rypath::contribution_wire::enhanced_null_slot) {
        if (!dups::canonical_invalid(label))
            duplication_fail(d, dupw::invalid_provenance);
        return 0;
    }
    if (!dups::valid_label(label, n) || !rypath_replay::valid(h.payload)) {
        duplication_fail(d, dupw::invalid_provenance);
        return 2;
    }
    key = d.config->mode == dups::current ? dups::current_key(h.payload, h.selected_slot)
                                          : dups::label_key(label);
    return 1;
}
kernel void rypath_enhanced_duplication_count_v1(constant DuplicationArguments &d [[buffer(2)]],
                                                 uint2 group [[threadgroup_position_in_grid]],
                                                 uint2 lane [[thread_position_in_threadgroup]],
                                                 uint2 shape [[threads_per_threadgroup]]) {
    const auto c = *d.config;
    if (!dupw::valid_config(c) || shape.x != 8 || shape.y != 8) {
        if (lane.x == 0 && lane.y == 0)
            duplication_fail(d, dupw::invalid_config);
        return;
    }
    // 576*(32+4)+291*4=21900B. All halo loads are finite-image addressed.
    threadgroup dups::Key keys[576];
    threadgroup uint valid[576];
    threadgroup atomic_uint histogram[dups::histogram_bins];
    const uint tid = lane.y * 8 + lane.x;
    for (uint i = tid; i < dups::histogram_bins; i += 64)
        atomic_store_explicit(&histogram[i], 0u, memory_order_relaxed);
    for (uint i = tid; i < 576; i += 64) {
        const long x = long(group.x) * 8 + long(i % 24) - 8;
        const long y = long(group.y) * 8 + long(i / 24) - 8;
        dups::Key key{};
        uint state = 0;
        if (x >= 0 && y >= 0 && x < long(c.width) && y < long(c.height))
            state = duplication_load_key(d, uint(y) * c.width + uint(x), key);
        keys[i] = key;
        valid[i] = state;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const ulong x = ulong(group.x) * 8 + lane.x;
    const ulong y = ulong(group.y) * 8 + lane.y;
    if (x < c.width && y < c.height) {
        const uint pixel = uint(y) * c.width + uint(x);
        const uint center = (lane.y + 8) * 24 + lane.x + 8;
        uint bin = valid[center] == 2 ? dups::unknown_bin : dups::null_bin;
        ushort count = ushort(dups::missing);
        if (valid[center] == 1) {
            uint matches = 0;
            const auto key = keys[center];
            for (uint dy = 0; dy < 17; ++dy)
                for (uint dx = 0; dx < 17; ++dx) {
                    if (dx == 8 && dy == 8)
                        continue;
                    const uint index = (lane.y + dy) * 24 + lane.x + dx;
                    if (valid[index] == 1 && dups::equal_key(key, keys[index], c.mode))
                        ++matches;
                }
            count = ushort(matches);
            bin = matches;
        }
        d.count_map[pixel] = count;
        atomic_fetch_add_explicit(&histogram[bin], 1u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const uint n = c.width * c.height;
    if (tid == 0) {
        const ulong x0 = ulong(group.x) * 8;
        const ulong y0 = ulong(group.y) * 8;
        const uint nx = x0 < c.width ? uint(min(ulong(8), ulong(c.width) - x0)) : 0u;
        const uint ny = y0 < c.height ? uint(min(ulong(8), ulong(c.height) - y0)) : 0u;
        duplication_completed(d, d.counters->counted_completed, nx * ny);
    }
    for (uint i = tid; i < dups::histogram_bins; i += 64) {
        const uint count = atomic_load_explicit(&histogram[i], memory_order_relaxed);
        if (count == 0)
            continue;
        const uint old =
            atomic_fetch_add_explicit(&d.counters->histogram[i], count, memory_order_relaxed);
        if (count > n || old > n - count)
            duplication_fail(d, dupw::invalid_count);
    }
}
