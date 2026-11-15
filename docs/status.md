# Project status

## Current milestone

[Polygonal OBJ/MTL import (PR #34)](https://github.com/rlefko/rypath/pull/34) is
merged at `ae7a883`. Its [import profile](obj-import.md) and
[validation record](obj-validation.md) retain the declared semantics and evidence.

The STL import change adds portable ASCII/binary preflight and relocatable PBRT
bundles, preserving flat triangle geometry and provenance. VisCAM/SolidView and
Materialise colors use an explicit sRGB-to-diffuse profile; Materialise alpha maps
to stochastic surface coverage. Ambiguous colored inputs require a convention
selection. See the [STL profile](stl-import.md) and [validation](stl-validation.md).

A next bounded task is defining and implementing the static glTF/GLB import
profile. Broader OBJ/MTL and STL profiles, further PBRT support, long-run
convergence studies, licensed corpus acceptance and measured M4 Max benchmarks
remain on the [roadmap](roadmap.md). This does not establish offline MVP
completion, full-spec OBJ support, or full pinned-corpus coverage.

## Validation

STL implementation `9a1aaf6` passes Debug, Release and ASan/UBSan (142/142 each),
the Metal-disabled LLVM 18 build (109/109), all 112 Python checks, changed-unit
Clang-Tidy and repository hooks. Tests ran on Apple M4 Max, macOS 26.6 and
Xcode 26.6.

Six imported geometry/color/coverage cases match independently authored PBRT
models within predeclared raw-XYZ statistical thresholds on M4 Max. Negative
controls detect ignored alpha, missing color transfer and accidental smoothing.
A small pinned upstream STL triangle bundle renders with both the pinned PBRT
CPU renderer and rypath. This establishes selected asset acceptance, without an
external-renderer pixel-equivalence claim. Independent review has no remaining
actionable findings.

Earlier OBJ, surface, media, spectrum, texture, HDR and checkpoint records retain
their historical validation evidence. Their counts describe those revisions.
