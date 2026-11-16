// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render_wire.h"

namespace rypath::lens_probe {
// Internal numerical probe; directions use the production camera helpers.
struct Result {
    render_wire::Point origin, direction, dx, dy, lens_offset;
    render_wire::Word lens_words[2];
    render_wire::Word camera_words[4];
    render_wire::Point dpdx, dpdy;
};
static_assert(sizeof(Result) == 108 && alignof(Result) == 4, "lens probe layout");
} // namespace rypath::lens_probe
