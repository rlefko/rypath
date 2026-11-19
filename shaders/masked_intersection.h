// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "intersection.h"

// Native auto-accept remains disabled. Shadows terminate only after our own
// validated and alpha-accepted candidate establishes occlusion.
template <typename Alpha>
inline SurfaceHit trace_masked_surface(metal::raytracing::ray ray,
                                       metal::raytracing::instance_acceleration_structure scene,
                                       uint mask, thread Alpha &alpha, bool any_hit = false) {
    return trace_validated_surface(ray, scene, mask, alpha, any_hit);
}
