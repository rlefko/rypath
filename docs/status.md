# Project status

## Current milestone

[16-bit PNG textures (PR #24)](https://github.com/rlefko/rypath/pull/24) is merged.
The current slice adds noninterlaced indexed PNG diffuse textures at 1-, 2-, 4-,
and 8-bit index depth. Checked palette expansion preserves RGB8 colors even at
zero alpha and reuses the existing portable and Metal texture paths.

`--render-pbrt SCENE --output IMAGE.png` supports the declared spectral diffuse
profile, a pinhole camera, constant environment and directly placed area lights,
box/triangle filters, and independent or Sobol sampling. It honors finite depth,
retains calibrated raw XYZ and complete sample batches, and publishes a final PNG.
Portable `--check-pbrt` checks scene structure and supported inputs without Metal.
The [render profile](pbrt-rendering.md) defines supported semantics and limitations.

Selected external coverage includes the unchanged Cornell scene, a licensed PLY
asset in owned wrappers, and unmodified RGB, grayscale, and indexed PNGSuite
images in owned wrappers. Textured emission, wider color spaces, additional
materials and importers, OpenEXR, checkpointing, and the interactive app remain
on the [roadmap](roadmap.md). The next bounded compatibility task is to scope
1-, 2-, and 4-bit grayscale PNG diffuse inputs against pinned PBRT, including
sample expansion, transparency keys, and resource limits.

## Validation

The [indexed PNG record](indexed-texture-validation.md) records implementation
`de970a8` on Apple M4 Max: **74/74 tests** in each of Debug, Release, and
ASan/UBSan, **61/61** with Metal disabled, and **15/15** with Metal API and
shader validation, without skips.

Exact reference checks cover 31,548 source RGB codes, 179,256 within-backend mip
codes, and 528 selected pinned PBRT mip codes. Native RGB8 controls match 8,192
path radiances exactly. Independent review covers 344 valid and 276 malformed
inputs plus resource boundaries; the prior binary16 reference checks also pass.
The full pinned PBRT suite passes **585 statistical gates and eight sensitivity
controls** across 51 fixtures and 1,640 linear outputs. All 68 Python tests,
static analysis, repository hooks, and an inspected CLI preview pass.
Hosted checks are reported on the PR separately.

The [16-bit record](image16-texture-validation.md) and
[EWA record](ewa-texture-validation.md) retain historical evidence for PRs #24
and #23. This remains selected coverage, not full-corpus support or offline
MVP completion.
