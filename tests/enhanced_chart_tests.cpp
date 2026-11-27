// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_chart_shared.h"

#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace c = rypath_enhanced_chart;
namespace ew = rypath::enhanced_wire;
namespace cw = rypath::contribution_wire;
namespace rw = rypath::render_wire;
using Words = std::array<std::uint32_t, 12>;
void need(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
float f(std::uint32_t word) { return std::bit_cast<float>(word); }
std::uint32_t b(float value) { return std::bit_cast<std::uint32_t>(value); }
c::Surface interior() {
    return {c::interior_zomega, 1, 17, 23, rw::triangle_primitive, .25f, .5f, {0, 0, 1}};
}
void codec() {
    auto surface = interior();
    surface.omega = {.6f, -.8f, 0};
    const auto encoded = c::encode(surface, 4, 2);
    // Independent explicit layout witness, rather than decode(encode)==input
    // alone: every semantic scalar has a declared existing 48B storage word.
    const Words expected{ew::record_version,     5,           0x3f19999au, 1,           17, 23,
                         rw::triangle_primitive, 0xbf4ccccdu, 0x3e800000u, 0x3f000000u, 0,  0};
    need(encoded.status == c::Status::ok && std::bit_cast<Words>(encoded.value) == expected,
         "Interior codec must match the explicit twelve-word layout");
    const auto decoded = c::decode(encoded.value, 4, 2);
    need(decoded.status == c::Status::ok && decoded.value.tag == c::interior_zomega &&
             decoded.value.pivot_event == 1 && decoded.value.instance == 17 &&
             decoded.value.primitive == 23 &&
             decoded.value.primitive_kind == rw::triangle_primitive &&
             b(decoded.value.u) == 0x3e800000u && b(decoded.value.v) == 0x3f000000u &&
             b(decoded.value.omega.x) == 0x3f19999au && b(decoded.value.omega.y) == 0xbf4ccccdu &&
             b(decoded.value.omega.z) == 0,
         "Typed decode must recover every field without direction normalization");
    need(std::bit_cast<Words>(c::encode(decoded.value, 4, 2).value) == expected,
         "Typed record roundtrip must preserve every bit");
    surface = interior();
    surface.u = f(0x80000000u);
    surface.v = f(1);
    surface.omega = {f(0x80000001u), f(0x80000000u), 1};
    const auto tiny = c::encode(surface, 4, 2);
    need(tiny.status == c::Status::ok, "Positive subnormal UV and finite tiny omega are retained");
    const auto roundtrip = c::decode(tiny.value, 4, 2);
    need(roundtrip.status == c::Status::ok && b(roundtrip.value.u) == 0x80000000u &&
             b(roundtrip.value.v) == 1 && b(roundtrip.value.omega.x) == 0x80000001u &&
             b(roundtrip.value.omega.y) == 0x80000000u,
         "Signed zeros/subnormals must not be quantized or silently erased by the codec");
    surface = interior();
    surface.tag = c::terminal_surface;
    surface.omega = {};
    const auto terminal = c::encode(surface, 2, 1);
    const Words terminal_words{ew::record_version,     6, 0,           1,           17, 23,
                               rw::triangle_primitive, 0, 0x3e800000u, 0x3f000000u, 0,  0};
    need(terminal.status == c::Status::ok &&
             std::bit_cast<Words>(terminal.value) == terminal_words &&
             c::decode(terminal.value, 2, 1).status == c::Status::ok,
         "Terminal surface must use the same record with exact zero omega bits");
    surface.omega.y = f(0x80000000u);
    const auto bad = c::encode(surface, 2, 1);
    need(bad.status != c::Status::ok && bad.value.version == 0 && !c::valid_chart(bad.value, 2, 1),
         "Invalid terminal omega must fail closed, not silently encode generic or normalize");
}
void labels_and_depth() {
    const auto generic = ew::empty_record();
    need(c::valid_chart(generic, 0, 0) && c::valid_chart(generic, cw::enhanced_null_slot, 0),
         "D0 generic and canonical null records remain supported");
    need(!c::valid_chart(generic, 0xffffffffu, 65) && !c::valid_chart(generic, 1, 0) &&
             !c::valid_chart(generic, cw::enhanced_null_slot, 66),
         "Fresh request, missing N_D slot and invalid depth must not validate as selected charts");
    auto surface = interior();
    need(c::encode(surface, 4, 2).status == c::Status::ok &&
             c::encode(surface, 5, 3).status == c::Status::ok,
         "Interior pivot precedes selected E/N event");
    need(c::encode(surface, 2, 2).status != c::Status::ok &&
             c::encode(surface, 3, 2).status != c::Status::ok,
         "Selected event equal to pivot is not an interior suffix chart");
    surface.pivot_event = 0;
    need(c::encode(surface, 4, 2).status != c::Status::ok, "Primary event0 is outside this chart");
    surface.pivot_event = 64;
    need(c::encode(surface, 130, 65).status == c::Status::ok, "Last interior pivot64 before E65");
    surface.pivot_event = 65;
    need(c::encode(surface, 130, 65).status != c::Status::ok, "Pivot cannot equal selected E65");
    surface.tag = c::terminal_surface;
    surface.omega = {};
    need(c::encode(surface, 130, 65).status == c::Status::ok &&
             c::encode(surface, 131, 65).status != c::Status::ok,
         "Terminal E65 exists, terminal N65 does not");
    surface.pivot_event = 1;
    need(c::encode(surface, 2, 1).status == c::Status::ok &&
             c::encode(surface, 3, 2).status != c::Status::ok &&
             c::encode(surface, 4, 2).status != c::Status::ok,
         "Terminal surface label must equal an even selected event");
    surface.primitive_kind = rw::patch_primitive;
    need(c::encode(surface, 2, 1).status != c::Status::ok,
         "Patch is not silently a triangle chart");
    surface.primitive_kind = rw::analytic_primitive;
    need(c::encode(surface, 2, 1).status != c::Status::ok, "Analytic surface is unsupported");
    surface.primitive_kind = rw::triangle_primitive;
    surface.instance = surface.primitive = 0xfffffffeu;
    need(c::encode(surface, 2, 1).status == c::Status::ok,
         "Valid IDs are structural non-sentinels, not invented scene-range queries");
    need(c::decode(generic, 0, 0).status == c::Status::wrong_type,
         "A valid generic chart is not a decoded surface");
}
void old_light_layouts() {
    const ew::Record triangle{
        ew::record_version,     ew::triangle_area, 7, 0, 17, 23, rw::triangle_primitive, 0,
        {b(.25f), b(.5f), 0, 0}};
    need(c::valid_chart(triangle, 1, 1) && !c::valid_chart(triangle, 0, 1) &&
             !c::valid_chart(triangle, 3, 2) &&
             !c::valid_chart(triangle, cw::enhanced_null_slot, 1),
         "Old triangle layout must retain odd-slot/event identity and reject null");
    auto negative_tiny_uv = triangle;
    negative_tiny_uv.coordinate_bits[0] = 0x80000001u;
    need(!c::valid_chart(negative_tiny_uv, 1, 1),
         "Old triangle charts also reject a negative subnormal UV before FLOAT comparison");
    for (const auto tag : {ew::positional_mass, ew::distant_mass, ew::environment_omega}) {
        ew::Record record{ew::record_version, tag,         8, 1,           ew::invalid,
                          ew::invalid,        ew::invalid, 0, {0, 0, 0, 0}};
        if (tag == ew::environment_omega)
            record.coordinate_bits[1] = b(1.f);
        need(c::valid_chart(record, 3, 2) && !c::valid_chart(record, 2, 2),
             "Old counting/solid-angle light charts require their actual terminal N slot");
        const auto original = std::bit_cast<Words>(record);
        need(c::decode(record, 3, 2).status == c::Status::wrong_type &&
                 std::bit_cast<Words>(record) == original,
             "Structural light validation must preserve existing record words");
        record.reserved = 1;
        need(!c::valid_chart(record, 3, 2), "Old light reserved fields remain zero");
    }
    auto dirty = ew::empty_record();
    dirty.coordinate_bits[0] = b(-0.f);
    need(!c::valid_chart(dirty, 0, 0), "Generic requires canonical positive-zero coordinate words");
}
void malformed_words_and_numbers() {
    const auto base = c::encode(interior(), 4, 2).value;
    const auto original = std::bit_cast<Words>(base);
    const Words poison{0,
                       7,
                       0x7f800000u,
                       0,
                       ew::invalid,
                       ew::invalid,
                       rw::patch_primitive,
                       0x7fc00000u,
                       0xbf800000u,
                       0xbf800000u,
                       0,
                       1};
    for (unsigned i = 0; i < original.size(); ++i) {
        auto words = original;
        words[i] = poison[i];
        need(!c::valid_chart(std::bit_cast<ew::Record>(words), 4, 2),
             "Each semantic/flags word must reject its independent invalid replacement");
    }
    for (float u : {f(0x80000001u), -.1f, std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::quiet_NaN(), std::nextafter(1.f, 2.f)}) {
        auto surface = interior();
        surface.u = u;
        need(c::encode(surface, 4, 2).status != c::Status::ok,
             "Negative-nonzero/subnormal/nonfinite/outside UV must be rejected by bits/range");
    }
    auto edge = interior();
    edge.u = 1;
    edge.v = 0x1p-23f;
    need(c::encode(edge, 4, 2).status == c::Status::ok,
         "One-ULP rounded triangle sum slab retained");
    edge.v = 0x1p-22f;
    need(c::encode(edge, 4, 2).status != c::Status::ok, "Two-ULP triangle sum is outside the slab");
    for (const auto omega : {c::XYZ{}, c::XYZ{0, 0, f(1)}, c::XYZ{1.f + 0x1p-18f, 0, 0},
                             c::XYZ{std::numeric_limits<float>::infinity(), 0, 0},
                             c::XYZ{0, std::numeric_limits<float>::quiet_NaN(), 1}}) {
        auto surface = interior();
        surface.omega = omega;
        need(c::encode(surface, 4, 2).status != c::Status::ok,
             "Zero/subnormal-length/nonunit/nonfinite omega must not be normalized into validity");
    }
    auto near_unit = interior();
    near_unit.omega = {1.f + 0x1p-20f, 0, 0};
    const auto accepted = c::encode(near_unit, 4, 2);
    need(accepted.status == c::Status::ok && accepted.value.light_id == b(near_unit.omega.x),
         "Existing unit tolerance admits represented direction without modifying its bits");
}
} // namespace
int main() try {
    codec();
    labels_and_depth();
    old_light_layouts();
    malformed_words_and_numbers();
    std::cout << "Enhanced owned surface/light structural codec checks passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
