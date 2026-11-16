# Project status

## Current milestone

The declared mesh profiles are merged: polygonal OBJ/MTL in
[PR #34](https://github.com/rlefko/rypath/pull/34), STL in
[PR #35](https://github.com/rlefko/rypath/pull/35), and static glTF/GLB in
[PR #36](https://github.com/rlefko/rypath/pull/36), through `cf110a5`.
Their import profiles and validation records retain the supported semantics,
material adaptations and deferred features.

Spectral point lighting adds up to 64 PBRT point sources, transformed positions,
photometric intensity/power, finite visibility and direct surface/volume
illumination, including geometry-free homogeneous media. Checkpoints now use
renderer and identity contract 3. See the [point-light profile](pbrt-rendering.md#point-lights)
and [validation record](point-light-validation.md).

The next bounded compatibility task is spectral spot lighting. Long-run
convergence, licensed corpus acceptance and measured M4 Max benchmarks remain
on the [roadmap](roadmap.md). This does not establish offline MVP completion or
full pinned-corpus coverage.

## Validation

Point-light implementation `ed5c120` passes Debug, Release and ASan/UBSan (165/165 each),
Metal-disabled LLVM 18 tests (128/128), all 115 Python checks, changed-unit
Clang-Tidy, Objective-C++ analysis and repository hooks on M4 Max. All 39 Metal
tests also pass with API validation enabled and no GPU availability skips.

Eight matched PBRT scenes pass all 120 independent-seed region/channel gates.
Native checks cover light equations, GGX evaluation, finite visibility, alpha,
glass, Beer attenuation and independent HG quadrature. Actual shader mutations
detect incorrect MIS and omitted geometry-free illumination. Six existing scenes
retain byte-identical linear outputs against `cf110a5`; point-lit checkpoints
resume to exact Film state under independent and Sobol sampling.

Earlier [glTF](gltf-validation.md), [STL](stl-validation.md),
[OBJ](obj-validation.md), surface, media, texture, HDR and checkpoint records
retain historical evidence for those revisions. Their test counts are not
counts for the point-light revision.
