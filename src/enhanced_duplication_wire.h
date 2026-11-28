// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "enhanced_duplication_shared.h"
namespace rypath::enhanced_duplication_wire {
namespace shared = rypath_enhanced_duplication;
using Word = shared::Word;
using Label = shared::Label;
enum : Word { contract = 1 };
struct Config {
    Word version, width, height, mode, serial_low, serial_high, reserved0, reserved1;
};
#if defined(__METAL_VERSION__)
using Counter = metal::atomic_uint;
#else
using Counter = Word;
#endif
struct Counters {
    Counter failure, temporal_completed, final_completed, counted_completed;
    Counter histogram[shared::histogram_bins];
};
enum Failure : Word {
    invalid_config = 1,
    invalid_provenance = 2,
    invalid_count = 4,
    invalid_payload = 8
};
enum Argument : Word {
    config_id,
    history_source_labels_id,
    history_destination_labels_id,
    temporal_labels_id,
    count_map_id,
    counters_id,
    history_destination_id
};
struct Contract {
    Word version, config_bytes, label_bytes, counters_bytes, count_bytes, histogram_bins,
        tile_width, tile_height;
};
inline bool valid_config(Config c) {
    return c.version == contract && c.width && c.height && c.width <= 0xffffffffu / c.height &&
           c.mode >= shared::initial_seed && c.mode <= shared::current && !c.reserved0 &&
           !c.reserved1;
}
static_assert(sizeof(Config) == 32 && sizeof(Counters) == 1180 && sizeof(Contract) == 32 &&
                  alignof(Config) == 4 && __builtin_offsetof(Counters, histogram) == 16,
              "Optional duplication diagnostic v1 ABI");
} // namespace rypath::enhanced_duplication_wire
