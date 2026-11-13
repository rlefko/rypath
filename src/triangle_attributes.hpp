// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "geometry.hpp"

namespace rypath {

// reverse_winding is the existing geometric flip: authored reversal XOR
// handedness of the original instance transform. Translation is irrelevant.
// The owned result contains world-space normals with one common positive scale;
// individual vertex lengths are not normalized away.
std::string prepare_shading_normals(const GeometryMesh &mesh, const GeometryTransform &transform,
                                    bool reverse_winding, std::vector<GeometryPoint> &normals);

// Reject positive-area one-sided ties and numerically indeterminate orientation
// where pinned PBRT hit/sample agreement cannot be retained. Uses the world vertices
// that emitter preparation bakes and the actual packed normal representation.
std::string validate_normal_emission(const GeometryMesh &mesh, const GeometryTransform &transform,
                                     bool reverse_winding, bool two_sided,
                                     std::span<const GeometryPoint> normals);

} // namespace rypath
