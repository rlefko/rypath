// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "contribution_wire.h"
#include "enhanced_wire.h"
#if !defined(__METAL_VERSION__)
#include <bit>
#endif

// Structural codec for the owned triangle/world-direction chart. This does not
// validate scene indices, visibility, chart support, inverse maps or Jacobians.
// Existing light tags/layouts 0..4 and the 48-byte record remain unchanged.
namespace rypath_enhanced_chart {
namespace ew = rypath::enhanced_wire;
namespace cw = rypath::contribution_wire;
namespace rw = rypath::render_wire;
using Word = ew::Word;
enum SurfaceTag : Word { interior_zomega = 5, terminal_surface = 6 };
enum class Status : Word { ok, invalid_depth, invalid_slot, invalid_record, wrong_type };
struct XYZ {
    float x = 0, y = 0, z = 0;
};
struct Surface {
    SurfaceTag tag = interior_zomega;
    // Scatter ordinal k-1, not a transport vertex-array index; must be >=1.
    Word pivot_event = ew::invalid;
    Word instance = ew::invalid, primitive = ew::invalid;
    Word primitive_kind = rw::triangle_primitive;
    float u = 0, v = 0;
    // Fixed outgoing WORLD solid-angle direction at an interior pivot. Terminal
    // surface records instead require all three components to have +0 bits.
    XYZ omega;
};
struct Encoded {
    ew::Record value{}; // Invalid version on failure; never silently generic.
    Status status = Status::invalid_record;
};
struct Decoded {
    Surface value;
    Status status = Status::invalid_record;
};
inline Word bits(float value) {
#if defined(__METAL_VERSION__)
    return as_type<Word>(value);
#else
    return std::bit_cast<Word>(value);
#endif
}
inline float from_bits(Word word) {
#if defined(__METAL_VERSION__)
    return as_type<float>(word);
#else
    return std::bit_cast<float>(word);
#endif
}
inline bool finite(float value) { return (bits(value) & 0x7f800000u) != 0x7f800000u; }
inline bool nonnegative(float value) {
    const Word word = bits(value);
    // Signed zero is preserved. Check sign/magnitude BEFORE FLOAT comparisons:
    // a negative subnormal may otherwise flush to a comparison's apparent -0.
    return finite(value) && (!(word & 0x80000000u) || (word & 0x7fffffffu) == 0);
}
inline bool triangle_uv(float u, float v) {
    return nonnegative(u) && nonnegative(v) && u <= 1 && v <= 1 && u + v <= 0x1.000002p0f;
}
inline bool unit_omega(XYZ omega) {
    if (!finite(omega.x) || !finite(omega.y) || !finite(omega.z))
        return false;
    const float norm = (omega.x * omega.x + omega.y * omega.y) + omega.z * omega.z;
    if (!finite(norm))
        return false;
    const float error = norm - 1.f;
    return (error < 0 ? -error : error) <= 0x1p-18f;
}
inline bool canonical_generic(ew::Record record) {
    return record.version == ew::record_version && record.tag == ew::generic &&
           record.light_id == ew::invalid && record.event_ordinal == ew::invalid &&
           record.instance == ew::invalid && record.primitive == ew::invalid &&
           record.kind == ew::invalid && record.reserved == 0 &&
           (record.coordinate_bits[0] | record.coordinate_bits[1] | record.coordinate_bits[2] |
            record.coordinate_bits[3]) == 0;
}
inline Status validate_chart(ew::Record record, Word selected_slot, Word depth) {
    if (depth > 65)
        return Status::invalid_depth;
    const bool null = selected_slot == cw::enhanced_null_slot;
    if (!null && selected_slot >= 2u * depth + 1u)
        return Status::invalid_slot;
    if (record.version != ew::record_version)
        return Status::invalid_record;
    if (record.tag == ew::generic)
        return canonical_generic(record) ? Status::ok : Status::invalid_record;
    if (null || record.coordinate_bits[3] != 0)
        return Status::invalid_record;
    const Word event = selected_slot / 2u;
    if (record.tag == interior_zomega || record.tag == terminal_surface) {
        if (record.event_ordinal < 1 || record.event_ordinal > depth ||
            record.instance == ew::invalid || record.primitive == ew::invalid ||
            record.kind != rw::triangle_primitive ||
            !triangle_uv(from_bits(record.coordinate_bits[0]),
                         from_bits(record.coordinate_bits[1])))
            return Status::invalid_record;
        const XYZ omega{from_bits(record.light_id), from_bits(record.reserved),
                        from_bits(record.coordinate_bits[2])};
        if (record.tag == interior_zomega)
            return record.event_ordinal < event && unit_omega(omega) ? Status::ok
                                                                     : Status::invalid_record;
        return !(selected_slot & 1u) && record.event_ordinal == event &&
                       (record.light_id | record.reserved | record.coordinate_bits[2]) == 0
                   ? Status::ok
                   : Status::invalid_record;
    }
    if (record.tag > ew::environment_omega || !(selected_slot & 1u) ||
        record.event_ordinal != event || record.light_id == ew::invalid || record.reserved != 0)
        return Status::invalid_record;
    if (record.tag == ew::triangle_area)
        return record.instance != ew::invalid && record.primitive != ew::invalid &&
                       record.kind == rw::triangle_primitive && record.coordinate_bits[2] == 0 &&
                       triangle_uv(from_bits(record.coordinate_bits[0]),
                                   from_bits(record.coordinate_bits[1]))
                   ? Status::ok
                   : Status::invalid_record;
    if (record.instance != ew::invalid || record.primitive != ew::invalid ||
        record.kind != ew::invalid)
        return Status::invalid_record;
    if (record.tag == ew::environment_omega)
        return unit_omega({from_bits(record.coordinate_bits[0]),
                           from_bits(record.coordinate_bits[1]),
                           from_bits(record.coordinate_bits[2])})
                   ? Status::ok
                   : Status::invalid_record;
    return (record.coordinate_bits[0] | record.coordinate_bits[1] | record.coordinate_bits[2]) == 0
               ? Status::ok
               : Status::invalid_record;
}
inline bool valid_chart(ew::Record record, Word selected_slot, Word depth) {
    return validate_chart(record, selected_slot, depth) == Status::ok;
}
inline Encoded encode(Surface surface, Word selected_slot, Word depth) {
    const ew::Record record{ew::record_version,
                            Word(surface.tag),
                            bits(surface.omega.x),
                            surface.pivot_event,
                            surface.instance,
                            surface.primitive,
                            surface.primitive_kind,
                            bits(surface.omega.y),
                            {bits(surface.u), bits(surface.v), bits(surface.omega.z), 0}};
    const auto status = validate_chart(record, selected_slot, depth);
    if (surface.tag != interior_zomega && surface.tag != terminal_surface)
        return {{}, Status::wrong_type};
    return status == Status::ok ? Encoded{record, Status::ok} : Encoded{{}, status};
}
inline Decoded decode(ew::Record record, Word selected_slot, Word depth) {
    const auto status = validate_chart(record, selected_slot, depth);
    if (status != Status::ok)
        return {{}, status};
    if (record.tag != interior_zomega && record.tag != terminal_surface)
        return {{}, Status::wrong_type};
    const Surface surface{SurfaceTag(record.tag),
                          record.event_ordinal,
                          record.instance,
                          record.primitive,
                          record.kind,
                          from_bits(record.coordinate_bits[0]),
                          from_bits(record.coordinate_bits[1]),
                          {from_bits(record.light_id), from_bits(record.reserved),
                           from_bits(record.coordinate_bits[2])}};
    return {surface, Status::ok};
}
static_assert(sizeof(ew::Record) == 48 && alignof(ew::Record) == 4,
              "Owned chart codec preserves the existing record ABI");
} // namespace rypath_enhanced_chart
