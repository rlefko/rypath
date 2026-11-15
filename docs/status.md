# Project status

## Current milestone

[Resumable rendering (PR #33)](https://github.com/rlefko/rypath/pull/33) is merged
at `53dafb9`. Its [checkpoint contract](checkpointing.md) and
[validation record](checkpoint-validation.md) retain the measured evidence.

The current polygonal OBJ/MTL PR adds portable preflight and conversion to a
relocatable PBRT bundle, preserving mesh attributes and provenance. Legacy and
PBR material conversion follows a named approximation profile with warnings;
unsupported geometry and material behavior produce explicit diagnostics. See
the [import profile](obj-import.md) and [validation](obj-validation.md).

A next bounded task is STL import through the mesh conversion path. Broader
OBJ/MTL profiles, glTF/GLB, further PBRT support, long-run convergence studies,
licensed corpus acceptance and measured M4 Max benchmarks remain on the
[roadmap](roadmap.md). This does not establish completion of the offline MVP,
full-spec OBJ support, or full pinned-corpus coverage.

## Validation

OBJ implementation `5d0364c` passes Debug, Release and ASan/UBSan (127/127 each),
the Metal-disabled LLVM 18 build (95/95), all 112 Python checks, changed-unit
Clang-Tidy and repository hooks. Tests ran on Apple M4 Max, macOS 26.6 and
Xcode 26.6.

Ten imported material cases match independently authored PBRT models within
predeclared raw-XYZ statistical thresholds on M4 Max. Bump/normal paired checks
and omitted-map controls pass. A small pinned upstream cube bundle renders in
both the pinned PBRT CPU renderer and rypath; this establishes selected asset
acceptance, not external-renderer pixel equivalence. Independent review has no
remaining actionable findings.

Earlier surface, media, spectrum, texture, HDR and checkpoint records retain
their historical validation evidence. Their counts describe those revisions.
