# Project status

## Current milestone

[Named/file reflectance (PR #20)](https://github.com/rlefko/rypath/pull/20) is merged.
The current slice adds continuous blackbody diffuse reflectance to inline/named
materials and spectrum constant textures. Checked temperatures feed independent
CPU and Metal evaluators with PBRT's Wien-peak normalization, preserving existing
bindings, sampling, and raw accumulation.

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
The next bounded compatibility task is to scope point filtering for PNG textures
against pinned PBRT.

## Validation

The [blackbody reflectance record](blackbody-reflectance-validation.md) records
implementation `93bda16` on Apple M4 Max: 74/74 tests in each of Debug, Release,
and ASan/UBSan, 61/61 with Metal disabled, and 15/15 with Metal API and shader
validation, without skips. An independent high-precision oracle checked 32,402
samples per translation route; pinned PBRT checked 11,286 per route. The direct
GPU probe passed 9,487 cases, and four transport controls covered 131,072 paths.

The full image comparison passed 534 statistical gates and two sensitivity
controls across 44 fixtures and 1,410 linear PFM outputs. Exact batch invariance,
52 Python tests, static analysis, repository hooks, an inspected CLI render, and
independent review passed. Device underflow limits remain explicit. Hosted checks
are reported on the PR separately.

The [spectral reflectance record](spectral-reflectance-validation.md) retains
historical evidence for PR #20. This remains selected coverage, not full-corpus
support or offline MVP completion.
