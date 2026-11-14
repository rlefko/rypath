# Project status

## Current milestone

[File lighting (PR #19)](https://github.com/rlefko/rypath/pull/19) is merged.
The current slice adds built-in named and file spectra to diffuse materials and
spectrum constant textures. It retains original spectral knots and applies the
diffuse model's clamp after interpolation, including signed and above-one file
values. CPU compilation preserves the existing Metal material and sampling path.

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
The next bounded compatibility task is to scope blackbody diffuse reflectance
against pinned PBRT.

## Validation

The [spectral reflectance record](spectral-reflectance-validation.md) records
implementation `3d437e3` on Apple M4 Max: 73/73 tests in each of Debug, Release,
and ASan/UBSan, 61/61 with Metal disabled, and 14/14 with Metal API and shader
validation, without skips. Independent exact-rational checks covered 273 curves
and 140,062 sampled wavelengths. Both translation routes for all 88 built-in
spectra passed 165,528 pinned PBRT comparisons each.

The full image comparison passed 516 statistical gates and two filter-sensitivity
gates across 42 fixtures and 1,346 linear PFM outputs. Native spectral controls,
batch invariance, static analysis, 46 Python tests, repository hooks, and independent
review passed. Review verified the exact-storage allocation fix and documented
inherited Metal underflow for tiny reflectance. Hosted checks are reported on the
PR separately.

The [file-spectrum record](file-spectrum-validation.md) retains historical
evidence for PR #19. This remains selected coverage, not full-corpus support.
