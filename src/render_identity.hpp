// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "render.hpp"
#include "sha256.hpp"
namespace rypath {
// Versioned canonical owned data, including inactive fields and exact IEEE bits.
// Excludes exposure when the film is unclamped, batch size and BSDF source/working
// byte accounting. Finite-clamp accumulation includes sensor exposure. Shared
// assets encode their contents, never allocation addresses or sharing topology.
Sha256Digest render_identity(const RenderScene &scene, const RenderSettings &settings);
} // namespace rypath
