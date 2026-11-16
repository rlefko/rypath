# Project status

## Current milestone

Polygonal OBJ/MTL, STL and static glTF/GLB profiles are merged in PRs #34–36.
Spectral point lighting merged in [PR #37](https://github.com/rlefko/rypath/pull/37),
through `fc41487`. Their profiles retain the supported semantics, adaptations
and deferred features.

Spectral spot lighting merged in [PR #38](https://github.com/rlefko/rypath/pull/38)
through `381512f`. It extends direct surface and homogeneous-volume illumination,
including geometry-free fog, with cubic cosine falloff, affine aiming and
photometric power. Point and spot sources share a 64-light bound. Renderer and
checkpoint identity contracts advanced to version 4 in that change. See the
[spot profile](pbrt-rendering.md#spot-lights) and
[validation record](spot-light-validation.md).

The bounded-medium reliability follow-up in
[PR #39](https://github.com/rlefko/rypath/pull/39) repairs the inherited fog failure with
certified null-boundary departure and fixed finite visibility endpoints.
Renderer checkpoint contract 5 marks the changed numerical policy; see the
[repair record](bounded-fog-validation.md). Spectral distant lights are the next
bounded lighting task. Long-run convergence,
licensed corpus acceptance and measured M4 Max benchmarks remain on the
[roadmap](roadmap.md). Offline MVP completion and full pinned-corpus coverage
remain unestablished.

## Validation

Fog repair `bc4dacc` passes Debug, Release and ASan/UBSan (167/167 each),
Metal-disabled LLVM 18 (129/129), all 40 Metal API tests without availability
skips, 119 Python checks, changed-unit static analysis and hooks. The original
point and spot bounded scenes complete at 768×512, 512 spp. Both pinned PBRT
volume comparisons pass all 30 region/channel gates; the exact parent failure
and a projected-endpoint shader mutation establish regression sensitivity.

Historical spot implementation `708bb55` passes Debug, Release and ASan/UBSan
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
