# Project status

## Current milestone

Polygonal OBJ/MTL, STL and static glTF/GLB profiles are merged in PRs #34–36.
Spectral point lighting merged in [PR #37](https://github.com/rlefko/rypath/pull/37),
through `fc41487`. Their profiles retain the supported semantics, adaptations
and deferred features.

Spectral spot lighting extends direct surface and homogeneous-volume illumination,
including geometry-free fog, with cubic cosine falloff, affine aiming and
photometric power. Point and spot sources share a 64-light bound. Renderer and
checkpoint identity contracts are now version 4. See the
[spot profile](pbrt-rendering.md#spot-lights) and
[validation record](spot-light-validation.md).

The next bounded lighting task is spectral distant lights. A larger fog
demonstration also exposed an inherited bounded-medium arithmetic failure,
reproduced on main; its [diagnosis record](spot-light-validation.md#demonstration-images-and-a-bounded-medium-follow-up)
is a separate reliability follow-up. Long-run convergence,
licensed corpus acceptance and measured M4 Max benchmarks remain on the
[roadmap](roadmap.md). Offline MVP completion and full pinned-corpus coverage
remain unestablished.

## Validation

Spot implementation `708bb55` passes Debug, Release and ASan/UBSan
(167/167 each), Metal-disabled LLVM 18 (129/129), 119 Python checks,
changed-unit Clang-Tidy, native analysis and hooks on M4 Max. All 40 Metal tests
pass with API validation and no availability skips.

Independent cone/BSDF/visibility and HG-quadrature checks pass. Four actual shader
mutations are detected; review-driven regressions reject subnormal angular-support
changes. Eight matched PBRT scenes pass all 120 region/channel gates, with an
explicit reference-only uniform sampler for the affine case to avoid a pinned
PBRT light-bound defect. Nine baseline scenes retain byte-identical linear
outputs, and spot-lit checkpoints resume to exact Film state with both samplers.

Earlier [point](point-light-validation.md), [glTF](gltf-validation.md),
[STL](stl-validation.md), [OBJ](obj-validation.md), surface, media, texture, HDR
and checkpoint records retain historical evidence for their recorded revisions.
