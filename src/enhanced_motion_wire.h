// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "enhanced_motion_shared.h"
#include "render_wire.h"
namespace rypath::enhanced_motion_wire {
using Word = render_wire::Word;
namespace motion = rypath_enhanced_motion;
enum : Word { contract = 1, absent = 0xffffffffu };
enum GeometryStatus : Word { empty = 0, finite = 1, sky = 2, unsupported = 3 };
// Immutable representative geometry, independently of physical replay U,j.
// Material is the ROOT instance material, not a resolved mix-material leaf.
struct Geometry {
    Word status, instance, primitive, primitive_kind;
    float distance, uv_x, uv_y;
    Word material;
    render_wire::Point camera_relative_position;
    float pixel_footprint;
    render_wire::Point geometric_normal;
    Word group;
};
struct Donor {
    Word source_pixel, classification, serial_low, serial_high;
};
inline bool valid_donor(Donor donor, Word pixels, Word serial_low, Word serial_high) {
    return donor.source_pixel < pixels &&
           donor.classification >= Word(motion::Classification::ordinary) &&
           donor.classification <= Word(motion::Classification::same_pixel_fallback) &&
           donor.serial_low == serial_low && donor.serial_high == serial_high;
}
struct Config {
    Word version, width, height, history_valid;
    motion::Camera current_camera, previous_camera;
    // Both cameras project from the CURRENT origin. Old Geometry positions are
    // relative to their own old origin; add this host-double-formed difference.
    motion::Vec3 previous_origin_to_common;
    motion::Policy policy;
    Word serial_low, serial_high, reserved;
};
#if defined(__METAL_VERSION__)
using Counter = metal::atomic_uint;
#else
using Counter = Word;
#endif
struct Counters {
    Counter failure, geometry_completed, geometry_hits, geometry_sky;
    Counter geometry_unsupported, donor_completed, cold, ordinary, dual, fallback;
    Counter reserved0, reserved1;
};
enum Failure : Word {
    invalid_config = 1u,
    invalid_geometry = 2u,
    invalid_projection = 4u,
    invalid_count = 8u
};
enum Argument : Word {
    config_id,
    current_geometry_id,
    previous_geometry_id,
    donors_id,
    counters_id
};
struct Contract {
    Word version, config_bytes, geometry_bytes, donor_bytes, counters_bytes, camera_bytes;
};
static_assert(sizeof(Geometry) == 64 && alignof(Geometry) == 4 && sizeof(Donor) == 16 &&
                  alignof(Donor) == 4 && sizeof(Config) == 352 && alignof(Config) == 4 &&
                  sizeof(Counters) == 48 && sizeof(motion::Camera) == 148 && sizeof(Contract) == 24,
              "Enhanced camera-only motion host/shader ABI");
static_assert(__builtin_offsetof(Geometry, material) == 28 &&
                  __builtin_offsetof(Geometry, pixel_footprint) == 44 &&
                  __builtin_offsetof(Config, previous_origin_to_common) == 312 &&
                  __builtin_offsetof(Config, serial_low) == 340,
              "Enhanced motion scalar fields have no vector padding");
} // namespace rypath::enhanced_motion_wire
