# Project status

## Current milestone

[Core PBRT texturing (PR #27)](https://github.com/rlefko/rypath/pull/27) is merged
at `8aaa003e`. It adds Adam7 PNG/JPEG, typed graphs and procedural textures,
alpha, bump/normal maps, PLY displacement, and image area/environment lighting.

Linear HDR image I/O is ready for review. The implementation adds bounded HALF/FLOAT EXR and Radiance RGBE inputs, binary32
texture/light storage, and atomic linear EXR output alongside display PNG.
`--render-pbrt SCENE --output IMAGE.exr` honors `Film.savefp16` (default HALF;
false selects FLOAT). Portable preflight remains separate from Metal rendering.
See the [render profile](pbrt-rendering.md) and [HDR record](hdr-image-validation.md).

Fractional emitting alpha, Ptex, wider color spaces, additional materials and
importers, media, checkpointing and the interactive app remain on the
[roadmap](roadmap.md). These slices do not establish offline MVP completion.

## Validation

Current HDR working-tree evidence includes independent installed OpenEXR
readback of 48 film channel values, 380 pinned PBRT source/mip dimension/channel
checks, and 90 matched render gates across six fixtures and 192 PFM outputs.
The [HDR record](hdr-image-validation.md) identifies revision, environment,
thresholds and selected external coverage. Implementation `8a6ff53` passes 87/87 Debug, Release and ASan/UBSan tests,
68/68 portable tests, 71 Python checks, and changed-unit static analysis. The
native HDR probe passes 2,304 HALF/FLOAT queries; overflow regressions verify
whole-batch failure without partial accumulation. All 21 Metal tests pass API
validation; shader instrumentation passes 20/21, with the combined texture test
timing out while Apple's compiler service is active.

Historical PR #27 evidence is retained in [core texture validation](core-texture-validation.md)
and [performance](core-texture-performance.md): 82/82 Debug, Release and sanitizer
tests, 64/64 portable tests, native GPU validation, and pinned PBRT comparisons.
Those counts describe that completed slice, not the current HDR revision.
Selected external Cornell, PLY, PNGSuite and OpenEXR coverage remains distinct
from full pinned-corpus support.
