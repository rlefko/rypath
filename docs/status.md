# Project status

## Current milestone

[Blackbody reflectance (PR #21)](https://github.com/rlefko/rypath/pull/21) is merged.
The current slice adds point filtering for spectrum PNG diffuse textures. CPU
and Metal lookup preserve signed texel rounding, floor mip selection, and
explicit operation order at discontinuities. Point, bilinear, and trilinear
descriptors share image storage, and reporting identifies modes bound to placed
geometry.

`--render-pbrt SCENE --output IMAGE.png` supports the declared spectral diffuse
profile, a pinhole camera, constant environment and directly placed area lights,
box/triangle filters, and independent or Sobol sampling. It honors finite depth,
retains calibrated raw XYZ and complete sample batches, and publishes a final PNG.
Portable `--check-pbrt` checks scene structure and supported inputs without Metal.
The [render profile](pbrt-rendering.md) defines supported semantics and limitations.

Selected external coverage includes the unchanged Cornell scene, a licensed PLY
asset in owned wrappers, and an unmodified PNGSuite image in an owned wrapper.
Textured emission, wider color spaces, additional materials and importers,
OpenEXR, checkpointing, and the interactive app remain on the [roadmap](roadmap.md).
The next bounded compatibility task is to scope EWA filtering for PNG textures
against pinned PBRT.

## Validation

The [point texture record](point-texture-validation.md) records implementation
`6490115` on Apple M4 Max: 74/74 tests in each of Debug, Release,
and ASan/UBSan, 61/61 with Metal disabled, and 15/15 with Metal API and shader
validation, without skips. The native probe checks 31 point cases at four sample
budgets. Independent review passed 65,289 repeat-coordinate and 6,600 LOD
arithmetic checks. Triangle and patch RGB controls, secondary-hit sensitivity,
and exact batch invariance passed.

The full pinned PBRT comparison passed 540 statistical gates and four sensitivity
controls across 45 fixtures and 1,444 linear PFM outputs.
All 53 Python tests, static analysis, repository hooks, an inspected CLI render,
and independent review passed. Immediate LOD-boundary differences between CPU
and GPU logarithms remain explicit. Hosted checks are reported on the PR separately.

The [blackbody reflectance record](blackbody-reflectance-validation.md) retains
historical evidence for PR #21. This remains selected coverage, not full-corpus
support or offline MVP completion.
