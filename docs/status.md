# Project status

## Current milestone

[Packed grayscale PNG textures (PR #26)](https://github.com/rlefko/rypath/pull/26)
is merged. The current core texturing slice adds Adam7 PNG and bounded JPEG,
typed texture graphs and procedural textures, nonemitting alpha and certified
binary emitter masks, bump/normal maps, PLY displacement, and image area and
equal-area environment lights.

`--render-pbrt SCENE --output IMAGE.png` renders the declared spectral diffuse
profile with a pinhole camera, box/triangle filtering, independent or Sobol
sampling, authored finite depth, complete raw XYZ batches, and final PNG output.
Portable `--check-pbrt` validates supported inputs and reports unsupported features
before rendering. The [render profile](pbrt-rendering.md) defines exact boundaries.

Fractional emitting alpha, Ptex, HDR/floating-point image inputs, wider color
spaces, additional materials/importers, OpenEXR output, checkpointing, and the
interactive app remain on the [roadmap](roadmap.md). Project foundations and the
foundation audit were already merged in PRs #1 and #12.

## Validation

The [core texture record](core-texture-validation.md) separates numerical
adaptations, independent review findings, and implementation measurements.
The current implementation passes **82/82 Debug, Release and ASan/UBSan tests** and
**64/64 portable tests**. Native M4 Max checks include 1,403 graph queries,
32 masked traversal cases, 1,115 exact alpha hashes, seven surface-frame cases,
and concentrated environment-PDF stress. All 20 Metal tests also pass with API and shader validation enabled.

Pinned PBRT comparisons pass **6,440 graph values**, **48 new render gates**, and
all **594 established render gates plus eight sensitivity controls**. The 71
Python policy/comparison tests and changed-unit static analysis pass. Complete
render timings and measured regressions are recorded separately in
[performance](core-texture-performance.md); hosted checks are reported on the PR.

Selected external Cornell, PLY, and PNGSuite coverage remains distinct from full
pinned-corpus support. Historical records retain their own tested revisions;
these results do not establish offline MVP completion.
