// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "lens_probe_wire.h"
#include "render_wire.h"
#include <vector>
std::vector<rypath::lens_probe::Result> lens_gpu_probe(const rypath::render_wire::Parameters &p);
