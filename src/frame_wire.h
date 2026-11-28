// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
namespace rypath {
namespace frame_wire {
using Word = render_wire::Word;
enum : Word { contract = 2 };
struct Parameters {
    Word count, pixel_begin, sample_low, sample_high;
    Word stride, packet, model, scratch_bytes;
};
struct Status {
    Word failure, completed;
};
static_assert(sizeof(Parameters) == 32 && sizeof(Status) == 8, "Frame ABI");
} // namespace frame_wire
} // namespace rypath
