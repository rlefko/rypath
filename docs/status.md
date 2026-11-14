# Project status

## Current milestone

[EWA image textures (PR #23)](https://github.com/rlefko/rypath/pull/23) is merged.
The current slice adds noninterlaced 16-bit PNG diffuse textures, preserving
source precision through PBRT-compatible binary16 mip storage and float filtering.
Portable and Metal paths preserve dark half values and shared mixed-format image
storage. Grayscale-alpha16 and grayscale16 transparency-key inputs are explicitly
documented extensions beyond the pinned PBRT decoder.

`--render-pbrt SCENE --output IMAGE.png` supports the declared spectral diffuse
profile, a pinhole camera, constant environment and directly placed area lights,
box/triangle filters, and independent or Sobol sampling. It honors finite depth,
retains calibrated raw XYZ and complete sample batches, and publishes a final PNG.
Portable `--check-pbrt` checks scene structure and supported inputs without Metal.
The [render profile](pbrt-rendering.md) defines supported semantics and limitations.

Selected external coverage includes the unchanged Cornell scene, a licensed PLY
asset in owned wrappers, and unmodified 8-bit and 16-bit PNGSuite images in owned wrappers.
Textured emission, wider color spaces, additional materials and importers,
OpenEXR, checkpointing, and the interactive app remain on the [roadmap](roadmap.md).
The next bounded compatibility task is to scope indexed-color PNG diffuse inputs
against pinned PBRT, including palette, transparency, and resource-limit behavior.

## Validation

The [16-bit PNG record](image16-texture-validation.md) records implementation `28bffc7`
on Apple M4 Max: 74/74 tests in each of Debug, Release, and ASan/UBSan,
61/61 with Metal disabled, and 15/15 with Metal API and shader validation,
without skips. Pinned PBRT matches 540,882 stored RGB half values, covering
every 16-bit source value under both encodings and selected mip/resize cases.

Independent review verified all 65,536 native half patterns, 393,304 native
lookups, failure/retry accounting, and 8,075,112 unchanged encoded8 mip words.
Non-power-of-two resize rounding differences remain explicit in the record.
An allocation-cap diagnostic finding is fixed and covered by regression tests.
The full pinned PBRT comparison passed 573 statistical gates and eight sensitivity
controls across 49 fixtures and 1,576 linear PFM outputs. All 61 Python tests,
static analysis, repository hooks, an inspected CLI render, and independent review
passed. Hosted checks are reported on the PR separately.

The [EWA record](ewa-texture-validation.md) retains historical
evidence for PR #23. This remains selected coverage, not full-corpus support
or offline MVP completion.
