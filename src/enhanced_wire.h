// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"

namespace rypath {
namespace enhanced_wire {
using Word = render_wire::Word;
enum : Word {
    record_version = 1,
    invalid = 0xffffffffu,
    history_bank_0 = 0,
    history_bank_1 = 1,
    fresh_bank = 2,
    temporal_bank = 3,
    bank_count = 4
};
enum LightTag : Word {
    generic = 0,
    triangle_area = 1,
    positional_mass = 2,
    distant_mass = 3,
    environment_omega = 4
};
// The owned endpoint is a coordinate in area, counting, or solid-angle measure.
// It is not a cached source radiance or a replacement random seed. Old random
// words at this terminal sampling site are inert implementation bits.
struct Record {
    Word version, tag, light_id, event_ordinal;
    Word instance, primitive, kind, reserved;
    Word coordinate_bits[4];
};
struct Reference {
    Word bank, pixel;
};
// Shared by Area and wavefront for one evaluation. Prepare publishes references
// and clears ready; transport publishes the destination target's scale then
// ready=1. The source record is immutable throughout this leaf. Fresh/mutation
// may write the distinct result record; ordinary imports retain source identity.
struct Evaluation {
    Reference source, result;
    float target_h_fraction;
    int target_h_exponent;
    Word ready, reserved;
};
inline Reference absent() { return {invalid, invalid}; }
inline bool absent(Reference r) { return r.bank == invalid && r.pixel == invalid; }
inline bool valid(Reference r, Word pixels) { return r.bank < bank_count && r.pixel < pixels; }
inline Record empty_record() {
    return {record_version, generic, invalid, invalid, invalid, invalid, invalid, 0, {0, 0, 0, 0}};
}
static_assert(sizeof(Record) == 48 && alignof(Record) == 4 && sizeof(Reference) == 8 &&
                  sizeof(Evaluation) == 32 && alignof(Evaluation) == 4,
              "Enhanced owned-light record ABI");
} // namespace enhanced_wire
} // namespace rypath
