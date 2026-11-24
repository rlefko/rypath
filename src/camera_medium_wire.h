// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"
namespace rypath {
namespace camera_medium_wire {
using Word = render_wire::Word;
enum : Word { contract = 3, maximum_crossings = 4096 };
enum class Status : Word {
    success,
    invalid_config,
    invalid_hit,
    invalid_geometry,
    invalid_medium,
    inconsistent_side,
    unresolved_boundary,
    unresolved_endpoint,
    boundary_exhaustion,
    unrepresentable_segment,
    unsupported_interface
};
struct Config {
    Word version, crossing_limit, reserved[2];
    float destination[3], padding;
};
struct Result {
    Status status;
    Word medium, crossings, last_instance;
    Word last_primitive, last_kind, reserved[2];
};
struct Contract {
    Word version, config_bytes, result_bytes, maximum;
    Word reserved[4];
};
enum Argument : Word { config_id, result_id };
static_assert(sizeof(Config) == 32 && sizeof(Result) == 32 && sizeof(Contract) == 32,
              "Camera medium query ABI");
} // namespace camera_medium_wire
} // namespace rypath
