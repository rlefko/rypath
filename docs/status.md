# Project status

## Current milestone

[Trilinear image textures (PR #16)](https://github.com/rlefko/rypath/pull/16)
are merged. The current slice adds blackbody emission to supported infinite
and diffuse area lights, with stable spectral calibration and explicit numeric
diagnostics. Blackbody reflectance remains unsupported.

`--render-pbrt SCENE --output IMAGE.png` supports sampled spectra, linear sRGB,
and blackbody lighting,
a pinhole camera, constant environment and directly placed area lights,
box/triangle filters, and independent or Sobol sampling. It honors finite depth,
retains calibrated raw XYZ and complete sample batches, and publishes a final PNG.
Portable `--check-pbrt` checks scene structure and supported image inputs without
Metal. The [render profile](pbrt-rendering.md) defines the subset.

Selected external coverage includes the unchanged Cornell scene, a PLY asset
in an owned wrapper, and an unmodified PNGSuite image in an owned wrapper.
Textured emission, wider color spaces, additional materials and importers,
OpenEXR, checkpointing, and the interactive app remain on the [roadmap](roadmap.md).
The next bounded compatibility task is to scope built-in named emission spectra
against pinned PBRT before expanding the spectral profile.

## Validation

The [blackbody record](blackbody-light-validation.md) records checks for
implementation `6a6d88d` and this PR's validation tests on Apple M4 Max:
70/70 tests in each of Debug, Release, and ASan/UBSan, 58/58 with Metal disabled,
and 14/14 with Metal API and shader validation. The pinned PBRT comparison
passed 402 statistical gates and two filter-sensitivity gates across 32 fixtures
and 1,026 linear PFM outputs. Independent spectral/endpoint controls, batch
invariance, static analysis, 27 Python tests, repository hooks, and independent
review passed. Hosted checks are reported on the PR separately.

The [trilinear record](trilinear-texture-validation.md) retains historical
evidence for PR #16. This remains selected coverage, not full-corpus support.
